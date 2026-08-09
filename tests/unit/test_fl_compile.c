/*
 * Sprint 30 DoD 3 and 10: the compiler, and the opcode coverage the
 * sprint says must have ZERO GAPS.
 *
 * A hand-written "opcode -> test" comment table is a lie the day
 * someone adds opcode 61, so the table below is generated FROM the
 * corpus by the test itself: `covering_program` names, for each
 * opcode, the source that emits it, and
 * test_fl_compile_every_opcode_is_covered walks every compiled chunk
 * (including nested functions) and fails naming any opcode no program
 * in the corpus reaches.  Adding an opcode without adding a program
 * breaks the build's test lane, which is the point.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct CFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    u32 ndiag;
    char first[256];
} CFix;

static void ccapture(void *ctx, FlDiagLevel level, FlSpan sp,
                     const char *msg, const char *rendered)
{
    CFix *f = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    if (f->ndiag == 0U)
        (void)snprintf(f->first, sizeof(f->first), "%s", msg);
    f->ndiag++;
}

static void cf_open(CFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, ccapture, f);
    fl_vm_init(&f->vm, &f->arena, &f->in, &f->dc);
}

static void cf_close(CFix *f)
{
    fl_vm_free(&f->vm);
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

static FlFn *cf_compile(CFix *f, const char *src)
{
    FlProgram p;
    /* CLI origin, no capabilities: these tests never call io,
     * and a grant nobody needs is a grant nobody notices is
     * wrong. */
    FlOrigin origin = {(u8)FL_ORIGIN_CLI, 0U, 0U};

    (void)fl_diag_add_file(&f->dc, "t.fl", src, strlen(src));
    p = fl_parse(&f->arena, &f->dc, &f->in, src, strlen(src), 0U);
    if (p.had_error)
        return NULL;
    return fl_compile(&f->vm, &f->dc, &p, 0U, origin);
}

/* ---------------------------------------------------------------- */
/* Opcode coverage: the table, and the walk that proves it complete  */
/* ---------------------------------------------------------------- */

/*
 * One row per opcode, in opcodes.def order.  A row's program need not
 * be minimal -- it needs to be a program a reader can look at and see
 * why that opcode appears.
 */
typedef struct OpProgram {
    FlOp op;
    const char *src;
} OpProgram;

static const OpProgram OP_CORPUS[] = {
    {FL_OP_CONST,          "return 100000\n"},
    {FL_OP_NIL,            "let x = nil\nreturn x\n"},
    {FL_OP_TRUE,           "return true\n"},
    {FL_OP_FALSE,          "return false\n"},
    {FL_OP_INT8,           "return 7\n"},
    {FL_OP_NIL_N,          "fn f() { let a\nlet b\nlet c\n"
                           "return 0 }\nreturn f()\n"},
    {FL_OP_POP,            "1 + 1\nreturn 0\n"},
    {FL_OP_POPN,           "fn f() { if true { let a = 1\nlet b = 2 }\n"
                           "return 0 }\nreturn f()\n"},
    {FL_OP_DUP,            NULL},
    {FL_OP_GET_LOCAL,      "fn f() { let a = 1\nreturn a }\nreturn f()\n"},
    {FL_OP_SET_LOCAL,      "fn f() { let a = 1\na = 2\nreturn a }\n"
                           "return f()\n"},
    {FL_OP_GET_UPVAL,      "fn f() { let a = 1\nreturn fn() a }\n"
                           "return 0\n"},
    {FL_OP_SET_UPVAL,      "fn f() { let a = 1\n"
                           "return fn() { a = 2\nreturn a } }\nreturn 0\n"},
    {FL_OP_CLOSE_UPVALS,   "fn f() { let a = 1\nreturn fn() a }\n"
                           "return 0\n"},
    {FL_OP_GET_GLOBAL,     "let g = 1\nreturn g\n"},
    {FL_OP_SET_GLOBAL,     "let g = 1\ng = 2\nreturn g\n"},
    {FL_OP_DEF_GLOBAL,     "let g = 1\nreturn g\n"},
    {FL_OP_ADD,            "return 1 + 2\n"},
    {FL_OP_SUB,            "return 3 - 1\n"},
    {FL_OP_MUL,            "return 3 * 2\n"},
    {FL_OP_DIV,            "return 6 / 2\n"},
    {FL_OP_MOD,            "return 7 % 3\n"},
    {FL_OP_NEG,            "let a = 1\nreturn -a\n"},
    {FL_OP_NOT,            "return not false\n"},
    {FL_OP_EQ,             "return 1 == 1\n"},
    {FL_OP_NE,             "return 1 != 2\n"},
    {FL_OP_LT,             "return 1 < 2\n"},
    {FL_OP_LE,             "return 1 <= 2\n"},
    {FL_OP_GT,             "return 2 > 1\n"},
    {FL_OP_GE,             "return 2 >= 1\n"},
    {FL_OP_JUMP,           "if true { return 1 } else { return 2 }\n"},
    {FL_OP_JUMP_BACK,      "let n = 0\nwhile n < 3 { n = n + 1 }\n"
                           "return n\n"},
    {FL_OP_JUMP_IF_FALSE,  "if true { return 1 }\nreturn 0\n"},
    {FL_OP_JUMP_IF_TRUE,   NULL},
    {FL_OP_OR_JUMP,        "return nil or 1\n"},
    {FL_OP_AND_JUMP,       "return 1 and 2\n"},
    {FL_OP_CALL,           "fn f() { return 1 }\nreturn f()\n"},
    {FL_OP_RETURN,         "fn f() { return 1 }\nreturn f()\n"},
    {FL_OP_RETURN_NIL,     "fn f() { let a = 1 }\nreturn 0\n"},
    {FL_OP_CLOSURE,        "fn f() { return 1 }\nreturn f()\n"},
    {FL_OP_LIST,           "return [1, 2][0]\n"},
    {FL_OP_MAP,            "let m = {a: 1}\nreturn m.a\n"},
    {FL_OP_INDEX_GET,      "return [1, 2][0]\n"},
    {FL_OP_INDEX_SET,      "let l = [1]\nl[0] = 2\nreturn l[0]\n"},
    {FL_OP_FIELD_GET,      "let m = {a: 1}\nreturn m.a\n"},
    {FL_OP_FIELD_SET,      "let m = {a: 1}\nm.a = 2\nreturn m.a\n"},
    {FL_OP_ITER_NEW,       "let t = 0\nfor x in [1] { t = x }\nreturn t\n"},
    {FL_OP_ITER_NEXT1,     "let t = 0\nfor x in [1] { t = x }\nreturn t\n"},
    {FL_OP_ITER_NEXT2,     "let t = 0\nlet m = {a: 1}\n"
                           "for k, v in m { t = v }\nreturn t\n"},
    {FL_OP_MOTION,         "edit { @[ 2v ] }\nreturn 0\n"},
    {FL_OP_IMPORT,         "import io\nreturn 0\n"},
    {FL_OP_EDIT_BEGIN,     "edit { @[ 2v ] }\nreturn 0\n"},
    {FL_OP_EDIT_END,       "edit { @[ 2v ] }\nreturn 0\n"},
    {FL_OP_THROW,          NULL},
    {FL_OP_TRY_PUSH,       "try { return 1 } catch e { return 2 }\n"},
    {FL_OP_TRY_POP,        "try { return 1 } catch e { return 2 }\n"},
    {FL_OP_HALT,           "return 0\n"},
    {FL_OP_LIST_APPEND,    NULL},
    {FL_OP_NOT_NIL,        NULL},
    {FL_OP_TRACE_LINE,     NULL}
};

/*
 * Six opcodes have no source construct that emits them THIS sprint --
 * by design, and each row below says by whose design.  They are still
 * implemented, disassembled and executed, so they are covered by a
 * hand-assembled chunk that the VM runs and the test checks the result
 * of.  What this does NOT claim is that the compiler emits them; the
 * `why` string is the honest half of DoD 3's "be honest now", and the
 * test refuses a row with an empty one.
 */
typedef struct OpSynth {
    FlOp op;
    const char *why;
    u8 code[24];
    u32 ncode;
    i64 want;        /* expected FL_INT result, or the raise flag below */
    bool want_raise;
} OpSynth;

static const OpSynth OP_SYNTH[] = {
    {FL_OP_DUP, "no source construct duplicates a value; s31's "
                "compound helpers are its first user",
     {FL_OP_INT8, 7U, FL_OP_DUP, FL_OP_ADD, FL_OP_RETURN}, 5U, 14, false},

    {FL_OP_JUMP_IF_TRUE, "the compiler inverts conditions with "
                         "JUMP_IF_FALSE; s33's peephole pass picks the "
                         "cheaper polarity",
     {FL_OP_TRUE,
      FL_OP_JUMP_IF_TRUE, 3U, 0U,     /* pc 1, lands on pc 7 */
      FL_OP_INT8, 1U, FL_OP_RETURN,   /* pc 4: skipped */
      FL_OP_INT8, 2U, FL_OP_RETURN},  /* pc 7 */
     10U, 2, false},

    {FL_OP_THROW, "the language has no `throw` keyword (§2); s31's "
                  "error() builtin raises TOS",
     {FL_OP_INT8, 9U, FL_OP_THROW}, 3U, 9, true},

    {FL_OP_LIST_APPEND, "list literals under 65535 items use LIST; the "
                        "opcode is for larger ones and s31's helpers",
     {FL_OP_LIST, 0U, 0U,             /* slot 1 := []                   */
      FL_OP_INT8, 5U,
      FL_OP_LIST_APPEND, 1U,
      FL_OP_GET_LOCAL, 1U,
      FL_OP_INT8, 0U,
      FL_OP_INDEX_GET,
      FL_OP_RETURN}, 13U, 5, false},

    {FL_OP_NOT_NIL, "the nil-coalescing fast path is for s31's "
                    "generated code, not for source `or`",
     {FL_OP_NIL, FL_OP_NOT_NIL,
      FL_OP_JUMP_IF_FALSE, 3U, 0U,
      FL_OP_INT8, 1U, FL_OP_RETURN,
      FL_OP_INT8, 0U, FL_OP_RETURN}, 11U, 0, false},

    {FL_OP_TRACE_LINE, "emitted only in FL_VM_CHECKS builds and never "
                       "in release, so no ordinary compile reaches it",
     {FL_OP_TRACE_LINE, 4U, 0U, FL_OP_INT8, 3U, FL_OP_RETURN}, 6U, 3,
     false}
};

/* Builds a runnable zero-arity FlFn around a literal byte sequence. */
static FlFn *synth_fn(CFix *f, const OpSynth *s)
{
    FlFn *fn = fl_gc_alloc(&f->vm, sizeof(*fn), FL_FN);
    u8 *code = arena_alloc(&f->arena, s->ncode, 1U);
    FlLineRun *lines = arena_alloc(&f->arena, sizeof(*lines),
                                   _Alignof(FlLineRun));

    (void)memcpy(code, s->code, s->ncode);
    lines[0].pc = 0U;
    lines[0].line = 1U;
    lines[0].col = 1U;
    fn->ch.code = code;
    fn->ch.ncode = s->ncode;
    fn->ch.consts = NULL;
    fn->ch.nconsts = 0U;
    fn->ch.lines = lines;
    fn->ch.nlines = 1U;
    fn->ch.file_id = 0U;
    fn->name_id = 0U;
    fn->arity = 0U;
    fn->nup = 0U;
    /* Room for slot 0 (the callee), the LIST_APPEND row's list in slot
     * 1, and the deepest expression any row builds. */
    fn->max_stack = 8U;
    return fn;
}

void test_fl_compile_synthetic_opcodes_execute(void)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(OP_SYNTH); i++) {
        CFix f;
        FlFn *fn;
        FlValue out;
        bool ok;

        /* An empty `why` is the whole point of the field. */
        SAG_ASSERT_NOT_NULL(OP_SYNTH[i].why);
        SAG_ASSERT(OP_SYNTH[i].why[0] != '\0');

        cf_open(&f);
        fn = synth_fn(&f, &OP_SYNTH[i]);
        /* A miscounted ncode truncates the last instruction and the VM
         * runs into whatever follows -- checked here rather than
         * debugged from the segfault it otherwise produces. */
        {
            u32 pc = 0U;

            while (pc < fn->ch.ncode)
                pc += fl_op_length(&fn->ch, pc);
            if (pc != fn->ch.ncode)
                (void)fprintf(stderr, "synth %s: ncode %u, walk ends %u\n",
                              fl_op_name(OP_SYNTH[i].op),
                              (unsigned)fn->ch.ncode, (unsigned)pc);
            SAG_ASSERT_EQ_U64(pc, fn->ch.ncode);
        }
        ok = fl_vm_run(&f.vm, fn, &out);
        if (ok == OP_SYNTH[i].want_raise)
            (void)fprintf(stderr, "synth %s: raise=%d, wanted raise=%d\n",
                          fl_op_name(OP_SYNTH[i].op), (int)!ok,
                          (int)OP_SYNTH[i].want_raise);
        SAG_ASSERT_EQ_I64((i64)ok, (i64)!OP_SYNTH[i].want_raise);
        SAG_ASSERT_EQ_U64((u64)out.t, (u64)FL_INT);
        if (out.as.i != OP_SYNTH[i].want)
            (void)fprintf(stderr, "synth %s: got %lld, want %lld\n",
                          fl_op_name(OP_SYNTH[i].op), (long long)out.as.i,
                          (long long)OP_SYNTH[i].want);
        SAG_ASSERT_EQ_I64(out.as.i, OP_SYNTH[i].want);
        cf_close(&f);
    }
}

void test_fl_compile_synthetic_opcodes_disassemble(void)
{
    size_t i;

    /* DoD 3 asks for "implemented, disassembled, and reached".  This is
     * the disassembled third: every synthetic chunk round-trips through
     * fl_disasm_chunk naming its opcode, with no BAD_OP. */
    for (i = 0U; i < SAG_ARRAY_LEN(OP_SYNTH); i++) {
        CFix f;
        FlFn *fn;
        Bytebuf bb;

        cf_open(&f);
        fn = synth_fn(&f, &OP_SYNTH[i]);
        bytebuf_init(&bb);
        fl_disasm_chunk(&bb, &fn->ch, &f.in);
        bytebuf_append(&bb, "", 1U);
        SAG_ASSERT_NOT_NULL(strstr((const char *)bb.data,
                                   fl_op_name(OP_SYNTH[i].op)));
        SAG_ASSERT(strstr((const char *)bb.data, "BAD_OP") == NULL);
        bytebuf_free(&bb);
        cf_close(&f);
    }
}

/* Marks every opcode reachable in `ch` and, recursively, in the
 * functions it holds as constants. */
static void mark_chunk_ops(const FlChunk *ch, bool *seen)
{
    u32 i;
    u32 pc = 0U;

    while (pc < ch->ncode) {
        FlOp op = (FlOp)ch->code[pc];

        if ((u32)op >= (u32)FL_OP__COUNT) {
            (void)fprintf(stderr, "bad opcode %u at pc %u\n",
                          (unsigned)op, (unsigned)pc);
            SAG_ASSERT(false);
            return;
        }
        seen[(u32)op] = true;
        pc += fl_op_length(ch, pc);
    }
    for (i = 0U; i < ch->nconsts; i++) {
        if (ch->consts[i].t == (u8)FL_FN)
            mark_chunk_ops(&((const FlFn *)ch->consts[i].as.o)->ch, seen);
    }
}

void test_fl_compile_every_opcode_is_covered(void)
{
    bool seen[FL_OP__COUNT];
    size_t i;
    u32 k;
    u32 gaps = 0U;
    u32 bad_rows = 0U;

    (void)memset(seen, 0, sizeof(seen));
    /*
     * Every row is checked before anything is asserted: stopping at
     * the first gap turns "which opcodes are unreachable" into a
     * one-per-rebuild guessing game.
     */
    for (i = 0U; i < SAG_ARRAY_LEN(OP_CORPUS); i++) {
        CFix f;
        FlFn *fn;
        bool one[FL_OP__COUNT];

        if (OP_CORPUS[i].src == NULL) {
            /* Covered by OP_SYNTH; the union is asserted below. */
            continue;
        }
        cf_open(&f);
        fn = cf_compile(&f, OP_CORPUS[i].src);
        if (fn == NULL) {
            /* A typo in a corpus program would otherwise read as an
             * uncovered opcode somewhere else. */
            (void)fprintf(stderr, "row %u (%s) failed to compile: %s\n",
                          (unsigned)i, fl_op_name(OP_CORPUS[i].op),
                          f.first);
            bad_rows++;
            cf_close(&f);
            continue;
        }
        (void)memset(one, 0, sizeof(one));
        mark_chunk_ops(&fn->ch, one);
        /* The row's own claim: this program emits THAT opcode. */
        if (!one[(u32)OP_CORPUS[i].op]) {
            (void)fprintf(stderr,
                          "row %u claims %s but its program does not "
                          "emit it:\n%s\n", (unsigned)i,
                          fl_op_name(OP_CORPUS[i].op), OP_CORPUS[i].src);
            bad_rows++;
        }
        for (k = 0U; k < (u32)FL_OP__COUNT; k++)
            seen[k] = seen[k] || one[k];
        cf_close(&f);
    }
    /* The synthetic rows carry the rest.  Their chunks are EXECUTED by
     * test_fl_compile_synthetic_opcodes_execute; here they only need to
     * close the coverage set. */
    for (i = 0U; i < SAG_ARRAY_LEN(OP_SYNTH); i++)
        seen[(u32)OP_SYNTH[i].op] = true;
    /* Every NULL-source row must have exactly one synthetic row, and
     * vice versa -- otherwise a row could go NULL and quietly stop
     * being covered at all. */
    for (k = 0U; k < (u32)FL_OP__COUNT; k++) {
        u32 nsyn = 0U;

        for (i = 0U; i < SAG_ARRAY_LEN(OP_SYNTH); i++) {
            if ((u32)OP_SYNTH[i].op == k)
                nsyn++;
        }
        if (nsyn != (OP_CORPUS[k].src == NULL ? 1U : 0U)) {
            (void)fprintf(stderr, "%s: %u synthetic rows, source is %s\n",
                          fl_op_name((FlOp)k), (unsigned)nsyn,
                          OP_CORPUS[k].src == NULL ? "NULL" : "present");
            bad_rows++;
        }
    }
    /* And the gapless claim. */
    for (k = 0U; k < (u32)FL_OP__COUNT; k++) {
        if (!seen[k]) {
            (void)fprintf(stderr, "UNCOVERED opcode: %s\n",
                          fl_op_name((FlOp)k));
            gaps++;
        }
    }
    SAG_ASSERT_EQ_U64(bad_rows, 0U);
    SAG_ASSERT_EQ_U64(gaps, 0U);
    /* One row per opcode: a duplicate row hides a missing one. */
    SAG_ASSERT_EQ_U64((u64)SAG_ARRAY_LEN(OP_CORPUS), (u64)FL_OP__COUNT);
    for (i = 0U; i < SAG_ARRAY_LEN(OP_CORPUS); i++)
        SAG_ASSERT_EQ_U64((u64)OP_CORPUS[i].op, (u64)i);
}

/* ---------------------------------------------------------------- */
/* Compile errors are diagnostics, never catchable values (§9)      */
/* ---------------------------------------------------------------- */

static void assert_compile_error(const char *src, const char *needle)
{
    CFix f;
    FlFn *fn;

    cf_open(&f);
    fn = cf_compile(&f, src);
    SAG_ASSERT(fn == NULL);
    SAG_ASSERT(f.ndiag != 0U);
    if (strstr(f.first, needle) == NULL)
        (void)fprintf(stderr, "want '%s' in '%s'\n  source: %s\n",
                      needle, f.first, src);
    SAG_ASSERT_NOT_NULL(strstr(f.first, needle));
    cf_close(&f);
}

void test_fl_compile_rejects_bad_scopes(void)
{
    /* `let x = x` names the shadow rather than silently capturing the
     * outer x -- the depth -1 marker in FlLocal exists for this. */
    assert_compile_error("fn f() { let x = 1\n"
                         "if true { let x = x } }\nreturn 0\n",
                         "own initializer");
    assert_compile_error("fn f() { let a = 1\nlet a = 2 }\nreturn 0\n",
                         "already declared");
    assert_compile_error("break\n", "outside");
    assert_compile_error("continue\n", "outside");
    /*
     * Globals live in a runtime map that overwrites happily, so a
     * duplicate top-level binding has to be refused by the compiler or
     * it is refused by nobody -- and the second `let` would silently
     * win over the first.
     */
    assert_compile_error("let g = 1\nlet g = 2\nreturn g\n",
                         "already declared in this module");
    assert_compile_error("fn f() { }\nlet f = 1\nreturn f\n",
                         "already declared in this module");
    /* Shadowing an outer binding inside a block stays legal (§7). */
    {
        CFix f;

        cf_open(&f);
        SAG_ASSERT_NOT_NULL(cf_compile(&f,
            "let g = 1\nfn h() { let g = 2\nreturn g }\nreturn h()\n"));
        SAG_ASSERT_EQ_U64(f.ndiag, 0U);
        cf_close(&f);
    }
}

void test_fl_compile_dedupes_constants(void)
{
    CFix f;
    FlFn *fn;

    /*
     * The same literal used twice takes ONE slot; an int and a float
     * of equal value do NOT share one, because the tag is part of the
     * identity and merging them would make `1 == 1.0` load the wrong
     * type at runtime.
     */
    cf_open(&f);
    fn = cf_compile(&f, "let a = 100000\nlet b = 100000\nreturn a + b\n");
    SAG_ASSERT_NOT_NULL(fn);
    {
        u32 i;
        u32 hits = 0U;

        for (i = 0U; i < fn->ch.nconsts; i++) {
            if (fn->ch.consts[i].t == (u8)FL_INT &&
                fn->ch.consts[i].as.i == 100000)
                hits++;
        }
        SAG_ASSERT_EQ_U64(hits, 1U);
    }
    cf_close(&f);

    cf_open(&f);
    fn = cf_compile(&f, "let a = 100000\nlet b = 100000.0\nreturn 0\n");
    SAG_ASSERT_NOT_NULL(fn);
    {
        u32 i;
        u32 ints = 0U;
        u32 flts = 0U;

        for (i = 0U; i < fn->ch.nconsts; i++) {
            if (fn->ch.consts[i].t == (u8)FL_INT &&
                fn->ch.consts[i].as.i == 100000)
                ints++;
            if (fn->ch.consts[i].t == (u8)FL_FLOAT)
                flts++;
        }
        SAG_ASSERT_EQ_U64(ints, 1U);
        SAG_ASSERT_EQ_U64(flts, 1U);
    }
    cf_close(&f);
}

void test_fl_compile_small_ints_avoid_the_constant_pool(void)
{
    CFix f;
    FlFn *fn;
    bool seen[FL_OP__COUNT];

    /* INT8 exists to keep the pool out of the hot path for the values
     * that dominate real scripts. */
    cf_open(&f);
    fn = cf_compile(&f, "return 7\n");
    SAG_ASSERT_NOT_NULL(fn);
    (void)memset(seen, 0, sizeof(seen));
    mark_chunk_ops(&fn->ch, seen);
    SAG_ASSERT(seen[FL_OP_INT8]);
    SAG_ASSERT(!seen[FL_OP_CONST]);
    cf_close(&f);

    cf_open(&f);
    fn = cf_compile(&f, "return 100000\n");
    SAG_ASSERT_NOT_NULL(fn);
    (void)memset(seen, 0, sizeof(seen));
    mark_chunk_ops(&fn->ch, seen);
    SAG_ASSERT(seen[FL_OP_CONST]);
    cf_close(&f);
}

void test_fl_compile_tracks_max_stack(void)
{
    CFix f;
    FlFn *fn;

    /*
     * max_stack is what the VM reserves per frame.  If it undercounts,
     * a deep expression writes past the frame -- a corruption bug, not
     * a crash, so it is asserted rather than trusted.
     */
    cf_open(&f);
    fn = cf_compile(&f, "return 1 + (2 * (3 - (4 / (5 % 6))))\n");
    SAG_ASSERT_NOT_NULL(fn);
    SAG_ASSERT(fn->max_stack >= 6U);
    cf_close(&f);

    cf_open(&f);
    fn = cf_compile(&f, "return [1, 2, 3, 4, 5, 6, 7, 8]\n");
    SAG_ASSERT_NOT_NULL(fn);
    SAG_ASSERT(fn->max_stack >= 2U);
    cf_close(&f);
}

void test_fl_compile_lines_are_recorded_for_every_op(void)
{
    CFix f;
    FlFn *fn;
    u32 pc = 0U;
    u32 r;
    bool is_start[512];

    /*
     * The line runs are keyed by INSTRUCTION START pc, which the header
     * says is what makes a Sprint 32 trace name the failing instruction
     * rather than the one after it.  A run keyed to an operand byte
     * would look fine in a disassembly and point one instruction wrong
     * in every traceback -- so the keys are checked against the actual
     * instruction starts.
     */
    cf_open(&f);
    fn = cf_compile(&f, "let a = 1\n"
                        "let b = 2\n"
                        "let c = a + b\n"
                        "return c\n");
    SAG_ASSERT_NOT_NULL(fn);
    SAG_ASSERT(fn->ch.ncode < SAG_ARRAY_LEN(is_start));
    (void)memset(is_start, 0, sizeof(is_start));
    while (pc < fn->ch.ncode) {
        is_start[pc] = true;
        pc += fl_op_length(&fn->ch, pc);
    }
    /* pc lands exactly on the end: no instruction ran off the chunk,
     * which is also what proves fl_op_length agrees with the emitter. */
    SAG_ASSERT_EQ_U64(pc, fn->ch.ncode);

    SAG_ASSERT(fn->ch.nlines != 0U);
    SAG_ASSERT_EQ_U64(fn->ch.lines[0].pc, 0U);
    for (r = 0U; r < fn->ch.nlines; r++) {
        SAG_ASSERT(fn->ch.lines[r].pc < fn->ch.ncode);
        SAG_ASSERT(is_start[fn->ch.lines[r].pc]);
        SAG_ASSERT(fn->ch.lines[r].line >= 1U);
        SAG_ASSERT(fn->ch.lines[r].line <= 4U);
        SAG_ASSERT(fn->ch.lines[r].col >= 1U);
        /* Strictly increasing, so a lookup can binary-search and two
         * runs never disagree about one pc. */
        if (r != 0U)
            SAG_ASSERT(fn->ch.lines[r].pc > fn->ch.lines[r - 1U].pc);
    }
    cf_close(&f);
}

void test_fl_compile_operand_lengths_match_the_emitter(void)
{
    size_t i;

    /*
     * Walking each corpus chunk by fl_op_length and landing exactly on
     * the end proves the decode table and the emitter agree for every
     * opcode -- a disagreement would desynchronize the interpreter on
     * the first use and is otherwise found by a crash in a user
     * script.
     */
    for (i = 0U; i < SAG_ARRAY_LEN(OP_CORPUS); i++) {
        CFix f;
        FlFn *fn;
        u32 pc = 0U;

        if (OP_CORPUS[i].src == NULL)
            continue;
        cf_open(&f);
        fn = cf_compile(&f, OP_CORPUS[i].src);
        SAG_ASSERT_NOT_NULL(fn);
        while (pc < fn->ch.ncode)
            pc += fl_op_length(&fn->ch, pc);
        if (pc != fn->ch.ncode)
            (void)fprintf(stderr, "row %u (%s) desynced: pc %u n %u\n",
                          (unsigned)i, fl_op_name(OP_CORPUS[i].op),
                          (unsigned)pc, (unsigned)fn->ch.ncode);
        SAG_ASSERT_EQ_U64(pc, fn->ch.ncode);
        cf_close(&f);
    }
}

void test_fl_compile_is_deterministic_across_instances(void)
{
    CFix a;
    CFix b;
    FlFn *fa;
    FlFn *fb;
    static const char *const src =
        "fn outer(p) {\n"
        "    let n = p\n"
        "    let g = fn() { n = n + 1\nreturn n }\n"
        "    for k, v in {a: 1, b: 2} { n = n + v }\n"
        "    try { n = n / 0 } catch e { n = 0 }\n"
        "    return g\n"
        "}\n"
        "return outer(1)\n";

    /* Invariant 5's compile-side half: the same source compiles to the
     * same bytes, in a fresh process-like VM, every time. */
    cf_open(&a);
    cf_open(&b);
    fa = cf_compile(&a, src);
    fb = cf_compile(&b, src);
    SAG_ASSERT_NOT_NULL(fa);
    SAG_ASSERT_NOT_NULL(fb);
    SAG_ASSERT_EQ_U64(fa->ch.ncode, fb->ch.ncode);
    SAG_ASSERT_EQ_I64(memcmp(fa->ch.code, fb->ch.code, fa->ch.ncode), 0);
    SAG_ASSERT_EQ_U64(fa->ch.nconsts, fb->ch.nconsts);
    SAG_ASSERT_EQ_U64(fa->max_stack, fb->max_stack);
    cf_close(&b);
    cf_close(&a);
}
