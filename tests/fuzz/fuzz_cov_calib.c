/* cov-selftest fixture, never a campaign target: a checker whose coverage
 * depends on process history the way product code does.  A campaign
 * process keeps state across executions -- lazy initialisation, scratch
 * buffers grown past a high-water mark, call counters -- so an edge can be
 * reached exactly once and never again by the same bytes.  The coverage
 * driver must calibrate such edges away instead of admitting them or
 * aborting with "coverage minimizer lost novel edge". */
#include "fuzzlib.h"

static volatile unsigned calib_sink;
static unsigned calib_calls;
static bool calib_warmed;
static size_t calib_high_water;

static bool check_cov_calib(const u8 *data, size_t len, char *why,
                            size_t why_cap)
{
    (void)why;
    (void)why_cap;
    calib_calls++;
    /* One-shot by call count: whichever input lands here reaches it. */
    if (calib_calls == 64U)
        calib_sink ^= 1U;
    /* High-water mark: reached again only by a strictly longer input. */
    if (len > calib_high_water + 16U) {
        calib_high_water = len;
        calib_sink ^= 2U;
    }
    /* Input-determined: the only edge an admission may carry.  No builtin
     * seed has two equal leading bytes, so the replay never reaches it. */
    if (len >= 2U && data[0] == data[1]) {
        /* First-use initialisation behind an input-determined branch. */
        if (!calib_warmed) {
            calib_warmed = true;
            calib_sink ^= 4U;
        }
        calib_sink ^= 8U;
    }
    return true;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_cov_calib", NULL,
                         check_cov_calib);
}
