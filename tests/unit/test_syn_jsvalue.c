#include "harness.h"

#include <string.h>

#include "syn/attr.h"
#include "syn/defs.h"
#include "syn/engine.h"

typedef struct JsValueCase {
    const char *line;
    u32 slash;
    u8 attr;
} JsValueCase;

static u8 js_attr_at(const SynLineOut *out, u32 off)
{
    u32 i;

    for (i = 0U; i < out->n; i++) {
        u32 hi = out->spans[i].start + out->spans[i].len;

        if (off >= out->spans[i].start && off < hi)
            return out->spans[i].attr;
    }
    return UINT8_MAX;
}

static void js_assert_cases(const JsValueCase *cases, size_t n)
{
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named("javascript"));
    SynEngine *engine;
    size_t i;

    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    for (i = 0U; i < n; i++) {
        SynSpan spans[64];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        const SynState *exit;

        yew_syn_line(engine, YEW_SYN_STATE_ROOT,
                     (const u8 *)cases[i].line,
                     (u32)strlen(cases[i].line), &out);
        exit = yew_syn_state_get(yew_syn_engine_states(engine),
                                 out.exit_state);
        YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
        YEW_ASSERT_NOT_NULL(exit);
        YEW_ASSERT_EQ_U64(js_attr_at(&out, cases[i].slash), cases[i].attr);
        YEW_ASSERT_EQ_U64(exit->def, 0U);
        YEW_ASSERT((exit->flags & YEW_SYN_F_VALUE) == 0U);
    }
    yew_syn_engine_free(engine);
}

void test_syn_jsvalue_value_tokens_choose_division(void)
{
    static const JsValueCase cases[] = {
        {"identifier / 2", 11U, YEW_ATTR_OPERATOR},
        {"42 / 2", 3U, YEW_ATTR_OPERATOR},
        {"\"text\" / 2", 7U, YEW_ATTR_OPERATOR},
        {"`text` / 2", 7U, YEW_ATTR_OPERATOR},
        {"/re/ / 2", 5U, YEW_ATTR_OPERATOR},
        {") / 2", 2U, YEW_ATTR_OPERATOR},
        {"] / 2", 2U, YEW_ATTR_OPERATOR},
        {"} / 2", 2U, YEW_ATTR_OPERATOR},
        {"this / 2", 5U, YEW_ATTR_OPERATOR},
        {"super / 2", 6U, YEW_ATTR_OPERATOR},
        {"true / 2", 5U, YEW_ATTR_OPERATOR},
        {"false / 2", 6U, YEW_ATTR_OPERATOR},
        {"null / 2", 5U, YEW_ATTR_OPERATOR},
        {"undefined / 2", 10U, YEW_ATTR_OPERATOR},
        {"x++ / 2", 4U, YEW_ATTR_OPERATOR},
        {"x-- / 2", 4U, YEW_ATTR_OPERATOR},
    };

    js_assert_cases(cases, YEW_ARRAY_LEN(cases));
}

void test_syn_jsvalue_nonvalue_tokens_choose_regex(void)
{
    static const JsValueCase cases[] = {
        {"/re/", 0U, YEW_ATTR_STRING_SPECIAL},
        {"+ /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {"( /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {"[ /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {", /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {"; /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {": /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {"=> /re/", 3U, YEW_ATTR_STRING_SPECIAL},
        {"{ /re/", 2U, YEW_ATTR_STRING_SPECIAL},
        {"return /re/", 7U, YEW_ATTR_STRING_SPECIAL},
        {"typeof /re/", 7U, YEW_ATTR_STRING_SPECIAL},
        {"instanceof /re/", 11U, YEW_ATTR_STRING_SPECIAL},
        {"in /re/", 3U, YEW_ATTR_STRING_SPECIAL},
        {"of /re/", 3U, YEW_ATTR_STRING_SPECIAL},
        {"new /re/", 4U, YEW_ATTR_STRING_SPECIAL},
        {"delete /re/", 7U, YEW_ATTR_STRING_SPECIAL},
        {"void /re/", 5U, YEW_ATTR_STRING_SPECIAL},
        {"case /re/", 5U, YEW_ATTR_STRING_SPECIAL},
        {"do /re/", 3U, YEW_ATTR_STRING_SPECIAL},
        {"else /re/", 5U, YEW_ATTR_STRING_SPECIAL},
        {"yield /re/", 6U, YEW_ATTR_STRING_SPECIAL},
        {"await /re/", 6U, YEW_ATTR_STRING_SPECIAL},
    };

    js_assert_cases(cases, YEW_ARRAY_LEN(cases));
}

void test_syn_jsvalue_known_failures_are_line_bounded(void)
{
    static const JsValueCase failures[] = {
        {"if (x) /re/.test(s)", 7U, YEW_ATTR_OPERATOR},
        {"if (x) {} /re/.test(s)", 10U, YEW_ATTR_OPERATOR},
    };
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named("javascript"));
    SynEngine *engine;
    SynSpan spans[64];
    SynLineOut first = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynLineOut second = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *exit;

    js_assert_cases(failures, YEW_ARRAY_LEN(failures));
    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)failures[0].line,
                 (u32)strlen(failures[0].line), &first);
    yew_syn_line(engine, first.exit_state, (const u8 *)"/ok/", 4U,
                 &second);
    exit = yew_syn_state_get(yew_syn_engine_states(engine),
                             second.exit_state);
    YEW_ASSERT_EQ_U64(second.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(js_attr_at(&second, 0U), YEW_ATTR_STRING_SPECIAL);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(exit->lost, 0U);
    YEW_ASSERT_EQ_U64(exit->def, 0U);
    YEW_ASSERT((exit->flags & YEW_SYN_F_VALUE) == 0U);
    yew_syn_engine_free(engine);
}
