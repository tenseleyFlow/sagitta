#include "harness.h"

#include <string.h>

#include "syn/engine.h"

static SynState state2_root(u16 ctx, u8 def)
{
    SynState state;

    (void)memset(&state, 0, sizeof(state));
    state.f[0] = (SynFrame){ctx, def, 0U};
    state.depth = 1U;
    state.ndef = 1U;
    return state;
}

void test_syn_state2_layout(void)
{
    YEW_ASSERT_EQ_U64(sizeof(SynFrame), 4U);
    YEW_ASSERT_EQ_U64(sizeof(SynState), 84U);
}

void test_syn_state_unused_tails_are_canonicalized(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState clean = state2_root(9U, 2U);
    SynState dirty = clean;
    const SynState *stored;
    const SynFrame zero = {0U, 0U, 0U};
    u32 clean_id;
    u32 dirty_id;
    u32 i;

    clean.aux[0] = 42U;
    dirty.aux[0] = 42U;
    for (i = 1U; i < YEW_SYN_DEPTH_MAX; i++)
        dirty.f[i] = (SynFrame){(u16)(100U + i),
                                (u8)(i % YEW_SYN_DEF_MAX), 0xffU};
    for (i = 1U; i < YEW_SYN_DEF_MAX; i++)
        dirty.aux[i] = 0xa5a50000U + i;

    clean_id = yew_syn_state_intern(tab, &clean);
    dirty_id = yew_syn_state_intern(tab, &dirty);
    YEW_ASSERT_EQ_U64(dirty_id, clean_id);
    stored = yew_syn_state_get(tab, clean_id);
    YEW_ASSERT_NOT_NULL(stored);
    for (i = 1U; i < YEW_SYN_DEPTH_MAX; i++)
        YEW_ASSERT_EQ_MEM(&stored->f[i], &zero, sizeof(stored->f[i]));
    for (i = 1U; i < YEW_SYN_DEF_MAX; i++)
        YEW_ASSERT_EQ_U64(stored->aux[i], 0U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_cross_definition_identity(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(5U);
    SynState a = state2_root(5U, 1U);
    SynState b = a;
    u32 aid;
    u32 bid;

    b.f[0].def = 2U;
    aid = yew_syn_state_intern(tab, &a);
    bid = yew_syn_state_intern(tab, &b);
    YEW_ASSERT(aid > YEW_SYN_STATE_ROOT);
    YEW_ASSERT(bid > YEW_SYN_STATE_ROOT);
    YEW_ASSERT(aid != bid);
    YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &a), aid);
    YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &b), bid);
    yew_syn_state_tab_free(tab);
}
