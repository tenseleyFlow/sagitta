#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 34 §1, DoD 3 and 4: the generation-checked handle table.
 *
 * WHAT THIS FILE IS DEFENDING.  A script holds an editor handle for as
 * long as it likes and the editor closes the object underneath it
 * whenever the user says so.  handle.h's answer is two independent
 * guards, and the whole point of the pair is that NEITHER SUBSUMES THE
 * OTHER:
 *
 *   - the slot GENERATION catches handle-slot reuse (the slot is live
 *     again, for somebody else);
 *   - the stored OBJECT ID catches object death (the slot is still
 *     yours, but the buffer it names is gone).
 *
 * So the matrix below drives both failures against every kind rather
 * than proving one of them once.  A version that dropped the id
 * indirection passes every use-after-free test here and still
 * dereferences a recycled Buffer *; a version that dropped the
 * generation passes every object-close test and still resolves a
 * four-frees-ago handle onto a live but wrong object.
 *
 * Asserted through fl_h_alive and the RESOLVERS rather than by reading
 * slots, because "never dereferences" is a claim about what the
 * resolver does, and a test that inspected the table directly would
 * pass on a resolver that skipped its checks entirely.
 */

#include "flfix.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/handle.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "search/regex.h"
#include "text/edit.h"

typedef struct HandleFix {
    FlFix fl;
    Ed ed;
} HandleFix;

/*
 * An editor and a VM wired to it.  flfix builds a HEADLESS vm (vm.ed is
 * NULL), which is right for every other fl test and useless here: the
 * resolvers reach the table through vm->ed, and a NULL one raises
 * "no editor to resolve" long before any of the interesting checks.
 */
static void hf_open(HandleFix *f)
{
    flfix_open(&f->fl);
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    f->fl.vm.ed = &f->ed;
}

static void hf_close(HandleFix *f)
{
    f->fl.vm.ed = NULL;
    yew_ed_free(&f->ed);
    flfix_close(&f->fl);
}

/* The `kind` field of the in-flight raise, or "" if nothing is raised. */
static void raised_kind(FlVm *vm, char *out, size_t cap)
{
    FlValue got = FL_NIL_V;
    FlStr *k;

    out[0] = '\0';
    if (vm->err.t != (u8)FL_MAP)
        return;
    k = fl_str_new(vm, "kind", 4U);
    if (!fl_map_get((FlMap *)vm->err.as.o, FL_OBJ_V(FL_STR, k), &got))
        return;
    if (got.t != (u8)FL_STR)
        return;
    (void)snprintf(out, cap, "%.*s", (int)((const FlStr *)got.as.o)->len,
                   ((const FlStr *)got.as.o)->b);
}

static void clear_raise(FlVm *vm)
{
    vm->err = FL_NIL_V;
}

/* Resolves `v` as `kind` and returns the error kind that came back.
 * "" means the resolver SUCCEEDED, which is itself a failure for every
 * row of the dead matrix. */
static void resolve_kind(HandleFix *f, FlHandleKind kind, FlValue v,
                         char *out, size_t cap)
{
    FlVm *vm = &f->fl.vm;
    bool ok;

    clear_raise(vm);
    switch (kind) {
    case FL_H_BUF:
        ok = fl_h_buf(vm, v) != NULL;
        break;
    case FL_H_WIN:
        ok = fl_h_win(vm, v) != NULL;
        break;
    case FL_H_CUR: {
        Win *w = NULL;

        ok = fl_h_cur(vm, v, &w) != NULL;
        break;
    }
    case FL_H_SPAN: {
        Buffer *b = NULL;
        Span s = {0U, 0U};

        ok = fl_h_span(vm, v, &b, &s);
        break;
    }
    case FL_H_RE:
        ok = fl_h_re(vm, v) != NULL;
        break;
    default:
        YEW_BUG("resolve_kind: not a handle kind");
    }
    if (ok) {
        (void)snprintf(out, cap, "%s", "");
        return;
    }
    raised_kind(vm, out, cap);
}

/* A handle of `kind` over a payload that names nothing real.  Enough
 * for the free/reuse rows, which fail at the SLOT and so never look at
 * the payload — proving they fail there is the point. */
static FlValue synthetic(HandleFix *f, FlHandleKind kind)
{
    FlHandleSlot init;

    (void)memset(&init, 0, sizeof(init));
    switch (kind) {
    case FL_H_BUF:  init.as.buf = 4242U; break;
    case FL_H_WIN:  init.as.win = 4242U; break;
    case FL_H_CUR:  init.as.cur.win = 4242U; init.as.cur.index = 0U; break;
    case FL_H_SPAN: init.as.span.buf = 4242U; break;
    case FL_H_RE:   init.as.re.a = NULL; init.as.re.re = NULL; break;
    default:        YEW_BUG("synthetic: not a handle kind");
    }
    return fl_h_make(&f->ed.handles, kind, &init);
}

static const char *kind_name(FlHandleKind k)
{
    switch (k) {
    case FL_H_BUF:  return "buffer";
    case FL_H_CUR:  return "cursor";
    case FL_H_SPAN: return "span";
    case FL_H_WIN:  return "window";
    case FL_H_RE:   return "regex";
    default:        return "?";
    }
}

/*
 * "<kind>:<raised>", so a failure inside the five-kind loop names the
 * kind that broke.  The bare error string would report a line number
 * and leave the reader to work out which iteration it was on.
 */
static void resolve_tagged(HandleFix *f, FlHandleKind kind, FlValue v,
                           char *out, size_t cap)
{
    char raised[32];

    resolve_kind(f, kind, v, raised, sizeof(raised));
    (void)snprintf(out, cap, "%s:%s", kind_name(kind), raised);
}

/* The matching expectation, built the same way. */
static void tagged_want(FlHandleKind kind, const char *raised, char *out,
                        size_t cap)
{
    (void)snprintf(out, cap, "%s:%s", kind_name(kind), raised);
}

/*
 * DoD 3, rows 1 and 2, for ALL FIVE KINDS: a handle used after its own
 * free, and a handle used after its slot has been handed to somebody
 * else, both raise "handle" and neither reaches the payload.
 *
 * The reuse row is the one that needs the generation.  It re-makes a
 * handle of a DIFFERENT kind in the recycled slot, so a table that
 * compared only `kind` would notice — and then asserts the same-kind
 * case too, which is the one only `gen` can catch.
 */
void test_fl_handle_dead_matrix_free_and_reuse_all_kinds(void)
{
    static const FlHandleKind kinds[] = {FL_H_BUF, FL_H_CUR, FL_H_SPAN,
                                         FL_H_WIN, FL_H_RE};
    HandleFix f;
    size_t i;

    hf_open(&f);
    for (i = 0U; i < YEW_ARRAY_LEN(kinds); i++) {
        FlHandleKind k = kinds[i];
        FlValue h = synthetic(&f, k);
        FlValue reused;
        char got[64];
        char want[64];

        tagged_want(k, "handle", want, sizeof(want));
        YEW_ASSERT(fl_h_is(h));
        YEW_ASSERT_EQ_U64((u64)fl_h_kind_of(h), (u64)k);
        YEW_ASSERT(fl_h_alive(&f.ed.handles, h));

        /* Row 1: use after free. */
        YEW_ASSERT(fl_h_free(&f.ed, h));
        YEW_ASSERT(!fl_h_alive(&f.ed.handles, h));
        resolve_tagged(&f, k, h, got, sizeof(got));
        YEW_ASSERT_EQ_STR(got, want);
        /* Freeing twice is a clean false, not a double release. */
        YEW_ASSERT(!fl_h_free(&f.ed, h));

        /* Row 2: use after the SLOT is reused, same kind. */
        reused = synthetic(&f, k);
        YEW_ASSERT_EQ_U64(fl_h_decode(reused).slot, fl_h_decode(h).slot);
        YEW_ASSERT(fl_h_alive(&f.ed.handles, reused));
        YEW_ASSERT(!fl_h_alive(&f.ed.handles, h));
        resolve_tagged(&f, k, h, got, sizeof(got));
        YEW_ASSERT_EQ_STR(got, want);
        YEW_ASSERT(fl_h_free(&f.ed, reused));
    }
    hf_close(&f);
}

/*
 * DoD 3, row 3: use after the OBJECT closes, with the slot still
 * perfectly valid.
 *
 * Driven against a real buffer and a real window rather than a
 * synthetic id, because the claim is that the resolver LOOKS THE OBJECT
 * UP and finds it absent — an assertion a made-up id could satisfy by
 * accident on the very first call.
 */
void test_fl_handle_dead_matrix_object_close(void)
{
    HandleFix f;
    Arena *own;
    Buffer *b;
    YewRe *re;
    FlValue hb;
    FlValue hc;
    FlValue hr;
    FlValue hs;
    FlValue hw;
    char kindbuf[32];

    hf_open(&f);
    b = yew_ed_doc(&f.ed);
    YEW_ASSERT_NOT_NULL(b);
    hb = fl_h_buf_make(&f.ed, b);
    hw = fl_h_win_make(&f.ed, f.ed.win);
    hc = fl_h_cur_make(&f.ed, f.ed.win, 0U);
    hs = fl_h_span_make(&f.ed, b, 0U, 0U);

    /* Every editor-owned kind resolves while its object is open. */
    resolve_kind(&f, FL_H_BUF, hb, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "");
    resolve_kind(&f, FL_H_WIN, hw, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "");
    resolve_kind(&f, FL_H_CUR, hc, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "");
    resolve_kind(&f, FL_H_SPAN, hs, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "");
    /* The slot is alive in both the before and after cases; only the
     * object goes away.  Stated here so the assertion after the close
     * cannot be read as the generation guard doing the work. */
    YEW_ASSERT(fl_h_alive(&f.ed.handles, hb));

    /*
     * Close the objects the way the editor does.  fl_h_drop_buffer runs
     * BEFORE the MarkSet dies — a span whose marks outlive their set is
     * the one shape neither guard catches, which is why the drop exists
     * at all.
     */
    fl_h_drop_window(&f.ed, f.ed.win->id);
    resolve_kind(&f, FL_H_WIN, hw, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "handle");
    resolve_kind(&f, FL_H_CUR, hc, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "handle");

    fl_h_drop_buffer(&f.ed, b->id);
    resolve_kind(&f, FL_H_BUF, hb, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "handle");
    resolve_kind(&f, FL_H_SPAN, hs, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "handle");

    /* Regex is the one owning handle kind: closing the object means
     * freeing its handle, which must release the arena and stale the value. */
    own = yew_xmalloc(sizeof(*own));
    arena_init(own);
    re = yew_re_compile(own, "a+", 2U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    hr = fl_h_re_make(&f.ed, own, re);
    resolve_kind(&f, FL_H_RE, hr, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "");
    YEW_ASSERT(fl_h_free(&f.ed, hr));
    resolve_kind(&f, FL_H_RE, hr, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "handle");

    /*
     * THE OBJECT-ID GUARD ON ITS OWN.
     *
     * Everything above went through fl_h_drop_*, which RELEASES the
     * slot — so the generation guard is what actually refused those,
     * and the id indirection could have been missing entirely.  Here
     * the slot stays live and only the object is absent, which is the
     * one arrangement that reaches yew_ws_buf_by_id's NULL branch.
     *
     * That is the "neither guard subsumes the other" claim from
     * handle.h, asserted rather than restated: alive() says yes and the
     * resolver still says "handle".
     */
    {
        FlValue orphan = synthetic(&f, FL_H_BUF);

        YEW_ASSERT(fl_h_alive(&f.ed.handles, orphan));
        resolve_kind(&f, FL_H_BUF, orphan, kindbuf, sizeof(kindbuf));
        YEW_ASSERT_EQ_STR(kindbuf, "handle");

        /* And the same for a window, whose lookup walks every tab. */
        {
            FlValue worphan = synthetic(&f, FL_H_WIN);

            YEW_ASSERT(fl_h_alive(&f.ed.handles, worphan));
            resolve_kind(&f, FL_H_WIN, worphan, kindbuf, sizeof(kindbuf));
            YEW_ASSERT_EQ_STR(kindbuf, "handle");
        }
    }

    hf_close(&f);
}

static u64 handle_random(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u64 oracle_delete_pos(u64 pos, u64 at, u64 len)
{
    u64 end = at + len;

    if (pos < at)
        return pos;
    if (pos < end)
        return at;
    return pos - len;
}

/*
 * The handle stores MARKS, not the offsets handed to its creation routine.
 * Drive those marks through a thousand deterministic edits and compare
 * every resolution with a deliberately tiny offset oracle.  Boundary
 * inserts matter most: the left-biased low end stays put while the
 * right-biased high end moves, growing the span at either edge.
 */
void test_fl_handle_span_tracks_1000_random_edits(void)
{
    enum { EDITS = 1000 };
    HandleFix f;
    Buffer *b;
    EditCtx ec;
    FlValue hs;
    u64 state = UINT64_C(0xa0761d6478bd642f);
    u64 text_len = 256U;
    u64 lo = 64U;
    u64 hi = 192U;
    u8 initial[256];
    u32 i;

    hf_open(&f);
    b = yew_ed_doc(&f.ed);
    YEW_ASSERT_NOT_NULL(b);
    (void)memset(initial, 'x', sizeof(initial));
    ec = yew_ed_edit_ctx(&f.ed);
    /* The mark contract is independent of undo/cursor bookkeeping. */
    ec.undo = NULL;
    ec.cset = NULL;
    ec.on_change = NULL;
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), initial, sizeof(initial)));
    hs = fl_h_span_make(&f.ed, b, lo, hi);

    for (i = 0U; i < EDITS; i++) {
        bool insert = text_len == 0U || (handle_random(&state) & 1U) != 0U;
        u64 at;
        u64 len;
        Buffer *resolved = NULL;
        Span got = {0U, 0U};

        if (insert) {
            u8 bytes[8];

            /* Regularly force the two bias-sensitive boundary cases. */
            if (i % 17U == 0U)
                at = lo;
            else if (i % 19U == 0U)
                at = hi;
            else
                at = handle_random(&state) % (text_len + 1U);
            len = 1U + handle_random(&state) % sizeof(bytes);
            (void)memset(bytes, (int)(i & 0xffU), (size_t)len);
            YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(at), bytes, len));
            if (at < lo)
                lo += len;
            if (at <= hi)
                hi += len;
            text_len += len;
        } else {
            at = handle_random(&state) % text_len;
            len = 1U + handle_random(&state) % (text_len - at);
            if (len > 8U)
                len = 8U;
            YEW_ASSERT(yew_edit_delete(&ec, (Span){at, at + len}));
            lo = oracle_delete_pos(lo, at, len);
            hi = oracle_delete_pos(hi, at, len);
            if (lo > hi)
                lo = hi;
            text_len -= len;
        }
        clear_raise(&f.fl.vm);
        YEW_ASSERT(fl_h_span(&f.fl.vm, hs, &resolved, &got));
        YEW_ASSERT(resolved == b);
        YEW_ASSERT_EQ_U64(got.lo, lo);
        YEW_ASSERT_EQ_U64(got.hi, hi);
        YEW_ASSERT(got.hi <= text_len);
    }
    {
        const FlHandleSlot *slot = fl_h_peek(&f.ed.handles, hs);
        MarkId lo_mark;
        MarkId hi_mark;

        YEW_ASSERT_NOT_NULL(slot);
        lo_mark = slot->as.span.lo;
        hi_mark = slot->as.span.hi;
        YEW_ASSERT(fl_h_free(&f.ed, hs));
        YEW_ASSERT(!yew_mark_alive(b->marks, lo_mark));
        YEW_ASSERT(!yew_mark_alive(b->marks, hi_mark));
    }
    hf_close(&f);
}

/* Deleting all referenced text leaves a useful insertion point. */
void test_fl_handle_collapsed_span_resolves_zero_length(void)
{
    HandleFix f;
    Buffer *b;
    EditCtx ec;
    FlValue hs;
    Buffer *resolved = NULL;
    Span got = {0U, 0U};

    hf_open(&f);
    b = yew_ed_doc(&f.ed);
    YEW_ASSERT_NOT_NULL(b);
    ec = yew_ed_edit_ctx(&f.ed);
    ec.undo = NULL;
    ec.cset = NULL;
    ec.on_change = NULL;
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"0123456789",
                               10U));
    hs = fl_h_span_make(&f.ed, b, 2U, 8U);
    YEW_ASSERT(yew_edit_delete(&ec, (Span){1U, 9U}));
    YEW_ASSERT(fl_h_span(&f.fl.vm, hs, &resolved, &got));
    YEW_ASSERT(resolved == b);
    YEW_ASSERT_EQ_U64(got.lo, 1U);
    YEW_ASSERT_EQ_U64(got.hi, 1U);
    YEW_ASSERT_EQ_U64(got.hi - got.lo, 0U);
    hf_close(&f);
}

/*
 * Regex handles own the compiler arena.  Ten thousand create/free cycles
 * must reuse one table slot, clear its owning pointers, and leave no live
 * handles.  Valgrind supplies the independent heap-leak half of this test;
 * the bounded table is the deterministic structural evidence here.
 */
void test_fl_handle_regex_10000_cycles_keep_ownership_bounded(void)
{
    enum { CYCLES = 10000 };
    HandleFix f;
    u32 i;
    u32 slot = UINT32_MAX;

    hf_open(&f);
    for (i = 0U; i < CYCLES; i++) {
        Arena *own = yew_xmalloc(sizeof(*own));
        YewRe *re;
        FlValue h;
        FlHandle decoded;

        arena_init(own);
        re = yew_re_compile(own, "a(b|c)*z", 8U, 0U, NULL);
        YEW_ASSERT_NOT_NULL(re);
        h = fl_h_re_make(&f.ed, own, re);
        decoded = fl_h_decode(h);
        if (i == 0U)
            slot = decoded.slot;
        YEW_ASSERT_EQ_U64(decoded.slot, slot);
        YEW_ASSERT(fl_h_re(&f.fl.vm, h) == re);
        YEW_ASSERT_EQ_U64(f.ed.handles.live, 1U);
        YEW_ASSERT(fl_h_free(&f.ed, h));
        YEW_ASSERT_EQ_U64(f.ed.handles.live, 0U);
        YEW_ASSERT_EQ_U64(f.ed.handles.n, 1U);
        YEW_ASSERT_NULL(f.ed.handles.slots[slot].as.re.a);
        YEW_ASSERT_NULL(f.ed.handles.slots[slot].as.re.re);
    }
    hf_close(&f);
}

/*
 * A handle of the wrong kind is a "type" error, not a "handle" one.
 *
 * The distinction is the entire reason amendment A2 adds the kind:
 * `catch` has to tell "you passed the wrong argument" (the script is
 * wrong) from "the buffer closed under you" (the script is fine and the
 * world moved).  Collapsing them makes the second uncatchable in
 * practice.
 */
void test_fl_handle_wrong_kind_is_a_type_error(void)
{
    HandleFix f;
    FlValue hb;
    char kindbuf[32];

    hf_open(&f);
    hb = fl_h_buf_make(&f.ed, yew_ed_doc(&f.ed));

    resolve_kind(&f, FL_H_WIN, hb, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "type");
    resolve_kind(&f, FL_H_RE, hb, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "type");
    /* And a non-handle value is a type error too, not a crash. */
    resolve_kind(&f, FL_H_BUF, FL_NIL_V, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "type");

    hf_close(&f);
}

/*
 * DoD 4: a generation that would wrap RETIRES the slot.
 *
 * gen is bumped on free, and rolling it to 0 would make every handle
 * from the slot's first life resolve again — four billion frees is a
 * long time, and "eventually valid again" is not a property anyone can
 * reason about.  Leaking one slot is the cheaper failure.
 *
 * Walked up to the boundary rather than poked past it: the slot's gen
 * is set to one BELOW the limit and then driven over by real frees, so
 * this exercises the same arithmetic fl_h_free runs in production
 * instead of a hand-built end state.
 */
void test_fl_handle_generation_wraparound_retires_the_slot(void)
{
    HandleFix f;
    FlValue h;
    FlHandleSlot *s;
    u32 doomed;
    FlValue next;

    hf_open(&f);
    h = synthetic(&f, FL_H_BUF);
    doomed = fl_h_decode(h).slot;

    /*
     * Park the slot on the free list, then set its generation one short
     * of the roll.  Poking a FREE slot is the honest way in: the next
     * make hands back a value that genuinely encodes the boundary
     * generation, so the free below runs fl_h_free's real arithmetic
     * rather than a hand-built end state.
     */
    YEW_ASSERT(fl_h_free(&f.ed, h));
    s = &f.ed.handles.slots[doomed];
    s->gen = 0xFFFFFFFFU;

    /* Reused at the limit, then freed across it. */
    h = synthetic(&f, FL_H_BUF);
    YEW_ASSERT_EQ_U64(fl_h_decode(h).slot, doomed);
    YEW_ASSERT_EQ_U64(fl_h_decode(h).gen, 0xFFFFFFFFU);
    YEW_ASSERT(fl_h_free(&f.ed, h));
    YEW_ASSERT(!fl_h_alive(&f.ed.handles, h));

    /* Retired: the next allocation does NOT come back to this slot. */
    next = synthetic(&f, FL_H_BUF);
    YEW_ASSERT(fl_h_decode(next).slot != doomed);
    YEW_ASSERT(fl_h_alive(&f.ed.handles, next));
    YEW_ASSERT(fl_h_free(&f.ed, next));

    hf_close(&f);
}

/*
 * Closing a buffer releases every handle naming it, including spans,
 * whose marks belong to the buffer's MarkSet and must not outlive it.
 */
void test_fl_handle_drop_buffer_releases_spans(void)
{
    enum { REPEATS = 100000 };
    HandleFix f;
    Buffer *b;
    FlValue span;
    FlValue buf;
    FlValue win;
    FlValue cur;
    char kindbuf[32];
    u32 i;

    hf_open(&f);
    b = yew_ed_doc(&f.ed);
    YEW_ASSERT_NOT_NULL(b);
    span = fl_h_span_make(&f.ed, b, 0U, 0U);
    buf = fl_h_buf_make(&f.ed, b);
    win = fl_h_win_make(&f.ed, f.ed.win);
    cur = fl_h_cur_make(&f.ed, f.ed.win, 0U);
    YEW_ASSERT(fl_h_alive(&f.ed.handles, span));
    YEW_ASSERT(fl_h_alive(&f.ed.handles, buf));

    /* Stable identities and an equal live span are interned.  This is
     * the event/query hot-loop regression: repeated construction must
     * neither allocate slots nor add another pair of marks. */
    YEW_ASSERT_EQ_U64(f.ed.handles.live, 4U);
    for (i = 0U; i < REPEATS; i++) {
        YEW_ASSERT_EQ_I64(fl_h_buf_make(&f.ed, b).as.i, buf.as.i);
        YEW_ASSERT_EQ_I64(fl_h_win_make(&f.ed, f.ed.win).as.i, win.as.i);
        YEW_ASSERT_EQ_I64(fl_h_cur_make(&f.ed, f.ed.win, 0U).as.i,
                          cur.as.i);
        YEW_ASSERT_EQ_I64(fl_h_span_make(&f.ed, b, 0U, 0U).as.i,
                          span.as.i);
    }
    YEW_ASSERT_EQ_U64(f.ed.handles.live, 4U);
    YEW_ASSERT_EQ_U64(f.ed.handles.n, 4U);

    /* Cursor handles follow their stable stamp when normalization moves
     * the cursor to a different index, and never resurrect when another
     * cursor later occupies the abandoned index. */
    {
        Cursor at20 = {BYTEOFF(20U), {0U}, BYTEOFF(20U)};
        Cursor at10 = {BYTEOFF(10U), {0U}, BYTEOFF(10U)};
        FlValue moved;
        FlValue removed;
        Cursor *resolved;

        YEW_ASSERT(yew_cset_add(&f.ed.win->cs, at20));
        moved = fl_h_cur_make(&f.ed, f.ed.win, 1U);
        YEW_ASSERT(yew_cset_add(&f.ed.win->cs, at10));
        removed = fl_h_cur_make(&f.ed, f.ed.win, 1U);
        clear_raise(&f.fl.vm);
        resolved = fl_h_cur(&f.fl.vm, moved, NULL);
        YEW_ASSERT_NOT_NULL(resolved);
        YEW_ASSERT_EQ_U64(resolved->pos.v, 20U);
        YEW_ASSERT_EQ_U64(fl_h_decode(moved).slot,
                          fl_h_decode(fl_h_cur_make(&f.ed, f.ed.win, 2U)).slot);

        YEW_ASSERT(yew_cset_drop_latest(&f.ed.win->cs));
        resolve_kind(&f, FL_H_CUR, removed, kindbuf, sizeof(kindbuf));
        YEW_ASSERT_EQ_STR(kindbuf, "handle");
        YEW_ASSERT(yew_cset_add(&f.ed.win->cs, at10));
        resolve_kind(&f, FL_H_CUR, removed, kindbuf, sizeof(kindbuf));
        YEW_ASSERT_EQ_STR(kindbuf, "handle");
    }

    fl_h_drop_buffer(&f.ed, b->id);

    /* Released outright — not merely unresolvable. */
    YEW_ASSERT(!fl_h_alive(&f.ed.handles, span));
    YEW_ASSERT(!fl_h_alive(&f.ed.handles, buf));
    resolve_kind(&f, FL_H_SPAN, span, kindbuf, sizeof(kindbuf));
    YEW_ASSERT_EQ_STR(kindbuf, "handle");

    hf_close(&f);
}
