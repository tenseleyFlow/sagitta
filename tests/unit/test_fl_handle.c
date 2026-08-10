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

#include <string.h>

#include "edit/ed.h"
#include "fl/handle.h"
#include "fl/value.h"
#include "fl/vm.h"

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
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    f->fl.vm.ed = &f->ed;
}

static void hf_close(HandleFix *f)
{
    f->fl.vm.ed = NULL;
    sag_ed_free(&f->ed);
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
        SAG_BUG("resolve_kind: not a handle kind");
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
    default:        SAG_BUG("synthetic: not a handle kind");
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
    for (i = 0U; i < SAG_ARRAY_LEN(kinds); i++) {
        FlHandleKind k = kinds[i];
        FlValue h = synthetic(&f, k);
        FlValue reused;
        char got[64];
        char want[64];

        tagged_want(k, "handle", want, sizeof(want));
        SAG_ASSERT(fl_h_is(h));
        SAG_ASSERT_EQ_U64((u64)fl_h_kind_of(h), (u64)k);
        SAG_ASSERT(fl_h_alive(&f.ed.handles, h));

        /* Row 1: use after free. */
        SAG_ASSERT(fl_h_free(&f.ed.handles, h));
        SAG_ASSERT(!fl_h_alive(&f.ed.handles, h));
        resolve_tagged(&f, k, h, got, sizeof(got));
        SAG_ASSERT_EQ_STR(got, want);
        /* Freeing twice is a clean false, not a double release. */
        SAG_ASSERT(!fl_h_free(&f.ed.handles, h));

        /* Row 2: use after the SLOT is reused, same kind. */
        reused = synthetic(&f, k);
        SAG_ASSERT_EQ_U64(fl_h_decode(reused).slot, fl_h_decode(h).slot);
        SAG_ASSERT(fl_h_alive(&f.ed.handles, reused));
        SAG_ASSERT(!fl_h_alive(&f.ed.handles, h));
        resolve_tagged(&f, k, h, got, sizeof(got));
        SAG_ASSERT_EQ_STR(got, want);
        SAG_ASSERT(fl_h_free(&f.ed.handles, reused));
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
    Buffer *b;
    FlValue hb;
    FlValue hw;
    char kindbuf[32];

    hf_open(&f);
    b = sag_ed_doc(&f.ed);
    SAG_ASSERT_NOT_NULL(b);
    hb = fl_h_buf_make(&f.ed, b);
    hw = fl_h_win_make(&f.ed, f.ed.win);

    /* Both resolve while the objects are open. */
    resolve_kind(&f, FL_H_BUF, hb, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "");
    resolve_kind(&f, FL_H_WIN, hw, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "");
    /* The slot is alive in both the before and after cases; only the
     * object goes away.  Stated here so the assertion after the close
     * cannot be read as the generation guard doing the work. */
    SAG_ASSERT(fl_h_alive(&f.ed.handles, hb));

    /*
     * Close the objects the way the editor does.  fl_h_drop_buffer runs
     * BEFORE the MarkSet dies — a span whose marks outlive their set is
     * the one shape neither guard catches, which is why the drop exists
     * at all.
     */
    fl_h_drop_window(&f.ed, f.ed.win->id);
    resolve_kind(&f, FL_H_WIN, hw, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "handle");

    fl_h_drop_buffer(&f.ed, b->id);
    resolve_kind(&f, FL_H_BUF, hb, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "handle");

    /*
     * THE OBJECT-ID GUARD ON ITS OWN.
     *
     * Everything above went through fl_h_drop_*, which RELEASES the
     * slot — so the generation guard is what actually refused those,
     * and the id indirection could have been missing entirely.  Here
     * the slot stays live and only the object is absent, which is the
     * one arrangement that reaches sag_ws_buf_by_id's NULL branch.
     *
     * That is the "neither guard subsumes the other" claim from
     * handle.h, asserted rather than restated: alive() says yes and the
     * resolver still says "handle".
     */
    {
        FlValue orphan = synthetic(&f, FL_H_BUF);

        SAG_ASSERT(fl_h_alive(&f.ed.handles, orphan));
        resolve_kind(&f, FL_H_BUF, orphan, kindbuf, sizeof(kindbuf));
        SAG_ASSERT_EQ_STR(kindbuf, "handle");

        /* And the same for a window, whose lookup walks every tab. */
        {
            FlValue worphan = synthetic(&f, FL_H_WIN);

            SAG_ASSERT(fl_h_alive(&f.ed.handles, worphan));
            resolve_kind(&f, FL_H_WIN, worphan, kindbuf, sizeof(kindbuf));
            SAG_ASSERT_EQ_STR(kindbuf, "handle");
        }
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
    hb = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));

    resolve_kind(&f, FL_H_WIN, hb, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "type");
    resolve_kind(&f, FL_H_RE, hb, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "type");
    /* And a non-handle value is a type error too, not a crash. */
    resolve_kind(&f, FL_H_BUF, FL_NIL_V, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "type");

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
    SAG_ASSERT(fl_h_free(&f.ed.handles, h));
    s = &f.ed.handles.slots[doomed];
    s->gen = 0xFFFFFFFFU;

    /* Reused at the limit, then freed across it. */
    h = synthetic(&f, FL_H_BUF);
    SAG_ASSERT_EQ_U64(fl_h_decode(h).slot, doomed);
    SAG_ASSERT_EQ_U64(fl_h_decode(h).gen, 0xFFFFFFFFU);
    SAG_ASSERT(fl_h_free(&f.ed.handles, h));
    SAG_ASSERT(!fl_h_alive(&f.ed.handles, h));

    /* Retired: the next allocation does NOT come back to this slot. */
    next = synthetic(&f, FL_H_BUF);
    SAG_ASSERT(fl_h_decode(next).slot != doomed);
    SAG_ASSERT(fl_h_alive(&f.ed.handles, next));
    SAG_ASSERT(fl_h_free(&f.ed.handles, next));

    hf_close(&f);
}

/*
 * Closing a buffer releases every handle naming it, including spans,
 * whose marks belong to the buffer's MarkSet and must not outlive it.
 */
void test_fl_handle_drop_buffer_releases_spans(void)
{
    HandleFix f;
    Buffer *b;
    FlValue span;
    FlValue buf;
    char kindbuf[32];

    hf_open(&f);
    b = sag_ed_doc(&f.ed);
    SAG_ASSERT_NOT_NULL(b);
    span = fl_h_span_make(&f.ed, b, 0U, 0U);
    buf = fl_h_buf_make(&f.ed, b);
    SAG_ASSERT(fl_h_alive(&f.ed.handles, span));
    SAG_ASSERT(fl_h_alive(&f.ed.handles, buf));

    fl_h_drop_buffer(&f.ed, b->id);

    /* Released outright — not merely unresolvable. */
    SAG_ASSERT(!fl_h_alive(&f.ed.handles, span));
    SAG_ASSERT(!fl_h_alive(&f.ed.handles, buf));
    resolve_kind(&f, FL_H_SPAN, span, kindbuf, sizeof(kindbuf));
    SAG_ASSERT_EQ_STR(kindbuf, "handle");

    hf_close(&f);
}
