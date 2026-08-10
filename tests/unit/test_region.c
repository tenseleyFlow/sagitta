/*
 * Sprint 22 §6: the clickable-region registry.
 *
 * Small surface, and every rule on it exists because the alternative
 * fails somewhere specific: last-added-wins is what makes overlays work
 * without z-order bookkeeping, clear-at-begin is what stops an
 * early-returning frame from answering clicks out of a dead layout, and
 * the fixed capacity is a bug detector rather than a limit.
 */
#include "harness.h"

#include <string.h>

#include "ui/region.h"

void test_region_hit_finds_the_registered_rect(void)
{
    Region got;

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, (Rect){10U, 5U, 20U, 8U}, 3);
    YEW_ASSERT_EQ_U64(yew_region_count(), 1U);

    got = yew_region_hit(10U, 5U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_PANE);
    YEW_ASSERT_EQ_I64(got.payload, 3);
    /* The far corner is inside; one past it is not — a half-open rect,
     * the same convention Span uses. */
    got = yew_region_hit(29U, 12U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_PANE);
    got = yew_region_hit(30U, 12U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_NONE);
    got = yew_region_hit(29U, 13U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_NONE);
    got = yew_region_hit(9U, 5U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_NONE);
}

/* Last added wins, which is what lets a dialog drawn after the document
 * shadow it with no z-order bookkeeping at all. */
void test_region_last_added_wins_on_overlap(void)
{
    Region got;

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, (Rect){0U, 0U, 80U, 24U}, 1);
    yew_region_add(YEW_REGION_BLOCK, (Rect){10U, 10U, 20U, 4U}, 0);

    got = yew_region_hit(15U, 11U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_BLOCK);
    /* Outside the overlay the pane still answers. */
    got = yew_region_hit(5U, 5U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_PANE);
    YEW_ASSERT_EQ_I64(got.payload, 1);
}

/*
 * Cleared at frame BEGIN.  The test states the failure it prevents: a
 * frame that registers nothing must answer nothing, rather than
 * answering from the previous layout.
 */
void test_region_frame_begin_clears_the_table(void)
{
    Region got;

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, (Rect){0U, 0U, 80U, 24U}, 7);
    YEW_ASSERT_EQ_U64(yew_region_hit(1U, 1U).kind, YEW_REGION_PANE);

    /* A frame that draws nothing — an early return, a resize bail. */
    yew_region_frame_begin();
    YEW_ASSERT_EQ_U64(yew_region_count(), 0U);
    got = yew_region_hit(1U, 1U);
    YEW_ASSERT_EQ_U64(got.kind, YEW_REGION_NONE);
    YEW_ASSERT_EQ_I64(got.payload, 0);
}

/* DoD 10: querying an empty table is counted, because it means the
 * input path ran against a frame that never drew. */
void test_region_empty_query_is_counted(void)
{
    u32 before;

    yew_region_frame_begin();
    before = yew_region_empty_queries();
    (void)yew_region_hit(0U, 0U);
    YEW_ASSERT_EQ_U64(yew_region_empty_queries(), (u64)before + 1U);

    yew_region_add(YEW_REGION_PANE, (Rect){0U, 0U, 4U, 4U}, 0);
    (void)yew_region_hit(0U, 0U);
    /* A populated table does not count. */
    YEW_ASSERT_EQ_U64(yew_region_empty_queries(), (u64)before + 1U);
}

/* Past capacity it drops rather than growing: a frame with 256 regions
 * is a rendering bug, and growing would hide it. */
void test_region_capacity_drops_without_growing(void)
{
    u32 i;

    yew_region_frame_begin();
    for (i = 0U; i < (u32)YEW_REGION_MAX + 50U; i++)
        yew_region_add(YEW_REGION_PANE, (Rect){0U, (u16)(i % 24U), 1U, 1U},
                       (i32)i);
    YEW_ASSERT_EQ_U64(yew_region_count(), YEW_REGION_MAX);
    /* The table still answers, from what it did keep. */
    YEW_ASSERT_EQ_U64(yew_region_hit(0U, 0U).kind, YEW_REGION_PANE);
}

/* A collapsed pane has no cells and so is not clickable; registering
 * it would give the hit test a zero-area rect to reason about. */
void test_region_zero_area_is_not_registered(void)
{
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, (Rect){5U, 5U, 0U, 10U}, 1);
    yew_region_add(YEW_REGION_PANE, (Rect){5U, 5U, 10U, 0U}, 2);
    YEW_ASSERT_EQ_U64(yew_region_count(), 0U);
}

/*
 * The TAB payload sign convention, pinned here although groups land in
 * Sprint 24: the router reads it and the renderer writes it, and the
 * two must not invent it separately.
 */
void test_region_tab_payload_sign_convention(void)
{
    Region got;

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 10U, 1U}, 2);
    yew_region_add(YEW_REGION_TAB, (Rect){10U, 0U, 10U, 1U}, -5);

    got = yew_region_hit(1U, 0U);
    YEW_ASSERT(got.payload >= 0);
    YEW_ASSERT_EQ_I64(got.payload, 2);
    got = yew_region_hit(11U, 0U);
    YEW_ASSERT(got.payload < 0);
    YEW_ASSERT_EQ_I64(-got.payload, 5);
}
