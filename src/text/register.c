#include "text/register.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "text/clipboard.h"
#include "unicode/coords.h"
#include "util/base.h"
#include "util/log.h"

static void regval_clear(RegVal *v)
{
    v->type = SAG_REG_CHARWISE;
    v->ragged = false;
    v->width = 0U;
    v->bytes.len = 0U;
    v->rows.len = 0U;
    v->t_wall = 0;
}

void sag_regval_init(RegVal *v)
{
    if (v == NULL)
        SAG_BUG("sag_regval_init: NULL value");
    bytebuf_init(&v->bytes);
    v->rows.data = NULL;
    v->rows.len = 0U;
    v->rows.cap = 0U;
    regval_clear(v);
}

void sag_regval_free(RegVal *v)
{
    if (v == NULL)
        return;
    bytebuf_free(&v->bytes);
    SagRegRowVec_free(&v->rows);
    regval_clear(v);
}

static void regval_copy_raw(RegVal *dst, const RegVal *src)
{
    size_t i;

    if (dst == src)
        return;
    dst->bytes.len = 0U;
    dst->rows.len = 0U;
    bytebuf_append(&dst->bytes, src->bytes.data, src->bytes.len);
    for (i = 0U; i < src->rows.len; i++)
        SagRegRowVec_push(&dst->rows, src->rows.data[i]);
    dst->type = src->type;
    dst->ragged = src->ragged;
    dst->width = src->width;
    dst->t_wall = src->t_wall;
}

void sag_regval_copy(RegVal *dst, const RegVal *src)
{
    if (dst == NULL || src == NULL)
        SAG_BUG("sag_regval_copy: NULL value");
    regval_copy_raw(dst, src);
}

static void append_text_range(Bytebuf *out, const TextBuf *tb, Span range)
{
    TextIter it;
    u64 done = 0U;
    u64 total;

    if (range.lo > range.hi || range.hi > sag_textbuf_len(tb))
        SAG_BUG("register: source span out of bounds");
    total = range.hi - range.lo;
    if (total == 0U)
        return;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        SAG_BUG("register: cannot iterate source span");
    while (done < total) {
        const u8 *bytes;
        u64 len;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &bytes, &len))
            SAG_BUG("register: truncated source iterator");
        take = len < total - done ? len : total - done;
        bytebuf_append(out, bytes, (size_t)take);
        done += take;
        if (done < total && !sag_textiter_advance(&it, tb))
            SAG_BUG("register: truncated source iterator advance");
    }
}

static bool ends_in_lf(const Bytebuf *bytes)
{
    return bytes->len != 0U && bytes->data[bytes->len - 1U] == '\n';
}

void sag_regval_from_span(RegVal *out, const TextBuf *tb, Span range,
                          RegType type, const FileMeta *meta)
{
    const u8 *eol;
    size_t eol_len;

    if (out == NULL || tb == NULL)
        SAG_BUG("sag_regval_from_span: NULL argument");
    if (type == SAG_REG_BLOCKWISE)
        SAG_BUG("blockwise capture requires per-row geometry");
    regval_clear(out);
    out->type = (u8)type;
    append_text_range(&out->bytes, tb, range);
    if (type == SAG_REG_LINEWISE && !ends_in_lf(&out->bytes)) {
        if (meta == NULL)
            SAG_BUG("linewise register capture requires file metadata");
        sag_filemeta_eol_bytes(meta, &eol, &eol_len);
        bytebuf_append(&out->bytes, eol, eol_len);
    }
    out->t_wall = (i64)time(NULL);
}

static void init_many(RegVal *values, size_t count)
{
    size_t i;
    for (i = 0U; i < count; i++)
        sag_regval_init(&values[i]);
}

static void free_many(RegVal *values, size_t count)
{
    size_t i;
    for (i = 0U; i < count; i++)
        sag_regval_free(&values[i]);
}

void sag_reg_init(Registers *r)
{
    if (r == NULL)
        SAG_BUG("sag_reg_init: NULL register file");
    (void)memset(r, 0, sizeof(*r));
    init_many(r->named, SAG_ARRAY_LEN(r->named));
    sag_regval_init(&r->unnamed);
    init_many(r->numbered, SAG_ARRAY_LEN(r->numbered));
    sag_regval_init(&r->small_del);
    sag_regval_init(&r->last_insert);
    sag_regval_init(&r->search);
    sag_regval_init(&r->cmdline);
    sag_regval_init(&r->file);
    sag_regval_init(&r->alt_file);
    sag_regval_init(&r->system);
    init_many(r->ring, SAG_ARRAY_LEN(r->ring));
    r->ring_depth = SAG_KILL_RING_DEPTH_DEFAULT;
    r->ring_bytes_max = SAG_KILL_RING_BYTES_DEFAULT;
    r->clip_read_max = UINT64_C(64) * 1024U * 1024U;
    r->clipboard_sync = SAG_CLIP_SYNC_YANK;
}

void sag_reg_free(Registers *r)
{
    if (r == NULL)
        return;
    free_many(r->named, SAG_ARRAY_LEN(r->named));
    sag_regval_free(&r->unnamed);
    free_many(r->numbered, SAG_ARRAY_LEN(r->numbered));
    sag_regval_free(&r->small_del);
    sag_regval_free(&r->last_insert);
    sag_regval_free(&r->search);
    sag_regval_free(&r->cmdline);
    sag_regval_free(&r->file);
    sag_regval_free(&r->alt_file);
    sag_regval_free(&r->system);
    free_many(r->ring, SAG_ARRAY_LEN(r->ring));
    SagRegPasteSpanVec_free(&r->paste_spans);
    (void)memset(r, 0, sizeof(*r));
}

void sag_reg_bind_context(Registers *r, const UndoTree *undo,
                          const FileMeta *meta)
{
    if (r == NULL)
        SAG_BUG("sag_reg_bind_context: NULL register file");
    r->bound_undo = undo;
    r->bound_meta = meta;
}

RegVal *sag_reg_get(Registers *r, u8 name)
{
    if (r == NULL)
        return NULL;
    if (name == 0U || name == '"')
        return &r->unnamed;
    if (name >= 'a' && name <= 'z')
        return &r->named[name - 'a'];
    if (name >= 'A' && name <= 'Z')
        return &r->named[name - 'A'];
    if (name >= '0' && name <= '9')
        return &r->numbered[name - '0'];
    switch (name) {
    case '-': return &r->small_del;
    case '.':
        regval_clear(&r->last_insert);
        if (r->bound_undo != NULL)
            (void)sag_undo_last_insert(r->bound_undo,
                                       &r->last_insert.bytes,
                                       &r->last_insert.t_wall);
        return &r->last_insert;
    case '/': return &r->search;
    case ':': return &r->cmdline;
    case '%':
        regval_clear(&r->file);
        if (r->bound_meta != NULL && r->bound_meta->realpath != NULL)
            bytebuf_append(&r->file.bytes, r->bound_meta->realpath,
                           strlen(r->bound_meta->realpath));
        return &r->file;
    case '#': return &r->alt_file;
    case '+':
    case '*': return &r->system;
    case '_': return NULL;
    default: return NULL;
    }
}

static void reg_set_raw(RegVal *dst, const RegVal *src)
{
    regval_copy_raw(dst, src);
    dst->t_wall = (i64)time(NULL);
}

void sag_reg_set(Registers *r, u8 name, const RegVal *v)
{
    RegVal *dst;

    if (r == NULL || v == NULL)
        SAG_BUG("sag_reg_set: NULL argument");
    if (name == '_' || name == 0U)
        return;
    /*
     * These registers are written by the subsystem that owns them, each
     * through its own door, so that "what can put a value here" stays a
     * short list.  Sprint 21 opened `/` via sag_reg_set_search; the rest
     * are still owned elsewhere.
     */
    if (name == '.' || name == '/' || name == ':' || name == '%' ||
        name == '#')
        SAG_BUG("register %c is written by its owning subsystem, not by "
                "sag_reg_set", (int)name);
    if (name >= 'A' && name <= 'Z') {
        sag_reg_append(r, name, v);
        return;
    }
    dst = sag_reg_get(r, name);
    if (dst == NULL)
        SAG_BUG("unknown register name 0x%02x", (unsigned)name);
    reg_set_raw(dst, v);
}

void sag_reg_set_cmdline(Registers *r, const u8 *bytes, size_t len)
{
    if (r == NULL || (bytes == NULL && len != 0U))
        SAG_BUG("sag_reg_set_cmdline: NULL argument");
    regval_clear(&r->cmdline);
    bytebuf_append(&r->cmdline.bytes, bytes, len);
    r->cmdline.type = SAG_REG_CHARWISE;
    r->cmdline.t_wall = (i64)time(NULL);
}

/* Sprint 21 §6: the last accepted search pattern.  `:s//` and `^R /`
 * read it from here rather than from the live search state, so it
 * survives the prompt that produced it. */
void sag_reg_set_search(Registers *r, const u8 *bytes, size_t len)
{
    if (r == NULL || (bytes == NULL && len != 0U))
        SAG_BUG("sag_reg_set_search: NULL argument");
    regval_clear(&r->search);
    bytebuf_append(&r->search.bytes, bytes, len);
    r->search.type = SAG_REG_CHARWISE;
    r->search.t_wall = (i64)time(NULL);
}

static size_t eol_len_at_end(const RegVal *v)
{
    if (!ends_in_lf(&v->bytes))
        return 0U;
    if (v->bytes.len >= 2U && v->bytes.data[v->bytes.len - 2U] == '\r')
        return 2U;
    return 1U;
}

static void append_eol(Bytebuf *dst, const RegVal *style)
{
    size_t len = eol_len_at_end(style);
    static const u8 lf = '\n';

    if (len == 0U) {
        bytebuf_append(dst, &lf, 1U);
        return;
    }
    bytebuf_append(dst, style->bytes.data + style->bytes.len - len, len);
}

static void append_block(RegVal *dst, const RegVal *src)
{
    Bytebuf rebuilt;
    SagRegRowVec rows = {NULL, 0U, 0U};
    u32 result_width = (u32)sag_ccol_max((CCol){dst->width},
                                         (CCol){src->width}).v;
    bool result_ragged = dst->ragged || src->ragged;
    const RegVal *parts[2] = {dst, src};
    size_t p;

    bytebuf_init(&rebuilt);
    for (p = 0U; p < SAG_ARRAY_LEN(parts); p++) {
        const RegVal *part = parts[p];
        size_t i;
        for (i = 0U; i < part->rows.len; i++) {
            Span row = part->rows.data[i];
            Span next;

            if (row.lo > row.hi || row.hi > part->bytes.len)
                SAG_BUG("register: invalid block row span");
            next.lo = rebuilt.len;
            bytebuf_append(&rebuilt, part->bytes.data + row.lo,
                           (size_t)(row.hi - row.lo));
            if (!part->ragged) {
                u64 padding = sag_ccol_shortfall((CCol){result_width},
                                                  (CCol){part->width});
                while (padding != 0U) {
                    bytebuf_push_u8(&rebuilt, (u8)' ');
                    padding--;
                }
            }
            next.hi = rebuilt.len;
            SagRegRowVec_push(&rows, next);
        }
    }
    bytebuf_free(&dst->bytes);
    SagRegRowVec_free(&dst->rows);
    dst->bytes = rebuilt;
    dst->rows = rows;
    dst->width = result_width;
    dst->ragged = result_ragged;
    dst->type = SAG_REG_BLOCKWISE;
    dst->t_wall = (i64)time(NULL);
}

void sag_reg_append(Registers *r, u8 name, const RegVal *v)
{
    RegVal *dst;
    u8 lower;

    if (r == NULL || v == NULL)
        SAG_BUG("sag_reg_append: NULL argument");
    if (name < 'A' || name > 'Z')
        SAG_BUG("register append requires A-Z");
    lower = (u8)(name - 'A' + 'a');
    dst = sag_reg_get(r, lower);
    if (dst->bytes.len == 0U && dst->rows.len == 0U) {
        reg_set_raw(dst, v);
        return;
    }
    if (dst->type == SAG_REG_BLOCKWISE || v->type == SAG_REG_BLOCKWISE) {
        if (dst->type == SAG_REG_BLOCKWISE &&
            v->type == SAG_REG_BLOCKWISE) {
            append_block(dst, v);
        } else {
            sag_log(SAG_LOG_WARN,
                    "register: refused blockwise/non-blockwise append");
        }
        return;
    }
    if (dst->type == SAG_REG_CHARWISE && v->type == SAG_REG_LINEWISE) {
        append_eol(&dst->bytes, v);
        bytebuf_append(&dst->bytes, v->bytes.data, v->bytes.len);
        dst->type = SAG_REG_LINEWISE;
    } else if (dst->type == SAG_REG_LINEWISE &&
               v->type == SAG_REG_CHARWISE) {
        u8 eol[2];
        size_t eol_len = eol_len_at_end(dst);
        if (eol_len == 0U)
            SAG_BUG("linewise register payload lacks trailing EOL");
        (void)memcpy(eol, dst->bytes.data + dst->bytes.len - eol_len,
                     eol_len);
        bytebuf_append(&dst->bytes, v->bytes.data, v->bytes.len);
        bytebuf_append(&dst->bytes, eol, eol_len);
    } else {
        bytebuf_append(&dst->bytes, v->bytes.data, v->bytes.len);
    }
    dst->t_wall = (i64)time(NULL);
}

static void ring_drop_oldest(Registers *r)
{
    u32 index;

    if (r->ring_len == 0U)
        return;
    index = (r->ring_head + SAG_KILL_RING_MAX - (r->ring_len - 1U)) %
            SAG_KILL_RING_MAX;
    r->ring_bytes -= r->ring[index].bytes.len;
    regval_clear(&r->ring[index]);
    r->ring_len--;
}

void sag_reg_ring_push(Registers *r, const RegVal *v)
{
    u32 depth;
    u32 index;

    if (r == NULL || v == NULL)
        SAG_BUG("sag_reg_ring_push: NULL argument");
    r->paste_live = false;
    depth = r->ring_depth > SAG_KILL_RING_MAX ? SAG_KILL_RING_MAX :
                                               r->ring_depth;
    if (depth == 0U)
        return;
    while (r->ring_len >= depth)
        ring_drop_oldest(r);
    index = r->ring_len == 0U ? r->ring_head :
            (r->ring_head + 1U) % SAG_KILL_RING_MAX;
    r->ring_head = index;
    reg_set_raw(&r->ring[index], v);
    r->ring_len++;
    r->ring_bytes += r->ring[index].bytes.len;
    if (r->ring[index].bytes.len > r->ring_bytes_max) {
        while (r->ring_len > 1U)
            ring_drop_oldest(r);
    } else {
        while (r->ring_len > 1U && r->ring_bytes > r->ring_bytes_max)
            ring_drop_oldest(r);
    }
}

static void set_unnamed(Registers *r, const RegVal *v)
{
    reg_set_raw(&r->unnamed, v);
    sag_reg_ring_push(r, v);
}

static void set_explicit(Registers *r, u8 name, const RegVal *v)
{
    if (name == 0U)
        return;
    if (name >= 'A' && name <= 'Z') {
        sag_reg_append(r, name, v);
        return;
    }
    if (name >= '0' && name <= '9')
        return;
    if (name == '-')
        return;
    sag_reg_set(r, name, v);
}

void sag_reg_yank(Registers *r, u8 explicit_name, const RegVal *v)
{
    bool system;

    if (r == NULL || v == NULL)
        SAG_BUG("sag_reg_yank: NULL argument");
    if (explicit_name == '_')
        return;
    system = explicit_name == '+' || explicit_name == '*';
    set_explicit(r, explicit_name, v);
    set_unnamed(r, v);
    reg_set_raw(&r->numbered[0], v);
    if (system || r->clipboard_sync == SAG_CLIP_SYNC_YANK ||
        r->clipboard_sync == SAG_CLIP_SYNC_ALL ||
        r->clipboard_sync == SAG_CLIP_SYNC_UNNAMED) {
        reg_set_raw(&r->system, v);
        (void)sag_clip_write(v, explicit_name == '*' ? '*' : '+');
    }
}

static bool contains_lf(const RegVal *v)
{
    return v->bytes.len != 0U &&
           memchr(v->bytes.data, '\n', v->bytes.len) != NULL;
}

void sag_reg_delete(Registers *r, u8 explicit_name, const RegVal *v)
{
    size_t i;
    bool system;

    if (r == NULL || v == NULL)
        SAG_BUG("sag_reg_delete: NULL argument");
    if (explicit_name == '_')
        return;
    system = explicit_name == '+' || explicit_name == '*';
    set_explicit(r, explicit_name, v);
    set_unnamed(r, v);
    if (v->type == SAG_REG_LINEWISE || contains_lf(v)) {
        for (i = 9U; i > 1U; i--)
            reg_set_raw(&r->numbered[i], &r->numbered[i - 1U]);
        reg_set_raw(&r->numbered[1], v);
    } else {
        reg_set_raw(&r->small_del, v);
    }
    if (system || r->clipboard_sync == SAG_CLIP_SYNC_ALL ||
        r->clipboard_sync == SAG_CLIP_SYNC_UNNAMED) {
        reg_set_raw(&r->system, v);
        (void)sag_clip_write(v, explicit_name == '*' ? '*' : '+');
    }
}

static u8 text_byte(const TextBuf *tb, u64 off)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (off >= sag_textbuf_len(tb) ||
        !sag_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !sag_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        SAG_BUG("register: cannot read buffer byte");
    return bytes[0];
}

static size_t line_eol_len(const TextBuf *tb, Span line)
{
    if (line.hi == line.lo || text_byte(tb, line.hi - 1U) != '\n')
        return 0U;
    if (line.hi - line.lo >= 2U && text_byte(tb, line.hi - 2U) == '\r')
        return 2U;
    return 1U;
}

static void remember_insert(Registers *r, ByteOff at, u64 len)
{
    Span span;
    if (len == 0U)
        return;
    span.lo = at.v;
    span.hi = at.v + len;
    SagRegPasteSpanVec_push(&r->paste_spans, span);
}

static void set_primary_cursor(EditCtx *ec, ByteOff pos)
{
    Cursor *cursor;
    if (ec->cset == NULL)
        return;
    cursor = &ec->cset->curs.data[ec->cset->primary];
    cursor->pos = pos;
    cursor->anchor = pos;
    cursor->goal_col = (GCol){0U};
}

static ByteOff first_nonblank(const TextBuf *tb, ByteOff start)
{
    u64 at = start.v;
    u64 len = sag_textbuf_len(tb);
    while (at < len) {
        u8 byte = text_byte(tb, at);
        if (byte != ' ' && byte != '\t')
            break;
        at++;
    }
    return BYTEOFF(at);
}

static bool paste_char(Registers *r, EditCtx *ec, const RegVal *v,
                       bool before)
{
    Cursor cursor = ec->cset->curs.data[ec->cset->primary];
    LineNo line = sag_textbuf_line_of(ec->tb, cursor.pos);
    Span span = sag_textbuf_line_span(ec->tb, line);
    size_t eol_len = line_eol_len(ec->tb, span);
    ByteOff content_end = BYTEOFF(span.hi - eol_len);
    ByteOff at;
    ByteOff end;

    if (v->bytes.len == 0U)
        return false;
    if (before)
        at = cursor.pos;
    else if (cursor.pos.v >= content_end.v)
        at = content_end;
    else
        at = sag_grapheme_next(ec->tb, cursor.pos);
    if (!sag_edit_insert(ec, at, v->bytes.data, v->bytes.len))
        return false;
    remember_insert(r, at, v->bytes.len);
    end = BYTEOFF(at.v + v->bytes.len);
    set_primary_cursor(ec, sag_grapheme_prev(ec->tb, end));
    return true;
}

static bool paste_line(Registers *r, EditCtx *ec, const RegVal *v,
                       bool before)
{
    Cursor cursor = ec->cset->curs.data[ec->cset->primary];
    LineNo line = sag_textbuf_line_of(ec->tb, cursor.pos);
    Span span = sag_textbuf_line_span(ec->tb, line);
    size_t dst_eol_len = line_eol_len(ec->tb, span);
    ByteOff at;

    if (!ends_in_lf(&v->bytes))
        SAG_BUG("linewise register payload lacks trailing EOL");
    if (before) {
        at = sag_textbuf_line_start(ec->tb, line);
        if (!sag_edit_insert(ec, at, v->bytes.data, v->bytes.len))
            return false;
        remember_insert(r, at, v->bytes.len);
    } else if (dst_eol_len != 0U) {
        at = BYTEOFF(span.hi);
        if (!sag_edit_insert(ec, at, v->bytes.data, v->bytes.len))
            return false;
        remember_insert(r, at, v->bytes.len);
    } else {
        const u8 *eol;
        size_t eol_len;
        size_t payload_eol = eol_len_at_end(v);

        if (ec->meta == NULL)
            SAG_BUG("linewise paste requires file metadata");
        at = BYTEOFF(sag_textbuf_len(ec->tb));
        sag_filemeta_eol_bytes(ec->meta, &eol, &eol_len);
        if (!sag_edit_insert(ec, at, eol, eol_len))
            return false;
        remember_insert(r, at, eol_len);
        if (v->bytes.len > payload_eol) {
            ByteOff payload_at = BYTEOFF(at.v + eol_len);
            if (!sag_edit_insert(ec, payload_at, v->bytes.data,
                                 v->bytes.len - payload_eol))
                return false;
            remember_insert(r, payload_at, v->bytes.len - payload_eol);
        }
        at = BYTEOFF(at.v + eol_len);
    }
    set_primary_cursor(ec, first_nonblank(ec->tb, at));
    return true;
}

static ByteOff block_target(const TextBuf *tb, Cursor cursor, bool before)
{
    LineNo line = sag_textbuf_line_of(tb, cursor.pos);
    Span span = sag_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi - line_eol_len(tb, span));
    if (before || cursor.pos.v >= end.v)
        return cursor.pos.v > end.v ? end : cursor.pos;
    return sag_grapheme_next(tb, cursor.pos);
}

static bool insert_spaces(Registers *r, EditCtx *ec, ByteOff at, u64 count)
{
    static const u8 spaces[64] = {
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
    };
    u64 done = 0U;
    while (done < count) {
        u64 take = count - done < sizeof(spaces) ? count - done :
                                                     sizeof(spaces);
        if (!sag_edit_insert(ec, BYTEOFF(at.v + done), spaces, take))
            return false;
        remember_insert(r, BYTEOFF(at.v + done), take);
        done += take;
    }
    return true;
}

static bool paste_block(Registers *r, EditCtx *ec, const RegVal *v,
                        bool before, u32 tabw)
{
    Cursor cursor = ec->cset->curs.data[ec->cset->primary];
    LineNo first_line = sag_textbuf_line_of(ec->tb, cursor.pos);
    ByteOff target = block_target(ec->tb, cursor, before);
    Span first_span = sag_textbuf_line_span(ec->tb, first_line);
    CCol column = sag_off_to_ccol(ec->tb, first_span, target, tabw);
    ByteOff first_insert = target;
    size_t i;

    if (v->rows.len == 0U)
        return false;
    for (i = 0U; i < v->rows.len; i++) {
        LineNo line = LINENO(first_line.v + i);
        Span dst;
        ByteOff off;
        CCol landed;
        u64 pad;
        Span row = v->rows.data[i];

        while (line.v >= sag_textbuf_line_count(ec->tb)) {
            const u8 *eol;
            size_t eol_len;
            ByteOff end = BYTEOFF(sag_textbuf_len(ec->tb));
            if (ec->meta == NULL)
                SAG_BUG("blockwise line extension requires file metadata");
            sag_filemeta_eol_bytes(ec->meta, &eol, &eol_len);
            if (!sag_edit_insert(ec, end, eol, eol_len))
                return false;
            remember_insert(r, end, eol_len);
        }
        if (row.lo > row.hi || row.hi > v->bytes.len)
            SAG_BUG("register: invalid block row span");
        dst = sag_textbuf_line_span(ec->tb, line);
        off = sag_ccol_to_off_padded(ec->tb, dst, column, tabw);
        landed = sag_off_to_ccol(ec->tb, dst, off, tabw);
        pad = sag_ccol_shortfall(column, landed);
        if (!insert_spaces(r, ec, off, pad))
            return false;
        off.v += pad;
        if (!sag_edit_insert(ec, off, v->bytes.data + row.lo,
                             row.hi - row.lo))
            return false;
        remember_insert(r, off, row.hi - row.lo);
        if (i == 0U)
            first_insert = off;
    }
    set_primary_cursor(ec, first_insert);
    return true;
}

static bool paste_value(Registers *r, EditCtx *ec, const RegVal *v,
                        bool before, u32 tabw)
{
    if (v->type == SAG_REG_CHARWISE)
        return paste_char(r, ec, v, before);
    if (v->type == SAG_REG_LINEWISE)
        return paste_line(r, ec, v, before);
    if (v->type == SAG_REG_BLOCKWISE)
        return paste_block(r, ec, v, before, tabw);
    SAG_BUG("register: invalid value type");
}

static u32 find_ring_value(const Registers *r, const RegVal *v)
{
    u32 i;
    for (i = 0U; i < r->ring_len; i++) {
        u32 index = (r->ring_head + SAG_KILL_RING_MAX - i) %
                    SAG_KILL_RING_MAX;
        if (&r->ring[index] == v)
            return index;
    }
    return r->ring_head;
}

bool sag_reg_paste(Registers *r, EditCtx *ec, u8 name, bool before,
                   u32 tabw)
{
    RegVal *v;
    Cursor origin;
    bool changed;

    if (r == NULL || ec == NULL || ec->tb == NULL || ec->undo == NULL ||
        ec->cset == NULL)
        SAG_BUG("sag_reg_paste: incomplete context");
    if (name == '+' || name == '*') {
        sag_clip_set_read_max(r->clip_read_max);
        if (!sag_clip_read(&r->system, name == '*' ? '*' : '+'))
            return false;
        name = '+';
    } else if ((name == 0U || name == '"') &&
               r->clipboard_sync == SAG_CLIP_SYNC_UNNAMED) {
        sag_clip_set_read_max(r->clip_read_max);
        if (!sag_clip_read(&r->system, '+'))
            return false;
        set_unnamed(r, &r->system);
        name = '"';
    }
    v = sag_reg_get(r, name);
    if (v == NULL)
        return false;
    origin = ec->cset->curs.data[ec->cset->primary];
    r->paste_spans.len = 0U;
    sag_undo_begin(ec, SAG_TXN_PASTE);
    changed = paste_value(r, ec, v, before, tabw);
    sag_undo_end(ec);
    if (!changed) {
        r->paste_live = false;
        return false;
    }
    r->paste_origin = origin;
    r->paste_owner = ec->tb;
    r->paste_win_id = ec->win_id;
    r->paste_ring_index = find_ring_value(r, v);
    r->paste_tabw = tabw;
    r->paste_before = before;
    r->paste_live = true;
    return true;
}

bool sag_reg_ring_cycle(Registers *r, EditCtx *ec, i32 delta)
{
    i64 rank;
    u32 current_rank;
    size_t i;
    bool changed;

    if (r == NULL || ec == NULL || !r->paste_live || r->ring_len == 0U ||
        ec->undo == NULL || ec->cset == NULL || r->paste_owner != ec->tb ||
        r->paste_win_id != ec->win_id)
        return false;
    if (!sag_undo_reopen(ec, SAG_TXN_PASTE)) {
        r->paste_live = false;
        sag_log(SAG_LOG_WARN, "register: ring cycle refused after edit");
        return false;
    }
    i = r->paste_spans.len;
    while (i != 0U)
        sag_edit_delete(ec, r->paste_spans.data[--i]);
    set_primary_cursor(ec, r->paste_origin.pos);
    current_rank = (r->ring_head + SAG_KILL_RING_MAX -
                    r->paste_ring_index) % SAG_KILL_RING_MAX;
    if (current_rank >= r->ring_len)
        current_rank = 0U;
    rank = (i64)current_rank + (i64)delta;
    while (rank < 0)
        rank += r->ring_len;
    rank %= r->ring_len;
    r->paste_ring_index =
        (r->ring_head + SAG_KILL_RING_MAX - (u32)rank) %
        SAG_KILL_RING_MAX;
    r->paste_spans.len = 0U;
    changed = paste_value(r, ec, &r->ring[r->paste_ring_index],
                          r->paste_before, r->paste_tabw);
    sag_undo_end(ec);
    if (!changed)
        r->paste_live = false;
    return changed;
}

u32 sag_reg_ring_list(const Registers *r, RegInfo *out, u32 max)
{
    u32 count;
    u32 i;

    if (r == NULL)
        return 0U;
    count = r->ring_len < max ? r->ring_len : max;
    if (out == NULL)
        return r->ring_len;
    for (i = 0U; i < count; i++) {
        u32 index = (r->ring_head + SAG_KILL_RING_MAX - i) %
                    SAG_KILL_RING_MAX;
        const RegVal *v = &r->ring[index];
        out[i].type = v->type;
        out[i].ragged = v->ragged;
        out[i].width = v->width;
        out[i].rows = (u32)v->rows.len;
        out[i].bytes = v->bytes.len;
        out[i].t_wall = v->t_wall;
    }
    return count;
}
