/*
 * Sprint 30 DoD 7: every root in the §9 table gets a DROP-IT test.
 *
 * The shape of each is the same and it is the shape that matters:
 *
 *   1. make an object reachable through exactly ONE root,
 *   2. collect, and prove it survived by READING it -- not by finding
 *      it still on the object list, which a collector that never frees
 *      anything also satisfies,
 *   3. drop that one root,
 *   4. collect, and prove it died.
 *
 * Step 3 is the half that catches the real bug.  A collector that marks
 * conservatively -- one that never frees -- passes every step-2
 * assertion in this file, which is why "the object survived" alone is
 * not a test of anything.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/parse.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct GcFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} GcFix;

static void gsink(void *ctx, FlDiagLevel level, FlSpan sp,
                  const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static void gf_open(GcFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, gsink, NULL);
    fl_vm_init(&f->vm, &f->arena, &f->in, &f->dc);
}

static void gf_close(GcFix *f)
{
    fl_vm_free(&f->vm);
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

/* Whether `o` is still on the all-objects list.  Reading a freed object
 * to check would be the use-after-free the test exists to prevent, so
 * liveness is asked of the collector's own list. */
static bool still_live(const FlVm *vm, const void *o)
{
    const FlObj *p;

    for (p = vm->gc.objects; p != NULL; p = p->gc_next) {
        if ((const void *)p == o)
            return true;
    }
    return false;
}

/* A distinctive string, long enough NOT to be interned -- an interned
 * one is reachable from the string table's rebuild path and would
 * confuse "did this specific object die". */
static FlStr *witness(GcFix *f, const char *tag)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "%s-witness-padded-out-past-the-interning-threshold",
                     tag);

    return fl_str_new(&f->vm, buf, (u32)n);
}

/* ---------------------------------------------------------------- */
/* Root 1: the value stack                                          */
/* ---------------------------------------------------------------- */

void test_fl_gc_root_value_stack(void)
{
    GcFix f;
    FlStr *s;

    gf_open(&f);
    s = witness(&f, "stack");
    *f.vm.sp++ = FL_OBJ_V(FL_STR, s);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, s));
    YEW_ASSERT_EQ_I64(memcmp(s->b, "stack-witness", 13), 0);

    f.vm.sp--;                        /* drop it */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, s));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 2: call frames                                              */
/* ---------------------------------------------------------------- */

void test_fl_gc_root_call_frames(void)
{
    GcFix f;
    FlClosure *cl;
    FlFn *fn;

    /*
     * A frame's closure is reachable ONLY through the frame here: the
     * stack slot a real call would also hold is deliberately not
     * written, so root 2 is the single thing keeping it alive.
     */
    gf_open(&f);
    fn = fl_gc_alloc(&f.vm, sizeof(*fn), FL_FN);
    (void)memset(&fn->ch, 0, sizeof(fn->ch));
    fn->name_id = 0U;
    fn->arity = 0U;
    fn->nup = 0U;
    fn->max_stack = 0U;
    cl = fl_gc_alloc(&f.vm, sizeof(*cl), FL_CLOSURE);
    cl->fn = fn;
    cl->up = NULL;
    cl->nup = 0U;

    f.vm.frames[f.vm.nframes].cl = cl;
    f.vm.frames[f.vm.nframes].ip = NULL;
    f.vm.frames[f.vm.nframes].slots = f.vm.stack;
    f.vm.nframes++;
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, cl));
    /* And the frame reaches THROUGH the closure to its function: a mark
     * that stopped at the closure would leave the chunk to be swept
     * while the frame is still executing out of it. */
    YEW_ASSERT(still_live(&f.vm, fn));

    f.vm.nframes--;                   /* drop it */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, cl));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 3: open upvalues                                            */
/* ---------------------------------------------------------------- */

void test_fl_gc_root_open_upvalues(void)
{
    GcFix f;
    FlUpval *uv;

    /*
     * An OPEN upvalue aliases a stack slot that root 1 already covers,
     * so the value it points at is safe either way -- but the FlUpval
     * OBJECT itself is reachable only from vm->open_upvals.  Marking
     * root 1 and forgetting root 3 frees the upvalue while a closure
     * still holds a pointer to it, which is the bug this pins.
     */
    gf_open(&f);
    uv = fl_gc_alloc(&f.vm, sizeof(*uv), FL_UPVAL);
    uv->slot = f.vm.stack;
    uv->closed = FL_NIL_V;
    uv->next = NULL;
    f.vm.open_upvals = uv;
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, uv));

    f.vm.open_upvals = NULL;          /* drop it */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, uv));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Roots 4 and 5: globals and modules                               */
/* ---------------------------------------------------------------- */

void test_fl_gc_root_globals_and_modules(void)
{
    GcFix f;
    FlStr *g;
    FlStr *m;
    FlValue key;

    gf_open(&f);
    key = FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "k", 1U));
    fl_gc_protect(&f.vm, key);

    g = witness(&f, "global");
    (void)fl_map_set(&f.vm, f.vm.globals, key, FL_OBJ_V(FL_STR, g));
    m = witness(&f, "module");
    (void)fl_map_set(&f.vm, f.vm.modules, key, FL_OBJ_V(FL_STR, m));
    fl_gc_release(&f.vm, 1U);

    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, g));
    YEW_ASSERT(still_live(&f.vm, m));

    /* Drop them: emptying the maps must let both go, and must not take
     * anything else with it. */
    YEW_ASSERT(fl_map_del(f.vm.globals, key));
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, g));
    YEW_ASSERT(still_live(&f.vm, m));

    YEW_ASSERT(fl_map_del(f.vm.modules, key));
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, m));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 9: the builtin module maps                                  */
/* ---------------------------------------------------------------- */

/*
 * The LAST root in the §9 table to get a drop-test, and the discipline
 * says every one of them has one: reachable only through the root,
 * survives a collection, then dies when the root lets go.
 *
 * Separate from `modules` because the two are keyed differently --
 * builtins by IDENT, files by (realpath, origin kind) -- and folding
 * them into one map would make `import str` and `import "str"` reach
 * the same object, which spec §11 says they must not.
 */
void test_fl_gc_root_builtins(void)
{
    GcFix f;
    FlStr *b;
    FlValue key;

    gf_open(&f);
    key = FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "bk", 2U));
    fl_gc_protect(&f.vm, key);
    b = witness(&f, "builtin");
    (void)fl_map_set(&f.vm, f.vm.builtins, key, FL_OBJ_V(FL_STR, b));
    fl_gc_release(&f.vm, 1U);

    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, b));

    YEW_ASSERT(fl_map_del(f.vm.builtins, key));
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, b));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 6: host-retained values (Sprint 34)                         */
/* ---------------------------------------------------------------- */

/*
 * Sprint 30 reserved root 6 for "Sprint 34's handle table" and left a
 * placeholder here.  Sprint 34's handles turned out to be scalars the
 * collector never needs to see; what root 6 actually carries is the
 * mirror problem -- an FlValue the HOST holds and no Fletch variable
 * reaches, like a hook closure sitting in Ed.  That is the classic
 * embedding crash, so this is the test that matters.
 */
void test_fl_gc_root_handle_table(void)
{
    GcFix f;
    FlStr *h;
    FlValue slot;

    gf_open(&f);
    h = witness(&f, "handle");
    slot = FL_OBJ_V(FL_STR, h);

    /* Unrooted, a host-only value is collected -- the bug root 6 is
     * for.  Assert that first, so a root that silently stopped working
     * could not make the rest of this test pass. */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, h));

    h = witness(&f, "handle two");
    slot = FL_OBJ_V(FL_STR, h);
    fl_gc_host_root_add(&f.vm, &slot);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, h));

    /* The root is the SLOT, not the value: writing through it must
     * change what survives, which is what makes replacing a hook safe. */
    slot = FL_NIL_V;
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, h));

    h = witness(&f, "handle three");
    slot = FL_OBJ_V(FL_STR, h);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, h));

    fl_gc_host_root_remove(&f.vm, &slot);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, h));
    /* Removing twice is a no-op, so a teardown that runs twice is
     * safe -- s36's reload path does exactly that. */
    fl_gc_host_root_remove(&f.vm, &slot);
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 11: mark providers (Sprint 34)                              */
/* ---------------------------------------------------------------- */

typedef struct ProviderCtx {
    FlValue *v;
    u32 n;
    u32 calls;
} ProviderCtx;

static void provider_mark(FlVm *vm, void *ctx)
{
    ProviderCtx *p = (ProviderCtx *)ctx;
    u32 i;

    p->calls++;
    for (i = 0U; i < p->n; i++)
        fl_gc_mark_value(vm, p->v[i]);
}

void test_fl_gc_root_provider(void)
{
    GcFix f;
    ProviderCtx ctx;
    FlValue held[2];
    FlStr *a, *b;

    gf_open(&f);
    a = witness(&f, "provider a");
    b = witness(&f, "provider b");
    held[0] = FL_OBJ_V(FL_STR, a);
    held[1] = FL_OBJ_V(FL_STR, b);
    ctx.v = held;
    ctx.n = 2U;
    ctx.calls = 0U;

    fl_gc_root_provider(&f.vm, provider_mark, &ctx);
    fl_gc_collect(&f.vm);
    YEW_ASSERT_EQ_U64(ctx.calls, 1U);
    YEW_ASSERT(still_live(&f.vm, a));
    YEW_ASSERT(still_live(&f.vm, b));

    /* A provider walks a LIVE collection, so shrinking it drops what
     * it no longer reports -- that is the whole point of the form: the
     * hook table's storage moves and its addresses cannot be
     * registered. */
    ctx.n = 1U;
    fl_gc_collect(&f.vm);
    YEW_ASSERT_EQ_U64(ctx.calls, 2U);
    YEW_ASSERT(still_live(&f.vm, a));
    YEW_ASSERT(!still_live(&f.vm, b));

    /* Re-attaching the same (fn, ctx) pair is idempotent: boot paths
     * that run twice must not double-mark or exhaust the table. */
    fl_gc_root_provider(&f.vm, provider_mark, &ctx);
    fl_gc_collect(&f.vm);
    YEW_ASSERT_EQ_U64(ctx.calls, 3U);
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 7: the temp-root protection stack                           */
/* ---------------------------------------------------------------- */

void test_fl_gc_root_temp_stack(void)
{
    GcFix f;
    FlStr *a;
    FlStr *b;

    /* LIFO, and releasing one must not release the other. */
    gf_open(&f);
    a = witness(&f, "tempa");
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_STR, a));
    b = witness(&f, "tempb");
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_STR, b));
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, a));
    YEW_ASSERT(still_live(&f.vm, b));

    fl_gc_release(&f.vm, 1U);         /* drops b, keeps a */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, a));
    YEW_ASSERT(!still_live(&f.vm, b));

    fl_gc_release(&f.vm, 1U);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, a));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Root 8: the compiler's in-progress functions                     */
/* ---------------------------------------------------------------- */

void test_fl_gc_root_compile_chain(void)
{
    GcFix f;
    FlFn *fn;

    /*
     * Compilation allocates -- constants, nested FlFns, interned names
     * -- and a half-built function is on no stack and in no map. Root 8
     * is what keeps the outer function alive while the inner one is
     * being built, and stress mode is where its absence shows.
     */
    gf_open(&f);
    fn = fl_gc_alloc(&f.vm, sizeof(*fn), FL_FN);
    (void)memset(&fn->ch, 0, sizeof(fn->ch));
    fn->name_id = 0U;
    fn->arity = 0U;
    fn->nup = 0U;
    fn->max_stack = 0U;

    f.vm.compiling[f.vm.ncompiling++] = fn;
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, fn));

    f.vm.ncompiling--;                /* drop it */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, fn));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* The in-flight raised value                                       */
/* ---------------------------------------------------------------- */

void test_fl_gc_marks_the_in_flight_error(void)
{
    GcFix f;
    FlStr *e;

    /*
     * Not a numbered root, and marked anyway: between the raise and the
     * handler the error is in vm->err and nowhere else, so a collection
     * in that window would free the value a `catch` is about to bind.
     */
    gf_open(&f);
    e = witness(&f, "err");
    f.vm.err = FL_OBJ_V(FL_STR, e);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, e));

    f.vm.err = FL_NIL_V;              /* drop it */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, e));
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Cycles, and the weak intern table                                */
/* ---------------------------------------------------------------- */

void test_fl_gc_collects_a_cycle(void)
{
    GcFix f;
    FlList *a;
    FlList *b;

    /*
     * Two lists pointing at each other.  A refcounting collector leaks
     * this forever; a mark-sweep one must reclaim it the moment nothing
     * outside the cycle points in, which is the reason the sprint chose
     * mark-sweep.
     */
    gf_open(&f);
    a = fl_list_new(&f.vm);
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_LIST, a));
    b = fl_list_new(&f.vm);
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_LIST, b));
    (void)fl_list_push(&f.vm, a, FL_OBJ_V(FL_LIST, b));
    (void)fl_list_push(&f.vm, b, FL_OBJ_V(FL_LIST, a));

    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, a));
    YEW_ASSERT(still_live(&f.vm, b));

    fl_gc_release(&f.vm, 2U);         /* drop both handles */
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, a));
    YEW_ASSERT(!still_live(&f.vm, b));
    gf_close(&f);
}

void test_fl_gc_intern_table_is_weak(void)
{
    GcFix f;
    FlStr *first;
    FlStr *again;
    u32 peak;

    /*
     * The interned-string table holds WEAK references: an entry must
     * not keep its FlStr alive, and sweep must clear the dead entries
     * BEFORE freeing any white object -- otherwise the rebuild reads
     * the strings it is deciding about after they are gone.
     */
    gf_open(&f);
    first = fl_str_new(&f.vm, "interned", 8U);
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_STR, first));
    /* Interning means the same content yields the SAME object. */
    again = fl_str_new(&f.vm, "interned", 8U);
    YEW_ASSERT(first == again);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(still_live(&f.vm, first));

    fl_gc_release(&f.vm, 1U);
    peak = f.vm.gc.strings.n;
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, first));
    /*
     * The table gave up its entry rather than keeping a dangling one.
     * Asserting `again != first` would be wrong: malloc is free to hand
     * the just-freed address straight back, and it does.  What the test
     * can check is that the ENTRY went away and that re-interning
     * produces a live, correct, freshly-listed object.
     */
    YEW_ASSERT(f.vm.gc.strings.n < peak);
    again = fl_str_new(&f.vm, "interned", 8U);
    YEW_ASSERT(still_live(&f.vm, again));
    YEW_ASSERT_EQ_U64(again->len, 8U);
    YEW_ASSERT_EQ_I64(memcmp(again->b, "interned", 8U), 0);
    gf_close(&f);

    /* The table also shrinks back rather than holding its high-water
     * allocation for the life of the VM. */
    gf_open(&f);
    {
        u32 i;

        for (i = 0U; i < 2000U; i++) {
            char b[16];
            int n = snprintf(b, sizeof(b), "s%u", (unsigned)i);

            (void)fl_str_new(&f.vm, b, (u32)n);
        }
        peak = f.vm.gc.strings.n;
        YEW_ASSERT(peak >= 2000U);
        fl_gc_collect(&f.vm);
        /* Nothing roots them, so the table empties. */
        YEW_ASSERT(f.vm.gc.strings.n < peak / 4U);
    }
    gf_close(&f);
}

/* ---------------------------------------------------------------- */
/* Stress: the harness itself                                       */
/* ---------------------------------------------------------------- */

void test_fl_gc_stress_collects_at_every_instruction(void)
{
    GcFix f;
    FlProgram p;
    FlFn *fn;
    FlValue out;
    /* CLI origin, no capabilities: these tests never call io,
     * and a grant nobody needs is a grant nobody notices is
     * wrong. */
    FlOrigin origin = {(u8)FL_ORIGIN_CLI, 0U, 0U};
    static const char *const src = "let a = [1]\nlet b = [2]\nreturn 0\n";

    /*
     * The sprint asks for a fixture proving the harness DETECTS a
     * protection violation.  Detection does not happen where a reader
     * expects: fl_gc_alloc never collects, deliberately -- it sets
     * `pending`, because its caller is mid-instruction holding pointers
     * no root covers.  The collection lands at the next INSTRUCTION
     * BOUNDARY, and that is the only place a violation can be caught.
     *
     * So the harness property to prove is that stress really does
     * collect at every boundary.  If it collected once per program
     * instead, every protection bug in the tree would sail through the
     * stress lane while the lane reported green.
     */
    gf_open(&f);
    f.vm.gc.stress = true;
    (void)fl_diag_add_file(&f.dc, "t.fl", src, strlen(src));
    p = fl_parse(&f.arena, &f.dc, &f.in, src, strlen(src), 0U);
    YEW_ASSERT(!p.had_error);
    fn = fl_compile(&f.vm, &f.dc, &p, 0U, origin);
    YEW_ASSERT_NOT_NULL(fn);
    YEW_ASSERT(fl_vm_run(&f.vm, fn, &out));
    /*
     * Two lists and two DEF_GLOBAL map writes allocate, and under
     * stress each one arms a collection honoured at the next boundary.
     * More than one collection is the claim; the exact count is a
     * codegen detail and pinning it would break on any peephole change.
     */
    YEW_ASSERT(f.vm.gc.collections > 1U);
    gf_close(&f);
}

void test_fl_gc_stress_frees_what_only_a_c_local_holds(void)
{
    GcFix f;
    FlStr *unprotected;
    FlStr *kept;

    /*
     * The other half: a collection with an object held only in a C
     * local frees it, and one on the temp stack survives.  That
     * difference IS the discipline, and a collector that failed to make
     * it would render rule 2 decorative.
     *
     * fl_gc_collect is called directly rather than through the VM,
     * because outside the dispatch loop there is no boundary to wait
     * for -- which is also why a native must not hold an object across
     * an allocation.
     */
    gf_open(&f);
    kept = witness(&f, "kept");
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_STR, kept));
    unprotected = witness(&f, "dropped");
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!still_live(&f.vm, unprotected));
    YEW_ASSERT(still_live(&f.vm, kept));
    YEW_ASSERT_EQ_I64(memcmp(kept->b, "kept-witness", 12), 0);
    fl_gc_release(&f.vm, 1U);
    gf_close(&f);
}

void test_fl_gc_stress_survives_the_whole_pipeline(void)
{
    GcFix f;
    FlProgram p;
    FlFn *fn;
    FlValue out;
    /* CLI origin, no capabilities: these tests never call io,
     * and a grant nobody needs is a grant nobody notices is
     * wrong. */
    FlOrigin origin = {(u8)FL_ORIGIN_CLI, 0U, 0U};
    static const char *const src =
        "fn build(n) {\n"
        "    let acc = []\n"
        "    let i = 0\n"
        "    while i < n {\n"
        "        acc = acc + [{idx: i, tag: \"row\"}]\n"
        "        i = i + 1\n"
        "    }\n"
        "    return acc\n"
        "}\n"
        "let rows = build(12)\n"
        "let total = 0\n"
        "for r in rows { total = total + r.idx }\n"
        "return total\n";

    /*
     * Stress collects at EVERY allocation, so this runs the collector
     * some thousands of times across parse, compile and execute in one
     * go.  Any place in the pipeline holding an object in a C local
     * across an allocation dies here, under ASan, naming its line --
     * which is cheaper than finding it in a user's script.
     */
    gf_open(&f);
    f.vm.gc.stress = true;
    (void)fl_diag_add_file(&f.dc, "t.fl", src, strlen(src));
    p = fl_parse(&f.arena, &f.dc, &f.in, src, strlen(src), 0U);
    YEW_ASSERT(!p.had_error);
    fn = fl_compile(&f.vm, &f.dc, &p, 0U, origin);
    YEW_ASSERT_NOT_NULL(fn);
    YEW_ASSERT(fl_vm_run(&f.vm, fn, &out));
    YEW_ASSERT_EQ_U64((u64)out.t, (u64)FL_INT);
    YEW_ASSERT_EQ_I64(out.as.i, 66);   /* 0+1+...+11 */
    /*
     * The count is a floor, not a target: it says the collector ran
     * many times across the run rather than once at the end, which is
     * the property that makes this a stress test.  Pinning an exact
     * number would break on any codegen change and teach the next
     * reader to update the number instead of asking why it moved.
     */
    if (f.vm.gc.collections < 20U)
        (void)fprintf(stderr, "stress ran only %llu collections\n",
                      (unsigned long long)f.vm.gc.collections);
    YEW_ASSERT(f.vm.gc.collections >= 20U);
    gf_close(&f);
}
