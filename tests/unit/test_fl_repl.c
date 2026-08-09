/*
 * Sprint 32 §3 and §5: continuation, and what a prompt prints.
 *
 * COVERAGE
 * --------
 * s29's incomplete matrix        repl_classifies_the_s29_matrix
 * a 12-line nested entry         repl_accumulates_a_nested_entry
 * error beats incomplete         repl_classifies_the_s29_matrix
 * the trial parse is SILENT      repl_classifies_without_emitting
 * nil prints nothing at all      repl_prints_repr_and_nothing_for_nil
 * repr, not str                  repl_prints_repr_and_nothing_for_nil
 * the 2000-byte elision          repl_bounds_what_it_prints
 * depth 8 cutoff                 repl_bounds_what_it_prints
 * cycles print, never recurse    repl_bounds_what_it_prints
 */
#include "flfix.h"

#include <stdio.h>
#include <string.h>

#include "fl/gc.h"
#include "fl/repl.h"

static FlReplVerdict verdict(FlFix *f, const char *src)
{
    return sag_fl_repl_classify(&f->arena, &f->in, src, strlen(src));
}

void test_fl_repl_classifies_the_s29_matrix(void)
{
    FlFix f;

    flfix_open(&f);
    /* Complete and clean. */
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "1 + 1\n"), (u64)FL_REPL_RUN);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "let x = 1\n"), (u64)FL_REPL_RUN);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "fn f(x) { return x }\n"),
                      (u64)FL_REPL_RUN);
    /* Open brackets keep reading. */
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "fn f(\n"), (u64)FL_REPL_CONTINUE);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "fn f((\n"), (u64)FL_REPL_CONTINUE);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "fn f(x) {\n"), (u64)FL_REPL_CONTINUE);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "[1, 2\n"), (u64)FL_REPL_CONTINUE);
    /* A trailing operator is a continuation, per §1.2. */
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "1 +\n"), (u64)FL_REPL_CONTINUE);
    /* ...and the line that finishes it completes the entry. */
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "1 +\n1\n"), (u64)FL_REPL_RUN);
    /*
     * ERROR BEATS INCOMPLETE.  s29 pins that a syntax error inside an
     * open bracket sets had_error, not incomplete -- and a REPL that
     * read incomplete first would sit waiting for a `)` the user
     * already typed wrong.
     */
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "fn f()}\n"), (u64)FL_REPL_ERROR);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "return )\n"), (u64)FL_REPL_ERROR);
    SAG_ASSERT_EQ_U64((u64)verdict(&f, "let 5 = 1\n"), (u64)FL_REPL_ERROR);
    flfix_close(&f);
}

void test_fl_repl_accumulates_a_nested_entry(void)
{
    FlFix f;
    /* Twelve lines, each of which must be a continuation until the
     * last -- the case where deciding per line rather than over the
     * whole text goes wrong, because `}` alone looks like an error. */
    static const char *const lines[] = {
        "fn outer(n) {",
        "    let t = 0",
        "    for x in [1, 2, 3] {",
        "        if x > n {",
        "            t = t + x",
        "        } else {",
        "            t = t - x",
        "        }",
        "    }",
        "    return t",
        "}",
        "outer(1)"
    };
    char acc[1024];
    size_t at = 0U;
    size_t i;

    flfix_open(&f);
    acc[0] = '\0';
    for (i = 0U; i < SAG_ARRAY_LEN(lines); i++) {
        FlReplVerdict v;

        at += (size_t)snprintf(acc + at, sizeof(acc) - at, "%s\n", lines[i]);
        v = sag_fl_repl_classify(&f.arena, &f.in, acc, at);
        if (i + 2U < SAG_ARRAY_LEN(lines)) {
            if (v != FL_REPL_CONTINUE)
                (void)fprintf(stderr, "line %zu of the entry: verdict %d\n",
                              i, (int)v);
            SAG_ASSERT_EQ_U64((u64)v, (u64)FL_REPL_CONTINUE);
        }
    }
    /* The closing brace completes the function; the call after it
     * completes the entry. */
    SAG_ASSERT_EQ_U64((u64)sag_fl_repl_classify(&f.arena, &f.in, acc, at),
                      (u64)FL_REPL_RUN);
    flfix_close(&f);
}

void test_fl_repl_classifies_without_emitting(void)
{
    FlFix f;
    u32 before;

    flfix_open(&f);
    /*
     * THE TRIAL PARSE IS SILENT.  Every keystroke of a multiline entry
     * runs one, and a version that printed would emit a bogus
     * `unexpected end of input` under every line as it was typed.
     *
     * Asserted through the fixture's capturing sink: the classifier
     * builds its own throwaway context, so nothing reaches this one.
     */
    f.ndiag = 0U;
    before = f.ndiag;
    (void)verdict(&f, "fn f(\n");
    (void)verdict(&f, "fn f(x) {\n");
    (void)verdict(&f, "return )\n");
    (void)verdict(&f, "let 5 = 1\n");
    SAG_ASSERT_EQ_U64((u64)f.ndiag, (u64)before);
    flfix_close(&f);
}

void test_fl_repl_prints_repr_and_nothing_for_nil(void)
{
    FlFix f;
    Bytebuf bb;

    flfix_open(&f);
    bytebuf_init(&bb);
    /*
     * NIL PRINTS NOTHING AT ALL -- not even a blank line.  Otherwise
     * every `let`, every io.print and every void call spams one.
     */
    sag_fl_print_result(&f.vm, FL_NIL_V, &bb);
    SAG_ASSERT_EQ_U64((u64)bb.len, 0U);

    /* repr, not str: at a prompt the difference between the string
     * "a\nb" and a two-line result is information the user needs, and
     * repr output can be pasted straight back in. */
    sag_fl_print_result(&f.vm,
                        FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "a\nb", 3U)), &bb);
    /* `"a\nb"` is six characters plus the newline. */
    SAG_ASSERT_EQ_U64((u64)bb.len, 7U);
    SAG_ASSERT_EQ_I64(memcmp(bb.data, "\"a\\nb\"\n", 7U), 0);

    bb.len = 0U;
    sag_fl_print_result(&f.vm, FL_INT_V(42), &bb);
    SAG_ASSERT_EQ_I64(memcmp(bb.data, "42\n", 3U), 0);
    bytebuf_free(&bb);
    flfix_close(&f);
}

void test_fl_repl_bounds_what_it_prints(void)
{
    FlFix f;
    Bytebuf bb;
    FlList *l;
    u32 i;

    flfix_open(&f);
    bytebuf_init(&bb);
    /*
     * A CYCLE PRINTS rather than recursing.  `let l = []` and
     * `list.push(l, l)` is all it takes, and without the elision the
     * prompt would recurse until sag_bug -- which is a spectacular way
     * to lose a session over a two-word mistake.
     */
    l = fl_list_new(&f.vm);
    fl_gc_protect(&f.vm, FL_OBJ_V(FL_LIST, l));
    (void)fl_list_push(&f.vm, l, FL_OBJ_V(FL_LIST, l));
    sag_fl_print_result(&f.vm, FL_OBJ_V(FL_LIST, l), &bb);
    SAG_ASSERT_EQ_I64(memcmp(bb.data, "[[...]]\n", 8U), 0);
    fl_gc_release(&f.vm, 1U);

    /* Nested past the depth cap prints the marker rather than the
     * whole tree. */
    bb.len = 0U;
    {
        FlList *deep = fl_list_new(&f.vm);
        FlList *cur = deep;

        fl_gc_protect(&f.vm, FL_OBJ_V(FL_LIST, deep));
        for (i = 0U; i < 20U; i++) {
            FlList *next = fl_list_new(&f.vm);

            (void)fl_list_push(&f.vm, cur, FL_OBJ_V(FL_LIST, next));
            cur = next;
        }
        sag_fl_print_result(&f.vm, FL_OBJ_V(FL_LIST, deep), &bb);
        fl_gc_release(&f.vm, 1U);
    }
    SAG_ASSERT(bb.len < 64U);
    SAG_ASSERT(strstr((const char *)bb.data, "[...]") != NULL);

    /* And a long result is elided with a byte count, because a prompt
     * that hangs printing is worse than one that truncates. */
    bb.len = 0U;
    {
        Bytebuf big;
        FlStr *s;

        bytebuf_init(&big);
        for (i = 0U; i < 5000U; i++)
            bytebuf_push_u8(&big, (u8)'x');
        s = fl_str_new(&f.vm, (const char *)big.data, (u32)big.len);
        bytebuf_free(&big);
        sag_fl_print_result(&f.vm, FL_OBJ_V(FL_STR, s), &bb);
    }
    SAG_ASSERT(bb.len < 2200U);
    SAG_ASSERT(strstr((const char *)bb.data, "more bytes)") != NULL);
    bytebuf_free(&bb);
    flfix_close(&f);
}

/* ---------------------------------------------------------------- */
/* §4: the `:`-commands                                             */
/* ---------------------------------------------------------------- */

static void cmd(FlFix *f, FlRepl *r, const char *line, char *out, size_t cap,
                bool *quit)
{
    Bytebuf bb;
    size_t k;

    (void)f;
    bytebuf_init(&bb);
    out[0] = '\0';
    if (!sag_fl_repl_command(r, line, strlen(line), &bb, quit)) {
        (void)snprintf(out, cap, "<not a command>");
        bytebuf_free(&bb);
        return;
    }
    k = bb.len < cap - 1U ? bb.len : cap - 1U;
    if (k != 0U)
        (void)memcpy(out, bb.data, k);
    out[k] = '\0';
    bytebuf_free(&bb);
}

static void repl_open(FlFix *f, FlRepl *r)
{
    flfix_open(f);
    (void)memset(r, 0, sizeof(*r));
    r->vm = &f->vm;
    r->arena = &f->arena;
    r->in = &f->in;
    r->dc = &f->dc;
}

void test_fl_repl_recognises_only_a_leading_colon(void)
{
    FlFix f;
    FlRepl r;
    char got[4096];
    bool quit = false;

    repl_open(&f, &r);
    /*
     * A `.fl` file starting with `:` is an ordinary syntax error, and
     * that is correct -- these are REPL-only, so recognition is one
     * rule and needs no special case anywhere else.
     */
    cmd(&f, &r, "1 + 1\n", got, sizeof(got), &quit);
    SAG_ASSERT_EQ_STR(got, "<not a command>");
    cmd(&f, &r, "let x = 1\n", got, sizeof(got), &quit);
    SAG_ASSERT_EQ_STR(got, "<not a command>");
    /* Leading space is allowed; the colon must be the first byte that
     * is not one. */
    cmd(&f, &r, "   :caps\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "origin:") != NULL);
    flfix_close(&f);
}

void test_fl_repl_quits_and_helps(void)
{
    FlFix f;
    FlRepl r;
    char got[4096];
    bool quit = false;

    repl_open(&f, &r);
    cmd(&f, &r, ":quit\n", got, sizeof(got), &quit);
    SAG_ASSERT(quit);
    quit = false;
    cmd(&f, &r, ":q\n", got, sizeof(got), &quit);
    SAG_ASSERT(quit);
    quit = false;

    cmd(&f, &r, ":help\n", got, sizeof(got), &quit);
    SAG_ASSERT(!quit);
    /* Every command names itself in the table -- one list, so the help
     * text and the dispatcher cannot drift. */
    SAG_ASSERT(strstr(got, ":help") != NULL);
    SAG_ASSERT(strstr(got, ":quit") != NULL);
    SAG_ASSERT(strstr(got, ":load") != NULL);
    SAG_ASSERT(strstr(got, ":reload") != NULL);
    SAG_ASSERT(strstr(got, ":globals") != NULL);
    SAG_ASSERT(strstr(got, ":disasm") != NULL);
    SAG_ASSERT(strstr(got, ":caps") != NULL);
    /* And it says there is no debugger, so nobody hunts for a flag. */
    SAG_ASSERT(strstr(got, "no stepping debugger") != NULL);
    SAG_ASSERT(strstr(got, "fletch-spec.md") != NULL);

    /* :help NAME reads the registered signature. */
    cmd(&f, &r, ":help str.slice\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "(s, lo, [hi]) -> s") != NULL);
    cmd(&f, &r, ":help fmt.repr\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "(v, [indent]) -> str") != NULL);
    cmd(&f, &r, ":help nope.nope\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "no builtin called") != NULL);
    flfix_close(&f);
}

void test_fl_repl_suggests_over_the_command_names(void)
{
    FlFix f;
    FlRepl r;
    char got[4096];
    bool quit = false;

    repl_open(&f, &r);
    /* §7's fourth site: the candidate set is exactly the eight. */
    cmd(&f, &r, ":gloabls\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "unknown command ':gloabls'") != NULL);
    SAG_ASSERT(strstr(got, "did you mean 'globals'?") != NULL);
    cmd(&f, &r, ":relaod\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "did you mean 'reload'?") != NULL);
    /* Nothing close enough offers nothing, and does not leave a
     * dangling separator behind. */
    cmd(&f, &r, ":zzzzzzz\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "unknown command") != NULL);
    SAG_ASSERT(strstr(got, "did you mean") == NULL);
    SAG_ASSERT(strstr(got, "; \n") == NULL);
    flfix_close(&f);
}

void test_fl_repl_shows_globals_and_caps(void)
{
    FlFix f;
    FlRepl r;
    char got[4096];
    bool quit = false;
    char scratch[8192];

    repl_open(&f, &r);
    cmd(&f, &r, ":globals\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "(none yet)") != NULL);

    /* Bind two names and import a module: the module must NOT show up,
     * because ":globals" answers "what have I got" and the seven
     * builtins are not an answer to that. */
    flfix_run(&f, "import str\nlet total = 41\nlet name = \"a\"\n",
              scratch, sizeof(scratch));
    cmd(&f, &r, ":globals\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "total = 41") != NULL);
    SAG_ASSERT(strstr(got, "name = \"a\"") != NULL);
    SAG_ASSERT(strstr(got, "str =") == NULL);

    cmd(&f, &r, ":caps\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "origin:") != NULL);
    SAG_ASSERT(strstr(got, "(none)") != NULL);
    flfix_as(&f, (u8)FL_ORIGIN_REPL,
             (u32)FL_CAP_FS_READ | (u32)FL_CAP_NET);
    f.vm.root_origin = f.origin;
    cmd(&f, &r, ":caps\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "fs.read") != NULL);
    SAG_ASSERT(strstr(got, "net") != NULL);
    SAG_ASSERT(strstr(got, "fs.write") == NULL);
    flfix_close(&f);
}

void test_fl_repl_disassembles_and_loads(void)
{
    FlFix f;
    FlRepl r;
    char got[8192];
    char scratch[8192];
    char line[1024];
    bool quit = false;

    repl_open(&f, &r);
    cmd(&f, &r, ":disasm\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "needs a name") != NULL);
    cmd(&f, &r, ":disasm nope\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "no function called 'nope'") != NULL);

    flfix_run(&f, "fn twice(x) { return x + x }\n", scratch, sizeof(scratch));
    cmd(&f, &r, ":disasm twice\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "ADD") != NULL);
    SAG_ASSERT(strstr(got, "RETURN") != NULL);

    /*
     * :load evaluates into THIS VM and its globals persist -- that is
     * the whole point of it, and the reason to iterate on a config
     * from a prompt rather than restarting.
     */
    (void)flfix_tmpdir(&f);
    flfix_write(&f, "cfg.fl", "let tabwidth = 4\n");
    (void)snprintf(line, sizeof(line), ":load %s/cfg.fl\n",
                   flfix_tmpdir(&f));
    cmd(&f, &r, line, got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "loaded") != NULL);
    cmd(&f, &r, ":globals\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "tabwidth = 4") != NULL);

    /* :reload runs the same path again. */
    flfix_write(&f, "cfg.fl", "let tabwidth = 8\n");
    cmd(&f, &r, ":reload\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "loaded") != NULL);
    cmd(&f, &r, ":globals\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "tabwidth = 8") != NULL);

    cmd(&f, &r, ":load /definitely/not/here.fl\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "cannot read") != NULL);
    flfix_close(&f);

    /* :reload before any :load says so rather than doing nothing. */
    repl_open(&f, &r);
    cmd(&f, &r, ":reload\n", got, sizeof(got), &quit);
    SAG_ASSERT(strstr(got, "nothing has been") != NULL);
    flfix_close(&f);
}
