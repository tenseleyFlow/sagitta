/*
 * Sprint 22 §6.  See region.h for the one-source-of-truth rule.
 */
#include "ui/region.h"

#include <string.h>

#include "util/log.h"

static struct {
    Region v[SAG_REGION_MAX];
    u32 len;
    u32 empty_queries;
    bool warned_full;
} regions;

void sag_region_frame_begin(void)
{
    /*
     * Cleared at frame BEGIN, never at frame end.
     *
     * Clearing at the end reads as tidier and is wrong in the case that
     * matters: a frame that returns early — a draw that bails on a
     * resize, an error path, a crash — never reaches its cleanup, and
     * the next click is answered from a layout that no longer exists.
     * Clearing at the start makes the stale-table window empty by
     * construction.
     */
    regions.len = 0U;
    regions.warned_full = false;
}

void sag_region_add(RegionKind kind, Rect rect, i32 payload)
{
    Region *r;

    if (rect.w == 0U || rect.h == 0U)
        return; /* a collapsed pane is not clickable */
    if (regions.len >= (u32)SAG_REGION_MAX) {
        if (!regions.warned_full) {
            /* Once per frame, not once per drop: a full table means
             * hundreds of these, and a flooded log hides the cause. */
            sag_log(SAG_LOG_WARN,
                    "region table full at %d entries; dropping the rest",
                    SAG_REGION_MAX);
            regions.warned_full = true;
        }
        return;
    }
    r = &regions.v[regions.len++];
    r->kind = kind;
    r->rect = rect;
    r->payload = payload;
}

Region sag_region_hit(u16 x, u16 y)
{
    Region none;
    u32 i;

    (void)memset(&none, 0, sizeof(none));
    if (regions.len == 0U) {
        regions.empty_queries++;
        return none;
    }
    /*
     * Backwards, so the LAST region added wins an overlap.  That is
     * what makes an overlay work without any z-order bookkeeping: a
     * dialog drawn after the document is added after it, and therefore
     * answers first.
     */
    for (i = regions.len; i > 0U; i--) {
        const Region *r = &regions.v[i - 1U];

        if (x >= r->rect.x && x < (u32)r->rect.x + r->rect.w &&
            y >= r->rect.y && y < (u32)r->rect.y + r->rect.h)
            return *r;
    }
    return none;
}

u32 sag_region_count(void)
{
    return regions.len;
}

u32 sag_region_empty_queries(void)
{
    return regions.empty_queries;
}
