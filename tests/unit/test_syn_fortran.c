#include "harness.h"

#include <string.h>

#include "syn/attr.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "syn/fortran.h"
#include "util/arena.h"
#include "util/buf.h"

typedef struct FormCase {
    const char *text;
    SynFortranForm want;
} FormCase;

static u8 fortran_attr_at(const SynLineOut *out, u32 off)
{
    u32 i;

    for (i = 0U; i < out->n; i++) {
        u32 hi = out->spans[i].start + out->spans[i].len;

        if (off >= out->spans[i].start && off < hi)
            return out->spans[i].attr;
    }
    return UINT8_MAX;
}

static void fortran_fast_path_differential(u32 lang, u32 want_rules,
                                           const u8 *line, u32 len)
{
    SynDef *def = (SynDef *)yew_syn_def_for(lang);
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[64];
    SynSpan reference_spans[64];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};
    const SynState *fast_state;
    const SynState *reference_state;

    YEW_ASSERT_NOT_NULL(def);
    fast = yew_syn_engine_new(def);
    reference = yew_syn_engine_new(def);
    YEW_ASSERT_NOT_NULL(fast);
    YEW_ASSERT_NOT_NULL(reference);
    YEW_ASSERT_EQ_U64(yew_syn_engine_fortran_fast_rules(fast),
                      want_rules);
    yew_syn_engine_set_fortran_fast_path(reference, false);
    yew_syn_line(fast, YEW_SYN_STATE_ROOT, line, len, &fast_out);
    yew_syn_line(reference, YEW_SYN_STATE_ROOT, line, len, &reference_out);
    YEW_ASSERT_EQ_U64(fast_out.stop, reference_out.stop);
    YEW_ASSERT_EQ_U64(fast_out.n, reference_out.n);
    YEW_ASSERT_EQ_MEM(fast_out.spans, reference_out.spans,
                      fast_out.n * sizeof(*fast_out.spans));
    fast_state = yew_syn_state_get(yew_syn_engine_states(fast),
                                   fast_out.exit_state);
    reference_state = yew_syn_state_get(yew_syn_engine_states(reference),
                                        reference_out.exit_state);
    YEW_ASSERT_NOT_NULL(fast_state);
    YEW_ASSERT_NOT_NULL(reference_state);
    YEW_ASSERT_EQ_MEM(fast_state, reference_state, sizeof(*fast_state));
    yew_syn_engine_free(reference);
    yew_syn_engine_free(fast);
}

static void fortran_fast_path_differentials(u32 fixed, u32 free_form)
{
    static const char *const fixed_rows[] = {
        "C legacy comment", "#define VALUE 1", "\t1continue",
        "\tstatement", "12345Xcontinue", "123450continue",
        "     1continue", "      continue", "integer x",
        "      print *, 'save'", "ab\xc3\xa7 statement",
        ("     \xc3\xa9" "foo")
    };
    static const char *const free_rows[] = {
        "  #define VALUE 1", "  & continue", "  12345 statement",
        "  123456 statement", "integer :: x", "print *, 'save'",
        "\xc2\xa0& continue"
    };
    u8 long_fixed[75];
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(fixed_rows); i++) {
        fortran_fast_path_differential(
            fixed, 9U, (const u8 *)fixed_rows[i],
            (u32)strlen(fixed_rows[i]));
    }
    (void)memset(long_fixed, (u8)' ', sizeof(long_fixed));
    long_fixed[72] = (u8)'X';
    fortran_fast_path_differential(fixed, 9U, long_fixed,
                                   (u32)sizeof(long_fixed));
    for (i = 0U; i < YEW_ARRAY_LEN(free_rows); i++) {
        fortran_fast_path_differential(
            free_form, 3U, (const u8 *)free_rows[i],
            (u32)strlen(free_rows[i]));
    }
}

void test_syn_fortran_twenty_hand_scored_inputs(void)
{
    static const FormCase cases[] = {
        {"C legacy comment", YEW_FORTRAN_FIXED},
        {"c legacy comment", YEW_FORTRAN_FIXED},
        {"* legacy comment", YEW_FORTRAN_FIXED},
        {"! legacy comment", YEW_FORTRAN_FIXED},
        {"     1continue", YEW_FORTRAN_FIXED},
        {"12345Xcontinue", YEW_FORTRAN_FIXED},
        {"      integer :: x &", YEW_FORTRAN_FREE},
        {"integer :: x ! note", YEW_FORTRAN_FREE},
        {"module m", YEW_FORTRAN_FREE},
        {"PROGRAM x", YEW_FORTRAN_FREE},
        {"end", YEW_FORTRAN_FREE},
        {"C legacy\nmodule m\nend", YEW_FORTRAN_FREE},
        {"C legacy\nmodule m", YEW_FORTRAN_FIXED},
        {"      call work", YEW_FORTRAN_AUTO},
        {"     0continue", YEW_FORTRAN_AUTO},
        {"! ", YEW_FORTRAN_AUTO},
        {"1234A1bad", YEW_FORTRAN_AUTO},
        {"  value ! note", YEW_FORTRAN_FREE},
        {"\tinteger :: x", YEW_FORTRAN_AUTO},
        {"program x\n     1cont", YEW_FORTRAN_FIXED},
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        SynFortranScore score;
        SynFortranForm got = yew_syn_fortran_score_bytes(
            (const u8 *)cases[i].text, strlen(cases[i].text), &score);

        YEW_ASSERT_EQ_U64(got, cases[i].want);
        YEW_ASSERT(score.nonblank != 0U);
    }
}

void test_syn_fortran_tie_limit_and_column_signal_are_pinned(void)
{
    const SynDef *def;
    SynEngine *engine;
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynFortranScore contrary = {7, 0, 1U, 1U};
    char long_line[80];
    Bytebuf hundred;
    SynFortranScore score;
    SynFortranForm got;
    u32 fixed;
    u32 free_form;
    u32 i;

    fixed = yew_syn_lang_named("fortran-fixed");
    free_form = yew_syn_lang_named("fortran");
    YEW_ASSERT(fixed != YEW_LANG_NONE);
    YEW_ASSERT(free_form != YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for_scored(
                          "override.f90", NULL, 0U, &contrary,
                          YEW_FORTRAN_FIXED, false),
                      fixed);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for_scored(
                          "override.f", NULL, 0U, &contrary,
                          YEW_FORTRAN_FREE, false),
                      free_form);

    def = yew_syn_def_for(fixed);
    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    (void)memset(long_line, ' ', sizeof(long_line));
    long_line[71] = 'X';
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)long_line, 72U,
                 &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(fortran_attr_at(&out, 71U), YEW_ATTR_TEXT);
    out.n = 0U;
    out.stop = YEW_SYN_STOP_OK;
    long_line[71] = ' ';
    long_line[72] = 'X';
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)long_line, 73U,
                 &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(fortran_attr_at(&out, 71U), YEW_ATTR_TEXT);
    YEW_ASSERT_EQ_U64(fortran_attr_at(&out, 72U), YEW_ATTR_COMMENT);
    yew_syn_engine_free(engine);

    (void)memset(long_line, ' ', sizeof(long_line));
    long_line[72] = 'x';
    got = yew_syn_fortran_score_bytes((const u8 *)long_line,
                                      sizeof(long_line), &score);
    YEW_ASSERT_EQ_U64(got, YEW_FORTRAN_FREE);
    YEW_ASSERT_EQ_I64(score.fixed_form, -2);
    YEW_ASSERT_EQ_I64(score.free_form, 0);

    bytebuf_init(&hundred);
    for (i = 0U; i < 100U; i++)
        bytebuf_append(&hundred, "x\n", 2U);
    bytebuf_append(&hundred, "C ignored after limit\n", 22U);
    got = yew_syn_fortran_score_bytes(hundred.data, hundred.len, &score);
    YEW_ASSERT_EQ_U64(got, YEW_FORTRAN_AUTO);
    YEW_ASSERT_EQ_U64(score.nonblank, 100U);
    YEW_ASSERT_EQ_U64(score.signals, 0U);
    bytebuf_free(&hundred);

    fortran_fast_path_differentials(fixed, free_form);
    {
        Arena arena;
        SynCtx ctx = {0};
        SynRule rules[3] = {{0}};
        SynDef near_miss = {"fortran", 0U, 1U, 3U, &ctx, rules, NULL};

        arena_init(&arena);
        rules[0].re = yew_re_compile(
            &arena, "^\\s*#.+$", strlen("^\\s*#.+$"), 0U, NULL);
        rules[1].re = yew_re_compile(
            &arena, "^(\\s*)(&)", strlen("^(\\s*)(&)"), 0U, NULL);
        rules[2].re = yew_re_compile(
            &arena, "^(\\s*)([0-9]{1,5})(\\s+)",
            strlen("^(\\s*)([0-9]{1,5})(\\s+)"), 0U, NULL);
        YEW_ASSERT_NOT_NULL(rules[0].re);
        YEW_ASSERT_NOT_NULL(rules[1].re);
        YEW_ASSERT_NOT_NULL(rules[2].re);
        for (i = 0U; i < YEW_ARRAY_LEN(rules); i++)
            (void)memset(rules[i].caps, 0xff, sizeof(rules[i].caps));
        ctx.nrules = YEW_ARRAY_LEN(rules);
        (void)memset(ctx.first, 0xff, sizeof(ctx.first));
        engine = yew_syn_engine_new(&near_miss);
        YEW_ASSERT_NOT_NULL(engine);
        YEW_ASSERT_EQ_U64(yew_syn_engine_fortran_fast_rules(engine), 0U);
        yew_syn_engine_free(engine);
        arena_free_all(&arena);
    }
}
