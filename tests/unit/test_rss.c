#include "harness.h"

#include "util/rss.h"

void test_rss_reports_finite_nonzero_bytes(void)
{
    u64 current = yew_rss_bytes();
    u64 peak = yew_rss_peak_bytes();

    YEW_ASSERT(current > 0U);
    YEW_ASSERT(current < UINT64_MAX);
    YEW_ASSERT(peak > 0U);
    YEW_ASSERT(peak < UINT64_MAX);
}

void test_rss_peak_is_monotonic(void)
{
    u64 before = yew_rss_peak_bytes();
    u64 after = yew_rss_peak_bytes();

    YEW_ASSERT(before > 0U);
    YEW_ASSERT(after >= before);
}
