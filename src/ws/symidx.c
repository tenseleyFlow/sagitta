#define _POSIX_C_SOURCE 200809L

#include "ws/symidx.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/motion.h"
#include "syn/attr.h"
#include "text/edit.h"
#include "unicode/utf8.h"
#include "unicode/wordbreak.h"
#include "util/log.h"
#include "util/sort.h"

typedef struct SymPosting {
    u32 *data;
    size_t len;
    size_t cap;
} SymPosting;

static i64 sym_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("symbol index: monotonic clock failed");
    return (i64)ts.tv_sec * 1000000 + (i64)ts.tv_nsec / 1000;
}

static void posting_free_all(Strmap *map)
{
    StrmapIter it = strmap_iter(map);
    void *value;

    while (strmap_iter_next(&it, NULL, NULL, &value)) {
        SymPosting *posting = value;

        free(posting->data);
        free(posting);
    }
}

void yew_symidx_init(SymIndex *idx, Interner *intern)
{
    if (idx == NULL)
        return;
    (void)memset(idx, 0, sizeof(*idx));
    strmap_init(&idx->by_name);
    arena_init(&idx->arena);
    idx->intern = intern;
}

void yew_symidx_clear(SymIndex *idx)
{
    Interner *intern;
    u32 tick;

    if (idx == NULL)
        return;
    intern = idx->intern;
    tick = idx->tick == UINT32_MAX ? 1U : idx->tick + 1U;
    posting_free_all(&idx->by_name);
    strmap_free(&idx->by_name);
    arena_free_all(&idx->arena);
    idx->e.len = 0U;
    idx->updated.len = 0U;
    strmap_init(&idx->by_name);
    arena_init(&idx->arena);
    idx->tick = tick;
    idx->bytes = 0U;
    idx->intern = intern;
    idx->capped = false;
    idx->map_dirty = false;
}

void yew_symidx_free(SymIndex *idx)
{
    if (idx == NULL)
        return;
    posting_free_all(&idx->by_name);
    strmap_free(&idx->by_name);
    arena_free_all(&idx->arena);
    Vec_SymEntry_free(&idx->e);
    Vec_SymTick_free(&idx->updated);
    (void)memset(idx, 0, sizeof(*idx));
}

static void posting_push(SymPosting *posting, u32 entry)
{
    if (posting->len == posting->cap) {
        size_t cap = posting->cap == 0U ? 2U : posting->cap * 2U;

        if (cap < posting->cap)
            YEW_BUG("symbol index: posting overflow");
        posting->data = yew_xreallocarray(posting->data, cap,
                                          sizeof(*posting->data));
        posting->cap = cap;
    }
    posting->data[posting->len++] = entry;
}

static void symidx_map_add(SymIndex *idx, u32 entry)
{
    const SymEntry *sym = &idx->e.data[entry];
    const char *name = yew_intern_str(idx->intern, sym->name);
    size_t len = yew_intern_len(idx->intern, sym->name);
    SymPosting *posting;

    if (name == NULL)
        YEW_BUG("symbol index: unresolved interned name");
    posting = strmap_get(&idx->by_name, name, len);
    if (posting == NULL) {
        posting = yew_xcalloc(1U, sizeof(*posting));
        (void)strmap_put(&idx->by_name, name, len, posting);
        idx->bytes += sizeof(*posting) + len + 1U;
    }
    posting_push(posting, entry);
    idx->bytes += sizeof(entry);
}

static void symidx_map_rebuild(SymIndex *idx)
{
    size_t i;

    posting_free_all(&idx->by_name);
    strmap_free(&idx->by_name);
    strmap_init(&idx->by_name);
    idx->bytes = idx->e.len * (sizeof(SymEntry) + sizeof(u32));
    for (i = 0U; i < idx->e.len; i++)
        symidx_map_add(idx, (u32)i);
    idx->map_dirty = false;
}

static bool text_copy(const TextBuf *tb, Span span, u8 *out)
{
    TextIter it;
    u64 done = 0U;
    u64 need = span.hi - span.lo;

    if (need == 0U)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        return false;
    while (done < need) {
        const u8 *bytes;
        u64 avail;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &avail))
            return false;
        take = avail < need - done ? avail : need - done;
        (void)memcpy(out + (size_t)done, bytes, (size_t)take);
        done += take;
        if (done < need && !yew_textiter_advance(&it, tb))
            return false;
    }
    return true;
}

static bool text_byte(const TextBuf *tb, u64 at, u8 *out)
{
    TextIter it;
    const u8 *bytes;
    u64 n;

    return at < yew_textbuf_len(tb) &&
           yew_textiter_begin(&it, tb, BYTEOFF(at)) &&
           yew_textiter_chunk(&it, tb, &bytes, &n) && n != 0U &&
           ((*out = bytes[0]), true);
}

static bool identifier_shape(const u8 *bytes, u32 len)
{
    u32 at = 0U;
    bool first = true;

    if (len < YEW_SYM_MIN_LEN || len > YEW_SYM_MAX_LEN)
        return false;
    while (at < len) {
        u32 cp;
        size_t used = yew_utf8_decode(bytes + at, len - at, &cp);
        YewWb prop;

        if (used == 0U || yew_utf8_is_escape(cp))
            return false;
        prop = yew_wb_prop(cp);
        if (first) {
            if (cp != (u32)'_' && prop != YEW_WB_ALETTER)
                return false;
            first = false;
        } else if (prop != YEW_WB_ALETTER && prop != YEW_WB_NUMERIC &&
                   prop != YEW_WB_EXTENDNUMLET && prop != YEW_WB_EXTEND) {
            return false;
        }
        at += (u32)used;
    }
    return !first;
}

static bool attr_has_parent(u8 attr, u8 want)
{
    u8 guard;

    for (guard = 0U; guard < (u8)YEW_ATTR__COUNT; guard++) {
        u8 parent;

        if (attr == want)
            return true;
        parent = yew_syn_attr_parent(attr);
        if (parent == attr)
            break;
        attr = parent;
    }
    return false;
}

static bool syn_indexable(u8 attr)
{
    return !attr_has_parent(attr, YEW_ATTR_COMMENT) &&
           !attr_has_parent(attr, YEW_ATTR_STRING);
}

static u8 attr_at(const SynLineOut *out, u32 rel)
{
    u32 i;

    for (i = 0U; i < out->n; i++) {
        u32 lo = out->spans[i].start;
        u32 hi = lo + out->spans[i].len;

        if (rel >= lo && rel < hi)
            return out->spans[i].attr;
    }
    return YEW_ATTR_TEXT;
}

static bool bytes_equal(const u8 *bytes, u32 len, const char *word)
{
    size_t want = strlen(word);

    return want == len && memcmp(bytes, word, len) == 0;
}

static bool preceded_by_type(const TextBuf *tb, Span line, Span word)
{
    u64 at = word.lo;
    u64 hi;
    u64 lo;
    u8 bytes[8];

    while (at > line.lo) {
        u8 byte;

        if (!text_byte(tb, at - 1U, &byte) ||
            (byte != ' ' && byte != '\t'))
            break;
        at--;
    }
    hi = at;
    while (at > line.lo && hi - at < sizeof(bytes)) {
        u8 byte;

        if (!text_byte(tb, at - 1U, &byte) ||
            !((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') || byte == '_'))
            break;
        at--;
    }
    lo = at;
    if (hi == lo || hi - lo > sizeof(bytes) ||
        !text_copy(tb, (Span){lo, hi}, bytes))
        return false;
    return bytes_equal(bytes, (u32)(hi - lo), "struct") ||
           bytes_equal(bytes, (u32)(hi - lo), "enum") ||
           bytes_equal(bytes, (u32)(hi - lo), "union") ||
           bytes_equal(bytes, (u32)(hi - lo), "class") ||
           bytes_equal(bytes, (u32)(hi - lo), "type");
}

static bool followed_by_lparen(const TextBuf *tb, Span line, Span word)
{
    u64 at = word.hi;
    u8 byte;

    while (at < line.hi && text_byte(tb, at, &byte) &&
           (byte == ' ' || byte == '\t'))
        at++;
    return at < line.hi && text_byte(tb, at, &byte) && byte == '(';
}

static bool macro_name(const TextBuf *tb, Span line, Span word)
{
    static const char define[] = "#define";
    u64 at = line.lo;
    size_t i;
    u8 byte;

    while (at < line.hi && text_byte(tb, at, &byte) &&
           (byte == ' ' || byte == '\t'))
        at++;
    for (i = 0U; i < sizeof(define) - 1U; i++) {
        if (!text_byte(tb, at + i, &byte) || byte != (u8)define[i])
            return false;
    }
    at += sizeof(define) - 1U;
    if (!text_byte(tb, at, &byte) || (byte != ' ' && byte != '\t'))
        return false;
    while (at < word.lo && text_byte(tb, at, &byte) &&
           (byte == ' ' || byte == '\t'))
        at++;
    return at == word.lo;
}

static void infer_kind(const Buffer *buf, Span line, Span word, u8 attr,
                       u8 *kind, u8 *flags)
{
    *kind = attr_has_parent(attr, YEW_ATTR_KEYWORD) ? YEW_SYMK_KEYWORD
                                                    : YEW_SYMK_WORD;
    *flags = 0U;
    if (macro_name(buf->tb, line, word))
        *kind = YEW_SYMK_MACRO;
    else if (preceded_by_type(buf->tb, line, word))
        *kind = YEW_SYMK_TYPE;
    else if (followed_by_lparen(buf->tb, line, word))
        *kind = YEW_SYMK_FUNC;
    if (*kind == YEW_SYMK_FUNC || *kind == YEW_SYMK_TYPE ||
        *kind == YEW_SYMK_MACRO)
        *flags |= YEW_SYMF_DECL;
}

static bool same_source(const SymEntry *entry, u32 buf_id, u32 file)
{
    return entry->buf_id == buf_id && entry->file == file;
}

static bool symidx_add(SymIndex *idx, const u8 *name, u32 len, u32 buf_id,
                       u32 file, u64 off, u32 line, u8 kind, u8 flags)
{
    SymPosting *posting;
    size_t i;
    u32 name_id;
    u64 need = sizeof(SymEntry) + sizeof(u32) + len + 1U;

    if (idx->capped || idx->intern == NULL ||
        idx->bytes + need > YEW_SYMIDX_BYTES_MAX) {
        idx->capped = true;
        return false;
    }
    posting = strmap_get(&idx->by_name, (const char *)name, len);
    if (posting != NULL) {
        for (i = 0U; i < posting->len; i++) {
            SymEntry *entry = &idx->e.data[posting->data[i]];

            if (same_source(entry, buf_id, file)) {
                if (entry->hits != UINT16_MAX)
                    entry->hits++;
                if ((flags & YEW_SYMF_DECL) != 0U &&
                    (entry->flags & YEW_SYMF_DECL) == 0U) {
                    entry->kind = kind;
                    entry->flags = flags;
                }
                idx->updated.data[posting->data[i]] = idx->tick;
                return true;
            }
        }
    }
    name_id = yew_intern(idx->intern, (const char *)name, len);
    Vec_SymEntry_push(&idx->e,
                      (SymEntry){name_id, buf_id, file, off, line, 1U,
                                 kind, flags});
    Vec_SymTick_push(&idx->updated, idx->tick);
    symidx_map_add(idx, (u32)(idx->e.len - 1U));
    idx->bytes += sizeof(SymEntry) + sizeof(u32);
    return true;
}

u32 yew_symidx_scan(SymIndex *idx, Buffer *buf, Span range)
{
    UnitCtx unit;
    SynSpan *spans;
    u64 text_len;
    LineNo first;
    LineNo last;
    u64 line_no;
    u32 file = 0U;
    u32 count = 0U;

    if (idx == NULL || buf == NULL || buf->tb == NULL || idx->intern == NULL)
        return 0U;
    text_len = yew_textbuf_len(buf->tb);
    if (range.lo > text_len)
        range.lo = text_len;
    if (range.hi > text_len)
        range.hi = text_len;
    if (range.hi < range.lo)
        range.hi = range.lo;
    if (buf->path != NULL)
        file = yew_intern_cstr(idx->intern, buf->path);
    if (idx->tick == UINT32_MAX)
        idx->tick = 1U;
    else
        idx->tick++;
    if (range.lo == range.hi)
        return 0U;
    first = yew_textbuf_line_of(buf->tb, BYTEOFF(range.lo));
    last = yew_textbuf_line_of(buf->tb, BYTEOFF(range.hi - 1U));
    spans = yew_xcalloc(YEW_SYN_MAX_SPANS, sizeof(*spans));
    unit.tb = buf->tb;
    unit.buf = buf;
    unit.win = NULL;
    for (line_no = first.v; line_no <= last.v; line_no++) {
        Span line = yew_textbuf_line_span(buf->tb, LINENO(line_no));
        SynLineOut out = {spans, 0U, YEW_SYN_MAX_SPANS, 0U,
                          YEW_SYN_STOP_OK};
        u64 at = line.lo;

        yew_syn_spans(&buf->syn, buf->tb, LINENO(line_no), &out);
        while (at < line.hi) {
            Span word = yew_unit_word.span(&unit, BYTEOFF(at), false);
            u64 len;
            u8 token[YEW_SYM_MAX_LEN];
            u32 rel;
            u8 attr;
            u8 kind;
            u8 flags;

            if (word.hi <= at) {
                at++;
                continue;
            }
            at = word.hi;
            if (word.lo < line.lo || word.hi > line.hi ||
                word.hi <= range.lo || word.lo >= range.hi)
                continue;
            len = word.hi - word.lo;
            if (len < YEW_SYM_MIN_LEN || len > YEW_SYM_MAX_LEN ||
                !text_copy(buf->tb, word, token) ||
                !identifier_shape(token, (u32)len))
                continue;
            rel = (u32)(word.lo - line.lo);
            attr = attr_at(&out, rel);
            if (!syn_indexable(attr))
                continue;
            infer_kind(buf, line, word, attr, &kind, &flags);
            if (!symidx_add(idx, token, (u32)len, buf->id, file, word.lo,
                            (u32)line_no, kind, flags))
                break;
            count++;
        }
        if (idx->capped || line_no == UINT64_MAX)
            break;
    }
    free(spans);
    return count;
}

static SymBufIndex *symbuf_find(Workspace *ws, u32 buf_id)
{
    size_t i;

    for (i = 0U; i < ws->sym_buf.len; i++) {
        if (ws->sym_buf.data[i].buf_id == buf_id)
            return &ws->sym_buf.data[i];
    }
    return NULL;
}

static const SymBufIndex *symbuf_find_const(const Workspace *ws, u32 buf_id)
{
    size_t i;

    for (i = 0U; i < ws->sym_buf.len; i++) {
        if (ws->sym_buf.data[i].buf_id == buf_id)
            return &ws->sym_buf.data[i];
    }
    return NULL;
}

SymIndex *yew_symidx_buffer(Workspace *ws, u32 buf_id, bool create)
{
    SymBufIndex *found;
    SymBufIndex item;

    if (ws == NULL || buf_id == 0U)
        return NULL;
    found = symbuf_find(ws, buf_id);
    if (found != NULL)
        return &found->idx;
    if (!create || ws->owner == NULL)
        return NULL;
    (void)memset(&item, 0, sizeof(item));
    item.buf_id = buf_id;
    yew_symidx_init(&item.idx, &ws->owner->interner);
    Vec_SymBufIndex_push(&ws->sym_buf, item);
    return &ws->sym_buf.data[ws->sym_buf.len - 1U].idx;
}

void yew_symidx_drop_buffer(Workspace *ws, u32 buf_id)
{
    size_t i;

    if (ws == NULL)
        return;
    for (i = 0U; i < ws->sym_buf.len; i++) {
        if (ws->sym_buf.data[i].buf_id != buf_id)
            continue;
        yew_symidx_free(&ws->sym_buf.data[i].idx);
        if (i + 1U < ws->sym_buf.len)
            (void)memmove(&ws->sym_buf.data[i], &ws->sym_buf.data[i + 1U],
                          (ws->sym_buf.len - i - 1U) *
                              sizeof(*ws->sym_buf.data));
        ws->sym_buf.len--;
        return;
    }
}

void yew_symidx_workspace_free(Workspace *ws)
{
    size_t i;

    if (ws == NULL)
        return;
    for (i = 0U; i < ws->sym_buf.len; i++)
        yew_symidx_free(&ws->sym_buf.data[i].idx);
    Vec_SymBufIndex_free(&ws->sym_buf);
    yew_symidx_free(&ws->sym_ws);
    Vec_SymHit_free(&ws->sym_query);
    free(ws->sym_seen);
    free(ws->sym_slot);
    ws->sym_seen = NULL;
    ws->sym_slot = NULL;
    ws->sym_seen_cap = 0U;
}

static void dirty_union(SymDirty *dirty, LineNo lo, LineNo hi)
{
    if (!dirty->pending) {
        dirty->post_lo = lo;
        dirty->post_hi = hi;
        dirty->pending = true;
        dirty->prepared = false;
        return;
    }
    if (lo.v < dirty->post_lo.v)
        dirty->post_lo = lo;
    if (hi.v > dirty->post_hi.v)
        dirty->post_hi = hi;
}

void yew_symidx_note_pre(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    SymBufIndex *sb;
    u64 end;

    if (ec == NULL || ec->ed == NULL || ec->buffer == NULL)
        return;
    (void)yew_symidx_buffer(&ec->ed->ws, ec->buffer->id, true);
    sb = symbuf_find(&ec->ed->ws, ec->buffer->id);
    if (sb == NULL)
        return;
    end = kind == YEW_JOURNAL_DEL && len <= yew_textbuf_len(ec->tb) - at.v
              ? at.v + len
              : at.v;
    sb->dirty.pre_lo = yew_textbuf_line_of(ec->tb, at);
    sb->dirty.pre_hi = yew_textbuf_line_of(ec->tb, BYTEOFF(end));
    if (sb->dirty.pre_lo.v != 0U)
        sb->dirty.pre_lo.v--;
    if (sb->dirty.pre_hi.v + 1U < yew_textbuf_line_count(ec->tb))
        sb->dirty.pre_hi.v++;
    sb->dirty.old_lines = yew_textbuf_line_count(ec->tb);
    sb->dirty.have_pre = true;
}

static void symidx_apply_edit(SymBufIndex *sb, u64 new_lines)
{
    SymIndex *idx = &sb->idx;
    SymDirty *dirty = &sb->dirty;
    i64 delta = new_lines >= dirty->old_lines
                    ? (i64)(new_lines - dirty->old_lines)
                    : -(i64)(dirty->old_lines - new_lines);
    size_t read;
    size_t write = 0U;

    for (read = 0U; read < idx->e.len; read++) {
        SymEntry entry = idx->e.data[read];

        if ((u64)entry.line >= dirty->pre_lo.v &&
            (u64)entry.line <= dirty->pre_hi.v)
            continue;
        if ((u64)entry.line > dirty->pre_hi.v) {
            i64 shifted = (i64)entry.line + delta;

            entry.line = shifted < 0 ? 0U : (u32)shifted;
        }
        idx->e.data[write] = entry;
        idx->updated.data[write] = idx->updated.data[read];
        write++;
    }
    if (write != idx->e.len) {
        idx->e.len = write;
        idx->updated.len = write;
        idx->map_dirty = true;
    }
}

void yew_symidx_note_post(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    SymBufIndex *sb;
    u64 lines;
    u64 end;
    LineNo lo;
    LineNo hi;
    i64 delta;

    (void)kind;
    if (ec == NULL || ec->ed == NULL || ec->buffer == NULL)
        return;
    sb = symbuf_find(&ec->ed->ws, ec->buffer->id);
    if (sb == NULL || !sb->dirty.have_pre)
        return;
    lines = yew_textbuf_line_count(ec->tb);
    delta = lines >= sb->dirty.old_lines
                ? (i64)(lines - sb->dirty.old_lines)
                : -(i64)(sb->dirty.old_lines - lines);
    if (sb->dirty.pending) {
        if (sb->dirty.post_lo.v > sb->dirty.pre_hi.v) {
            sb->dirty.post_lo.v = (u64)((i64)sb->dirty.post_lo.v + delta);
            sb->dirty.post_hi.v = (u64)((i64)sb->dirty.post_hi.v + delta);
        } else if (sb->dirty.post_hi.v >= sb->dirty.pre_lo.v) {
            if (sb->dirty.pre_lo.v < sb->dirty.post_lo.v)
                sb->dirty.post_lo = sb->dirty.pre_lo;
            if (sb->dirty.post_hi.v >= sb->dirty.pre_lo.v && delta > 0)
                sb->dirty.post_hi.v += (u64)delta;
        }
    }
    symidx_apply_edit(sb, lines);
    end = at.v;
    if (kind == YEW_JOURNAL_INS && len <= yew_textbuf_len(ec->tb) - at.v)
        end += len;
    lo = yew_textbuf_line_of(ec->tb, at);
    hi = yew_textbuf_line_of(ec->tb, BYTEOFF(end));
    if (lo.v != 0U)
        lo.v--;
    if (hi.v + 1U < lines)
        hi.v++;
    dirty_union(&sb->dirty, lo, hi);
    sb->dirty.prepared = false;
    sb->dirty.have_pre = false;
}

static void symidx_remove_lines(SymIndex *idx, LineNo lo, LineNo hi)
{
    size_t read;
    size_t write = 0U;

    for (read = 0U; read < idx->e.len; read++) {
        if ((u64)idx->e.data[read].line >= lo.v &&
            (u64)idx->e.data[read].line <= hi.v)
            continue;
        idx->e.data[write] = idx->e.data[read];
        idx->updated.data[write] = idx->updated.data[read];
        write++;
    }
    if (write != idx->e.len) {
        idx->e.len = write;
        idx->updated.len = write;
        idx->map_dirty = true;
    }
}

static void symidx_seed_missing(Ed *ed)
{
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++) {
        Buffer *buf = ed->ws.bufs[i];
        SymBufIndex *sb;

        if (buf == NULL || buf->tb == NULL ||
            symbuf_find(&ed->ws, buf->id) != NULL)
            continue;
        (void)yew_symidx_buffer(&ed->ws, buf->id, true);
        sb = symbuf_find(&ed->ws, buf->id);
        if (sb != NULL) {
            sb->dirty.post_lo = LINENO(0U);
            sb->dirty.post_hi = LINENO(yew_textbuf_line_count(buf->tb) - 1U);
            sb->dirty.pending = true;
        }
    }
}

bool yew_symidx_pending(const Ed *ed)
{
    u32 b;
    size_t i;

    if (ed == NULL || !ed->model_ready)
        return false;
    for (b = 0U; b < ed->ws.nbufs; b++) {
        Buffer *buf = ed->ws.bufs[b];

        if (buf != NULL && buf->tb != NULL &&
            symbuf_find_const(&ed->ws, buf->id) == NULL)
            return true;
    }
    for (i = 0U; i < ed->ws.sym_buf.len; i++) {
        if (ed->ws.sym_buf.data[i].dirty.pending)
            return true;
    }
    return false;
}

void yew_symidx_pump(Ed *ed, i64 budget_us)
{
    i64 start;
    size_t rounds;

    if (ed == NULL || !ed->model_ready || budget_us <= 0)
        return;
    symidx_seed_missing(ed);
    start = sym_now_us();
    rounds = ed->ws.sym_buf.len;
    while (rounds-- != 0U && ed->ws.sym_buf.len != 0U) {
        size_t slot = ed->ws.sym_rr++ % ed->ws.sym_buf.len;
        SymBufIndex *sb = &ed->ws.sym_buf.data[slot];
        Buffer *buf = yew_ws_buf_by_id(ed, sb->buf_id);
        SymDirty *dirty = &sb->dirty;

        if (buf == NULL || buf->tb == NULL || !dirty->pending)
            continue;
        if (!dirty->prepared) {
            symidx_remove_lines(&sb->idx, dirty->post_lo, dirty->post_hi);
            if (sb->idx.map_dirty)
                symidx_map_rebuild(&sb->idx);
            dirty->prepared = true;
        }
        while (dirty->post_lo.v <= dirty->post_hi.v) {
            Span line = yew_textbuf_line_span(buf->tb, dirty->post_lo);

            (void)yew_symidx_scan(&sb->idx, buf, line);
            dirty->post_lo.v++;
            if (sym_now_us() - start >= budget_us)
                return;
        }
        (void)memset(dirty, 0, sizeof(*dirty));
        if (sym_now_us() - start >= budget_us)
            return;
    }
}

static bool same_dir(const char *left, const char *right)
{
    const char *ls;
    const char *rs;
    size_t ln;
    size_t rn;

    if (left == NULL || right == NULL)
        return false;
    ls = strrchr(left, '/');
    rs = strrchr(right, '/');
    ln = ls == NULL ? 0U : (size_t)(ls - left);
    rn = rs == NULL ? 0U : (size_t)(rs - right);
    return ln == rn && (ln == 0U || memcmp(left, right, ln) == 0);
}

static SymProx sym_prox(const Workspace *ws, const SymEntry *entry,
                        const SymQuery *q, u32 cursor_line)
{
    const Buffer *current;
    const char *entry_path;

    if (entry->buf_id == q->buf_id) {
        u32 delta = entry->line > cursor_line ? entry->line - cursor_line
                                              : cursor_line - entry->line;

        return delta <= 200U ? YEW_PROX_CURSOR : YEW_PROX_BUFFER;
    }
    current = ws->owner == NULL ? NULL : yew_ws_buf_by_id(ws->owner,
                                                           q->buf_id);
    entry_path = ws->owner == NULL ? NULL :
        yew_intern_str(&ws->owner->interner, entry->file);
    if (current != NULL && same_dir(current->path, entry_path))
        return YEW_PROX_DIR;
    return YEW_PROX_WS;
}

static int symhit_cmp(const void *left, const void *right, void *ctx)
{
    const SymHit *a = left;
    const SymHit *b = right;
    const Interner *intern = ctx;
    const char *an;
    const char *bn;
    size_t al;
    size_t bl;
    int bytes;

    if (a->rank != b->rank)
        return a->rank > b->rank ? -1 : 1;
    an = yew_intern_str(intern, a->name);
    bn = yew_intern_str(intern, b->name);
    al = yew_intern_len(intern, a->name);
    bl = yew_intern_len(intern, b->name);
    if (al != bl)
        return al < bl ? -1 : 1;
    bytes = memcmp(an, bn, al);
    if (bytes != 0)
        return bytes < 0 ? -1 : 1;
    if (a->file != b->file)
        return a->file < b->file ? -1 : 1;
    if (a->off != b->off)
        return a->off < b->off ? -1 : 1;
    return 0;
}

static void sym_seen_prepare(Workspace *ws)
{
    size_t need = yew_intern_count(&ws->owner->interner) + 1U;
    size_t old = ws->sym_seen_cap;

    if (old < need) {
        ws->sym_seen = yew_xreallocarray(ws->sym_seen, need,
                                         sizeof(*ws->sym_seen));
        ws->sym_slot = yew_xreallocarray(ws->sym_slot, need,
                                         sizeof(*ws->sym_slot));
        (void)memset(ws->sym_seen + old, 0,
                     (need - old) * sizeof(*ws->sym_seen));
        ws->sym_seen_cap = need;
    }
    if (++ws->sym_seen_tick == 0U) {
        (void)memset(ws->sym_seen, 0,
                     ws->sym_seen_cap * sizeof(*ws->sym_seen));
        ws->sym_seen_tick = 1U;
    }
}

static void query_index(Workspace *ws, SymIndex *idx, const SymQuery *q,
                        u32 cursor_line, bool buffer_tier)
{
    size_t i;

    for (i = 0U; i < idx->e.len; i++) {
        const SymEntry *entry = &idx->e.data[i];
        const char *name = yew_intern_str(idx->intern, entry->name);
        size_t name_len = yew_intern_len(idx->intern, entry->name);
        FzMatch match;
        i32 fuzzy;
        SymProx prox;
        SymHit hit;
        u32 seen_slot;
        bool seen;

        if (entry->name >= ws->sym_seen_cap || name == NULL ||
            (!q->keywords && entry->kind == YEW_SYMK_KEYWORD))
            continue;
        seen = ws->sym_seen[entry->name] == ws->sym_seen_tick;
        if (!buffer_tier && seen)
            continue;
        fuzzy = yew_fz_score(q->stem, q->slen, name, (u32)name_len, &match);
        if (fuzzy == YEW_FZ_NO_MATCH)
            continue;
        if (entry->buf_id == q->buf_id && entry->hits == 1U &&
            entry->line == cursor_line && q->slen == name_len &&
            memcmp(q->stem, name, name_len) == 0)
            continue;
        prox = sym_prox(ws, entry, q, cursor_line);
        hit = (SymHit){entry->name,
                       yew_sym_rank(fuzzy, idx->tick - idx->updated.data[i],
                                    prox, entry->kind, entry->hits),
                       entry->kind, (u8)prox, match, entry->file,
                       entry->line, entry->off};
        if (!seen) {
            Vec_SymHit_push(&ws->sym_query, hit);
            ws->sym_seen[entry->name] = ws->sym_seen_tick;
            ws->sym_slot[entry->name] = (u32)(ws->sym_query.len - 1U);
            continue;
        }
        seen_slot = ws->sym_slot[entry->name];
        if (seen_slot < ws->sym_query.len &&
            symhit_cmp(&hit, &ws->sym_query.data[seen_slot], idx->intern) < 0)
            ws->sym_query.data[seen_slot] = hit;
    }
}

u32 yew_symidx_query(Workspace *ws, const SymQuery *q, SymHit *out, u32 max)
{
    Buffer *current;
    u32 cursor_line = 0U;
    u32 limit;
    size_t i;

    if (ws == NULL || ws->owner == NULL || q == NULL || out == NULL ||
        q->stem == NULL || max == 0U)
        return 0U;
    current = yew_ws_buf_by_id(ws->owner, q->buf_id);
    if (current != NULL && current->tb != NULL)
        cursor_line = (u32)yew_textbuf_line_of(current->tb, q->pos).v;
    ws->sym_query.len = 0U;
    sym_seen_prepare(ws);
    for (i = 0U; i < ws->sym_buf.len; i++)
        query_index(ws, &ws->sym_buf.data[i].idx, q, cursor_line, true);
    query_index(ws, &ws->sym_ws, q, cursor_line, false);
    yew_sort_stable(ws->sym_query.data, ws->sym_query.len,
                    sizeof(*ws->sym_query.data), symhit_cmp,
                    &ws->owner->interner);
    limit = q->max == 0U || q->max > YEW_SYM_QUERY_MAX
                ? YEW_SYM_QUERY_MAX : q->max;
    if (limit > max)
        limit = max;
    if ((size_t)limit > ws->sym_query.len)
        limit = (u32)ws->sym_query.len;
    if (limit != 0U)
        (void)memcpy(out, ws->sym_query.data, limit * sizeof(*out));
    return limit;
}
