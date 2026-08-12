#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syn/engine.h"
#include "util/arena.h"

#include "syn_toy.h"

typedef struct LineRow {
    u8 entry_ctx;
    const char *line;
    u16 probe;
    u8 attr;
    u8 exit_ctx;
} LineRow;

static u8 attr_at(const SynLineOut *out, u32 off)
{
    u32 i;

    for (i = 0U; i < out->n; i++) {
        u32 hi = out->spans[i].start + out->spans[i].len;
        if (off >= out->spans[i].start && off < hi)
            return out->spans[i].attr;
    }
    return UINT8_MAX;
}

static void assert_spans_well_formed(const SynLineOut *out, u32 len)
{
    u32 i;
    u32 end = 0U;

    for (i = 0U; i < out->n; i++) {
        YEW_ASSERT(out->spans[i].len > 0U);
        YEW_ASSERT(out->spans[i].start >= end);
        YEW_ASSERT(out->spans[i].start + out->spans[i].len <= len);
        YEW_ASSERT(out->spans[i].attr < YEW_ATTR__COUNT);
        end = out->spans[i].start + out->spans[i].len;
    }
}

void test_syn_line_toy_golden_table(void)
{
    static const LineRow rows[] = {
        {0, "alpha", 0, YEW_ATTR_TEXT, 0},
        {0, "if", 0, YEW_ATTR_KEYWORD_CONTROL, 0},
        {0, "else", 1, YEW_ATTR_KEYWORD_CONTROL, 0},
        {0, "while", 2, YEW_ATTR_KEYWORD_CONTROL, 0},
        {0, "return", 5, YEW_ATTR_KEYWORD_CONTROL, 0},
        {0, "ifx", 0, YEW_ATTR_TEXT, 0},
        {0, "xif", 1, YEW_ATTR_TEXT, 0},
        {0, "0", 0, YEW_ATTR_NUMBER, 0},
        {0, "123456", 3, YEW_ATTR_NUMBER, 0},
        {0, "x9", 1, YEW_ATTR_NUMBER, 0},
        {0, "true", 0, YEW_ATTR_BOOLEAN, 0},
        {0, "false", 4, YEW_ATTR_BOOLEAN, 0},
        {0, "truth", 0, YEW_ATTR_TEXT, 0},
        {0, "+", 0, YEW_ATTR_OPERATOR, 0},
        {0, "=-", 1, YEW_ATTR_OPERATOR, 0},
        {0, "x+y", 1, YEW_ATTR_OPERATOR, 0},
        {0, "\"abc\"", 0, YEW_ATTR_STRING, 0},
        {0, "\"abc\"", 3, YEW_ATTR_STRING, 0},
        {0, "\"a\\nb\"", 2, YEW_ATTR_STRING_ESCAPE, 0},
        {0, "\"unterminated", 5, YEW_ATTR_STRING, 0},
        {1, "body", 0, YEW_ATTR_STRING, 0},
        {1, "\\t", 0, YEW_ATTR_STRING_ESCAPE, 0},
        {1, "end\"", 3, YEW_ATTR_STRING, 0},
        {0, "// comment", 0, YEW_ATTR_COMMENT, 0},
        {0, "// comment", 8, YEW_ATTR_COMMENT, 0},
        {3, "carried line", 2, YEW_ATTR_COMMENT, 0},
        {0, "/* open", 0, YEW_ATTR_COMMENT, 2},
        {0, "/* open", 5, YEW_ATTR_COMMENT, 2},
        {2, "body", 2, YEW_ATTR_COMMENT, 2},
        {2, "close */", 6, YEW_ATTR_COMMENT, 0},
        {2, "*/ tail", 3, YEW_ATTR_TEXT, 0},
        {0, "x /* c", 2, YEW_ATTR_COMMENT, 2},
        {0, "if 42", 0, YEW_ATTR_KEYWORD_CONTROL, 0},
        {0, "if 42", 3, YEW_ATTR_NUMBER, 0},
        {0, "true+1", 4, YEW_ATTR_OPERATOR, 0},
        {0, "true+1", 5, YEW_ATTR_NUMBER, 0},
        {0, "a//b", 1, YEW_ATTR_COMMENT, 0},
        {0, "a\"b\"c", 2, YEW_ATTR_STRING, 0},
        {0, "99/*x", 0, YEW_ATTR_NUMBER, 2},
        {0, "99/*x", 3, YEW_ATTR_COMMENT, 2},
        {0, "return false", 7, YEW_ATTR_BOOLEAN, 0},
        {0, "while=0", 5, YEW_ATTR_OPERATOR, 0}
    };
    SynToy toy;
    SynSpan spans[64];
    u32 i;

    syn_toy_init(&toy);
    YEW_ASSERT(YEW_ARRAY_LEN(rows) >= 40U);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        SynLineOut out;
        u32 entry = rows[i].entry_ctx == 0U ? YEW_SYN_STATE_ROOT :
                    syn_toy_state(&toy, rows[i].entry_ctx);
        const SynState *exit;

        (void)syn_toy_line(&toy, entry, rows[i].line, spans,
                           YEW_ARRAY_LEN(spans), &out);
        exit = yew_syn_state_get(yew_syn_engine_states(toy.engine),
                                 out.exit_state);
        if (exit != NULL &&
            exit->f[exit->depth - 1U].ctx != rows[i].exit_ctx)
            (void)fprintf(stderr, "toy row %u (%s): exit ctx %u, want %u\n",
                          i, rows[i].line,
                          (unsigned)exit->f[exit->depth - 1U].ctx,
                          (unsigned)rows[i].exit_ctx);
        YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
        YEW_ASSERT_NOT_NULL(exit);
        YEW_ASSERT_EQ_U64(exit->f[exit->depth - 1U].ctx,
                          rows[i].exit_ctx);
        YEW_ASSERT_EQ_U64(attr_at(&out, rows[i].probe), rows[i].attr);
        assert_spans_well_formed(&out, (u32)strlen(rows[i].line));
    }
    syn_toy_free(&toy);
}

void test_syn_line_byte_cap_preserves_entry_state(void)
{
    SynToy toy;
    SynSpan spans[8];
    SynLineOut out;
    u8 *line = malloc(YEW_SYN_LINE_BYTE_CAP + 1U);
    u32 entry;

    YEW_ASSERT_NOT_NULL(line);
    (void)memset(line, 'x', YEW_SYN_LINE_BYTE_CAP + 1U);
    syn_toy_init(&toy);
    toy.rules[0].flags |= YEW_SYN_RULE_FIRST_LINE;
    yew_syn_engine_set_def(toy.engine, &toy.def);
    entry = syn_toy_state(&toy, SYN_TOY_COMMENT_BLOCK);
    out = (SynLineOut){spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    yew_syn_line(toy.engine, entry, line, YEW_SYN_LINE_BYTE_CAP + 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_BYTES);
    {
        const SynState *exit = yew_syn_state_get(
            yew_syn_engine_states(toy.engine), out.exit_state);

        YEW_ASSERT_NOT_NULL(exit);
        YEW_ASSERT_EQ_U64(exit->f[exit->depth - 1U].ctx,
                          SYN_TOY_COMMENT_BLOCK);
        YEW_ASSERT((exit->flags & YEW_SYN_F_PAST_FIRST) != 0U);
    }
    assert_spans_well_formed(&out, YEW_SYN_LINE_BYTE_CAP + 1U);
    syn_toy_free(&toy);
    free(line);
}

void test_syn_line_byte_cap_reuses_unchanged_state_id(void)
{
    SynToy toy;
    SynSpan spans[8];
    SynLineOut out;
    u8 *line = malloc(YEW_SYN_LINE_BYTE_CAP + 1U);
    u32 entry;

    YEW_ASSERT_NOT_NULL(line);
    (void)memset(line, 'x', YEW_SYN_LINE_BYTE_CAP + 1U);
    syn_toy_init(&toy);
    entry = syn_toy_state(&toy, SYN_TOY_COMMENT_BLOCK);
    out = (SynLineOut){spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    yew_syn_line(toy.engine, entry, line, YEW_SYN_LINE_BYTE_CAP + 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_BYTES);
    YEW_ASSERT_EQ_U64(out.exit_state, entry);
    assert_spans_well_formed(&out, YEW_SYN_LINE_BYTE_CAP + 1U);
    syn_toy_free(&toy);
    free(line);
}

void test_syn_line_span_cap_stops_without_overwriting_caller_storage(void)
{
    SynToy toy;
    SynSpan spans[2];
    SynLineOut out;
    static const char line[] = "if 1 true";

    syn_toy_init(&toy);
    spans[1] = (SynSpan){UINT32_MAX, UINT16_MAX, UINT8_MAX, UINT8_MAX};
    out = (SynLineOut){spans, 0U, 1U, 0U, 0U};
    yew_syn_line(toy.engine, YEW_SYN_STATE_ROOT, (const u8 *)line,
                 sizeof(line) - 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_SPANS);
    YEW_ASSERT(out.n <= 1U);
    YEW_ASSERT_EQ_U64(spans[1].start, UINT32_MAX);
    YEW_ASSERT_EQ_U64(spans[1].len, UINT16_MAX);
    syn_toy_free(&toy);
}

void test_syn_line_zero_width_rule_forces_utf8_progress(void)
{
    SynToy toy;
    SynSpan spans[16];
    SynLineOut out;
    YewReErr err = {0U, NULL};
    static const u8 line[] = "bbb";

    syn_toy_init(&toy);
    toy.rules[0].re = yew_re_compile(&toy.arena, "a*", 2U, 0U, &err);
    YEW_ASSERT_NOT_NULL(toy.rules[0].re);
    (void)memset(toy.rules[0].first, 0, sizeof(toy.rules[0].first));
    toy.rules[0].first['b' >> 3U] |= (u8)(1U << ('b' & 7U));
    toy.ctxs[0].first['b' >> 3U] |= (u8)(1U << ('b' & 7U));
    out = (SynLineOut){spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    yew_syn_line(toy.engine, YEW_SYN_STATE_ROOT, line, 3U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(out.exit_state, YEW_SYN_STATE_ROOT);
    YEW_ASSERT(out.n > 0U);
    assert_spans_well_formed(&out, 3U);
    /* Removing forced next-boundary advance makes this test hang. */
    syn_toy_free(&toy);
}

void test_syn_line_step_cap_degrades_instead_of_stalling(void)
{
    enum { NRULES = 4200 };
    Arena arena;
    SynRule *rules = calloc(NRULES, sizeof(*rules));
    SynCtx ctx = {0U, NRULES, YEW_ATTR_TEXT, SYN_OP_STAY, 0U, 0U, {0}, 0U,
                  {0}};
    SynDef def = {"steps", 0U, 1U, NRULES, &ctx, rules, NULL};
    SynEngine *engine;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    YewRe *never;
    u32 i;

    YEW_ASSERT_NOT_NULL(rules);
    arena_init(&arena);
    never = yew_re_compile(&arena, "^xy", 3U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(never);
    ctx.first['x' >> 3U] |= (u8)(1U << ('x' & 7U));
    for (i = 0U; i < NRULES; i++) {
        rules[i].re = never;
        rules[i].attr = YEW_ATTR_TEXT;
        (void)memset(rules[i].caps, 0xff, sizeof(rules[i].caps));
        rules[i].first['x' >> 3U] |= (u8)(1U << ('x' & 7U));
    }
    rules[0].flags |= YEW_SYN_RULE_FIRST_LINE;
    engine = yew_syn_engine_new(&def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"x", 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_STEPS);
    {
        const SynState *exit = yew_syn_state_get(
            yew_syn_engine_states(engine), out.exit_state);

        YEW_ASSERT_NOT_NULL(exit);
        YEW_ASSERT_EQ_U64(exit->f[0].ctx, 0U);
        YEW_ASSERT((exit->flags & YEW_SYN_F_PAST_FIRST) != 0U);
    }
    yew_syn_engine_free(engine);
    arena_free_all(&arena);
    free(rules);
}

void test_syn_line_empty_candidate_set_does_not_scan_declared_first(void)
{
    enum { NRULES = 4200 };
    Arena arena;
    SynRule *rules = calloc(NRULES, sizeof(*rules));
    SynCtx ctx = {0U, NRULES, YEW_ATTR_TEXT, SYN_OP_STAY, 0U, 0U, {0}, 0U,
                  {0}};
    SynDef def = {"empty-candidates", 0U, 1U, NRULES, &ctx, rules, NULL};
    SynEngine *engine;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    YewRe *never;
    u32 i;

    YEW_ASSERT_NOT_NULL(rules);
    arena_init(&arena);
    never = yew_re_compile(&arena, "z", 1U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(never);
    ctx.first['x' >> 3U] |= (u8)(1U << ('x' & 7U));
    for (i = 0U; i < NRULES; i++) {
        rules[i].re = never;
        rules[i].attr = YEW_ATTR_TEXT;
        (void)memset(rules[i].caps, 0xff, sizeof(rules[i].caps));
        rules[i].first['x' >> 3U] |= (u8)(1U << ('x' & 7U));
    }
    engine = yew_syn_engine_new(&def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"x", 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(out.n, 1U);
    YEW_ASSERT_EQ_U64(out.spans[0].attr, YEW_ATTR_TEXT);
    yew_syn_engine_free(engine);
    arena_free_all(&arena);
    free(rules);
}

void test_syn_line_large_candidate_index_falls_back_without_semantic_change(void)
{
    enum { NRULES = 9000 };
    Arena arena;
    SynRule *rules = calloc(NRULES, sizeof(*rules));
    SynCtx ctx = {0U, NRULES, YEW_ATTR_TEXT, SYN_OP_STAY, 0U, 0U, {0}, 0U,
                  {0}};
    SynDef def = {"candidate-cap", 0U, 1U, NRULES, &ctx, rules, NULL};
    SynEngine *engine;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    YewRe *match;
    u32 i;

    YEW_ASSERT_NOT_NULL(rules);
    arena_init(&arena);
    match = yew_re_compile(&arena, ".", 1U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(match);
    (void)memset(ctx.first, 0xff, sizeof(ctx.first));
    for (i = 0U; i < NRULES; i++) {
        rules[i].re = match;
        rules[i].attr = YEW_ATTR_KEYWORD;
        (void)memset(rules[i].caps, 0xff, sizeof(rules[i].caps));
        (void)memset(rules[i].first, 0xff, sizeof(rules[i].first));
    }
    engine = yew_syn_engine_new(&def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"x", 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(out.n, 1U);
    YEW_ASSERT_EQ_U64(out.spans[0].attr, YEW_ATTR_KEYWORD);
    yew_syn_engine_free(engine);
    arena_free_all(&arena);
    free(rules);
}

void test_syn_line_consume_stops_emission_and_optional_aux_is_safe(void)
{
    Arena arena;
    Arena aux_arena;
    Interner aux;
    SynCtx ctx = {0};
    SynRule rules[2] = {{0}};
    SynDef def = {"edge", 0U, 1U, 2U, &ctx, rules, &aux};
    SynEngine *engine;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynState state = {0};
    u32 entry;
    const SynState *exit;

    arena_init(&arena);
    arena_init(&aux_arena);
    interner_init(&aux, &aux_arena);
    state.depth = 1U;
    state.ndef = 1U;
    ctx.nrules = 2U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    ctx.first['a' >> 3U] |= (u8)(1U << ('a' & 7U));
    ctx.first['c' >> 3U] |= (u8)(1U << ('c' & 7U));
    rules[0].re = yew_re_compile(&arena, "(ab)c", 5U, 0U, NULL);
    rules[0].attr = YEW_ATTR_TEXT;
    rules[0].consume = 1U;
    (void)memset(rules[0].caps, 0xff, sizeof(rules[0].caps));
    rules[0].caps[0] = YEW_ATTR_KEYWORD;
    rules[0].first['a' >> 3U] |= (u8)(1U << ('a' & 7U));
    rules[1].re = yew_re_compile(&arena, "c", 1U, 0U, NULL);
    rules[1].attr = YEW_ATTR_OPERATOR;
    (void)memset(rules[1].caps, 0xff, sizeof(rules[1].caps));
    rules[1].first['c' >> 3U] |= (u8)(1U << ('c' & 7U));
    engine = yew_syn_engine_new(&def);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"abc", 3U, &out);
    YEW_ASSERT_EQ_U64(attr_at(&out, 0U), YEW_ATTR_KEYWORD);
    YEW_ASSERT_EQ_U64(attr_at(&out, 1U), YEW_ATTR_KEYWORD);
    YEW_ASSERT_EQ_U64(attr_at(&out, 2U), YEW_ATTR_OPERATOR);
    yew_syn_engine_free(engine);

    (void)memset(&ctx, 0, sizeof(ctx));
    (void)memset(rules, 0, sizeof(rules));
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    ctx.first['b' >> 3U] |= (u8)(1U << ('b' & 7U));
    rules[0].re = yew_re_compile(&arena, "(a)?b", 5U, 0U, NULL);
    rules[0].attr = YEW_ATTR_TEXT;
    rules[0].flags = YEW_SYN_RULE_SET_AUX | YEW_SYN_RULE_STRIP;
    rules[0].aux_group = 1U;
    (void)memset(rules[0].caps, 0xff, sizeof(rules[0].caps));
    rules[0].first['b' >> 3U] |= (u8)(1U << ('b' & 7U));
    engine = yew_syn_engine_new(&def);
    state.aux[0] = yew_intern(&aux, "old", 3U);
    entry = yew_syn_state_intern(yew_syn_engine_states(engine), &state);
    out.n = 0U;
    yew_syn_line(engine, entry, (const u8 *)"b", 1U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(YEW_SYN_AUX_OF(exit), YEW_SYN_AUX_OF(&state));
    YEW_ASSERT_EQ_U64(exit->flags & YEW_SYN_F_STRIP, 0U);
    yew_syn_engine_free(engine);

    (void)memset(&ctx, 0, sizeof(ctx));
    (void)memset(rules, 0, sizeof(rules));
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    ctx.first[' ' >> 3U] |= (u8)(1U << (' ' & 7U));
    rules[0].re = yew_re_compile(&arena, "( +)([1-9]?)I",
                                 strlen("( +)([1-9]?)I"), 0U, NULL);
    YEW_ASSERT_NOT_NULL(rules[0].re);
    rules[0].attr = YEW_ATTR_TEXT;
    rules[0].flags = YEW_SYN_RULE_SET_AUX | YEW_SYN_RULE_AUX_INT;
    rules[0].aux_group = 1U;
    rules[0].aux_add = 1U;
    rules[0].aux_add_group = 2U;
    (void)memset(rules[0].caps, 0xff, sizeof(rules[0].caps));
    rules[0].first[' ' >> 3U] |= (u8)(1U << (' ' & 7U));
    engine = yew_syn_engine_new(&def);
    out.n = 0U;
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"  I", 3U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(YEW_SYN_AUX_OF(exit), 3U);
    out.n = 0U;
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"  7I", 4U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(YEW_SYN_AUX_OF(exit), 9U);
    yew_syn_engine_free(engine);

    rules[0].re = NULL;
    rules[0].flags = 0U;
    rules[0].aux_match = SYN_AUXM_LINE_EQ;
    ctx.first[0] = 0U;
    engine = yew_syn_engine_new(&def);
    state.aux[0] = yew_intern(&aux, "", 0U);
    entry = yew_syn_state_intern(yew_syn_engine_states(engine), &state);
    out.n = 0U;
    yew_syn_line(engine, entry, NULL, 0U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    yew_syn_engine_free(engine);
    interner_free(&aux);
    arena_free_all(&aux_arena);
    arena_free_all(&arena);
}

void test_syn_line_push_list_eol_target_and_clear_aux_pop_laws(void)
{
    Arena arena;
    SynCtx ctxs[3] = {{0}};
    SynRule rule = {0};
    SynDef def = {"transitions", 0U, 3U, 1U, ctxs, &rule, NULL};
    SynEngine *engine;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynState state = {0};
    const SynState *exit;
    u32 entry;

    arena_init(&arena);
    state.aux[0] = 77U;
    state.depth = 2U;
    state.ndef = 1U;
    state.flags = YEW_SYN_F_STRIP;
    ctxs[0].nrules = 1U;
    ctxs[0].dflt_attr = YEW_ATTR_TEXT;
    ctxs[0].first['x' >> 3U] |= (u8)(1U << ('x' & 7U));
    rule.re = yew_re_compile(&arena, "x", 1U, 0U, NULL);
    rule.attr = YEW_ATTR_TEXT;
    rule.op = SYN_OP_PUSH;
    rule.push[0] = 1U;
    rule.push[1] = 2U;
    rule.npush = 2U;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    rule.first['x' >> 3U] |= (u8)(1U << ('x' & 7U));
    engine = yew_syn_engine_new(&def);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"x", 1U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_EQ_U64(exit->depth, 3U);
    YEW_ASSERT_EQ_U64(exit->f[1].ctx, 1U);
    YEW_ASSERT_EQ_U64(exit->f[2].ctx, 2U);
    yew_syn_engine_free(engine);

    rule.op = SYN_OP_POP;
    rule.npush = 0U;
    rule.nop = 1U;
    rule.flags = YEW_SYN_RULE_CLR_AUX;
    engine = yew_syn_engine_new(&def);
    state.f[0].ctx = 0U;
    state.f[1].ctx = 0U;
    entry = yew_syn_state_intern(yew_syn_engine_states(engine), &state);
    out.n = 0U;
    yew_syn_line(engine, entry, (const u8 *)"x", 1U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(YEW_SYN_AUX_OF(exit), 0U);
    YEW_ASSERT_EQ_U64(exit->flags & YEW_SYN_F_STRIP, 0U);

    state.lost = 1U;
    entry = yew_syn_state_intern(yew_syn_engine_states(engine), &state);
    out.n = 0U;
    yew_syn_line(engine, entry, (const u8 *)"x", 1U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_EQ_U64(exit->depth, 2U);
    YEW_ASSERT_EQ_U64(exit->lost, 0U);
    YEW_ASSERT_EQ_U64(YEW_SYN_AUX_OF(exit), 77U);
    YEW_ASSERT(exit->flags & YEW_SYN_F_STRIP);
    yew_syn_engine_free(engine);

    (void)memset(&rule, 0, sizeof(rule));
    ctxs[0].at_eol = SYN_OP_SET;
    ctxs[0].eol_target = 2U;
    engine = yew_syn_engine_new(&def);
    out.n = 0U;
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, NULL, 0U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_EQ_U64(exit->f[0].ctx, 2U);
    yew_syn_engine_free(engine);
    arena_free_all(&arena);
}

static void assert_identifier_fast_path_equal(const char *pattern,
                                              const u8 *line, u32 len)
{
    Arena arena;
    SynCtx ctxs[2] = {{0}};
    SynRule rule = {0};
    SynDef def = {"identifier-fast", 0U, 2U, 1U, ctxs, &rule, NULL};
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[32];
    SynSpan reference_spans[32];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};
    const SynState *fast_state;
    const SynState *reference_state;

    arena_init(&arena);
    rule.re = yew_re_compile(&arena, pattern, strlen(pattern), 0U, NULL);
    YEW_ASSERT_NOT_NULL(rule.re);
    rule.attr = YEW_ATTR_TEXT;
    rule.op = SYN_OP_SET;
    rule.target = 1U;
    rule.consume = 1U;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    rule.caps[1] = YEW_ATTR_FUNCTION;
    ctxs[0].nrules = 1U;
    ctxs[0].dflt_attr = YEW_ATTR_TEXT;
    (void)memset(ctxs[0].first, 0xff, sizeof(ctxs[0].first));
    ctxs[1].dflt_attr = YEW_ATTR_COMMENT;
    fast = yew_syn_engine_new(&def);
    reference = yew_syn_engine_new(&def);
    YEW_ASSERT_EQ_U64(yew_syn_engine_identifier_fast_rules(fast), 1U);
    YEW_ASSERT_EQ_U64(yew_syn_engine_identifier_fast_rules(reference), 1U);
    yew_syn_engine_set_identifier_fast_path(reference, false);

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
    arena_free_all(&arena);
}

void test_syn_line_identifier_suffix_fast_path_matches_regex(void)
{
    static const char *const patterns[] = {
        "([A-Za-z_][A-Za-z0-9_]*)\\s*\\(",
        "([A-Za-z_][A-Za-z0-9_]*)\\s*:"
    };
    static const struct {
        const u8 *bytes;
        u32 len;
    } rows[] = {
        {(const u8 *)"function(", 9U},
        {(const u8 *)"_member42   :", 13U},
        {(const u8 *)"prefix\t(", 8U},
        {(const u8 *)"9prefix(", 8U},
        {(const u8 *)"plain", 5U},
        {(const u8 *)"prefix x(", 9U},
        {(const u8 *)"na\xc3\xafve(", 7U},
        {(const u8 *)"name\xc2\xa0(", 7U},
        {(const u8 *)"bad\xff(", 5U}
    };
    u32 pattern;
    u32 row;

    for (pattern = 0U; pattern < YEW_ARRAY_LEN(patterns); pattern++) {
        for (row = 0U; row < YEW_ARRAY_LEN(rows); row++) {
            assert_identifier_fast_path_equal(patterns[pattern],
                                              rows[row].bytes,
                                              rows[row].len);
        }
    }
}

static void assert_ascii_identifier_fast_path_equal(const char *pattern,
                                                    const u8 *line, u32 len)
{
    Arena arena;
    SynCtx ctx = {0};
    SynRule rule = {0};
    SynDef def = {"ascii-identifier-fast", 0U, 1U, 1U, &ctx, &rule, NULL};
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[16];
    SynSpan reference_spans[16];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};

    arena_init(&arena);
    rule.re = yew_re_compile(&arena, pattern, strlen(pattern), 0U, NULL);
    YEW_ASSERT_NOT_NULL(rule.re);
    rule.attr = YEW_ATTR_VARIABLE;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    (void)memset(ctx.first, 0xff, sizeof(ctx.first));
    fast = yew_syn_engine_new(&def);
    reference = yew_syn_engine_new(&def);
    YEW_ASSERT_EQ_U64(yew_syn_engine_identifier_fast_rules(fast), 1U);
    YEW_ASSERT_EQ_U64(yew_syn_engine_identifier_fast_rules(reference), 1U);
    yew_syn_engine_set_identifier_fast_path(reference, false);

    yew_syn_line(fast, YEW_SYN_STATE_ROOT, line, len, &fast_out);
    yew_syn_line(reference, YEW_SYN_STATE_ROOT, line, len, &reference_out);
    YEW_ASSERT_EQ_U64(fast_out.stop, reference_out.stop);
    YEW_ASSERT_EQ_U64(fast_out.exit_state, reference_out.exit_state);
    YEW_ASSERT_EQ_U64(fast_out.n, reference_out.n);
    YEW_ASSERT_EQ_MEM(fast_out.spans, reference_out.spans,
                      fast_out.n * sizeof(*fast_out.spans));

    yew_syn_engine_free(reference);
    yew_syn_engine_free(fast);
    arena_free_all(&arena);
}

static void test_yaml_block_key_fast_path_matrix(void);

void test_syn_line_ascii_identifier_fast_path_matches_regex(void)
{
    static const char *const patterns[] = {
        "\\b[A-Z][A-Z0-9_]*\\b",
        "\\b[A-Z][A-Za-z0-9]*\\b",
        "\\b[A-Za-z_][A-Za-z0-9_]*\\b",
        "'[A-Za-z_][A-Za-z0-9_]*",
        "r#[A-Za-z_][A-Za-z0-9_]*"
    };
    static const char *const rows[] = {
        "CONST_42!", "Camel42 ", "plain_name:", "'static str",
        "r#type,", "word_more", "9bad", "A_b", "na\xc3\xafve"
    };
    u32 pattern;
    u32 row;

    for (pattern = 0U; pattern < YEW_ARRAY_LEN(patterns); pattern++) {
        for (row = 0U; row < YEW_ARRAY_LEN(rows); row++)
            assert_ascii_identifier_fast_path_equal(
                patterns[pattern], (const u8 *)rows[row],
                (u32)strlen(rows[row]));
    }
    test_yaml_block_key_fast_path_matrix();
}

static void assert_word_literal_fast_path_equal(const char *pattern,
                                                u32 flags, u32 fast_rules,
                                                const u8 *line, u32 len)
{
    Arena arena;
    SynCtx ctx = {0};
    SynRule rule = {0};
    SynDef def = {"word-fast", 0U, 1U, 1U, &ctx, &rule, NULL};
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[16];
    SynSpan reference_spans[16];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};
    const SynState *fast_state;
    const SynState *reference_state;

    arena_init(&arena);
    rule.re = yew_re_compile(&arena, pattern, strlen(pattern), flags, NULL);
    YEW_ASSERT_NOT_NULL(rule.re);
    rule.attr = YEW_ATTR_KEYWORD;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    (void)memset(ctx.first, 0xff, sizeof(ctx.first));
    fast = yew_syn_engine_new(&def);
    reference = yew_syn_engine_new(&def);
    YEW_ASSERT_EQ_U64(yew_syn_engine_word_literal_fast_rules(fast),
                      fast_rules);
    YEW_ASSERT_EQ_U64(yew_syn_engine_word_literal_fast_rules(reference),
                      fast_rules);
    yew_syn_engine_set_word_literal_fast_path(reference, false);

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
    arena_free_all(&arena);
}

void test_syn_line_word_literal_fast_path_matches_regex(void)
{
    char exponential_pattern[256];
    static const char long_pattern[] =
        "\\b(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa)\\b";
    static const char long_word[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const struct {
        const u8 *bytes;
        u32 len;
    } rows[] = {
        {(const u8 *)"alpha", 5U},
        {(const u8 *)"BeTa", 4U},
        {(const u8 *)"gamma!", 6U},
        {(const u8 *)"alphabet", 8U},
        {(const u8 *)"x beta", 6U},
        {(const u8 *)"b\xc3\xa9ta", 5U}
    };
    u32 row;
    u32 at = 0U;

    for (row = 0U; row < YEW_ARRAY_LEN(rows); row++) {
        assert_word_literal_fast_path_equal("\\b(alpha|beta|gamma)\\b",
                                            YEW_RE_ICASE, 1U,
                                            rows[row].bytes, rows[row].len);
    }
    assert_word_literal_fast_path_equal(long_pattern, 0U, 1U,
                                        (const u8 *)long_word,
                                        (u32)strlen(long_word));
    exponential_pattern[at++] = '\\';
    exponential_pattern[at++] = 'b';
    for (row = 0U; row < 13U; row++) {
        static const char branch[] = "(?:a|b)";

        (void)memcpy(exponential_pattern + at, branch,
                     sizeof(branch) - 1U);
        at += (u32)sizeof(branch) - 1U;
    }
    exponential_pattern[at++] = '\\';
    exponential_pattern[at++] = 'b';
    exponential_pattern[at] = '\0';
    assert_word_literal_fast_path_equal(
        exponential_pattern, 0U, 0U,
        (const u8 *)"aaaaaaaaaaaaa", 13U);
}

static void assert_finite_literal_fast_path_equal(const char *pattern,
                                                  const u8 *line, u32 len)
{
    Arena arena;
    SynCtx ctx = {0};
    SynRule rule = {0};
    SynDef def = {"finite-fast", 0U, 1U, 1U, &ctx, &rule, NULL};
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[16];
    SynSpan reference_spans[16];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};

    arena_init(&arena);
    rule.re = yew_re_compile(&arena, pattern, strlen(pattern), 0U, NULL);
    YEW_ASSERT_NOT_NULL(rule.re);
    rule.attr = YEW_ATTR_OPERATOR;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    rule.caps[1] = YEW_ATTR_PUNCT_DELIM;
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    (void)memset(ctx.first, 0xff, sizeof(ctx.first));
    fast = yew_syn_engine_new(&def);
    reference = yew_syn_engine_new(&def);
    YEW_ASSERT_EQ_U64(yew_syn_engine_finite_literal_fast_rules(fast), 1U);
    YEW_ASSERT_EQ_U64(
        yew_syn_engine_finite_literal_fast_rules(reference), 1U);
    yew_syn_engine_set_finite_literal_fast_path(reference, false);

    yew_syn_line(fast, YEW_SYN_STATE_ROOT, line, len, &fast_out);
    yew_syn_line(reference, YEW_SYN_STATE_ROOT, line, len, &reference_out);
    YEW_ASSERT_EQ_U64(fast_out.stop, reference_out.stop);
    YEW_ASSERT_EQ_U64(fast_out.exit_state, reference_out.exit_state);
    YEW_ASSERT_EQ_U64(fast_out.n, reference_out.n);
    YEW_ASSERT_EQ_MEM(fast_out.spans, reference_out.spans,
                      fast_out.n * sizeof(*fast_out.spans));

    yew_syn_engine_free(reference);
    yew_syn_engine_free(fast);
    arena_free_all(&arena);
}

static void assert_json_key_fast_path_equal(const char *pattern,
                                            const u8 *line, u32 len,
                                            u32 indexed, bool matched)
{
    Arena arena;
    SynCtx ctx = {0};
    SynRule rule = {0};
    SynDef def = {"json-key-fast", 0U, 1U, 1U, &ctx, &rule, NULL};
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[16];
    SynSpan reference_spans[16];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};
    u32 colon = len;
    u32 i;

    arena_init(&arena);
    rule.re = yew_re_compile(&arena, pattern, strlen(pattern), 0U, NULL);
    YEW_ASSERT_NOT_NULL(rule.re);
    rule.attr = YEW_ATTR_TEXT;
    rule.consume = 1U;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    rule.caps[1] = YEW_ATTR_VARIABLE_MEMBER;
    rule.caps[2] = YEW_ATTR_STRING_ESCAPE;
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    (void)memset(ctx.first, 0xff, sizeof(ctx.first));
    fast = yew_syn_engine_new(&def);
    reference = yew_syn_engine_new(&def);
    YEW_ASSERT_EQ_U64(yew_syn_engine_json_key_fast_rules(fast), indexed);
    YEW_ASSERT_EQ_U64(yew_syn_engine_json_key_fast_rules(reference),
                      indexed);
    yew_syn_engine_set_json_key_fast_path(reference, false);

    yew_syn_line(fast, YEW_SYN_STATE_ROOT, line, len, &fast_out);
    yew_syn_line(reference, YEW_SYN_STATE_ROOT, line, len, &reference_out);
    YEW_ASSERT_EQ_U64(fast_out.stop, reference_out.stop);
    YEW_ASSERT_EQ_U64(fast_out.exit_state, reference_out.exit_state);
    YEW_ASSERT_EQ_U64(fast_out.n, reference_out.n);
    YEW_ASSERT_EQ_MEM(fast_out.spans, reference_out.spans,
                      fast_out.n * sizeof(*fast_out.spans));
    if (matched) {
        for (i = 0U; i < len; i++) {
            if (line[i] == (u8)':')
                colon = i;
        }
        YEW_ASSERT(colon < len);
        YEW_ASSERT_EQ_U64(attr_at(&fast_out, 0U),
                          YEW_ATTR_VARIABLE_MEMBER);
        YEW_ASSERT_EQ_U64(attr_at(&fast_out, colon), YEW_ATTR_TEXT);
    }

    yew_syn_engine_free(reference);
    yew_syn_engine_free(fast);
    arena_free_all(&arena);
}

static void assert_yaml_block_key_fast_path_equal(const char *pattern,
                                                  const u8 *line, u32 len,
                                                  u32 indexed)
{
    Arena arena;
    SynCtx ctx = {0};
    SynRule rule = {0};
    SynDef def = {"yaml-block-key-fast", 0U, 1U, 1U, &ctx, &rule, NULL};
    SynEngine *fast;
    SynEngine *reference;
    SynSpan fast_spans[16];
    SynSpan reference_spans[16];
    SynLineOut fast_out = {fast_spans, 0U, YEW_ARRAY_LEN(fast_spans),
                           0U, 0U};
    SynLineOut reference_out = {
        reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};

    arena_init(&arena);
    rule.re = yew_re_compile(&arena, pattern, strlen(pattern), 0U, NULL);
    YEW_ASSERT_NOT_NULL(rule.re);
    rule.attr = YEW_ATTR_TEXT;
    rule.consume = 3U;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    rule.caps[2] = YEW_ATTR_VARIABLE_MEMBER;
    rule.caps[3] = YEW_ATTR_PUNCT_DELIM;
    ctx.nrules = 1U;
    ctx.dflt_attr = YEW_ATTR_TEXT;
    (void)memset(ctx.first, 0xff, sizeof(ctx.first));
    fast = yew_syn_engine_new(&def);
    reference = yew_syn_engine_new(&def);
    YEW_ASSERT_EQ_U64(yew_syn_engine_yaml_block_key_fast_rules(fast),
                      indexed);
    YEW_ASSERT_EQ_U64(
        yew_syn_engine_yaml_block_key_fast_rules(reference), indexed);
    yew_syn_engine_set_yaml_block_key_fast_path(reference, false);

    yew_syn_line(fast, YEW_SYN_STATE_ROOT, line, len, &fast_out);
    yew_syn_line(reference, YEW_SYN_STATE_ROOT, line, len, &reference_out);
    YEW_ASSERT_EQ_U64(fast_out.stop, reference_out.stop);
    YEW_ASSERT_EQ_U64(fast_out.exit_state, reference_out.exit_state);
    YEW_ASSERT_EQ_U64(fast_out.n, reference_out.n);
    YEW_ASSERT_EQ_MEM(fast_out.spans, reference_out.spans,
                      fast_out.n * sizeof(*fast_out.spans));

    yew_syn_engine_free(reference);
    yew_syn_engine_free(fast);
    arena_free_all(&arena);
}

static void test_yaml_block_key_fast_path_matrix(void)
{
    static const char yaml_key[] =
        "^(\\s*)([^\\s#][^:#]*)(:)(\\s|$)";
    static const char yaml_near_miss[] =
        "^(\\s*)([^\\s#][^:#]*)(:)(\\s*)";
    static const u8 invalid_key[] = {
        'b', 'a', 'd', 0xffU, ':', ' ', 'x'
    };
    static const struct {
        const u8 *bytes;
        u32 len;
    } rows[] = {
        {(const u8 *)"key: value", 10U},
        {(const u8 *)"           flow: [a, {b: [c, {d: e}]}]", 38U},
        {(const u8 *)"key:", 4U},
        {(const u8 *)" key:\tvalue", 11U},
        {(const u8 *)" key : value", 12U},
        {(const u8 *)"key#bad: value", 14U},
        {(const u8 *)" #comment", 9U},
        {(const u8 *)" : value", 8U},
        {(const u8 *)"key:value", 9U},
        {(const u8 *)"na\xc3\xafve: value", 13U},
        {(const u8 *)"\xc2\xa0key: value", 12U},
        {invalid_key, YEW_ARRAY_LEN(invalid_key)}
    };
    u32 row;

    for (row = 0U; row < YEW_ARRAY_LEN(rows); row++) {
        assert_yaml_block_key_fast_path_equal(
            yaml_key, rows[row].bytes, rows[row].len, 1U);
    }
    assert_yaml_block_key_fast_path_equal(
        yaml_near_miss, (const u8 *)"key:value", 9U, 0U);
    {
        Arena arena;
        SynCtx ctx = {0};
        SynRule rules[2] = {{0}};
        SynDef def = {"yaml", 0U, 1U, 2U, &ctx, rules, NULL};
        SynEngine *fast;
        SynEngine *reference;
        SynSpan fast_spans[8];
        SynSpan reference_spans[8];
        SynLineOut fast_out = {
            fast_spans, 0U, YEW_ARRAY_LEN(fast_spans), 0U, 0U};
        SynLineOut reference_out = {
            reference_spans, 0U, YEW_ARRAY_LEN(reference_spans), 0U, 0U};
        const u8 line[] = "key: value";

        arena_init(&arena);
        rules[0].re = yew_re_compile(&arena, "^key: value$", 12U, 0U,
                                     NULL);
        rules[1].re = yew_re_compile(&arena, yaml_key,
                                     sizeof(yaml_key) - 1U, 0U, NULL);
        YEW_ASSERT_NOT_NULL(rules[0].re);
        YEW_ASSERT_NOT_NULL(rules[1].re);
        rules[0].attr = YEW_ATTR_ERROR;
        rules[1].attr = YEW_ATTR_TEXT;
        rules[1].consume = 3U;
        (void)memset(rules[0].caps, 0xff, sizeof(rules[0].caps));
        (void)memset(rules[1].caps, 0xff, sizeof(rules[1].caps));
        rules[1].caps[2] = YEW_ATTR_VARIABLE_MEMBER;
        rules[1].caps[3] = YEW_ATTR_PUNCT_DELIM;
        ctx.nrules = YEW_ARRAY_LEN(rules);
        ctx.dflt_attr = YEW_ATTR_TEXT;
        (void)memset(ctx.first, 0xff, sizeof(ctx.first));
        fast = yew_syn_engine_new(&def);
        reference = yew_syn_engine_new(&def);
        YEW_ASSERT_EQ_U64(yew_syn_engine_yaml_block_key_fast_rules(fast),
                          1U);
        yew_syn_engine_set_yaml_block_key_fast_path(reference, false);
        yew_syn_line(fast, YEW_SYN_STATE_ROOT, line, sizeof(line) - 1U,
                     &fast_out);
        yew_syn_line(reference, YEW_SYN_STATE_ROOT, line,
                     sizeof(line) - 1U, &reference_out);
        YEW_ASSERT_EQ_U64(fast_out.n, reference_out.n);
        YEW_ASSERT_EQ_MEM(fast_out.spans, reference_out.spans,
                          fast_out.n * sizeof(*fast_out.spans));
        YEW_ASSERT_EQ_U64(attr_at(&fast_out, 0U), YEW_ATTR_ERROR);
        yew_syn_engine_free(reference);
        yew_syn_engine_free(fast);
        arena_free_all(&arena);
    }
}

void test_syn_line_finite_literal_fast_path_matches_regex(void)
{
    static const char operator_pattern[] =
        "(=>|->|::|\\.\\.=|\\.\\.|<<=|>>=|<<|>>|&&|\\|\\||==|!=|"
        "<=|>=|\\+=|-=|\\*=|/=|%=|&=|\\|=|\\^=|[?+*/%&|^!~=<>-])";
    static const char *const rows[] = {
        "=> rest", "..=", ".", "^=", "?", "word", "\xc3\xa9"
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++)
        assert_finite_literal_fast_path_equal(
            operator_pattern, (const u8 *)rows[i],
            (u32)strlen(rows[i]));
    assert_finite_literal_fast_path_equal("[(){}\\[\\]]",
                                          (const u8 *)"]x", 2U);
    /* Left branch priority is semantic when one literal prefixes another. */
    assert_finite_literal_fast_path_equal("(=|==)",
                                          (const u8 *)"==", 2U);
    {
        static const char json_key[] =
            "(\"(\\\\[\"\\\\/bfnrt]|\\\\u[0-9A-Fa-f]{4}|"
            "[^\"\\\\\\x00-\\x1F])*\")\\s*:";
        static const char json_equal[] =
            "(\"(\\\\[\"\\\\/bfnrt]|\\\\u[0-9A-Fa-f]{4}|"
            "[^\"\\\\\\x00-\\x1F])*\")\\s*=";
        static const u8 raw_control[] = {'"', 'a', 0x1fU, '"', ':', '0'};

        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"name\": 1", 9U, 1U, true);
        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"\":0", 4U, 1U, true);
        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"a\\\"b\" \t: 1", 11U, 1U, true);
        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"u\\u0041\":0", 11U, 1U, true);
        /* Non-ASCII keys are deliberately delegated to the regex VM. */
        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"na\xc3\xafve\":1", 10U, 1U, true);
        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"bad\\q\":0", 9U, 1U, false);
        assert_json_key_fast_path_equal(
            json_key, raw_control, YEW_ARRAY_LEN(raw_control), 1U, false);
        assert_json_key_fast_path_equal(
            json_key, (const u8 *)"\"value\"", 7U, 1U, false);
        assert_json_key_fast_path_equal(
            json_equal, (const u8 *)"\"name\"=1", 8U, 0U, false);
    }
}

void test_syn_line_coverage_is_optional_and_excludes_stack_probes(void)
{
    SynToy toy;
    SynCoverage coverage;
    SynSpan spans[16];
    SynLineOut out;
    SynState at;
    u64 context_hits;
    u64 rule_hits;
    u32 i;

    syn_toy_init(&toy);
    YEW_ASSERT(yew_syn_coverage_init(&coverage, &toy.def));
    yew_syn_engine_set_coverage(toy.engine, &coverage);
    (void)syn_toy_line(&toy, YEW_SYN_STATE_ROOT, "\"a\\nb\"", spans,
                       YEW_ARRAY_LEN(spans), &out);
    YEW_ASSERT_EQ_U64(coverage.contexts[SYN_TOY_MAIN], 2U);
    YEW_ASSERT_EQ_U64(coverage.contexts[SYN_TOY_STRING], 1U);
    YEW_ASSERT_EQ_U64(coverage.rules[2], 1U);
    YEW_ASSERT_EQ_U64(coverage.rules[7], 1U);
    YEW_ASSERT_EQ_U64(coverage.rules[8], 1U);

    context_hits = 0U;
    for (i = 0U; i < coverage.nctxs; i++)
        context_hits += coverage.contexts[i];
    rule_hits = 0U;
    for (i = 0U; i < coverage.nrules; i++)
        rule_hits += coverage.rules[i];
    YEW_ASSERT(yew_syn_stack_at(toy.engine, YEW_SYN_STATE_ROOT,
                                (const u8 *)"\"a\\nb\"", 6U, 3U, &at));
    for (i = 0U; i < coverage.nctxs; i++)
        context_hits -= coverage.contexts[i];
    for (i = 0U; i < coverage.nrules; i++)
        rule_hits -= coverage.rules[i];
    YEW_ASSERT_EQ_U64(context_hits, 0U);
    YEW_ASSERT_EQ_U64(rule_hits, 0U);

    yew_syn_coverage_clear(&coverage);
    for (i = 0U; i < coverage.nctxs; i++)
        YEW_ASSERT_EQ_U64(coverage.contexts[i], 0U);
    for (i = 0U; i < coverage.nrules; i++)
        YEW_ASSERT_EQ_U64(coverage.rules[i], 0U);
    yew_syn_engine_set_coverage(toy.engine, NULL);
    yew_syn_coverage_free(&coverage);
    syn_toy_free(&toy);
}

void test_syn_line_line_eq_ws_ignores_only_leading_ascii_space_and_tab(void)
{
    static const struct {
        const char *line;
        bool matches;
    } cases[] = {
        {"END", true},
        {" END", true},
        {"\t \tEND", true},
        {"\vEND", false},
        {"END ", false},
        {"end", false},
    };
    Arena aux_arena;
    Interner aux;
    SynCtx ctxs[2] = {{0}};
    SynRule rule = {0};
    SynDef def = {"line-eq-ws", 0U, 2U, 1U, ctxs, &rule, &aux};
    SynEngine *engine;
    SynState state = {0};
    u32 entry;
    size_t i;

    arena_init(&aux_arena);
    interner_init(&aux, &aux_arena);
    ctxs[0].dflt_attr = YEW_ATTR_TEXT;
    ctxs[1].nrules = 1U;
    ctxs[1].dflt_attr = YEW_ATTR_STRING_SPECIAL;
    (void)memset(ctxs[1].first, 0xff, sizeof(ctxs[1].first));
    rule.attr = YEW_ATTR_STRING;
    rule.op = SYN_OP_POP;
    rule.nop = 1U;
    rule.aux_match = SYN_AUXM_LINE_EQ_WS;
    (void)memset(rule.caps, 0xff, sizeof(rule.caps));
    (void)memset(rule.first, 0xff, sizeof(rule.first));
    state.f[0].ctx = 0U;
    state.f[1].ctx = 1U;
    state.depth = 2U;
    state.ndef = 1U;
    state.aux[0] = yew_intern(&aux, "END", 3U);
    engine = yew_syn_engine_new(&def);
    entry = yew_syn_state_intern(yew_syn_engine_states(engine), &state);

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        SynSpan spans[8];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        const SynState *exit;

        yew_syn_line(engine, entry, (const u8 *)cases[i].line,
                     (u32)strlen(cases[i].line), &out);
        exit = yew_syn_state_get(yew_syn_engine_states(engine),
                                 out.exit_state);
        YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
        YEW_ASSERT_NOT_NULL(exit);
        YEW_ASSERT_EQ_U64(exit->depth, cases[i].matches ? 1U : 2U);
        YEW_ASSERT_EQ_U64(attr_at(&out, 0U),
                          cases[i].matches ? YEW_ATTR_STRING :
                                            YEW_ATTR_STRING_SPECIAL);
    }
    yew_syn_engine_free(engine);
    interner_free(&aux);
    arena_free_all(&aux_arena);
}
