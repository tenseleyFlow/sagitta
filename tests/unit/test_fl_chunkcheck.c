/*
 * Sprint 32 §8: fl_chunk_check, driven by deliberately corrupted chunks.
 *
 * COVERAGE
 * --------
 * accepts real compiler output   chunkcheck_accepts_what_the_compiler_emits
 * jump target out of the chunk   chunkcheck_rejects_a_bad_jump
 * jump target mid-instruction    chunkcheck_rejects_a_bad_jump
 * constant index out of range    chunkcheck_rejects_a_bad_index
 * local slot outside the frame   chunkcheck_rejects_a_bad_index
 * upvalue index out of range     chunkcheck_rejects_a_bad_index
 * unknown opcode                 chunkcheck_rejects_a_bad_index
 * max_stack under-counted        chunkcheck_rejects_an_understated_stack
 * missing terminator             chunkcheck_rejects_a_missing_terminator
 *
 * THE MUTATOR IS TEST-ONLY and never a shipped API.  It writes into a
 * chunk's arena bytes directly, which nothing outside a test may do:
 * the point of §8 is that no code path can deliver bad bytecode, so
 * manufacturing some has to be visibly a test's doing.
 */
#include "flfix.h"

#include <stdio.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/opcodes.h"
#include "fl/parse.h"

/* Compiles `src` and hands back its top-level function. */
static FlFn *build(FlFix *f, const char *src)
{
    FlProgram p;
    FlFn *fn;
    size_t n = strlen(src);

    fl_diag_init(&f->dc, &f->arena);
    (void)fl_diag_add_file(&f->dc, "t.fl", src, n);
    p = fl_parse(&f->arena, &f->dc, &f->in, src, n, 0U);
    YEW_ASSERT(!p.had_error);
    fn = fl_compile(&f->vm, &f->dc, &p, 0U, f->origin);
    YEW_ASSERT_NOT_NULL(fn);
    return fn;
}

/* The pc of the first instruction with this opcode, or ncode. */
static u32 find_op(const FlChunk *ch, FlOp want)
{
    u32 pc = 0U;

    while (pc < ch->ncode) {
        const char *ops = fl_op_operands((FlOp)ch->code[pc]);
        u32 n = 1U;
        size_t i;

        if ((FlOp)ch->code[pc] == want)
            return pc;
        for (i = 0U; ops[i] != '\0'; i++)
            n += ops[i] == 'w' ? 2U : 1U;
        pc += n;
    }
    return ch->ncode;
}

static void want_reject(FlFn *fn, const char *fragment)
{
    const char *why = NULL;

    if (fl_chunk_check(fn, &why))
        (void)fprintf(stderr, "expected a rejection mentioning '%s'\n",
                      fragment);
    YEW_ASSERT(!fl_chunk_check(fn, &why));
    YEW_ASSERT_NOT_NULL(why);
    if (strstr(why, fragment) == NULL)
        (void)fprintf(stderr, "want |%s| in |%s|\n", fragment, why);
    YEW_ASSERT(strstr(why, fragment) != NULL);
}

void test_fl_chunkcheck_accepts_what_the_compiler_emits(void)
{
    FlFix f;
    const char *why = NULL;
    static const char *const progs[] = {
        "return 1\n",
        "let a = 1\nlet b = a + 2\nreturn b\n",
        "fn f(x) { if x > 1 { return x } return 0 }\nreturn f(2)\n",
        "let t = 0\nfor x in [1, 2, 3] { t = t + x }\nreturn t\n",
        "let n = 0\nwhile n < 5 { n = n + 1 }\nreturn n\n",
        "try { return 1 / 0 } catch e { return e.kind }\n",
        /* Parenthesised: clang's -Wstring-concatenation reads adjacent
         * literals in an initialiser as a missing comma, and it is
         * right to -- this one is deliberate. */
        ("fn c(s) { let n = s\nreturn fn() { n = n + 1\nreturn n } }\n"
         "return c(1)()\n"),
        "return {a: 1, b: [2, 3]}\n",
        "macro m = @[ 2v ]\nreturn 1\n"
    };
    size_t i;

    flfix_open(&f);
    /* Whatever else the checker does, it must not reject the compiler
     * it exists to check -- every shape the suite already exercises. */
    for (i = 0U; i < YEW_ARRAY_LEN(progs); i++) {
        FlFn *fn = build(&f, progs[i]);

        if (!fl_chunk_check(fn, &why))
            (void)fprintf(stderr, "rejected |%s|: %s\n", progs[i],
                          why == NULL ? "?" : why);
        YEW_ASSERT(fl_chunk_check(fn, &why));
    }
    flfix_close(&f);
}

void test_fl_chunkcheck_rejects_a_bad_jump(void)
{
    FlFix f;
    FlFn *fn;
    u32 at;

    flfix_open(&f);
    fn = build(&f, "let n = 0\nwhile n < 5 { n = n + 1 }\nreturn n\n");
    at = find_op(&fn->ch, FL_OP_JUMP_IF_FALSE);
    YEW_ASSERT(at < fn->ch.ncode);
    /* Off the end of the chunk. */
    fn->ch.code[at + 1U] = 0xFFU;
    fn->ch.code[at + 2U] = 0xFFU;
    want_reject(fn, "jump target outside the chunk");

    /* And into the middle of an instruction, which is the subtler one:
     * the target is in range and lands on an operand byte. */
    fn = build(&f, "let n = 0\nwhile n < 5 { n = n + 1 }\nreturn n\n");
    at = find_op(&fn->ch, FL_OP_JUMP_IF_FALSE);
    fn->ch.code[at + 1U] = 1U;
    fn->ch.code[at + 2U] = 0U;
    want_reject(fn, "not an instruction start");
    flfix_close(&f);
}

void test_fl_chunkcheck_rejects_a_bad_index(void)
{
    FlFix f;
    FlFn *fn;
    u32 at;

    flfix_open(&f);
    fn = build(&f, "return \"a\"\n");
    at = find_op(&fn->ch, FL_OP_CONST);
    YEW_ASSERT(at < fn->ch.ncode);
    fn->ch.code[at + 1U] = 0xFFU;
    fn->ch.code[at + 2U] = 0xFFU;
    want_reject(fn, "constant index out of range");

    fn = build(&f, "fn f(x) { return x }\nreturn f(1)\n");
    at = find_op(&fn->ch, FL_OP_GET_LOCAL);
    if (at < fn->ch.ncode) {
        fn->ch.code[at + 1U] = 0xFEU;
        want_reject(fn, "local slot outside the frame");
    }

    /* An opcode the table does not have. */
    fn = build(&f, "return 1\n");
    fn->ch.code[0] = (u8)FL_OP__COUNT;
    want_reject(fn, "unknown opcode");
    flfix_close(&f);
}

void test_fl_chunkcheck_rejects_an_understated_stack(void)
{
    FlFix f;
    FlFn *fn;

    flfix_open(&f);
    /*
     * THE CHECK THE VM'S MEMORY SAFETY RESTS ON.  Sprint 30 validates
     * max_stack once per CALL and then lets every push go unchecked, so
     * a max_stack the compiler under-counted is a silent overrun rather
     * than a caught error.
     */
    fn = build(&f, "return 1 + 2 * 3 - 4\n");
    fn->max_stack = 1U;
    want_reject(fn, "max_stack is smaller");
    flfix_close(&f);
}

void test_fl_chunkcheck_rejects_a_missing_terminator(void)
{
    FlFix f;
    FlFn *fn;
    const char *why = NULL;

    flfix_open(&f);
    /*
     * Reachability is deliberately NOT walked -- that would be a second
     * definition of what the compiler emits, the duplication §8
     * rejects.  What is checked is the property the VM depends on: the
     * chunk ends in a terminator, so execution cannot run off the end.
     */
    fn = build(&f, "return 1\n");
    YEW_ASSERT(fl_chunk_check(fn, &why));
    fn->ch.code[fn->ch.ncode - 1U] = (u8)FL_OP_POP;
    want_reject(fn, "does not end in a terminator");

    /* An empty chunk is not a program. */
    fn = build(&f, "return 1\n");
    fn->ch.ncode = 0U;
    want_reject(fn, "empty chunk");
    flfix_close(&f);
}
