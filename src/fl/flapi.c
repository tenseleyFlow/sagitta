#define _POSIX_C_SOURCE 200809L

#include "fl/flapi.h"

#include "edit/batch.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "fl/gc.h"
#include "fl/flruntime.h"
#include "fl/fltxn.h"
#include "fl/suggest.h"
#include "search/regex.h"
#include "text/mark.h"
#include "text/piece.h"
#include "unicode/coords.h"
#include "ui/layout.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"

/*
 * This is the only point in src/fl where an editor effect is invoked.
 * A descriptor is marshalled into CmdCtx, capability-checked, and then
 * handed to the editor's resolved dispatcher.  No text, cursor, undo or
 * register writer is reachable from this file.
 */

static Ed *api_ed(FlVm *vm)
{
    if (vm == NULL || vm->ed == NULL) {
        if (vm != NULL)
            (void)fl_raise(vm, "handle", "no editor is attached");
        return NULL;
    }
    return vm->ed;
}

static const char *api_call_name(const FlVm *vm)
{
    const char *name = vm == NULL || vm->in == NULL ? NULL :
                       sag_intern_str(vm->in, vm->cur_native);
    return name == NULL ? "editor API" : name;
}

static FlValue api_str(FlVm *vm, const char *s, u32 n)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s, n));
}

static FlValue api_cstr(FlVm *vm, const char *s)
{
    return s == NULL ? FL_NIL_V : api_str(vm, s, (u32)strlen(s));
}

static bool need_type(FlVm *vm, FlValue v, FlType want, u32 arg)
{
    if (v.t == (u8)want)
        return true;
    return fl_raise(vm, "type", "%s: argument %lu must be %s, found %s",
                    api_call_name(vm), (unsigned long)(arg + 1U),
                    fl_type_name(want), fl_type_name((FlType)v.t));
}

static bool need_bool(FlVm *vm, FlValue v, u32 arg, bool *out)
{
    if (!need_type(vm, v, FL_BOOL, arg))
        return false;
    *out = v.as.b;
    return true;
}

static bool need_int(FlVm *vm, FlValue v, u32 arg, i64 *out)
{
    if (!need_type(vm, v, FL_INT, arg))
        return false;
    *out = v.as.i;
    return true;
}

static bool need_str(FlVm *vm, FlValue v, u32 arg, const FlStr **out)
{
    if (!need_type(vm, v, FL_STR, arg))
        return false;
    *out = (const FlStr *)v.as.o;
    return true;
}

static bool text_range(FlVm *vm, const Buffer *b, Span range, FlValue *out)
{
    Bytebuf bytes;
    TextIter it;
    u64 at;

    if (b == NULL || b->tb == NULL || range.lo > range.hi ||
        range.hi > sag_buf_len(b))
        return fl_raise(vm, "range", "buffer span is outside the text");
    bytebuf_init(&bytes);
    at = range.lo;
    if (at < range.hi && sag_textiter_begin(&it, b->tb, BYTEOFF(at))) {
        do {
            const u8 *chunk;
            u64 n;
            u64 take;

            if (!sag_textiter_chunk(&it, b->tb, &chunk, &n))
                break;
            take = n < range.hi - at ? n : range.hi - at;
            bytebuf_append(&bytes, chunk, (size_t)take);
            at += take;
        } while (at < range.hi && sag_textiter_advance(&it, b->tb));
    }
    if (bytes.len > UINT32_MAX) {
        bytebuf_free(&bytes);
        return fl_raise(vm, "limit", "buffer text exceeds string limit");
    }
    *out = api_str(vm, (const char *)bytes.data, (u32)bytes.len);
    bytebuf_free(&bytes);
    return true;
}

static Win *win_for_buffer(Ed *ed, const Buffer *b)
{
    u32 i;

    if (ed->win != NULL && ed->win->buf == b)
        return ed->win;
    for (i = 0U; i < ed->tabs.v.len; i++) {
        Pane *leaves[SAG_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 k;

        sag_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                SAG_ARRAY_LEN(leaves), &n);
        for (k = 0U; k < n; k++)
            if (leaves[k]->win != NULL && leaves[k]->win->buf == b)
                return leaves[k]->win;
    }
    return NULL;
}

static bool q_buf_current(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Ed *ed = api_ed(vm);
    (void)a; (void)n;
    if (ed == NULL)
        return false;
    *out = fl_h_buf_make(ed, sag_ed_doc(ed));
    if (out->t == (u8)FL_NIL)
        return fl_raise(vm, "handle", "no current buffer");
    return true;
}

static bool q_buf_list(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Ed *ed = api_ed(vm);
    FlList *list;
    u32 i;
    (void)a; (void)n;
    if (ed == NULL)
        return false;
    list = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    for (i = 0U; i < ed->ws.nbufs; i++)
        (void)fl_list_push(vm, list, fl_h_buf_make(ed, ed->ws.bufs[i]));
    *out = FL_OBJ_V(FL_LIST, list);
    fl_gc_release(vm, 1U);
    return true;
}

static bool q_buf_path(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = api_cstr(vm, b->path);
    return true;
}

static bool q_buf_name(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = api_cstr(vm, sag_buf_label(b));
    return true;
}

static bool q_buf_lang(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = b->lang == NULL ? FL_NIL_V : api_cstr(vm, b->lang);
    return true;
}

static bool q_buf_len(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = FL_INT_V((i64)sag_buf_len(b));
    return true;
}

static bool q_buf_lines(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = FL_INT_V((i64)sag_buf_line_count(b));
    return true;
}

static bool q_buf_text(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    Buffer *sb = NULL;
    Span s;
    if (b == NULL)
        return false;
    if (n == 1U)
        s = (Span){0U, sag_buf_len(b)};
    else if (!fl_h_span(vm, a[1], &sb, &s))
        return false;
    else if (sb != b)
        return fl_raise(vm, "range", "span belongs to another buffer");
    return text_range(vm, b, s, out);
}

static bool q_buf_line(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    i64 line;
    Span s;
    (void)n;
    if (b == NULL || !need_int(vm, a[1], 1U, &line))
        return false;
    if (line < 1 || (u64)line > sag_buf_line_count(b))
        return fl_raise(vm, "range", "line %lld is outside the buffer",
                        (long long)line);
    s = sag_buf_line_span(b, LINENO((u64)line - 1U));
    if (s.hi > s.lo && s.hi <= sag_buf_len(b)) {
        FlValue last;
        if (!text_range(vm, b, (Span){s.hi - 1U, s.hi}, &last))
            return false;
        if (((FlStr *)last.as.o)->len == 1U &&
            ((FlStr *)last.as.o)->b[0] == '\n') {
            s.hi--;
            if (s.hi > s.lo &&
                text_range(vm, b, (Span){s.hi - 1U, s.hi}, &last) &&
                ((FlStr *)last.as.o)->b[0] == '\r')
                s.hi--;
        }
    }
    return text_range(vm, b, s, out);
}

static bool q_buf_span(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    i64 lo, hi;
    (void)n;
    if (b == NULL || !need_int(vm, a[1], 1U, &lo) ||
        !need_int(vm, a[2], 2U, &hi))
        return false;
    if (lo < 0 || hi < lo || (u64)hi > sag_buf_len(b))
        return fl_raise(vm, "range", "invalid byte span");
    *out = fl_h_span_make(vm->ed, b, (u64)lo, (u64)hi);
    return true;
}

static bool q_buf_line_span(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    i64 line;
    Span s;
    (void)n;
    if (b == NULL || !need_int(vm, a[1], 1U, &line))
        return false;
    if (line < 1 || (u64)line > sag_buf_line_count(b))
        return fl_raise(vm, "range", "line is outside the buffer");
    s = sag_buf_line_span(b, LINENO((u64)line - 1U));
    *out = fl_h_span_make(vm->ed, b, s.lo, s.hi);
    return true;
}

static bool q_buf_dirty(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = FL_BOOL_V(sag_buf_dirty(b));
    return true;
}

static bool q_buf_readonly(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    (void)n;
    if (b == NULL)
        return false;
    *out = FL_BOOL_V(sag_buf_readonly(b));
    return true;
}

static bool q_buf_mark(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    i64 at;
    (void)n;
    if (b == NULL || !need_int(vm, a[1], 1U, &at))
        return false;
    if (at < 0 || (u64)at > sag_buf_len(b))
        return fl_raise(vm, "range", "mark offset is outside the buffer");
    *out = fl_h_span_make(vm->ed, b, (u64)at, (u64)at);
    return true;
}

static bool q_buf_find(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    const SagRe *re;
    SagReInput input;
    SagReMatch match;
    i64 from = 0;

    if (b == NULL)
        return false;
    re = fl_h_re(vm, a[1]);
    if (re == NULL)
        return false;
    if (n == 3U && !need_int(vm, a[2], 2U, &from))
        return false;
    if (from < 0 || (u64)from > sag_buf_len(b))
        return fl_raise(vm, "range", "find offset is outside the buffer");
    input = sag_re_input_textbuf(b->tb);
    if (!sag_re_search(re, &input, BYTEOFF((u64)from), &match)) {
        *out = FL_NIL_V;
        return true;
    }
    *out = fl_h_span_make(vm->ed, b, match.g[0].lo, match.g[0].hi);
    return true;
}

static bool q_buf_find_all(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b = fl_h_buf(vm, a[0]);
    const SagRe *re;
    SagReInput input;
    SagReMatch match;
    FlList *list;
    u64 at = 0U;
    u64 len;
    (void)n;

    if (b == NULL)
        return false;
    re = fl_h_re(vm, a[1]);
    if (re == NULL)
        return false;
    len = sag_buf_len(b);
    input = sag_re_input_textbuf(b->tb);
    list = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    while (at <= len && sag_re_search(re, &input, BYTEOFF(at), &match)) {
        (void)fl_list_push(vm, list,
                           fl_h_span_make(vm->ed, b, match.g[0].lo,
                                          match.g[0].hi));
        if (match.g[0].hi > at)
            at = match.g[0].hi;
        else if (at < len)
            at++;
        else
            break;
    }
    *out = FL_OBJ_V(FL_LIST, list);
    fl_gc_release(vm, 1U);
    return true;
}

static bool q_win_current(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Ed *ed = api_ed(vm);
    (void)a; (void)n;
    if (ed == NULL || ed->win == NULL)
        return ed == NULL ? false : fl_raise(vm, "handle", "no current window");
    *out = fl_h_win_make(ed, ed->win);
    return true;
}

static bool q_win_list(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Ed *ed = api_ed(vm);
    FlList *list;
    u32 i;
    (void)a; (void)n;
    if (ed == NULL)
        return false;
    list = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    if (ed->tabs.v.len == 0U) {
        if (ed->win != NULL)
            (void)fl_list_push(vm, list, fl_h_win_make(ed, ed->win));
    } else for (i = 0U; i < ed->tabs.v.len; i++) {
        Pane *leaves[SAG_PANE_MAX_LEAVES];
        u32 count = 0U;
        u32 k;
        sag_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                SAG_ARRAY_LEN(leaves), &count);
        for (k = 0U; k < count; k++)
            (void)fl_list_push(vm, list,
                               fl_h_win_make(ed, leaves[k]->win));
    }
    *out = FL_OBJ_V(FL_LIST, list);
    fl_gc_release(vm, 1U);
    return true;
}

static bool q_win_buf(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w = fl_h_win(vm, a[0]);
    (void)n;
    if (w == NULL)
        return false;
    *out = fl_h_buf_make(vm->ed, w->buf);
    return true;
}

static bool q_win_cursors(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w = fl_h_win(vm, a[0]);
    FlList *list;
    u32 i;
    (void)n;
    if (w == NULL)
        return false;
    list = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    for (i = 0U; i < w->cs.curs.len; i++)
        (void)fl_list_push(vm, list, fl_h_cur_make(vm->ed, w, i));
    *out = FL_OBJ_V(FL_LIST, list);
    fl_gc_release(vm, 1U);
    return true;
}

static bool q_win_selection(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w = fl_h_win(vm, a[0]);
    Cursor *c;
    u64 lo, hi;
    (void)n;
    if (w == NULL)
        return false;
    c = &w->cs.curs.data[w->cs.primary];
    if (c->pos.v == c->anchor.v) {
        *out = FL_NIL_V;
        return true;
    }
    lo = c->pos.v < c->anchor.v ? c->pos.v : c->anchor.v;
    hi = c->pos.v < c->anchor.v ? c->anchor.v : c->pos.v;
    *out = fl_h_span_make(vm->ed, w->buf, lo, hi);
    return true;
}

static bool map_put(FlVm *vm, FlMap *m, const char *key, FlValue value)
{
    return fl_map_set(vm, m, api_cstr(vm, key), value);
}

static bool q_win_viewport(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w = fl_h_win(vm, a[0]);
    FlMap *m;
    (void)n;
    if (w == NULL)
        return false;
    m = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, m));
    (void)map_put(vm, m, "top", FL_INT_V((i64)w->vp.top.v + 1));
    (void)map_put(vm, m, "rows", FL_INT_V((i64)w->vp.rows));
    (void)map_put(vm, m, "cols", FL_INT_V((i64)w->vp.cols));
    *out = FL_OBJ_V(FL_MAP, m);
    fl_gc_release(vm, 1U);
    return true;
}

static bool q_cur_valid(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlValue saved;
    Cursor *cursor;
    Win *w;
    (void)n;
    if (api_ed(vm) == NULL)
        return false;
    saved = vm->err;
    cursor = fl_h_cur(vm, a[0], &w);
    if (cursor == NULL)
        vm->err = saved;
    *out = FL_BOOL_V(cursor != NULL);
    return true;
}

static bool cur_value(FlVm *vm, FlValue v, Cursor **out, Win **win)
{
    *out = fl_h_cur(vm, v, win);
    return *out != NULL;
}

static bool q_cur_pos(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    *out = FL_INT_V((i64)c->pos.v); return true;
}

static bool q_cur_anchor(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    *out = FL_INT_V((i64)c->anchor.v); return true;
}

static bool q_cur_line(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    *out = FL_INT_V((i64)sag_buf_line_of(w->buf, c->pos).v + 1);
    return true;
}

static bool q_cur_col(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w; Span line; GCol col;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    line = sag_buf_line_span(w->buf, sag_buf_line_of(w->buf, c->pos));
    col = sag_off_to_gcol(w->buf->tb, line, c->pos);
    *out = FL_INT_V((i64)col.v); return true;
}

static bool q_cur_span(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w; u64 lo, hi;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    if (c->pos.v == c->anchor.v) { *out = FL_NIL_V; return true; }
    lo = c->pos.v < c->anchor.v ? c->pos.v : c->anchor.v;
    hi = c->pos.v < c->anchor.v ? c->anchor.v : c->pos.v;
    *out = fl_h_span_make(vm->ed, w->buf, lo, hi); return true;
}

static bool q_cur_line_span(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w; Span s;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    s = sag_buf_line_span(w->buf, sag_buf_line_of(w->buf, c->pos));
    *out = fl_h_span_make(vm->ed, w->buf, s.lo, s.hi); return true;
}

static bool q_cur_word(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Cursor *c; Win *w; UnitCtx unit; Span s;
    (void)n;
    if (!cur_value(vm, a[0], &c, &w)) return false;
    unit.tb = w->buf->tb; unit.buf = w->buf; unit.win = w;
    s = sag_unit_word.span(&unit, c->pos, false);
    *out = fl_h_span_make(vm->ed, w->buf, s.lo, s.hi); return true;
}

static bool q_cur_primary(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlHandleSlot *s; Cursor *cursor; Win *w;
    (void)n;
    if (api_ed(vm) == NULL) return false;
    cursor = fl_h_cur(vm, a[0], &w);
    if (cursor == NULL) return false;
    s = fl_h_peek(&vm->ed->handles, a[0]);
    if (s == NULL || s->kind != (u8)FL_H_CUR ||
        s->as.cur.index >= w->cs.curs.len)
        return fl_raise(vm, "handle", "this cursor handle is closed");
    *out = FL_BOOL_V(s->as.cur.index == w->cs.primary); return true;
}

static bool q_span_valid(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlHandleSlot *s; Buffer *b;
    (void)n;
    if (api_ed(vm) == NULL) return false;
    s = fl_h_peek(&vm->ed->handles, a[0]);
    b = s == NULL || s->kind != (u8)FL_H_SPAN ? NULL :
        sag_ws_buf_by_id(vm->ed, s->as.span.buf);
    *out = FL_BOOL_V(b != NULL && b->marks != NULL &&
                     sag_mark_alive(b->marks, s->as.span.lo) &&
                     sag_mark_alive(b->marks, s->as.span.hi));
    return true;
}

static bool span_value(FlVm *vm, FlValue v, Buffer **b, Span *s)
{
    return fl_h_span(vm, v, b, s);
}

static bool q_span_lo(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b; Span s; (void)n;
    if (!span_value(vm, a[0], &b, &s)) return false;
    *out = FL_INT_V((i64)s.lo); return true;
}

static bool q_span_hi(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b; Span s; (void)n;
    if (!span_value(vm, a[0], &b, &s)) return false;
    *out = FL_INT_V((i64)s.hi); return true;
}

static bool q_span_len(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b; Span s; (void)n;
    if (!span_value(vm, a[0], &b, &s)) return false;
    *out = FL_INT_V((i64)(s.hi - s.lo)); return true;
}

static bool q_span_text(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b; Span s; (void)n;
    if (!span_value(vm, a[0], &b, &s)) return false;
    return text_range(vm, b, s, out);
}

static bool q_span_lines(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b; Span s; FlValue text; FlList *lines;
    (void)n;
    if (!span_value(vm, a[0], &b, &s) || !text_range(vm, b, s, &text))
        return false;
    fl_gc_protect(vm, text);
    lines = (FlList *)fl_split_lines(vm, ((FlStr *)text.as.o)->b,
                                     ((FlStr *)text.as.o)->len).as.o;
    *out = FL_OBJ_V(FL_LIST, lines);
    fl_gc_release(vm, 1U);
    return true;
}

#define QUERY(name_, recv_, min_, max_, fn_)                                 \
    { name_, NULL, {0U}, recv_, min_, max_, {0U, 0U, 0U}, 0U, fn_ }
#define COMMAND(name_, cmd_, recv_, min_, max_, caps_)                        \
    { name_, cmd_, {0U}, recv_, min_, max_, {0U, 0U, 0U}, caps_, NULL }
#define COMMAND_ARGS(name_, cmd_, recv_, min_, max_, caps_, a0_, a1_, a2_)    \
    { name_, cmd_, {0U}, recv_, min_, max_, {a0_, a1_, a2_}, caps_, NULL }

FlBindDesc fl_api[] = {
    QUERY("buf.current", FL_H_NONE, 0U, 0U, q_buf_current),
    QUERY("buf.list", FL_H_NONE, 0U, 0U, q_buf_list),
    COMMAND_ARGS("buf.open", "ed.buf.open", FL_H_NONE, 1U, 1U,
                 FL_CAP_FS_READ, FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("buf.close", "ed.buf.close", FL_H_BUF, 1U, 2U, 0U,
                 FL_ARG_BOOL, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("buf.save", "ed.file.write", FL_H_BUF, 1U, 2U,
                 FL_CAP_FS_WRITE, FL_ARG_VALUE, FL_ARG_NONE, FL_ARG_NONE),
    QUERY("buf.path", FL_H_BUF, 1U, 1U, q_buf_path),
    QUERY("buf.name", FL_H_BUF, 1U, 1U, q_buf_name),
    QUERY("buf.lang", FL_H_BUF, 1U, 1U, q_buf_lang),
    QUERY("buf.len", FL_H_BUF, 1U, 1U, q_buf_len),
    QUERY("buf.lines", FL_H_BUF, 1U, 1U, q_buf_lines),
    QUERY("buf.text", FL_H_BUF, 1U, 2U, q_buf_text),
    QUERY("buf.line", FL_H_BUF, 2U, 2U, q_buf_line),
    QUERY("buf.span", FL_H_BUF, 3U, 3U, q_buf_span),
    QUERY("buf.line_span", FL_H_BUF, 2U, 2U, q_buf_line_span),
    COMMAND_ARGS("buf.insert", "ed.edit.insert.at", FL_H_BUF, 3U, 3U, 0U,
                 FL_ARG_INT, FL_ARG_STR, FL_ARG_NONE),
    COMMAND_ARGS("buf.delete", "ed.edit.delete.span", FL_H_BUF, 2U, 2U,
                 0U, FL_ARG_HANDLE_SPAN, FL_ARG_NONE, FL_ARG_NONE),
    QUERY("buf.find", FL_H_BUF, 2U, 3U, q_buf_find),
    QUERY("buf.find_all", FL_H_BUF, 2U, 2U, q_buf_find_all),
    QUERY("buf.dirty", FL_H_BUF, 1U, 1U, q_buf_dirty),
    QUERY("buf.readonly", FL_H_BUF, 1U, 1U, q_buf_readonly),
    QUERY("buf.mark", FL_H_BUF, 2U, 2U, q_buf_mark),

    QUERY("win.current", FL_H_NONE, 0U, 0U, q_win_current),
    QUERY("win.list", FL_H_NONE, 0U, 0U, q_win_list),
    QUERY("win.buf", FL_H_WIN, 1U, 1U, q_win_buf),
    QUERY("win.cursors", FL_H_WIN, 1U, 1U, q_win_cursors),
    QUERY("win.selection", FL_H_WIN, 1U, 1U, q_win_selection),
    COMMAND_ARGS("win.split", "ed.win.split", FL_H_WIN, 2U, 2U, 0U,
                 FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND("win.close", "ed.pane.close", FL_H_WIN, 1U, 1U, 0U),
    COMMAND("win.focus", "ed.win.focus", FL_H_WIN, 1U, 1U, 0U),
    QUERY("win.viewport", FL_H_WIN, 1U, 1U, q_win_viewport),
    COMMAND_ARGS("win.set_cursors", "ed.cursor.set_many", FL_H_WIN,
                 2U, 2U, 0U, FL_ARG_LIST, FL_ARG_NONE, FL_ARG_NONE),

    QUERY("cur.pos", FL_H_CUR, 1U, 1U, q_cur_pos),
    QUERY("cur.line", FL_H_CUR, 1U, 1U, q_cur_line),
    QUERY("cur.col", FL_H_CUR, 1U, 1U, q_cur_col),
    QUERY("cur.anchor", FL_H_CUR, 1U, 1U, q_cur_anchor),
    QUERY("cur.span", FL_H_CUR, 1U, 1U, q_cur_span),
    COMMAND_ARGS("cur.goto", "ed.cursor.set", FL_H_CUR, 2U, 2U, 0U,
                 FL_ARG_INT, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("cur.move", "ed.cursor.move", FL_H_CUR, 3U, 4U, 0U,
                 FL_ARG_STR, FL_ARG_STR, FL_ARG_COUNT),
    QUERY("cur.word", FL_H_CUR, 1U, 1U, q_cur_word),
    QUERY("cur.line_span", FL_H_CUR, 1U, 1U, q_cur_line_span),
    COMMAND_ARGS("cur.insert", "ed.edit.insert.text", FL_H_CUR, 2U, 2U,
                 0U, FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    QUERY("cur.is_primary", FL_H_CUR, 1U, 1U, q_cur_primary),
    QUERY("cur.valid", FL_H_CUR, 1U, 1U, q_cur_valid),

    QUERY("span.lo", FL_H_SPAN, 1U, 1U, q_span_lo),
    QUERY("span.hi", FL_H_SPAN, 1U, 1U, q_span_hi),
    QUERY("span.len", FL_H_SPAN, 1U, 1U, q_span_len),
    QUERY("span.text", FL_H_SPAN, 1U, 1U, q_span_text),
    QUERY("span.lines", FL_H_SPAN, 1U, 1U, q_span_lines),
    COMMAND_ARGS("span.replace", "ed.edit.replace.span", FL_H_SPAN, 2U,
                 2U, 0U, FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("span.replace_lines", "ed.edit.replace.span", FL_H_SPAN,
                 2U, 2U, 0U, FL_ARG_LIST, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("span.prepend", "ed.edit.insert.at", FL_H_SPAN, 2U, 2U,
                 0U, FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("span.append", "ed.edit.insert.at", FL_H_SPAN, 2U, 2U,
                 0U, FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND("span.delete", "ed.edit.delete.span", FL_H_SPAN, 1U, 1U, 0U),
    COMMAND_ARGS("span.yank", "ed.edit.yank", FL_H_SPAN, 1U, 2U, 0U,
                 FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    QUERY("span.valid", FL_H_SPAN, 1U, 1U, q_span_valid),

    COMMAND_ARGS("opt.get", "ed.opt.get", FL_H_NONE, 1U, 1U, 0U,
                 FL_ARG_STR, FL_ARG_NONE, FL_ARG_NONE),
    COMMAND_ARGS("opt.set", "ed.opt.set", FL_H_NONE, 2U, 2U, 0U,
                 FL_ARG_STR, FL_ARG_VALUE, FL_ARG_NONE)
};

const u32 fl_api_len = (u32)SAG_ARRAY_LEN(fl_api);

void fl_api_init(void)
{
    static bool initialized;
    u32 i;

    if (initialized)
        return;
    for (i = 0U; i < fl_api_len; i++) {
        FlBindDesc *d = &fl_api[i];
        u32 base = d->recv == (u8)FL_H_NONE ? 0U : 1U;
        u32 mapped;

        if (d->nmin > d->nmax || d->nmax < base ||
            d->nmax - base > SAG_ARRAY_LEN(d->argmap))
            SAG_BUG("Fletch binding has invalid arity: %s", d->fl_name);
        if (d->cmd == NULL) {
            if (d->query == NULL)
                SAG_BUG("Fletch binding has neither command nor query: %s",
                        d->fl_name);
            continue;
        }
        if (d->query != NULL)
            SAG_BUG("Fletch binding has command and query: %s", d->fl_name);
        for (mapped = 0U; mapped < d->nmax - base; mapped++)
            if (d->argmap[mapped] == (u8)FL_ARG_NONE)
                SAG_BUG("Fletch binding lacks argument mapping: %s arg %u",
                        d->fl_name, (unsigned)(mapped + base));
        d->resolved_id = sag_cmd_lookup(d->cmd, (u32)strlen(d->cmd));
        if (d->resolved_id.v == 0U)
            SAG_BUG("Fletch binding names unknown command: %s -> %s",
                    d->fl_name, d->cmd);
    }
    initialized = true;
}

void fl_ed_attach(FlVm *vm, Ed *ed, const FlHost *host)
{
    if (vm == NULL)
        SAG_BUG("fl_ed_attach: NULL VM");
    vm->ed = ed;
    vm->host = host == NULL ? &fl_host_null : host;
}

void fl_ed_detach(FlVm *vm)
{
    if (vm == NULL)
        return;
    if (vm->txn.entry_active)
        (void)vm->host->run_end(vm, false);
    vm->ed = NULL;
    vm->host = &fl_host_null;
}

const FlBindDesc *fl_api_find(const char *name, u32 len)
{
    u32 i;
    if (name == NULL)
        return NULL;
    for (i = 0U; i < fl_api_len; i++)
        if (strlen(fl_api[i].fl_name) == (size_t)len &&
            memcmp(fl_api[i].fl_name, name, len) == 0)
            return &fl_api[i];
    return NULL;
}

const FlBindDesc *fl_api_find_receiver(FlValue recv,
                                       const char *member, u32 len)
{
    FlHandleKind kind = fl_h_kind_of(recv);
    const char *prefix;
    char qualified[64];
    size_t plen;
    if (kind == FL_H_BUF) prefix = "buf";
    else if (kind == FL_H_WIN) prefix = "win";
    else if (kind == FL_H_CUR) prefix = "cur";
    else if (kind == FL_H_SPAN) prefix = "span";
    else return NULL;
    plen = strlen(prefix);
    if (member == NULL || plen + 1U + len >= sizeof(qualified))
        return NULL;
    (void)memcpy(qualified, prefix, plen);
    qualified[plen] = '.';
    (void)memcpy(qualified + plen + 1U, member, len);
    return fl_api_find(qualified, (u32)(plen + 1U + len));
}

static bool api_native(FlVm *vm, FlValue *a, u32 n, FlValue *out);

bool fl_api_bind_receiver(FlVm *vm, FlValue recv,
                          const char *member, u32 len, FlValue *out)
{
    const FlBindDesc *d;
    FlNative *nat;

    if (vm == NULL || out == NULL)
        return false;
    d = fl_api_find_receiver(recv, member, len);
    if (d == NULL)
        return false;
    if (d->recv == (u8)FL_H_NONE || d->nmin == 0U || d->nmax == 0U)
        SAG_BUG("Fletch receiver binding lacks its receiver: %s", d->fl_name);
    nat = fl_gc_alloc(vm, sizeof(*nat), FL_NATIVE);
    nat->fn = api_native;
    nat->name_id = sag_intern(vm->in, d->fl_name, strlen(d->fl_name));
    nat->min_ar = (u8)(d->nmin - 1U);
    nat->max_ar = d->nmax == FL_VARIADIC ? FL_VARIADIC :
                  (u8)(d->nmax - 1U);
    nat->has_recv = 1U;
    nat->caps = d->caps;
    nat->recv = recv;
    *out = FL_OBJ_V(FL_NATIVE, nat);
    return true;
}

static bool command_error(FlVm *vm, const FlBindDesc *d, CmdStatus st)
{
    const CmdDesc *cmd = sag_cmd_desc(d->resolved_id);
    switch (st) {
    case SAG_CMD_ERR_ARG:
        return fl_raise(vm, "type", "%s rejected its arguments", d->fl_name);
    case SAG_CMD_ERR_STATE:
        return fl_raise(vm, "user", "%s is not valid in the current state",
                        d->fl_name);
    case SAG_CMD_ERR_IO:
        return fl_raise(vm, "io", "%s failed", d->fl_name);
    case SAG_CMD_ERR_DEFERRED:
        return fl_raise(vm, "name", "%s is deferred: %s", d->cmd,
                        cmd == NULL ? "unknown sprint" : cmd->help);
    case SAG_CMD_OK:
        break;
    }
    return fl_raise(vm, "user", "%s failed", d->fl_name);
}

static bool target_cursor(FlVm *vm, FlValue v, Win **out, u32 *index)
{
    const FlHandleSlot *slot;
    Cursor *cursor = fl_h_cur(vm, v, out);
    if (cursor == NULL)
        return false;
    slot = fl_h_peek(&vm->ed->handles, v);
    if (slot == NULL)
        return fl_raise(vm, "handle", "this cursor handle is closed");
    *index = slot->as.cur.index;
    return true;
}

static bool marshal_command(FlVm *vm, const FlBindDesc *d,
                            FlValue *a, u32 n, CmdCtx *cx,
                            OptVal *opt_in, OptVal *opt_out,
                            Bytebuf *scratch, CmdCursorArg **owned_cursors)
{
    const FlStr *s;
    Buffer *b;
    Buffer *sb;
    Span span;
    i64 integer;
    u32 base;
    u32 i;

    cx->ed = api_ed(vm);
    if (cx->ed == NULL)
        return false;
    cx->win = cx->ed->win;
    cx->count = 1U;
    cx->source = fl_runtime_cmd_source(vm);

    if (strcmp(d->fl_name, "opt.get") == 0 ||
        strcmp(d->fl_name, "opt.set") == 0) {
        if (!need_str(vm, a[0], 0U, &s))
            return false;
        cx->sarg = s->b;
        cx->sarg_len = s->len;
        if (strcmp(d->fl_name, "opt.get") == 0) {
            cx->opt_out = opt_out;
            return true;
        }
        if (a[1].t == (u8)FL_BOOL) {
            *opt_in = (OptVal){SAG_OPT_BOOL, {.b = a[1].as.b}};
        } else if (a[1].t == (u8)FL_INT) {
            *opt_in = (OptVal){SAG_OPT_INT, {.i = a[1].as.i}};
        } else if (a[1].t == (u8)FL_STR) {
            const FlStr *value = (const FlStr *)a[1].as.o;

            *opt_in = (OptVal){SAG_OPT_STR,
                               {.str = {value->b, value->len}}};
        } else {
            return fl_raise(vm, "type",
                            "opt.set: value must be bool, int, or str");
        }
        cx->opt_in = opt_in;
        return true;
    }

    if (d->recv == (u8)FL_H_BUF) {
        b = fl_h_buf(vm, a[0]);
        if (b == NULL) return false;
        cx->win = win_for_buffer(cx->ed, b);
        if (cx->win == NULL)
            return fl_raise(vm, "user", "buffer has no window");
    }
    if (d->recv == (u8)FL_H_WIN) {
        cx->win = fl_h_win(vm, a[0]);
        if (cx->win == NULL) return false;
    }
    if (d->recv == (u8)FL_H_CUR) {
        if (!target_cursor(vm, a[0], &cx->win, &cx->cursor_index))
            return false;
        cx->cursor_given = true;
    }
    if (d->recv == (u8)FL_H_SPAN) {
        if (!fl_h_span(vm, a[0], &b, &span)) return false;
        cx->win = win_for_buffer(cx->ed, b);
        if (cx->win == NULL) return fl_raise(vm, "user", "span buffer has no window");
        cx->range.given = true; cx->range.tok = span;
    }

    if (strcmp(d->fl_name, "cur.move") == 0) {
        const FlStr *unit;
        const FlStr *dir;

        if (!need_str(vm, a[1], 1U, &unit) ||
            !need_str(vm, a[2], 2U, &dir))
            return false;
        if (unit->len > UINT32_MAX - dir->len - 1U)
            return fl_raise(vm, "limit", "cur.move arguments are too long");
        bytebuf_append(scratch, unit->b, unit->len);
        {
            const u8 sep = (u8)':';
            bytebuf_append(scratch, &sep, 1U);
        }
        bytebuf_append(scratch, dir->b, dir->len);
        cx->sarg = (const char *)scratch->data;
        cx->sarg_len = (u32)scratch->len;
        if (n == 4U) {
            if (!need_int(vm, a[3], 3U, &integer))
                return false;
            if (integer < 1 || integer > SAG_COUNT_MAX)
                return fl_raise(vm, "type",
                                "cur.move count must be 1..%u",
                                (unsigned)SAG_COUNT_MAX);
            cx->count = (u32)integer;
            cx->count_given = true;
        }
        return true;
    }

    if (strcmp(d->fl_name, "win.set_cursors") == 0) {
        FlList *list;

        if (!need_type(vm, a[1], FL_LIST, 1U))
            return false;
        list = (FlList *)a[1].as.o;
        if (list->n == 0U || list->n > SAG_MC_MAX)
            return fl_raise(vm, "range", "win.set_cursors needs 1..%u cursors",
                            (unsigned)SAG_MC_MAX);
        *owned_cursors = sag_xcalloc(list->n, sizeof(**owned_cursors));
        for (i = 0U; i < list->n; i++) {
            Win *source;
            Cursor *cursor = fl_h_cur(vm, list->v[i], &source);

            if (cursor == NULL)
                return false;
            if (source->buf != cx->win->buf)
                return fl_raise(vm, "range",
                                "cursor belongs to another buffer");
            (*owned_cursors)[i] = (CmdCursorArg){cursor->pos,
                                                cursor->anchor,
                                                cursor->goal_col.v};
        }
        cx->cursor_args = *owned_cursors;
        cx->cursor_args_len = list->n;
        return true;
    }

    if (strcmp(d->fl_name, "span.replace_lines") == 0) {
        FlList *list;
        const u8 *eol;
        size_t eol_len;

        if (!need_type(vm, a[1], FL_LIST, 1U))
            return false;
        list = (FlList *)a[1].as.o;
        sag_filemeta_eol_bytes(&cx->win->buf->meta, &eol, &eol_len);
        for (i = 0U; i < list->n; i++) {
            const FlStr *line;

            if (!need_str(vm, list->v[i], 1U, &line))
                return false;
            if (memchr(line->b, '\n', line->len) != NULL ||
                memchr(line->b, '\r', line->len) != NULL)
                return fl_raise(vm, "type",
                                "span.replace_lines entries cannot contain EOL bytes");
            if (i != 0U)
                bytebuf_append(scratch, eol, eol_len);
            bytebuf_append(scratch, line->b, line->len);
            if (scratch->len > UINT32_MAX)
                return fl_raise(vm, "limit",
                                "span.replace_lines result is too large");
        }
        cx->sarg = (const char *)scratch->data;
        cx->sarg_len = (u32)scratch->len;
        return true;
    }

    base = d->recv == (u8)FL_H_NONE ? 0U : 1U;
    for (i = base; i < n; i++) {
        FlArgSlot slot = (FlArgSlot)d->argmap[i - base];

        switch (slot) {
        case FL_ARG_BOOL:
            if (!need_bool(vm, a[i], i, &cx->bang)) return false;
            break;
        case FL_ARG_INT:
            if (!need_int(vm, a[i], i, &cx->iarg)) return false;
            break;
        case FL_ARG_STR:
            if (!need_str(vm, a[i], i, &s)) return false;
            cx->sarg = s->b; cx->sarg_len = s->len;
            break;
        case FL_ARG_COUNT:
            if (!need_int(vm, a[i], i, &integer) || integer < 1 ||
                integer > SAG_COUNT_MAX)
                return fl_raise(vm, "type", "count must be 1..%u",
                                (unsigned)SAG_COUNT_MAX);
            cx->count = (u32)integer; cx->count_given = true;
            break;
        case FL_ARG_HANDLE_WIN:
            cx->win = fl_h_win(vm, a[i]);
            if (cx->win == NULL) return false;
            break;
        case FL_ARG_HANDLE_BUF:
            b = fl_h_buf(vm, a[i]);
            if (b == NULL) return false;
            cx->win = win_for_buffer(cx->ed, b);
            if (cx->win == NULL)
                return fl_raise(vm, "user", "buffer has no window");
            break;
        case FL_ARG_HANDLE_CUR:
            if (!target_cursor(vm, a[i], &cx->win, &cx->cursor_index))
                return false;
            cx->cursor_given = true;
            break;
        case FL_ARG_HANDLE_SPAN:
            if (!fl_h_span(vm, a[i], &sb, &span)) return false;
            cx->range.given = true; cx->range.tok = span;
            break;
        case FL_ARG_LIST:
            if (!need_type(vm, a[i], FL_LIST, i)) return false;
            break;
        case FL_ARG_MAP:
            if (!need_type(vm, a[i], FL_MAP, i)) return false;
            break;
        case FL_ARG_VALUE:
            if (strcmp(d->fl_name, "buf.save") == 0 &&
                a[i].t == (u8)FL_STR) {
                s = (const FlStr *)a[i].as.o;
                cx->sarg = s->b;
                cx->sarg_len = s->len;
            } else if (strcmp(d->fl_name, "buf.save") == 0 &&
                       a[i].t == (u8)FL_MAP) {
                FlValue force = FL_NIL_V;
                FlStr *key = fl_str_new(vm, "force", 5U);

                if (!fl_map_get((FlMap *)a[i].as.o,
                                FL_OBJ_V(FL_STR, key), &force) ||
                    force.t != (u8)FL_BOOL)
                    return fl_raise(vm, "type",
                                    "buf.save options require boolean force");
                cx->bang = force.as.b;
            } else {
                return fl_raise(vm, "type",
                                "buf.save argument 2 must be path string or options map");
            }
            break;
        case FL_ARG_NONE:
        default:
            SAG_BUG("Fletch descriptor lacks argmap slot: %s", d->fl_name);
        }
    }

    if (strcmp(d->fl_name, "buf.delete") == 0) {
        b = fl_h_buf(vm, a[0]);
        if (b == NULL || !fl_h_span(vm, a[1], &sb, &span)) return false;
        if (b != sb)
            return fl_raise(vm, "range", "span belongs to another buffer");
    }
    if (strcmp(d->fl_name, "span.prepend") == 0)
        cx->iarg = (i64)cx->range.tok.lo;
    else if (strcmp(d->fl_name, "span.append") == 0)
        cx->iarg = (i64)cx->range.tok.hi;
    if ((strcmp(d->fl_name, "buf.open") == 0 ||
         strcmp(d->fl_name, "buf.save") == 0) && cx->sarg != NULL &&
        memchr(cx->sarg, '\0', cx->sarg_len) != NULL)
        return fl_raise(vm, "type", "%s path contains an embedded NUL",
                        d->fl_name);
    return true;
}

static bool invoke_command(FlVm *vm, CmdId id, CmdCtx *cx,
                           CmdStatus *out)
{
    const CmdDesc *desc = sag_cmd_desc(id);
    const char *alternative = desc == NULL ? NULL :
        sag_batch_command_alternative(desc->name, cx);
    bool changes = desc != NULL &&
                   (desc->flags & SAG_CMD_CHANGES_BUFFER) != 0U;
    EditCtx ec;

    if (cx->ed != NULL && cx->ed->headless && desc != NULL &&
        (((desc->flags & SAG_CMD_INTERACTIVE) != 0U) ||
         alternative != NULL)) {
        if (alternative == NULL)
            SAG_BUG("interactive command lacks a batch refusal: %s",
                    desc->name);
        return fl_raise(vm, "capability",
                        "\"%s\" requires a terminal and is not available "
                        "under --batch; %s", desc->name, alternative);
    }

    if (changes && cx->ed != NULL && cx->ed->fl_model_teardown)
        return fl_raise(vm, "user",
                        "editor mutation during model teardown");
    if (changes) {
        ec = sag_ed_edit_ctx_for(cx->ed, cx->win);
        /* Opening a MACRO transaction is valid for a multi-cursor replay,
         * but the undo layer's begin-time cursor check only admits MULTI.
         * The resolved dispatcher supplies the per-cursor safety marker;
         * enlist without a begin snapshot, then refresh the full context
         * after dispatch for commit or rollback. */
        if (ec.undo != NULL && ec.undo->depth == 0U && ec.cset != NULL &&
            ec.cset->curs.len > 1U)
            ec.cset = NULL;
        if (!fl_txn_enlist(vm, &ec))
            return false;
    }
    *out = sag_ed_dispatch_resolved(cx->ed, id, cx);
    if (changes) {
        ec = sag_ed_edit_ctx_for(cx->ed, cx->win);
        if (!fl_txn_enlist(vm, &ec))
            return false;
    }
    return true;
}

static bool option_command_error(FlVm *vm, const CmdCtx *cx)
{
    if (cx->opt_error == SAG_OPT_ERROR_NAME) {
        const OptProvider *provider = sag_opt_provider(vm->ed);
        const char *names[64];
        u32 nname = provider->list(vm->ed, names,
                                   (u32)SAG_ARRAY_LEN(names));
        FlSuggest suggest;
        Bytebuf msg;
        u32 i;

        fl_suggest_reset(&suggest);
        for (i = 0U; i < nname; i++)
            fl_suggest_add(&suggest, names[i], (u32)strlen(names[i]),
                           FL_SCOPE_GLOBAL);
        bytebuf_init(&msg);
        (void)fl_suggest_render(&suggest, cx->sarg, cx->sarg_len, &msg);
        if (msg.len == 0U) {
            bytebuf_free(&msg);
            return fl_raise(vm, "name", "unknown option '%.*s'",
                            (int)cx->sarg_len, cx->sarg);
        }
        (void)fl_raise(vm, "name", "unknown option '%.*s'; %.*s",
                       (int)cx->sarg_len, cx->sarg, (int)msg.len,
                       (const char *)msg.data);
        bytebuf_free(&msg);
        return false;
    }
    return fl_raise(vm, "type", "%s", cx->opt_error_msg == NULL ?
                    "invalid option value" : cx->opt_error_msg);
}

static bool option_map_error(FlVm *vm, const FlStr *name, const char *err)
{
    if (err != NULL && strcmp(err, "unknown option") == 0) {
        const char *names[64];
        u32 nname = sag_opt_list(names, (u32)SAG_ARRAY_LEN(names));
        FlSuggest suggest;
        Bytebuf msg;
        u32 i;

        fl_suggest_reset(&suggest);
        for (i = 0U; i < nname; i++)
            fl_suggest_add(&suggest, names[i], (u32)strlen(names[i]),
                           FL_SCOPE_GLOBAL);
        bytebuf_init(&msg);
        (void)fl_suggest_render(&suggest, name->b, name->len, &msg);
        if (msg.len == 0U) {
            bytebuf_free(&msg);
            return fl_raise(vm, "name", "unknown option '%.*s'",
                            (int)name->len, name->b);
        }
        (void)fl_raise(vm, "name", "unknown option '%.*s'; %.*s",
                       (int)name->len, name->b, (int)msg.len,
                       (const char *)msg.data);
        bytebuf_free(&msg);
        return false;
    }
    return fl_raise(vm,
                    err != NULL && strstr(err, "already being changed") != NULL
                        ? "user" : "type",
                    "%s", err == NULL ? "invalid option value" : err);
}

typedef struct FlOptStage {
    const FlStr *name;
    OptVal value;
    u32 checkpoint;
    u32 ledger_id;
    bool created;
} FlOptStage;

static void option_stage_rollback(Ed *ed, FlOptStage *staged, u32 n)
{
    while (n != 0U) {
        FlOptStage *stage = &staged[--n];

        if (stage->created)
            (void)sag_opt_remove(ed, stage->ledger_id);
        else
            (void)sag_opt_rollback(ed, stage->checkpoint);
    }
}

static bool option_from_fl(FlVm *vm, FlValue value, OptVal *out)
{
    if (value.t == (u8)FL_BOOL) {
        *out = (OptVal){SAG_OPT_BOOL, {.b = value.as.b}};
        return true;
    }
    if (value.t == (u8)FL_INT) {
        *out = (OptVal){SAG_OPT_INT, {.i = value.as.i}};
        return true;
    }
    if (value.t == (u8)FL_STR) {
        const FlStr *s = (const FlStr *)value.as.o;

        *out = (OptVal){SAG_OPT_STR, {.str = {s->b, s->len}}};
        return true;
    }
    return fl_raise(vm, "type",
                    "set: option values must be bool, int, or string");
}

bool fl_api_set_options(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    FlMap *map;
    FlOptStage *staged;
    FlValue key;
    FlValue value;
    u32 cursor = 0U;
    u32 n = 0U;
    u32 i;
    u32 origin;

    if (vm == NULL || out == NULL || nargs != 1U ||
        args[0].t != (u8)FL_MAP)
        return vm == NULL ? false :
               fl_raise(vm, "type", "set expects one map argument");
    if (api_ed(vm) == NULL)
        return false;
    map = (FlMap *)args[0].as.o;
    origin = fl_origin_of_frame(vm);
    if (origin == FL_ORIGIN_ID_NONE)
        return fl_raise(vm, "handle", "set: callback has no editor origin");
    staged = sag_xcalloc(fl_map_count(map) == 0U ? 1U : fl_map_count(map),
                         sizeof(*staged));
    while (fl_map_iter(map, &cursor, &key, &value)) {
        const char *err = NULL;

        if (key.t != (u8)FL_STR) {
            free(staged);
            return fl_raise(vm, "type", "set: option names must be strings");
        }
        staged[n].name = (const FlStr *)key.as.o;
        if (!option_from_fl(vm, value, &staged[n].value)) {
            free(staged);
            return false;
        }
        if (!sag_opt_validate(vm->ed, SAG_OPT_SCOPE_DECLARED,
                              staged[n].name->b, staged[n].name->len,
                              &staged[n].value, &err)) {
            const FlStr *bad = staged[n].name;

            free(staged);
            return option_map_error(vm, bad, err);
        }
        n++;
    }
    for (i = 0U; i < n; i++) {
        const char *err = NULL;

        staged[i].checkpoint = sag_opt_checkpoint(
            vm->ed, staged[i].name->b, staged[i].name->len, &err);
        if (staged[i].checkpoint == 0U) {
            const FlStr *bad = staged[i].name;
            option_stage_rollback(vm->ed, staged, i);
            free(staged);
            return option_map_error(vm, bad, err);
        }
        if (!sag_opt_set(vm->ed, SAG_OPT_SCOPE_DECLARED,
                         staged[i].name->b, staged[i].name->len,
                         &staged[i].value, &err)) {
            const FlStr *bad = staged[i].name;
            sag_opt_discard(vm->ed, staged[i].checkpoint);
            option_stage_rollback(vm->ed, staged, i);
            free(staged);
            return option_map_error(vm, bad, err);
        }
        staged[i].ledger_id = sag_opt_commit(vm->ed, origin,
                                             staged[i].checkpoint,
                                             &staged[i].created);
        if (staged[i].ledger_id == 0U)
            SAG_BUG("validated option registration could not commit");
    }
    for (i = 0U; i < n; i++)
        if (!staged[i].created)
            sag_opt_discard(vm->ed, staged[i].checkpoint);
    free(staged);
    *out = FL_NIL_V;
    return true;
}

bool fl_api_invoke(FlVm *vm, const FlBindDesc *d,
                   FlValue *argv, u32 argc, FlValue *out)
{
    CmdCtx cx = {0};
    OptVal opt_in = {0};
    OptVal opt_out = {0};
    Bytebuf scratch;
    CmdCursorArg *owned_cursors = NULL;
    CmdStatus st;
    bool ok;
    if (vm == NULL || d == NULL || out == NULL)
        return false;
    if (argc < d->nmin || argc > d->nmax)
        return fl_raise(vm, "arity", "%s got %lu arguments", d->fl_name,
                        (unsigned long)argc);
    if (d->recv != (u8)FL_H_NONE &&
        (argc == 0U || fl_h_kind_of(argv[0]) != (FlHandleKind)d->recv))
        return fl_raise(vm, "type", "%s requires a %s receiver", d->fl_name,
                        d->recv == FL_H_BUF ? "buffer" :
                        d->recv == FL_H_WIN ? "window" :
                        d->recv == FL_H_CUR ? "cursor" : "span");
    if (d->cmd == NULL)
        return d->query(vm, argv, argc, out);
    fl_api_init();
    if (d->caps != 0U && !fl_cap_check(vm, d->caps))
        return false;
    bytebuf_init(&scratch);
    if (!marshal_command(vm, d, argv, argc, &cx, &opt_in, &opt_out,
                         &scratch, &owned_cursors)) {
        free(owned_cursors);
        bytebuf_free(&scratch);
        return false;
    }
    if (!invoke_command(vm, d->resolved_id, &cx, &st)) {
        free(owned_cursors);
        bytebuf_free(&scratch);
        return false;
    }
    if (st != SAG_CMD_OK && cx.opt_error != SAG_OPT_ERROR_NONE) {
        ok = option_command_error(vm, &cx);
        free(owned_cursors);
        bytebuf_free(&scratch);
        return ok;
    }
    if (st != SAG_CMD_OK) {
        ok = command_error(vm, d, st);
        free(owned_cursors);
        bytebuf_free(&scratch);
        return ok;
    }
    if (strcmp(d->fl_name, "buf.open") == 0) {
        Buffer *opened = sag_ed_doc(vm->ed);
        if (opened == NULL) {
            ok = fl_raise(vm, "io", "buf.open did not produce a buffer");
            free(owned_cursors);
            bytebuf_free(&scratch);
            return ok;
        }
        *out = fl_h_buf_make(vm->ed, opened);
    } else if (strcmp(d->fl_name, "win.split") == 0) {
        if (vm->ed->win == NULL) {
            ok = fl_raise(vm, "handle", "win.split produced no window");
            free(owned_cursors);
            bytebuf_free(&scratch);
            return ok;
        }
        *out = fl_h_win_make(vm->ed, vm->ed->win);
    } else if (strcmp(d->fl_name, "opt.get") == 0) {
        switch ((OptValType)opt_out.type) {
        case SAG_OPT_BOOL:
            *out = FL_BOOL_V(opt_out.as.b);
            break;
        case SAG_OPT_INT:
            *out = FL_INT_V(opt_out.as.i);
            break;
        case SAG_OPT_STR:
        case SAG_OPT_ENUM:
            *out = api_str(vm, opt_out.as.str.s, opt_out.as.str.len);
            break;
        default:
            ok = fl_raise(vm, "type", "option provider returned bad type");
            free(owned_cursors);
            bytebuf_free(&scratch);
            return ok;
        }
    } else {
        *out = FL_NIL_V;
    }
    free(owned_cursors);
    bytebuf_free(&scratch);
    return true;
}

static bool api_native(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const char *name = api_call_name(vm);
    const FlBindDesc *d = fl_api_find(name, (u32)strlen(name));
    if (d == NULL)
        return fl_raise(vm, "name", "unknown editor binding %s", name);
    return fl_api_invoke(vm, d, a, n, out);
}

static bool key_is(const FlStr *key, const char *want)
{
    size_t n = strlen(want);
    return key->len == n && memcmp(key->b, want, n) == 0;
}

static bool command_has_prefix(const CmdDesc *d, const char *prefix)
{
    size_t n = strlen(prefix);

    return d != NULL && strncmp(d->name, prefix, n) == 0;
}

/* Generic ed.run is the same effect boundary as a named binding.  Its
 * command name therefore determines the minimum host capability instead
 * of inheriting the all-powerful native frame that implements ed.run. */
static u32 command_caps(const CmdDesc *d)
{
    if (d == NULL)
        return 0U;
    if (strcmp(d->name, "ed.buf.open") == 0 ||
        strcmp(d->name, "ed.file.open") == 0 ||
        strcmp(d->name, "ed.file.new") == 0 ||
        strcmp(d->name, "ed.file.reload") == 0 ||
        strcmp(d->name, "ed.find.file") == 0 ||
        strcmp(d->name, "ed.ws.restore_state") == 0)
        return FL_CAP_FS_READ;
    if (strcmp(d->name, "ed.file.save") == 0 ||
        strcmp(d->name, "ed.file.write") == 0 ||
        strcmp(d->name, "ed.file.write_quit") == 0 ||
        strcmp(d->name, "ed.ws.save_state") == 0 ||
        strcmp(d->name, "ed.ws.forget") == 0)
        return FL_CAP_FS_WRITE;
    if (command_has_prefix(d, "ed.shell.") ||
        strcmp(d->name, "ed.job.rerun") == 0)
        return FL_CAP_SHELL;
    return 0U;
}

static bool command_path_arg(const CmdDesc *d)
{
    return d != NULL &&
           (strcmp(d->name, "ed.buf.open") == 0 ||
            strcmp(d->name, "ed.file.open") == 0 ||
            strcmp(d->name, "ed.file.new") == 0 ||
            strcmp(d->name, "ed.file.write") == 0 ||
            strcmp(d->name, "ed.file.write_quit") == 0);
}

bool fl_api_ed_run(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *name;
    FlMap *args;
    CmdId id;
    CmdCtx cx = {0};
    u32 cursor = 0U;
    FlValue key, value;
    const FlStr *range_kind = NULL;
    i64 range_lo = 0;
    i64 range_hi = 0;
    bool range_given = false;
    bool have_range_kind = false;
    bool have_range_lo = false;
    bool have_range_hi = false;
    bool have_range_given = false;
    CmdStatus st;
    const CmdDesc *desc;
    (void)n;
    if (api_ed(vm) == NULL || !need_str(vm, a[0], 0U, &name) ||
        !need_type(vm, a[1], FL_MAP, 1U))
        return false;
    args = (FlMap *)a[1].as.o;
    id = sag_cmd_lookup(name->b, name->len);
    desc = sag_cmd_desc(id);
    if (desc == NULL)
        return fl_raise(vm, "name", "unknown command %.*s", (int)name->len,
                        name->b);
    if (command_caps(desc) != 0U && !fl_cap_check(vm, command_caps(desc)))
        return false;
    cx.ed = vm->ed; cx.win = vm->ed->win; cx.count = 1U;
    cx.source = fl_runtime_cmd_source(vm);
    while (fl_map_iter(args, &cursor, &key, &value)) {
        const FlStr *k;
        i64 integer;
        if (!need_type(vm, key, FL_STR, 1U)) return false;
        k = (const FlStr *)key.as.o;
        if (key_is(k, "count")) {
            if (!need_int(vm, value, 1U, &integer)) return false;
            if (integer < 1 || integer > SAG_COUNT_MAX)
                return fl_raise(vm, "type", "ed.run count must be 1..%u",
                                (unsigned)SAG_COUNT_MAX);
            cx.count = (u32)integer; cx.count_given = true;
        } else if (key_is(k, "iarg")) {
            if (!need_int(vm, value, 1U, &cx.iarg)) return false;
        } else if (key_is(k, "sarg")) {
            const FlStr *s;
            if (!need_str(vm, value, 1U, &s)) return false;
            if (command_path_arg(desc) &&
                memchr(s->b, '\0', s->len) != NULL)
                return fl_raise(vm, "type",
                                "%s path contains an embedded NUL",
                                desc->name);
            cx.sarg = s->b; cx.sarg_len = s->len;
        } else if (key_is(k, "bang")) {
            if (!need_bool(vm, value, 1U, &cx.bang)) return false;
        } else if (key_is(k, "range_kind")) {
            if (!need_str(vm, value, 1U, &range_kind)) return false;
            have_range_kind = true;
        } else if (key_is(k, "range_lo")) {
            if (!need_int(vm, value, 1U, &range_lo)) return false;
            have_range_lo = true;
        } else if (key_is(k, "range_hi")) {
            if (!need_int(vm, value, 1U, &range_hi)) return false;
            have_range_hi = true;
        } else if (key_is(k, "range_given")) {
            if (!need_bool(vm, value, 1U, &range_given)) return false;
            have_range_given = true;
        } else if (key_is(k, "win")) {
            cx.win = fl_h_win(vm, value);
            if (cx.win == NULL) return false;
        } else {
            return fl_raise(vm, "key", "ed.run: unknown key %.*s",
                            (int)k->len, k->b);
        }
    }
    if (have_range_kind || have_range_lo || have_range_hi ||
        have_range_given) {
        bool endpoints;

        if (!have_range_kind || !have_range_given)
            return fl_raise(vm, "type",
                            "ed.run range needs range_kind and range_given");
        endpoints = key_is(range_kind, "lines") ||
                    key_is(range_kind, "span");
        if (endpoints != (have_range_lo && have_range_hi) ||
            range_lo < 0 || range_hi < range_lo)
            return fl_raise(vm, "range", "ed.run range is invalid");
        cx.range.given = range_given;
        if (key_is(range_kind, "lines")) {
            cx.range.kind = SAG_RANGE_LINES;
            cx.range.lo = LINENO((u64)range_lo);
            cx.range.hi = LINENO((u64)range_hi);
        } else if (key_is(range_kind, "buffer")) {
            cx.range.kind = SAG_RANGE_BUFFER;
        } else if (key_is(range_kind, "selection")) {
            cx.range.kind = SAG_RANGE_SELECTION;
        } else if (key_is(range_kind, "span")) {
            if (!range_given)
                return fl_raise(vm, "range",
                                "ed.run span range must be given");
            cx.range.tok = (Span){(u64)range_lo, (u64)range_hi};
        } else {
            return fl_raise(vm, "range", "ed.run range kind is invalid");
        }
    }
    if (!invoke_command(vm, id, &cx, &st))
        return false;
    if (st == SAG_CMD_OK) { *out = FL_NIL_V; return true; }
    {
        FlBindDesc dynamic = {"ed.run", desc->name, id, FL_H_NONE,
                              2U, 2U, {0U, 0U, 0U}, 0U, NULL};
        return command_error(vm, &dynamic, st);
    }
}

bool fl_api_ed_commands(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *list;
    u32 i;
    (void)a; (void)n;
    list = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *d = sag_cmd_at(i);
        FlMap *m = fl_map_new(vm);
        fl_gc_protect(vm, FL_OBJ_V(FL_MAP, m));
        (void)map_put(vm, m, "name", api_cstr(vm, d->name));
        (void)map_put(vm, m, "help", api_cstr(vm, d->help));
        (void)map_put(vm, m, "flags", FL_INT_V((i64)d->flags));
        (void)map_put(vm, m, "word", api_cstr(vm, d->word));
        (void)fl_list_push(vm, list, FL_OBJ_V(FL_MAP, m));
        fl_gc_release(vm, 1U);
    }
    *out = FL_OBJ_V(FL_LIST, list);
    fl_gc_release(vm, 1U);
    return true;
}

#define NATIVE(name_, min_, max_) {name_, api_native, min_, max_, 0U, "editor API"}

static const FlNativeDef BUF_DEFS[] = {
    NATIVE("current", 0U, 0U), NATIVE("list", 0U, 0U),
    NATIVE("open", 1U, 1U), NATIVE("close", 1U, 2U),
    NATIVE("save", 1U, 2U), NATIVE("path", 1U, 1U),
    NATIVE("name", 1U, 1U), NATIVE("lang", 1U, 1U),
    NATIVE("len", 1U, 1U), NATIVE("lines", 1U, 1U),
    NATIVE("text", 1U, 2U), NATIVE("line", 2U, 2U),
    NATIVE("span", 3U, 3U), NATIVE("line_span", 2U, 2U),
    NATIVE("insert", 3U, 3U), NATIVE("delete", 2U, 2U),
    NATIVE("find", 2U, 3U), NATIVE("find_all", 2U, 2U),
    NATIVE("dirty", 1U, 1U), NATIVE("readonly", 1U, 1U),
    NATIVE("mark", 2U, 2U)
};
static const FlNativeDef WIN_DEFS[] = {
    NATIVE("current", 0U, 0U), NATIVE("list", 0U, 0U),
    NATIVE("buf", 1U, 1U), NATIVE("cursors", 1U, 1U),
    NATIVE("selection", 1U, 1U), NATIVE("split", 2U, 2U),
    NATIVE("close", 1U, 1U), NATIVE("focus", 1U, 1U),
    NATIVE("viewport", 1U, 1U), NATIVE("set_cursors", 2U, 2U)
};
static const FlNativeDef CUR_DEFS[] = {
    NATIVE("pos", 1U, 1U), NATIVE("line", 1U, 1U),
    NATIVE("col", 1U, 1U), NATIVE("anchor", 1U, 1U),
    NATIVE("span", 1U, 1U), NATIVE("goto", 2U, 2U),
    NATIVE("move", 3U, 4U),
    NATIVE("word", 1U, 1U),
    NATIVE("line_span", 1U, 1U), NATIVE("insert", 2U, 2U),
    NATIVE("is_primary", 1U, 1U), NATIVE("valid", 1U, 1U)
};
static const FlNativeDef SPAN_DEFS[] = {
    NATIVE("lo", 1U, 1U), NATIVE("hi", 1U, 1U),
    NATIVE("len", 1U, 1U), NATIVE("text", 1U, 1U),
    NATIVE("lines", 1U, 1U), NATIVE("replace", 2U, 2U),
    NATIVE("replace_lines", 2U, 2U),
    NATIVE("prepend", 2U, 2U), NATIVE("append", 2U, 2U),
    NATIVE("delete", 1U, 1U), NATIVE("yank", 1U, 2U),
    NATIVE("valid", 1U, 1U)
};
static const FlNativeDef OPT_DEFS[] = {
    NATIVE("get", 1U, 1U), NATIVE("set", 2U, 2U)
};
static const FlNativeDef ED_DEFS[] = {
    {"run", fl_api_ed_run, 2U, 2U, 0U, "(name, args) -> nil"},
    {"commands", fl_api_ed_commands, 0U, 0U, 0U, "() -> list"}
};

const FlModuleDef fl_mod_buf = {"buf", BUF_DEFS, SAG_ARRAY_LEN(BUF_DEFS), NULL, 0U};
const FlModuleDef fl_mod_win = {"win", WIN_DEFS, SAG_ARRAY_LEN(WIN_DEFS), NULL, 0U};
const FlModuleDef fl_mod_cur = {"cur", CUR_DEFS, SAG_ARRAY_LEN(CUR_DEFS), NULL, 0U};
const FlModuleDef fl_mod_span = {"span", SPAN_DEFS, SAG_ARRAY_LEN(SPAN_DEFS), NULL, 0U};
const FlModuleDef fl_mod_opt = {"opt", OPT_DEFS, SAG_ARRAY_LEN(OPT_DEFS), NULL, 0U};
const FlModuleDef fl_mod_ed = {"ed", ED_DEFS, SAG_ARRAY_LEN(ED_DEFS), NULL, 0U};
