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

void test_syn_state_canonical_roundtrip_randomized_10000(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    u32 rng = 0x243f6a88U;
    u32 iteration;

    /* This is a transition test, not merely a random-struct test.  Every
     * generated sequence contains push/pop, embed/exit, set/restore, and
     * lost-depth round trips.  The synthetic embed exit deliberately leaves
     * the frames and aux slot it vacated dirty, as an optimized transition
     * is allowed to do before the interner canonicalizes the tuple.
     *
     * Mutation proof: removing tail zeroing from yew_syn_state_intern makes
     * returned_id differ from base_id below.  In the incremental scanner
     * those duplicate semantic rows also prevent equal entry states from
     * collapsing at the normal <=2-line fixpoint; this test locks the byte
     * identity prerequisite while test_syn_diff locks the fixpoint result. */
    for (iteration = 0U; iteration < 10000U; iteration++) {
        SynState base = state2_root(0U, 0U);
        SynState returned = base;
        SynState active_mutant;
        u32 base_id = yew_syn_state_intern(tab, &base);
        u32 steps;

        rng = rng * 1664525U + 1013904223U;
        steps = 4U + rng % 29U;
        for (u32 step = 0U; step < steps; step++) {
            u16 old_ctx = returned.f[returned.depth - 1U].ctx;

            rng = rng * 1664525U + 1013904223U;
            switch (rng & 3U) {
            case 0U:
                yew_syn_state_push(&returned, (u16)(rng >> 16U));
                yew_syn_state_pop(&returned, 1U);
                break;
            case 1U: {
                u8 depth = returned.depth;
                u8 ndef = returned.ndef;

                returned.f[depth] = (SynFrame){(u16)rng,
                    returned.f[depth - 1U].def, YEW_SYN_FR_BRIDGE};
                returned.f[depth + 1U] = (SynFrame){(u16)(rng >> 16U),
                    ndef, 0U};
                returned.aux[ndef] = rng ^ UINT32_C(0xa5a55a5a);
                returned.depth = (u8)(depth + 2U);
                returned.ndef = (u8)(ndef + 1U);
                /* Logical embed exit.  Vacated storage remains poisoned so
                 * canonicalization, rather than the test helper, owns it. */
                returned.depth = depth;
                returned.ndef = ndef;
                break;
            }
            case 2U:
                yew_syn_state_set(&returned, (u16)(rng >> 16U));
                yew_syn_state_set(&returned, old_ctx);
                break;
            default:
                returned.depth = YEW_SYN_DEPTH_MAX;
                for (u8 i = 1U; i < returned.depth; i++)
                    returned.f[i] = (SynFrame){(u16)(rng + i), 0U, 0U};
                yew_syn_state_push(&returned, (u16)rng);
                yew_syn_state_pop(&returned, 1U);
                returned.depth = 1U;
                break;
            }
        }
        YEW_ASSERT_EQ_U64(returned.depth, base.depth);
        YEW_ASSERT_EQ_U64(returned.ndef, base.ndef);
        YEW_ASSERT_EQ_U64(returned.lost, base.lost);
        YEW_ASSERT_EQ_MEM(returned.f, base.f, sizeof(base.f[0]));
        YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &returned), base_id);

        active_mutant = returned;
        active_mutant.f[0].ctx = 1U;
        YEW_ASSERT(yew_syn_state_intern(tab, &active_mutant) != base_id);
    }
    yew_syn_state_tab_free(tab);
}
