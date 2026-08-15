#include "mod/lsp/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/lsp/client.h"
#include "term/grid.h"
#include "ui/gutter.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/u16.h"
#include "util/sort.h"

enum {
    YEW_DIAG_STRING_MAX = 64U * 1024U,
    YEW_DIAG_PICK_MAX = 4096U
};

typedef struct DiagPickRef {
    u32 buf_id;
    u32 identity;
} DiagPickRef;

typedef struct DiagPickCtx {
    PickItem rows[YEW_DIAG_PICK_MAX];
    DiagPickRef refs[YEW_DIAG_PICK_MAX];
    char *text;
    size_t text_len;
    size_t text_cap;
    u32 n;
} DiagPickCtx;

typedef struct DiagCandidate {
    Buffer *b;
    Diagnostic *d;
} DiagCandidate;

static DiagPickCtx picker;

static const char *arena_copy_json(Arena *a, const JsonValue *v)
{
    const u8 *s;
    u32 n;

    s = yew_json_str(v, &n);
    if (s == NULL)
        return NULL;
    if (n > YEW_DIAG_STRING_MAX)
        n = YEW_DIAG_STRING_MAX;
    return arena_strndup(a, (const char *)s, n);
}

static char *arena_copy_code(Arena *a, const JsonValue *v)
{
    char buf[64];

    if (v == NULL || v->kind == YEW_JS_NULL)
        return NULL;
    if (v->kind == YEW_JS_STR)
        return (char *)arena_copy_json(a, v);
    if (v->kind != YEW_JS_INT)
        return NULL;
    (void)snprintf(buf, sizeof(buf), "%lld", (long long)v->i);
    return arena_strdup(a, buf);
}

static void diagnostic_drop_marks(Buffer *b, const Diagnostic *d)
{
    if (b->marks == NULL)
        return;
    if (yew_mark_alive(b->marks, d->lo))
        yew_mark_del(b->marks, d->lo);
    if (yew_mark_alive(b->marks, d->hi))
        yew_mark_del(b->marks, d->hi);
}

DiagStore *yew_diag_store_new(void)
{
    DiagStore *store = yew_xcalloc(1U, sizeof(*store));

    arena_init(&store->arena);
    store->version = -1;
    store->next_identity = 1U;
    return store;
}

void yew_diag_store_free(Buffer *b)
{
    u32 i;

    if (b == NULL || b->diag == NULL)
        return;
    for (i = 0U; i < b->diag->d.len; i++)
        diagnostic_drop_marks(b, &b->diag->d.data[i]);
    DiagnosticVec_free(&b->diag->d);
    arena_free_all(&b->diag->arena);
    free(b->diag);
    b->diag = NULL;
}

static u64 line_content_end(const TextBuf *tb, Span line)
{
    TextIter it;
    const u8 *bytes;
    u64 len;
    u64 end = line.hi;

    if (line.lo == line.hi)
        return line.hi;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(line.hi - 1U)) ||
        !yew_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        return line.hi;
    if (bytes[0] != (u8)'\n')
        return line.hi;
    end--;
    if (end == line.lo)
        return end;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(end - 1U)) ||
        !yew_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        return end;
    return bytes[0] == (u8)'\r' ? end - 1U : end;
}

static ByteOff diag_position(const Ed *ed, const Buffer *b, u32 server,
                             const JsonValue *position)
{
    i64 raw_line = yew_json_int(yew_json_get(position, "line"), 0);
    i64 raw_ch = yew_json_int(yew_json_get(position, "character"), 0);
    u64 lines = yew_textbuf_line_count(b->tb);
    u64 line = raw_line < 0 ? 0U : (u64)raw_line;
    u64 ch = raw_ch < 0 ? 0U : (u64)raw_ch;
    Span span;
    u64 end;
    u8 enc = YEW_POSENC_UTF16;

    if (lines == 0U)
        return BYTEOFF(0U);
    if (line >= lines)
        line = lines - 1U;
    span = yew_textbuf_line_span(b->tb, LINENO(line));
    end = line_content_end(b->tb, span);
    (void)yew_lsp_server_pos_enc(ed, server, &enc);
    if (enc == YEW_POSENC_UTF8)
        return BYTEOFF(ch > end - span.lo ? end : span.lo + ch);
    span.hi = end;
    return yew_u16col_to_off(b->tb, span, U16COL(ch));
}

static u8 diag_severity(const JsonValue *v)
{
    i64 sev = yew_json_int(v, YEW_DIAG_ERROR);

    return sev < YEW_DIAG_ERROR || sev > YEW_DIAG_HINT
               ? YEW_DIAG_ERROR
               : (u8)sev;
}

static u8 diag_tags(const JsonValue *v)
{
    u8 tags = 0U;
    u32 i;

    if (v == NULL || v->kind != YEW_JS_ARR)
        return 0U;
    for (i = 0U; i < v->arr.n; i++) {
        i64 tag = yew_json_int(v->arr.v[i], 0);

        if (tag == 1)
            tags |= YEW_DIAGT_UNNECESSARY;
        else if (tag == 2)
            tags |= YEW_DIAGT_DEPRECATED;
    }
    return tags;
}

static int diag_order(const void *left, const void *right, void *ctx)
{
    const Diagnostic *a = left;
    const Diagnostic *b = right;

    (void)ctx;
    if (a->line != b->line)
        return a->line < b->line ? -1 : 1;
    if (a->cache.lo != b->cache.lo)
        return a->cache.lo < b->cache.lo ? -1 : 1;
    return a->identity < b->identity ? -1 : a->identity != b->identity;
}

static void retain_other_servers(Buffer *b, DiagStore *store, u32 server,
                                 DiagnosticVec *kept, Arena *arena)
{
    u32 i;

    for (i = 0U; i < store->d.len; i++) {
        Diagnostic d = store->d.data[i];

        if (d.server == server) {
            diagnostic_drop_marks(b, &d);
            continue;
        }
        d.code = d.code == NULL ? NULL : arena_strdup(arena, d.code);
        d.source = d.source == NULL ? NULL : arena_strdup(arena, d.source);
        d.message = arena_strdup(arena, d.message);
        DiagnosticVec_push(kept, d);
    }
}

static void store_recount(DiagStore *store)
{
    u32 i;

    (void)memset(store->n, 0, sizeof(store->n));
    for (i = 0U; i < store->d.len; i++)
        store->n[store->d.data[i].sev]++;
}

void yew_diag_replace(Ed *ed, Buffer *b, u32 server, const JsonValue *arr,
                      i64 version)
{
    DiagnosticVec next = {0};
    Arena arena;
    DiagStore *store;
    u32 i;

    if (ed == NULL || b == NULL || b->tb == NULL || b->marks == NULL ||
        arr == NULL || arr->kind != YEW_JS_ARR)
        return;
    if (b->diag == NULL)
        b->diag = yew_diag_store_new();
    store = b->diag;
    if (version >= 0 && store->version >= 0 && version < store->version)
        return;
    arena_init(&arena);
    retain_other_servers(b, store, server, &next, &arena);
    for (i = 0U; i < arr->arr.n; i++) {
        const JsonValue *item = arr->arr.v[i];
        const JsonValue *range;
        const JsonValue *start;
        const JsonValue *end;
        const JsonValue *message;
        const u8 *msg;
        u32 msg_len;
        Diagnostic d;
        ByteOff lo;
        ByteOff hi;

        if (item == NULL || item->kind != YEW_JS_OBJ)
            continue;
        range = yew_json_get(item, "range");
        start = yew_json_get(range, "start");
        end = yew_json_get(range, "end");
        message = yew_json_get(item, "message");
        msg = yew_json_str(message, &msg_len);
        if (range == NULL || range->kind != YEW_JS_OBJ ||
            start == NULL || start->kind != YEW_JS_OBJ || end == NULL ||
            end->kind != YEW_JS_OBJ || msg == NULL)
            continue;
        if (msg_len > YEW_DIAG_STRING_MAX)
            msg_len = YEW_DIAG_STRING_MAX;
        lo = diag_position(ed, b, server, start);
        hi = diag_position(ed, b, server, end);
        if (hi.v < lo.v)
            hi = lo;
        (void)memset(&d, 0, sizeof(d));
        d.lo = yew_mark_add(b->marks, lo, YEW_BIAS_LEFT);
        d.hi = yew_mark_add(b->marks, hi, YEW_BIAS_RIGHT);
        d.cache = (Span){lo.v, hi.v};
        d.cache_gen = b->tb->gen;
        d.line = (u32)yew_textbuf_line_of(b->tb, lo).v;
        d.sev = diag_severity(yew_json_get(item, "severity"));
        d.tags = diag_tags(yew_json_get(item, "tags"));
        d.server = server;
        d.identity = store->next_identity++;
        if (store->next_identity == 0U)
            store->next_identity = 1U;
        d.code = arena_copy_code(&arena, yew_json_get(item, "code"));
        d.source = (char *)arena_copy_json(&arena,
                                           yew_json_get(item, "source"));
        d.message = arena_strndup(&arena, (const char *)msg, msg_len);
        DiagnosticVec_push(&next, d);
    }
    DiagnosticVec_free(&store->d);
    arena_free_all(&store->arena);
    store->d = next;
    store->arena = arena;
    if (store->d.len > 1U)
        yew_sort_stable(store->d.data, store->d.len,
                        sizeof(*store->d.data), diag_order, NULL);
    store_recount(store);
    store->stale = version < 0;
    if (version >= 0)
        store->version = version;
    store->gen++;
    yew_diag_refresh_view(ed, ed->win);
    ed->full_damage = true;
    ed->footer_dirty = true;
}

static Span diagnostic_span(Buffer *b, Diagnostic *d)
{
    u64 len;

    if (d->cache_gen == b->tb->gen)
        return d->cache;
    if (!yew_mark_alive(b->marks, d->lo) ||
        !yew_mark_alive(b->marks, d->hi))
        return (Span){0U, 0U};
    d->cache.lo = yew_mark_pos(b->marks, d->lo).v;
    d->cache.hi = yew_mark_pos(b->marks, d->hi).v;
    len = yew_textbuf_len(b->tb);
    if (d->cache.lo > len)
        d->cache.lo = len;
    if (d->cache.hi < d->cache.lo)
        d->cache.hi = d->cache.lo;
    if (d->cache.hi > len)
        d->cache.hi = len;
    d->line = (u32)yew_textbuf_line_of(b->tb, BYTEOFF(d->cache.lo)).v;
    d->cache_gen = b->tb->gen;
    return d->cache;
}

u32 yew_diag_at_line(const Buffer *buffer, LineNo line,
                     const Diagnostic **out, u32 max)
{
    Buffer *b = (Buffer *)buffer;
    u32 found = 0U;
    u32 i;

    if (b == NULL || b->diag == NULL)
        return 0U;
    for (i = 0U; i < b->diag->d.len; i++) {
        Diagnostic *d = &b->diag->d.data[i];

        (void)diagnostic_span(b, d);
        if (d->line != line.v)
            continue;
        if (out != NULL && found < max)
            out[found] = d;
        found++;
    }
    return found;
}

static Span visible_span(Buffer *b, Diagnostic *d)
{
    Span span = diagnostic_span(b, d);
    u64 len = yew_textbuf_len(b->tb);

    if (span.lo != span.hi)
        return span;
    if (span.lo < len)
        span.hi = yew_grapheme_next_boundary(b->tb, BYTEOFF(span.lo)).v;
    else if (span.lo != 0U)
        span.lo = yew_grapheme_prev_boundary(b->tb, BYTEOFF(span.lo)).v;
    return span;
}

Span yew_diag_span(Buffer *b, Diagnostic *d)
{
    if (b == NULL || d == NULL || b->tb == NULL)
        return (Span){0U, 0U};
    return visible_span(b, d);
}

const Diagnostic *yew_diag_at_point(const Buffer *buffer, ByteOff point)
{
    Buffer *b = (Buffer *)buffer;
    Diagnostic *best = NULL;
    u32 i;

    if (b == NULL || b->diag == NULL)
        return NULL;
    for (i = 0U; i < b->diag->d.len; i++) {
        Diagnostic *d = &b->diag->d.data[i];
        Span span = visible_span(b, d);

        if (point.v < span.lo || point.v >= span.hi)
            continue;
        if (best == NULL || d->sev < best->sev)
            best = d;
    }
    return best;
}

const char *yew_diag_glyph(u8 sev, size_t *len)
{
    static const char *const glyph[] = {
        "", "\xe2\x9c\x97", "\xe2\x96\xb2", "\xe2\x97\x8f", "\xc2\xb7"
    };
    const char *s = sev <= YEW_DIAG_HINT ? glyph[sev] : glyph[0];

    if (len != NULL)
        *len = strlen(s);
    return s;
}

const char *yew_diag_role(u8 sev)
{
    static const char *const roles[] = {
        "", "diag.error", "diag.warn", "diag.info", "diag.hint"
    };

    return sev <= YEW_DIAG_HINT ? roles[sev] : roles[0];
}

u16 yew_diag_attrs(u8 sev, u8 tags, bool truecolor)
{
    u16 attrs = 0U;

    if (sev == YEW_DIAG_ERROR || sev == YEW_DIAG_WARN)
        attrs = truecolor ? YEW_ATTR_UNDERCURL : YEW_ATTR_UNDERLINE;
    else if (sev == YEW_DIAG_INFO)
        attrs = YEW_ATTR_UNDERLINE;
    if (sev == YEW_DIAG_ERROR)
        attrs |= YEW_CELL_UL_ERROR;
    else if (sev == YEW_DIAG_WARN)
        attrs |= YEW_CELL_UL_WARN;
    else if (sev == YEW_DIAG_INFO)
        attrs |= YEW_CELL_UL_INFO;
    if ((tags & YEW_DIAGT_UNNECESSARY) != 0U)
        attrs |= YEW_ATTR_DIM;
    if ((tags & YEW_DIAGT_DEPRECATED) != 0U)
        attrs |= YEW_ATTR_STRIKE;
    return attrs;
}

MsgSev yew_diag_msg_sev(u8 sev)
{
    return sev == YEW_DIAG_ERROR ? YEW_MSG_ERROR :
           sev == YEW_DIAG_WARN ? YEW_MSG_WARN : YEW_MSG_INFO;
}

void yew_diag_refresh_view(Ed *ed, Win *w)
{
    DiagStore *store;
    u32 i;

    (void)ed;
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    yew_gutter_sign_clear_kind(w, LINENO(0U),
                               LINENO(yew_textbuf_line_count(w->buf->tb)),
                               YEW_SIGN_DIAG);
    store = w->buf->diag;
    if (store == NULL)
        return;
    for (i = 0U; i < store->d.len; i++) {
        Diagnostic *d = &store->d.data[i];
        size_t glyph_len;
        GutterSign sign;
        u32 j;
        bool beaten = false;

        (void)diagnostic_span(w->buf, d);
        for (j = 0U; j < i; j++) {
            Diagnostic *prior = &store->d.data[j];

            if (prior->line == d->line && prior->sev <= d->sev) {
                beaten = true;
                break;
            }
        }
        if (beaten)
            continue;
        sign.glyph = (const u8 *)yew_diag_glyph(d->sev, &glyph_len);
        sign.nbytes = (u8)glyph_len;
        sign.role = yew_diag_role(d->sev);
        sign.attrs = store->stale ? YEW_ATTR_DIM : 0U;
        yew_gutter_sign_set(w, LINENO(d->line), YEW_SIGN_DIAG, &sign);
    }
}

void yew_diag_cursor_hint(Ed *ed, Win *w)
{
    const Diagnostic *d;
    const Cursor *cursor;
    const Diagnostic *line_diags[32];
    u32 count;
    size_t glyph_len;
    const char *glyph;
    char suffix[32];

    if (ed == NULL || w == NULL || w->buf == NULL || w->cs.curs.len == 0U ||
        w->cs.primary >= w->cs.curs.len) {
        yew_msg_hint_clear(ed);
        return;
    }
    cursor = &w->cs.curs.data[w->cs.primary];
    d = yew_diag_at_point(w->buf, cursor->pos);
    if (d == NULL) {
        yew_msg_hint_clear(ed);
        return;
    }
    count = yew_diag_at_line(w->buf,
                             yew_textbuf_line_of(w->buf->tb, cursor->pos),
                             line_diags, YEW_ARRAY_LEN(line_diags));
    glyph = yew_diag_glyph(d->sev, &glyph_len);
    (void)glyph_len;
    if (count > 1U)
        (void)snprintf(suffix, sizeof(suffix), " (+%u more)",
                       (unsigned)(count - 1U));
    else
        suffix[0] = '\0';
    yew_msg_hint(ed, yew_diag_msg_sev(d->sev),
                 "%c %s%s%s%s%s%s%s%s%s%s",
                 d->sev == YEW_DIAG_ERROR ? 'E' :
                 d->sev == YEW_DIAG_WARN ? 'W' :
                 d->sev == YEW_DIAG_INFO ? 'I' : 'H', glyph,
                 d->source == NULL ? "" : " [",
                 d->source == NULL ? "" : d->source,
                 d->source == NULL ? "" : "]", " ", d->message,
                 d->code == NULL ? "" : " (",
                 d->code == NULL ? "" : d->code,
                 d->code == NULL ? "" : ")", suffix);
}

static void pick_text_reset(void)
{
    picker.n = 0U;
    picker.text_len = 0U;
}

static size_t pick_text(const char *s)
{
    size_t n = strlen(s) + 1U;
    size_t at = picker.text_len;

    if (at + n > picker.text_cap) {
        size_t cap = picker.text_cap == 0U ? 4096U : picker.text_cap;

        while (cap < at + n)
            cap *= 2U;
        picker.text = yew_xrealloc(picker.text, cap);
        picker.text_cap = cap;
    }
    (void)memcpy(picker.text + at, s, n);
    picker.text_len += n;
    return at;
}

static int candidate_order(const void *left, const void *right, void *ctx)
{
    const DiagCandidate *a = left;
    const DiagCandidate *b = right;
    int path;

    (void)ctx;
    if (a->d->sev != b->d->sev)
        return a->d->sev < b->d->sev ? -1 : 1;
    path = strcmp(yew_buf_label(a->b), yew_buf_label(b->b));
    if (path != 0)
        return path;
    if (a->d->line != b->d->line)
        return a->d->line < b->d->line ? -1 : 1;
    return a->d->identity < b->d->identity ? -1 :
           a->d->identity != b->d->identity;
}

u32 yew_diag_list(Ed *ed, PickItem *out, u32 max)
{
    DiagCandidate candidates[YEW_DIAG_PICK_MAX];
    size_t offsets[YEW_DIAG_PICK_MAX];
    u32 n = 0U;
    u32 bi;

    if (ed == NULL || out == NULL)
        return 0U;
    if (max > YEW_DIAG_PICK_MAX)
        max = YEW_DIAG_PICK_MAX;
    for (bi = 0U; bi < ed->ws.nbufs && n < max; bi++) {
        Buffer *b = ed->ws.bufs[bi];
        u32 i;

        if (b->diag == NULL)
            continue;
        for (i = 0U; i < b->diag->d.len && n < max; i++)
            candidates[n++] = (DiagCandidate){b, &b->diag->d.data[i]};
    }
    if (n > 1U)
        yew_sort_stable(candidates, n, sizeof(candidates[0]),
                        candidate_order, NULL);
    pick_text_reset();
    for (bi = 0U; bi < n; bi++) {
            Buffer *b = candidates[bi].b;
            Diagnostic *d = candidates[bi].d;
            Span sp = diagnostic_span(b, d);
            char row[1024];
            size_t glyph_len;
            const char *glyph = yew_diag_glyph(d->sev, &glyph_len);
            LineNo line = yew_textbuf_line_of(b->tb, BYTEOFF(sp.lo));
            Span line_span = yew_textbuf_line_span(b->tb, line);
            GCol col = yew_off_to_gcol(b->tb, line_span, BYTEOFF(sp.lo));

            (void)glyph_len;
            (void)snprintf(row, sizeof(row), "%s:%llu:%llu  %s %s",
                           yew_buf_label(b),
                           (unsigned long long)(line.v + 1U),
                           (unsigned long long)(col.v + 1U), glyph,
                           d->message);
            offsets[bi] = pick_text(row);
            out[bi].detail = NULL;
            out[bi].payload = (i32)bi;
            out[bi].flags = 0U;
            picker.refs[bi] = (DiagPickRef){b->id, d->identity};
    }
    for (bi = 0U; bi < n; bi++)
        out[bi].label = picker.text + offsets[bi];
    picker.n = n;
    return n;
}

static const PickItem *diag_pick_items(void *ctx, u32 *n)
{
    DiagPickCtx *p = ctx;

    *n = p->n;
    return p->rows;
}

static Diagnostic *find_identity(Buffer *b, u32 identity)
{
    u32 i;

    if (b == NULL || b->diag == NULL)
        return NULL;
    for (i = 0U; i < b->diag->d.len; i++) {
        if (b->diag->d.data[i].identity == identity)
            return &b->diag->d.data[i];
    }
    return NULL;
}

static bool diag_pick_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    DiagPickCtx *p = ctx;
    Buffer *b;
    Diagnostic *d;
    ByteOff from;

    (void)how;
    if (payload < 0 || (u32)payload >= p->n)
        return false;
    b = yew_ws_buf_by_id(ed, p->refs[payload].buf_id);
    d = find_identity(b, p->refs[payload].identity);
    if (b == NULL || d == NULL || !yew_ed_show_buffer(ed, b))
        return false;
    from = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    yew_jump_push(ed->win, from, ed->now_ms);
    ed->win->cs.curs.data[ed->win->cs.primary].pos =
        BYTEOFF(visible_span(b, d).lo);
    ed->win->cs.curs.data[ed->win->cs.primary].anchor =
        ed->win->cs.curs.data[ed->win->cs.primary].pos;
    ed->win->cs.curs.data[ed->win->cs.primary].goal_col = (GCol){0U};
    yew_win_follow_cursor(ed->win);
    ed->full_damage = true;
    return true;
}

void yew_diag_picker_open(Ed *ed)
{
    PickerSpec spec;

    if (ed == NULL)
        return;
    picker.n = yew_diag_list(ed, picker.rows, YEW_DIAG_PICK_MAX);
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Diagnostics";
    spec.items = diag_pick_items;
    spec.accept = diag_pick_accept;
    spec.ctx = &picker;
    yew_picker_open(ed, &spec);
}

bool yew_diag_jump(Ed *ed, Win *w, bool forward)
{
    DiagStore *store;
    ByteOff here;
    Diagnostic *target = NULL;
    u32 i;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->diag == NULL ||
        w->cs.curs.len == 0U || w->cs.primary >= w->cs.curs.len)
        return false;
    store = w->buf->diag;
    if (store->d.len == 0U)
        return false;
    here = w->cs.curs.data[w->cs.primary].pos;
    if (forward) {
        for (i = 0U; i < store->d.len; i++) {
            if (visible_span(w->buf, &store->d.data[i]).lo > here.v) {
                target = &store->d.data[i];
                break;
            }
        }
        if (target == NULL)
            target = &store->d.data[0];
    } else {
        for (i = store->d.len; i > 0U; i--) {
            if (visible_span(w->buf, &store->d.data[i - 1U]).lo < here.v) {
                target = &store->d.data[i - 1U];
                break;
            }
        }
        if (target == NULL)
            target = &store->d.data[store->d.len - 1U];
    }
    yew_jump_push(w, here, ed->now_ms);
    w->cs.curs.data[w->cs.primary].pos =
        BYTEOFF(visible_span(w->buf, target).lo);
    w->cs.curs.data[w->cs.primary].anchor =
        w->cs.curs.data[w->cs.primary].pos;
    w->cs.curs.data[w->cs.primary].goal_col = (GCol){0U};
    yew_win_follow_cursor(w);
    ed->full_damage = true;
    return true;
}
