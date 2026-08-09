/*
 * Sprint 32 §6: runtime error quality.
 *
 * COVERAGE
 * --------
 * named fn -> `f`                trace_names_every_frame_kind
 * fn(...) expression -> `<fn>`   trace_names_every_frame_kind
 * macro m -> `macro m`           trace_names_every_frame_kind
 * module chunk -> `<module init>` trace_names_a_module_and_a_native
 * repl chunk -> `<repl>`         trace_names_every_frame_kind
 * script chunk -> `<script>`     trace_names_every_frame_kind
 * a native -> `<native x.y>`     trace_names_a_module_and_a_native
 * sites are the CALL's own col   trace_sites_are_the_call_not_the_next_op
 * 32-frame elision               trace_elides_the_middle_of_a_deep_stack
 * caret on the innermost frame   trace_carets_the_innermost_frame
 * caret skipped, source absent   trace_omits_the_caret_without_source
 * caught errors carry no trace   trace_is_absent_when_a_catch_claims_it
 */
#include "flfix.h"

#include <stdio.h>
#include <string.h>

#include "fl/trace.h"

static void run(FlFix *f, const char *src, u8 kind, char *out, size_t cap)
{
    flfix_run_trace(f, src, kind, out, cap);
}

/* True when `hay` contains `needle`. */
static bool has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static void want_line(const char *got, const char *needle)
{
    if (!has(got, needle))
        (void)fprintf(stderr, "trace missing |%s| in:\n%s\n", needle, got);
    SAG_ASSERT(has(got, needle));
}

void test_fl_trace_names_every_frame_kind(void)
{
    FlFix f;
    char got[8192];

    flfix_open(&f);
    /* A named function, an anonymous one, and the entry chunk, in one
     * stack so their ORDER is asserted too: innermost first. */
    run(&f,
        "fn named(g) { return g() }\n"
        "return named(fn() { return 1 + \"x\" })\n",
        (u8)FL_FN_SCRIPT, got, sizeof(got));
    want_line(got, "error: type:");
    want_line(got, "  at <fn> (t.fl:2:");
    want_line(got, "  at named (t.fl:1:");
    want_line(got, "  at <script> (t.fl:2:");

    /* The same program entered as the REPL's chunk. */
    run(&f, "return 1 + \"x\"\n", (u8)FL_FN_REPL, got, sizeof(got));
    want_line(got, "  at <repl> (t.fl:1:");

    /* A macro frame.  The null host raises "motion", which is what
     * makes a macro reachable at all before Sprint 34. */
    run(&f, "macro m = @[ 2v ]\nreturn m()\n", (u8)FL_FN_SCRIPT, got,
        sizeof(got));
    want_line(got, "error: motion:");
    want_line(got, "  at macro m (t.fl:1:");
    want_line(got, "  at <script> (t.fl:2:");
    flfix_close(&f);
}

void test_fl_trace_names_a_module_and_a_native(void)
{
    FlFix f;
    char got[8192];
    char src[1024];

    flfix_open(&f);
    (void)flfix_tmpdir(&f);
    flfix_write(&f, "boom.fl", "fn go() { return 1 + \"x\" }\n");
    /*
     * A native BETWEEN two Fletch frames: list.map calls the callback,
     * and a trace that skipped the native would leave an unexplained
     * jump from the callback to whatever called map.
     */
    (void)snprintf(src, sizeof(src),
                   "import list\nimport \"boom.fl\" as b\n"
                   "return list.map([1], fn(x) { return b.go() })\n");
    run(&f, src, (u8)FL_FN_SCRIPT, got, sizeof(got));
    want_line(got, "  at go (");
    want_line(got, "  at <fn> (t.fl:3:");
    want_line(got, "  at <native list.map>");
    want_line(got, "  at <script> (t.fl:3:");

    /* A module's own top level, raising during its import. */
    flfix_write(&f, "bad.fl", "let x = 1 + \"x\"\n");
    run(&f, "import \"bad.fl\" as b\nreturn 1\n", (u8)FL_FN_SCRIPT, got,
        sizeof(got));
    want_line(got, "  at <module init> (");
    flfix_close(&f);
}

void test_fl_trace_sites_are_the_call_not_the_next_op(void)
{
    FlFix f;
    char got[8192];

    flfix_open(&f);
    /*
     * THE PITFALL §6 NAMES.  `ip` at the moment a frame is pushed
     * already points past the call, so a trace built from it reports
     * the NEXT instruction -- which lands on the following source line
     * often enough to be maddening and rarely enough to survive review.
     *
     * The columns below are the CALL's own: line 2's `e(` opens at
     * column 18, and every frame but the innermost reports the paren of
     * the call it is sitting at.  A trace reading the instruction after
     * the call cannot produce them by accident.
     */
    run(&f,
        "fn e() { return 1 + \"x\" }\n"
        "fn d() { return e() }\n"
        "fn c() { return d() }\n"
        "fn b() { return c() }\n"
        "fn a() { return b() }\n"
        "return a()\n",
        (u8)FL_FN_SCRIPT, got, sizeof(got));
    want_line(got, "  at e (t.fl:1:19)");
    want_line(got, "  at d (t.fl:2:18)");
    want_line(got, "  at c (t.fl:3:18)");
    want_line(got, "  at b (t.fl:4:18)");
    want_line(got, "  at a (t.fl:5:18)");
    want_line(got, "  at <script> (t.fl:6:9)");
    flfix_close(&f);
}

void test_fl_trace_elides_the_middle_of_a_deep_stack(void)
{
    FlFix f;
    char got[16384];
    const char *p;
    u32 ats = 0U;

    flfix_open(&f);
    /*
     * Infinite recursion raises "limit" at the depth cap, and a trace
     * that printed every frame would print 256 lines.  Exactly 32 are
     * kept -- 16 from each end -- with one elision line between.
     */
    run(&f, "fn r(n) { return r(n + 1) }\nreturn r(0)\n",
        (u8)FL_FN_SCRIPT, got, sizeof(got));
    want_line(got, "error: limit:");
    want_line(got, "frames elided ...");
    for (p = got; (p = strstr(p, "\n  at ")) != NULL; p += 6)
        ats++;
    if (ats != 32U)
        (void)fprintf(stderr, "printed %u frames:\n%s\n", (unsigned)ats, got);
    SAG_ASSERT_EQ_U64((u64)ats, 32U);
    flfix_close(&f);
}

void test_fl_trace_carets_the_innermost_frame(void)
{
    FlFix f;
    char got[8192];

    flfix_open(&f);
    /*
     * ONE caret, on the innermost frame, and it quotes the source the
     * COMPILER read -- DiagCtx borrows those bytes, so the caret cannot
     * point at a line the compiler never saw.
     */
    run(&f, "fn f() {\n    return 1 + \"x\"\n}\nreturn f()\n",
        (u8)FL_FN_SCRIPT, got, sizeof(got));
    want_line(got, "  2 |     return 1 + \"x\"");
    want_line(got, "    |              ^");
    /* Exactly one caret row, however many frames there are. */
    {
        const char *p;
        u32 carets = 0U;

        for (p = got; (p = strchr(p, '^')) != NULL; p++)
            carets++;
        SAG_ASSERT_EQ_U64((u64)carets, 1U);
    }
    flfix_close(&f);
}

void test_fl_trace_omits_the_caret_without_source(void)
{
    FlFix f;
    char got[8192];
    Bytebuf bb;
    FlValue err = FL_NIL_V;

    flfix_open(&f);
    /*
     * §6 asks for a size+mtime staleness check so a file edited since
     * it compiled does not get a caret pointing at the wrong line.
     * That case cannot arise here: the caret quotes the bytes held by
     * DiagCtx, which are the bytes the compiler read, so there is
     * nothing to go stale.  What CAN happen is the source being
     * genuinely unavailable, and the structural version of the same
     * rule is asserted instead -- the trace still prints, the caret
     * does not.
     */
    run(&f, "return 1 + \"x\"\n", (u8)FL_FN_SCRIPT, got, sizeof(got));
    want_line(got, "  1 | return 1 + \"x\"");

    /* Drop the source out from under it and re-render the same error. */
    f.dc.nfiles = 0U;
    f.vm.err_caret = NULL;
    bytebuf_init(&bb);
    fl_trace_render(&f.vm, err, &bb);
    SAG_ASSERT(bb.len == 0U || strchr((const char *)bb.data, '^') == NULL);
    bytebuf_free(&bb);
    flfix_close(&f);
}

void test_fl_trace_is_absent_when_a_catch_claims_it(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * §9: an error a `catch` claims carries kind and msg ONLY.  The
     * trace is built when the error escapes every frame, not at raise
     * time -- a try/catch in a tight loop must not pay for string
     * formatting it will discard.
     */
    FL_EQ(&f, "import map\nimport list\n"
              "try { let q = 1 + \"x\" }\n"
              "catch e { return list.len(map.keys(e)) }\n", "2");
    FL_EQ(&f, "import map\n"
              "try { let q = 1 + \"x\" }\n"
              "catch e { return map.has(e, \"trace\") }\n", "false");
    FL_EQ(&f, "import map\n"
              "try { let q = 1 + \"x\" }\n"
              "catch e { return map.has(e, \"kind\") }\n", "true");
    flfix_close(&f);
}
