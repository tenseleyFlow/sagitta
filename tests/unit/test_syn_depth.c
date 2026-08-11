#include "harness.h"

#include <string.h>

#include "syn/defs.h"
#include "syn/engine.h"

static u32 rust_line(SynEngine *engine, u32 entry, const char *line)
{
    SynSpan spans[128];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    u32 i;

    yew_syn_line(engine, entry, (const u8 *)line, (u32)strlen(line), &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    for (i = 0U; i < out.n; i++) {
        YEW_ASSERT(out.spans[i].len != 0U);
        YEW_ASSERT(out.spans[i].attr < YEW_ATTR__COUNT);
    }
    return out.exit_state;
}

void test_syn_depth_rust_twenty_nested_comments_balance_exactly(void)
{
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named("rust"));
    SynEngine *engine;
    const SynState *entry;
    const SynState *deep;
    const SynState *exit;
    u32 entry_id;
    u32 deep_id;
    u32 exit_id;

    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    entry_id = rust_line(engine, YEW_SYN_STATE_ROOT, "let warm = 1;");
    entry = yew_syn_state_get(yew_syn_engine_states(engine), entry_id);
    YEW_ASSERT_NOT_NULL(entry);
    deep_id = rust_line(engine, entry_id,
        "/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/* body");
    deep = yew_syn_state_get(yew_syn_engine_states(engine), deep_id);
    YEW_ASSERT_NOT_NULL(deep);
    YEW_ASSERT_EQ_U64(deep->depth, YEW_SYN_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(deep->lost, 20U - (YEW_SYN_DEPTH_MAX - 1U));
    YEW_ASSERT_EQ_U64(deep->def, 0U);
    exit_id = rust_line(engine, deep_id,
        "*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/*/");
    exit = yew_syn_state_get(yew_syn_engine_states(engine), exit_id);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_MEM(exit, entry, sizeof(*entry));
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(exit->lost, 0U);
    YEW_ASSERT_EQ_U64(exit->def, 0U);
    yew_syn_engine_free(engine);
}

void test_syn_depth_rust_recovers_after_shallow_edit(void)
{
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named("rust"));
    SynEngine *engine;
    const SynState *entry;
    const SynState *open;
    const SynState *exit;
    u32 entry_id;
    u32 open_id;
    u32 exit_id;

    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    entry_id = rust_line(engine, YEW_SYN_STATE_ROOT, "let warm = 1;");
    entry = yew_syn_state_get(yew_syn_engine_states(engine), entry_id);
    YEW_ASSERT_NOT_NULL(entry);
    open_id = rust_line(engine, entry_id, "/*/*/* edited");
    open = yew_syn_state_get(yew_syn_engine_states(engine), open_id);
    YEW_ASSERT_NOT_NULL(open);
    YEW_ASSERT_EQ_U64(open->depth, 4U);
    YEW_ASSERT_EQ_U64(open->lost, 0U);
    YEW_ASSERT_EQ_U64(open->def, 0U);
    exit_id = rust_line(engine, open_id, "*/*/*/");
    exit = yew_syn_state_get(yew_syn_engine_states(engine), exit_id);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_MEM(exit, entry, sizeof(*entry));
    YEW_ASSERT_EQ_U64(exit->lost, 0U);
    YEW_ASSERT_EQ_U64(exit->def, 0U);
    yew_syn_engine_free(engine);
}
