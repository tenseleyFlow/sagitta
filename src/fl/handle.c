/*
 * Sprint 34 deliverable 1: the generation-checked handle table.
 *
 * Read handle.h first; the two-guard rationale lives there.
 */

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "fl/handle.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "search/regex.h"
#include "text/mark.h"
#include "text/piece.h"
#include "ui/win.h"
#include "util/base.h"

/* ---------------------------------------------------------------- */
/* Tag <-> kind                                                     */
/* ---------------------------------------------------------------- */

static FlType tag_of_kind(FlHandleKind k)
{
    switch (k) {
    case FL_H_BUF:  return FL_BUF;
    case FL_H_CUR:  return FL_CURSOR;
    case FL_H_SPAN: return FL_SPAN;
    case FL_H_WIN:  return FL_WIN;
    case FL_H_RE:   return FL_REGEX;
    default:        break;
    }
    YEW_BUG("fletch handle: no type tag for kind");
}

FlHandleKind fl_h_kind_of(FlValue v)
{
    switch ((FlType)v.t) {
    case FL_BUF:    return FL_H_BUF;
    case FL_CURSOR: return FL_H_CUR;
    case FL_SPAN:   return FL_H_SPAN;
    case FL_WIN:    return FL_H_WIN;
    case FL_REGEX:  return FL_H_RE;
    default:        return FL_H_NONE;
    }
}

bool fl_h_is(FlValue v)
{
    return fl_h_kind_of(v) != FL_H_NONE;
}

/*
 * The payload rides in the eight bytes of the `as` union.  Encoding by
 * hand rather than adding a union member keeps value.h free of any
 * knowledge of handles -- FlValue is spec §4's value, and the editor is
 * a host detail.
 */
FlHandle fl_h_decode(FlValue v)
{
    FlHandle h;
    u64 bits = (u64)v.as.i;

    h.slot = (u32)(bits & 0xFFFFFFFFU);
    h.gen = (u32)(bits >> 32);
    return h;
}

static FlValue encode(FlHandleKind k, u32 slot, u32 gen)
{
    FlValue v = FL_NIL_V;

    v.t = (u8)tag_of_kind(k);
    v.as.i = (i64)(((u64)gen << 32) | (u64)slot);
    return v;
}

/* ---------------------------------------------------------------- */
/* Table                                                            */
/* ---------------------------------------------------------------- */

void fl_h_table_init(FlHandleTable *t)
{
    if (t == NULL)
        YEW_BUG("fletch handle table: NULL");
    (void)memset(t, 0, sizeof(*t));
}

static void slot_release(FlHandleSlot *s)
{
    if (s->kind == (u8)FL_H_RE && s->as.re.a != NULL) {
        arena_free_all(s->as.re.a);
        yew_xfree(s->as.re.a);
        s->as.re.a = NULL;
        s->as.re.re = NULL;
    }
}

void fl_h_table_free(FlHandleTable *t)
{
    u32 i;

    if (t == NULL)
        return;
    for (i = 0U; i < t->n; i++)
        slot_release(&t->slots[i]);
    yew_xfree(t->slots);
    (void)memset(t, 0, sizeof(*t));
}

FlValue fl_h_make(FlHandleTable *t, FlHandleKind k, const FlHandleSlot *init)
{
    FlHandleSlot *s;
    u32 idx;

    if (t == NULL || k == FL_H_NONE || k >= FL_H__N || init == NULL)
        YEW_BUG("fletch handle: bad make");
    if (t->free_head != 0U) {
        idx = t->free_head - 1U;
        t->free_head = t->slots[idx].next_free;
    } else {
        if (t->n == t->cap) {
            u32 want = t->cap == 0U ? 8U : t->cap * 2U;

            t->slots = (FlHandleSlot *)yew_xreallocarray(t->slots, want,
                                                         sizeof(*t->slots));
            t->cap = want;
        }
        idx = t->n++;
        (void)memset(&t->slots[idx], 0, sizeof(t->slots[idx]));
        t->slots[idx].gen = 1U;
    }
    s = &t->slots[idx];
    s->as = init->as;
    s->kind = (u8)k;
    s->next_free = 0U;
    t->live++;
    return encode(k, idx, s->gen);
}

static FlHandleSlot *resolve_slot(const FlHandleTable *t, FlValue v)
{
    FlHandle h;
    FlHandleKind k = fl_h_kind_of(v);
    FlHandleSlot *s;

    if (t == NULL || k == FL_H_NONE)
        return NULL;
    h = fl_h_decode(v);
    if (h.slot >= t->n)
        return NULL;
    s = &t->slots[h.slot];
    if (s->kind != (u8)k || s->gen != h.gen)
        return NULL;
    return s;
}

const FlHandleSlot *fl_h_peek(const FlHandleTable *t, FlValue v)
{
    return resolve_slot(t, v);
}

FlHandleSlot *fl_h_peek_mut(FlHandleTable *t, FlValue v)
{
    return resolve_slot(t, v);
}

static bool slot_free(FlHandleTable *t, FlValue v)
{
    FlHandleSlot *s = resolve_slot(t, v);
    u32 idx;

    if (s == NULL)
        return false;
    slot_release(s);
    idx = fl_h_decode(v).slot;
    s->kind = (u8)FL_H_NONE;
    t->live--;
    /*
     * Wraparound retires the slot.  A gen that rolled over to 0 would
     * make every handle taken in the slot's first life valid again;
     * one leaked 32-byte slot per four billion frees is the cheaper
     * failure by any measure.
     */
    if (s->gen == 0xFFFFFFFFU) {
        s->gen = 0U;
        s->next_free = 0U;
        return true;
    }
    s->gen++;
    s->next_free = t->free_head;
    t->free_head = idx + 1U;
    return true;
}

bool fl_h_alive(const FlHandleTable *t, FlValue v)
{
    return resolve_slot(t, v) != NULL;
}

/* ---------------------------------------------------------------- */
/* Constructors                                                     */
/* ---------------------------------------------------------------- */

static FlHandleTable *table_of(Ed *ed)
{
    if (ed == NULL)
        YEW_BUG("fletch handle: no editor");
    return &ed->handles;
}

static FlValue find_buf(const FlHandleTable *t, u32 buf_id)
{
    u32 i;

    for (i = 0U; i < t->n; i++) {
        const FlHandleSlot *s = &t->slots[i];

        if (s->kind == (u8)FL_H_BUF && s->as.buf == buf_id)
            return encode(FL_H_BUF, i, s->gen);
    }
    return FL_NIL_V;
}

static FlValue find_win(const FlHandleTable *t, u32 win_id)
{
    u32 i;

    for (i = 0U; i < t->n; i++) {
        const FlHandleSlot *s = &t->slots[i];

        if (s->kind == (u8)FL_H_WIN && s->as.win == win_id)
            return encode(FL_H_WIN, i, s->gen);
    }
    return FL_NIL_V;
}

static FlValue find_cur(const FlHandleTable *t, u32 win_id, u64 stamp)
{
    u32 i;

    for (i = 0U; i < t->n; i++) {
        const FlHandleSlot *s = &t->slots[i];

        if (s->kind == (u8)FL_H_CUR && s->as.cur.win == win_id &&
            s->as.cur.stamp == stamp)
            return encode(FL_H_CUR, i, s->gen);
    }
    return FL_NIL_V;
}

static FlValue find_span(const FlHandleTable *t, const Buffer *b,
                         u64 lo, u64 hi)
{
    u32 i;

    for (i = 0U; i < t->n; i++) {
        const FlHandleSlot *s = &t->slots[i];

        if (s->kind != (u8)FL_H_SPAN || s->as.span.buf != b->id ||
            !yew_mark_alive(b->marks, s->as.span.lo) ||
            !yew_mark_alive(b->marks, s->as.span.hi))
            continue;
        if (yew_mark_pos(b->marks, s->as.span.lo).v == lo &&
            yew_mark_pos(b->marks, s->as.span.hi).v == hi)
            return encode(FL_H_SPAN, i, s->gen);
    }
    return FL_NIL_V;
}

FlValue fl_h_buf_make(Ed *ed, const Buffer *b)
{
    FlHandleSlot init;
    FlValue found;

    if (b == NULL)
        return FL_NIL_V;
    found = find_buf(table_of(ed), b->id);
    if (found.t != (u8)FL_NIL)
        return found;
    (void)memset(&init, 0, sizeof(init));
    init.as.buf = b->id;
    return fl_h_make(table_of(ed), FL_H_BUF, &init);
}

FlValue fl_h_win_make(Ed *ed, const Win *w)
{
    FlHandleSlot init;
    FlValue found;

    if (w == NULL)
        return FL_NIL_V;
    found = find_win(table_of(ed), w->id);
    if (found.t != (u8)FL_NIL)
        return found;
    (void)memset(&init, 0, sizeof(init));
    init.as.win = w->id;
    return fl_h_make(table_of(ed), FL_H_WIN, &init);
}

FlValue fl_h_cur_make(Ed *ed, const Win *w, u32 index)
{
    FlHandleSlot init;
    FlValue found;
    u64 stamp;

    if (w == NULL || (size_t)index >= w->cs.curs.len ||
        (size_t)index >= w->cs.stamps.len)
        return FL_NIL_V;
    stamp = w->cs.stamps.data[index];
    found = find_cur(table_of(ed), w->id, stamp);
    if (found.t != (u8)FL_NIL)
        return found;
    (void)memset(&init, 0, sizeof(init));
    init.as.cur.win = w->id;
    init.as.cur.index = index;
    init.as.cur.stamp = stamp;
    return fl_h_make(table_of(ed), FL_H_CUR, &init);
}

FlValue fl_h_span_make(Ed *ed, Buffer *b, u64 lo, u64 hi)
{
    FlHandleSlot init;
    FlValue found;

    if (b == NULL || b->marks == NULL)
        return FL_NIL_V;
    if (lo > hi) {
        u64 t = lo;

        lo = hi;
        hi = t;
    }
    found = find_span(table_of(ed), b, lo, hi);
    if (found.t != (u8)FL_NIL)
        return found;
    (void)memset(&init, 0, sizeof(init));
    init.as.span.buf = b->id;
    /*
     * lo biases LEFT and hi biases RIGHT, so text inserted at either
     * end joins the span.  That is what makes s.prepend("(") then
     * s.append(")") bracket the same text rather than drift off it.
     */
    init.as.span.lo = yew_mark_add(b->marks, BYTEOFF(lo), YEW_BIAS_LEFT);
    init.as.span.hi = yew_mark_add(b->marks, BYTEOFF(hi), YEW_BIAS_RIGHT);
    return fl_h_make(table_of(ed), FL_H_SPAN, &init);
}

FlValue fl_h_re_make(Ed *ed, Arena *own, YewRe *re)
{
    FlHandleSlot init;

    if (own == NULL || re == NULL)
        return FL_NIL_V;
    (void)memset(&init, 0, sizeof(init));
    init.as.re.a = own;
    init.as.re.re = re;
    return fl_h_make(table_of(ed), FL_H_RE, &init);
}

/* ---------------------------------------------------------------- */
/* Bulk release                                                     */
/* ---------------------------------------------------------------- */

static void drop_span_marks(Buffer *b, FlHandleSlot *s)
{
    if (b == NULL || b->marks == NULL)
        return;
    if (yew_mark_alive(b->marks, s->as.span.lo))
        yew_mark_del(b->marks, s->as.span.lo);
    if (yew_mark_alive(b->marks, s->as.span.hi))
        yew_mark_del(b->marks, s->as.span.hi);
}

bool fl_h_free(Ed *ed, FlValue v)
{
    FlHandleSlot *s;

    if (ed == NULL)
        return false;
    s = resolve_slot(&ed->handles, v);
    if (s == NULL)
        return false;
    if (s->kind == (u8)FL_H_SPAN) {
        Buffer *b = yew_ws_buf_by_id(ed, s->as.span.buf);

        drop_span_marks(b, s);
    }
    return slot_free(&ed->handles, v);
}

void fl_h_drop_buffer(Ed *ed, u32 buf_id)
{
    FlHandleTable *t;
    u32 i;

    if (ed == NULL)
        return;
    t = &ed->handles;
    for (i = 0U; i < t->n; i++) {
        FlHandleSlot *s = &t->slots[i];

        if (s->kind == (u8)FL_H_BUF && s->as.buf == buf_id) {
            (void)fl_h_free(ed, encode(FL_H_BUF, i, s->gen));
        } else if (s->kind == (u8)FL_H_SPAN && s->as.span.buf == buf_id) {
            (void)fl_h_free(ed, encode(FL_H_SPAN, i, s->gen));
        }
    }
}

void fl_h_drop_window(Ed *ed, u32 win_id)
{
    FlHandleTable *t;
    u32 i;

    if (ed == NULL)
        return;
    t = &ed->handles;
    for (i = 0U; i < t->n; i++) {
        FlHandleSlot *s = &t->slots[i];

        if (s->kind == (u8)FL_H_WIN && s->as.win == win_id)
            (void)fl_h_free(ed, encode(FL_H_WIN, i, s->gen));
        else if (s->kind == (u8)FL_H_CUR && s->as.cur.win == win_id)
            (void)fl_h_free(ed, encode(FL_H_CUR, i, s->gen));
    }
}

/* ---------------------------------------------------------------- */
/* Resolvers                                                        */
/* ---------------------------------------------------------------- */

static const char *kind_word(FlHandleKind k)
{
    switch (k) {
    case FL_H_BUF:  return "buffer";
    case FL_H_CUR:  return "cursor";
    case FL_H_SPAN: return "span";
    case FL_H_WIN:  return "window";
    case FL_H_RE:   return "regex";
    default:        return "handle";
    }
}

/*
 * One place words a handle failure.  A resolver that spelled its own
 * message would drift, and the whole reason "handle" is its own error
 * kind (amendment A2) is so a catch can tell "you passed the wrong
 * thing" from "it closed under you".
 */
static bool h_raise_dead(FlVm *vm, FlHandleKind want)
{
    return fl_raise(vm, "handle", "this %s handle is closed",
                    kind_word(want));
}

static bool h_raise_gone(FlVm *vm, FlHandleKind want, u32 id)
{
    return fl_raise(vm, "handle", "%s %lu is closed", kind_word(want),
                    (unsigned long)id);
}

static FlHandleSlot *need(FlVm *vm, FlValue v, FlHandleKind want)
{
    FlHandleSlot *s;

    if (fl_h_kind_of(v) != want) {
        (void)fl_raise(vm, "type", "expected a %s handle, found %s",
                       kind_word(want), fl_type_name((FlType)v.t));
        return NULL;
    }
    if (vm->ed == NULL) {
        (void)fl_raise(vm, "handle", "no editor to resolve a %s handle",
                       kind_word(want));
        return NULL;
    }
    s = resolve_slot(&vm->ed->handles, v);
    if (s == NULL) {
        (void)h_raise_dead(vm, want);
        return NULL;
    }
    return s;
}

Buffer *fl_h_buf(FlVm *vm, FlValue v)
{
    FlHandleSlot *s = need(vm, v, FL_H_BUF);
    Buffer *b;

    if (s == NULL)
        return NULL;
    b = yew_ws_buf_by_id(vm->ed, s->as.buf);
    if (b == NULL) {
        (void)h_raise_gone(vm, FL_H_BUF, s->as.buf);
        return NULL;
    }
    return b;
}

Win *fl_h_win(FlVm *vm, FlValue v)
{
    FlHandleSlot *s = need(vm, v, FL_H_WIN);
    Win *w;

    if (s == NULL)
        return NULL;
    w = yew_ed_win_by_id(vm->ed, s->as.win);
    if (w == NULL) {
        (void)h_raise_gone(vm, FL_H_WIN, s->as.win);
        return NULL;
    }
    return w;
}

Cursor *fl_h_cur(FlVm *vm, FlValue v, Win **out_win)
{
    FlHandleSlot *s = need(vm, v, FL_H_CUR);
    Win *w;
    size_t index;

    if (s == NULL)
        return NULL;
    w = yew_ed_win_by_id(vm->ed, s->as.cur.win);
    if (w == NULL) {
        (void)h_raise_gone(vm, FL_H_WIN, s->as.cur.win);
        return NULL;
    }
    index = (size_t)s->as.cur.index;
    if (index >= w->cs.stamps.len ||
        w->cs.stamps.data[index] != s->as.cur.stamp) {
        for (index = 0U; index < w->cs.stamps.len; index++) {
            if (w->cs.stamps.data[index] == s->as.cur.stamp)
                break;
        }
    }
    if (index >= w->cs.curs.len || index >= w->cs.stamps.len) {
        (void)fl_raise(vm, "handle",
                       "this cursor was merged away");
        return NULL;
    }
    s->as.cur.index = (u32)index;
    if (out_win != NULL)
        *out_win = w;
    return &w->cs.curs.data[index];
}

bool fl_h_span(FlVm *vm, FlValue v, Buffer **out_buf, Span *out)
{
    FlHandleSlot *s = need(vm, v, FL_H_SPAN);
    Buffer *b;
    u64 lo, hi;

    if (s == NULL)
        return false;
    b = yew_ws_buf_by_id(vm->ed, s->as.span.buf);
    if (b == NULL || b->marks == NULL) {
        (void)h_raise_gone(vm, FL_H_BUF, s->as.span.buf);
        return false;
    }
    if (!yew_mark_alive(b->marks, s->as.span.lo) ||
        !yew_mark_alive(b->marks, s->as.span.hi)) {
        (void)fl_raise(vm, "handle", "this span's text was deleted");
        return false;
    }
    lo = yew_mark_pos(b->marks, s->as.span.lo).v;
    hi = yew_mark_pos(b->marks, s->as.span.hi).v;
    /*
     * A delete that swallowed the span collapses both marks onto the
     * deletion point, and both orders have been observed depending on
     * bias.  A zero-length span there is DEFINED, not an error: the
     * text the script was pointing at is gone and the position it was
     * at is the only honest answer.
     */
    if (lo > hi)
        lo = hi;
    if (out_buf != NULL)
        *out_buf = b;
    if (out != NULL) {
        out->lo = lo;
        out->hi = hi;
    }
    return true;
}

const YewRe *fl_h_re(FlVm *vm, FlValue v)
{
    FlHandleSlot *s = need(vm, v, FL_H_RE);

    if (s == NULL)
        return NULL;
    return s->as.re.re;
}
