#include "harness.h"

#include <string.h>

#include "syn/defs.h"
#include "syn/engine.h"

static SynState syn_root_state(u16 ctx, u8 def)
{
    SynState state;

    (void)memset(&state, 0, sizeof(state));
    state.f[0] = (SynFrame){ctx, def, 0U};
    state.depth = 1U;
    state.ndef = 1U;
    return state;
}

void test_syn_state_root_is_reserved_and_canonical(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(7U);
    const SynState *root = yew_syn_state_get(tab, YEW_SYN_STATE_ROOT);
    SynState copy;

    YEW_ASSERT_NOT_NULL(tab);
    YEW_ASSERT_NULL(yew_syn_state_get(tab, YEW_SYN_STATE_UNKNOWN));
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_U64(root->depth, 1U);
    YEW_ASSERT_EQ_U64(root->ndef, 1U);
    YEW_ASSERT_EQ_U64(root->f[0].ctx, 7U);
    YEW_ASSERT_EQ_U64(root->f[0].def, 0U);
    YEW_ASSERT_EQ_U64(root->lost, 0U);
    copy = *root;
    YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &copy), YEW_SYN_STATE_ROOT);
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), 2U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_equal_tuples_share_identity(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = syn_root_state(0U, 0U);
    u32 first;
    u32 i;

    state.aux[0] = 91U;
    state.flags = YEW_SYN_F_VALUE;
    first = yew_syn_state_intern(tab, &state);
    YEW_ASSERT(first > YEW_SYN_STATE_ROOT);
    for (i = 0U; i < 40U; i++)
        YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &state), first);
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), 3U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_different_tuples_have_different_identities(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = syn_root_state(0U, 0U);
    u32 ids[40];
    u32 i;
    u32 j;

    for (i = 0U; i < YEW_ARRAY_LEN(ids); i++) {
        state.aux[0] = i + 1U;
        ids[i] = yew_syn_state_intern(tab, &state);
        YEW_ASSERT(ids[i] > YEW_SYN_STATE_ROOT);
        for (j = 0U; j < i; j++)
            YEW_ASSERT(ids[i] != ids[j]);
    }
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), 42U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_depth_cap_balances_refused_pushes(void)
{
    SynState state = syn_root_state(0U, 2U);
    SynState entry = state;
    u32 i;

    for (i = 0U; i < 40U; i++)
        yew_syn_state_push(&state, (u16)(i + 1U));
    YEW_ASSERT_EQ_U64(state.depth, YEW_SYN_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(state.lost, 40U - (YEW_SYN_DEPTH_MAX - 1U));
    for (i = 0U; i < YEW_SYN_DEPTH_MAX; i++)
        YEW_ASSERT_EQ_U64(state.f[i].def, 2U);
    for (i = 0U; i < 40U; i++)
        yew_syn_state_pop(&state, 1U);
    YEW_ASSERT_EQ_MEM(&state, &entry, sizeof(state));
}

void test_syn_state_lost_saturates_and_marks_degraded(void)
{
    SynState state = syn_root_state(0U, 0U);
    u32 i;

    for (i = 1U; i < YEW_SYN_DEPTH_MAX; i++)
        yew_syn_state_push(&state, (u16)i);
    for (i = 0U; i < 300U; i++)
        yew_syn_state_push(&state, 99U);
    YEW_ASSERT_EQ_U64(state.depth, YEW_SYN_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(state.lost, YEW_SYN_LOST_MAX);
    YEW_ASSERT((state.flags & YEW_SYN_F_DEGRADED) != 0U);
    yew_syn_state_pop(&state, 4U);
    YEW_ASSERT_EQ_U64(state.lost, YEW_SYN_LOST_MAX - 4U);
    YEW_ASSERT_EQ_U64(state.depth, YEW_SYN_DEPTH_MAX);
}

void test_syn_state_pop_never_removes_root(void)
{
    SynState state = syn_root_state(17U, 3U);
    SynState entry;

    state.aux[0] = 12U;
    state.flags = YEW_SYN_F_VALUE;
    entry = state;
    yew_syn_state_pop(&state, 1U);
    YEW_ASSERT_EQ_MEM(&state, &entry, sizeof(state));
    yew_syn_state_pop(&state, 4U);
    YEW_ASSERT_EQ_MEM(&state, &entry, sizeof(state));
}

void test_syn_state_set_replaces_top_without_changing_depth(void)
{
    SynState state = syn_root_state(3U, 2U);
    const SynFrame zero = {0U, 0U, 0U};

    yew_syn_state_push(&state, 4U);
    yew_syn_state_push(&state, 5U);
    state.f[2].fl = YEW_SYN_FR_BRIDGE;
    yew_syn_state_set(&state, 77U);
    YEW_ASSERT_EQ_U64(state.depth, 3U);
    YEW_ASSERT_EQ_U64(state.f[0].ctx, 3U);
    YEW_ASSERT_EQ_U64(state.f[1].ctx, 4U);
    YEW_ASSERT_EQ_U64(state.f[2].ctx, 77U);
    YEW_ASSERT_EQ_U64(state.f[2].def, 2U);
    YEW_ASSERT_EQ_U64(state.f[2].fl, YEW_SYN_FR_BRIDGE);
    yew_syn_state_pop(&state, 1U);
    YEW_ASSERT_EQ_MEM(&state.f[2], &zero, sizeof(state.f[2]));
}

void test_syn_state_table_exhaustion_degrades_to_root(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = syn_root_state(0U, 0U);
    u32 last = 0U;
    u32 i;

    for (i = 1U; i < YEW_SYN_MAX_STATES + 8U; i++) {
        state.aux[0] = i;
        last = yew_syn_state_intern(tab, &state);
    }
    YEW_ASSERT(yew_syn_state_exhausted(tab));
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), YEW_SYN_MAX_STATES);
    YEW_ASSERT_EQ_U64(last, YEW_SYN_STATE_ROOT);
    YEW_ASSERT_NOT_NULL(yew_syn_state_get(tab, YEW_SYN_STATE_ROOT));
    yew_syn_state_tab_free(tab);
}

void test_syn_deferred_surfaces_fail_loudly(void)
{
    const SynDef *ini;
    u32 ini_lang;

    YEW_ASSERT_EQ_U64(yew_syn_lang_for("example.xyz", NULL, 0U),
                      YEW_LANG_NONE);
    ini_lang = yew_syn_lang_for("example.ini", NULL, 0U);
    YEW_ASSERT(ini_lang != YEW_LANG_NONE);
    ini = yew_syn_def_for(ini_lang);
    YEW_ASSERT_NOT_NULL(ini);
    YEW_ASSERT_EQ_STR(ini->name, "ini");
}
