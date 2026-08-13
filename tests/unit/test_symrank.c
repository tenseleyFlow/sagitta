#include "harness.h"

#include <limits.h>

#include "ws/symidx.h"

void test_symrank_proximity_and_recency_tables(void)
{
    /* Proximity: every table row at neutral recency/kind/frequency. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_CURSOR,
                                   YEW_SYMK_WORD, 0U), 1500);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_BUFFER,
                                   YEW_SYMK_WORD, 0U), 1300);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_DIR,
                                   YEW_SYMK_WORD, 0U), 1150);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1000);

    /* Recency: one boundary value from each saturating bucket. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 0U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1600);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 1U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1400);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 2U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1250);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 4U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1150);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 8U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1080);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 16U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1040);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 32U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1020);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1000);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, UINT32_MAX, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1000);
}

void test_symrank_kind_and_frequency_tables(void)
{
    /* Kind: every table row at neutral proximity/recency/frequency. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 1000);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_FUNC, 0U), 1180);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_TYPE, 0U), 1120);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_MACRO, 0U), 1050);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   YEW_SYMK_KEYWORD, 0U), 900);

    /* Frequency is additive only and saturates at sixteen. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(100, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 0U), 100);
    YEW_ASSERT_EQ_I64(yew_sym_rank(100, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 1U), 101);
    YEW_ASSERT_EQ_I64(yew_sym_rank(100, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 15U), 115);
    YEW_ASSERT_EQ_I64(yew_sym_rank(100, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 16U), 116);
    YEW_ASSERT_EQ_I64(yew_sym_rank(100, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, 17U), 116);
    YEW_ASSERT_EQ_I64(yew_sym_rank(100, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, UINT16_MAX), 116);
    YEW_ASSERT_EQ_I64(yew_sym_rank(0, 64U, YEW_PROX_WS,
                                   YEW_SYMK_WORD, UINT16_MAX), 16);
}

void test_symrank_integer_order_clamps_and_invalid_enums(void)
{
    /* Every division truncates before the next multiplier is applied. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(333, 1U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, 7U), 830);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1, 0U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, 0U), 1);

    /* Negative fuzzy values use C11 truncation toward zero and clamp. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(-100, 0U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, 0U), -283);
    YEW_ASSERT_EQ_I64(yew_sym_rank(-100, 0U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, 16U), -267);
    YEW_ASSERT_EQ_I64(yew_sym_rank(-1, 0U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, 0U), -1);
    YEW_ASSERT_EQ_I64(yew_sym_rank(INT32_MIN, 0U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, 0U), INT32_MIN);

    /* Large scores saturate rather than narrowing implementation-defined. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(INT32_MAX, 0U, YEW_PROX_CURSOR,
                                   YEW_SYMK_FUNC, UINT16_MAX), INT32_MAX);
    YEW_ASSERT_EQ_I64(yew_sym_rank(INT32_MAX, 64U, YEW_PROX_WS,
                                   YEW_SYMK_KEYWORD, 16U), 1932735298);

    /* Invalid enums degrade to the neutral workspace/word multipliers. */
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, (SymProx)-1,
                                   YEW_SYMK_WORD, 0U), 1000);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_N,
                                   YEW_SYMK_WORD, 0U), 1000);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   (u8)YEW_SYMK_NKIND, 0U), 1000);
    YEW_ASSERT_EQ_I64(yew_sym_rank(1000, 64U, YEW_PROX_WS,
                                   UINT8_MAX, 0U), 1000);
}
