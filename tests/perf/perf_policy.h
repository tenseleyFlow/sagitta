#ifndef YEW_TESTS_PERF_POLICY_H
#define YEW_TESTS_PERF_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { YEW_PERF_ADVISORY_SANITY_MULTIPLIER = 100 };

static inline bool yew_perf_advisory(void)
{
    const char *value = getenv("YEW_PERF_ADVISORY");

    return value != NULL && strcmp(value, "0") != 0;
}

static inline const char *yew_perf_mode(bool advisory)
{
    return advisory ? "ADVISORY" : "GATING";
}

static inline uint64_t yew_perf_sanity_ceiling(uint64_t budget)
{
    if (budget > UINT64_MAX / YEW_PERF_ADVISORY_SANITY_MULTIPLIER)
        return UINT64_MAX;
    return budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER;
}

static inline bool yew_perf_timing_sane(uint64_t value, uint64_t budget)
{
    if (value == 0U || budget == 0U)
        return false;
    return value <= yew_perf_sanity_ceiling(budget);
}

static inline bool yew_perf_timing_failed(uint64_t value, uint64_t budget,
                                   bool advisory)
{
    return !yew_perf_timing_sane(value, budget) ||
           (!advisory && value > budget);
}

static inline const char *yew_perf_timing_verdict(uint64_t value, uint64_t budget,
                                           bool advisory)
{
    if (!yew_perf_timing_sane(value, budget))
        return " SANITY-FAIL";
    if (value > budget)
        return advisory ? " WARN" : " REGRESSION";
    return " ok";
}

/*
 * One timed sample inside a population (a per-call check, a count of calls
 * over budget).  A single fast call may legitimately read 0 ns on a coarse
 * clock, so unlike yew_perf_timing_failed() zero is not insane here; the
 * advisory ceiling alone separates noise from a gross regression.
 */
static inline bool yew_perf_sample_failed(uint64_t value, uint64_t budget,
                                   bool advisory)
{
    return value > (advisory ? yew_perf_sanity_ceiling(budget) : budget);
}

/*
 * Throughput limits (fps, MiB/s) are minimums: the mirror image of a
 * timing budget.  A measurement is sane when it is positive and no worse
 * than minimum / YEW_PERF_ADVISORY_SANITY_MULTIPLIER.
 */
static inline bool yew_perf_throughput_sane(double value, double minimum)
{
    if (!(value > 0.0) || !(minimum > 0.0))
        return false;
    return value * (double)YEW_PERF_ADVISORY_SANITY_MULTIPLIER >= minimum;
}

static inline bool yew_perf_throughput_failed(double value, double minimum,
                                       bool advisory)
{
    return !yew_perf_throughput_sane(value, minimum) ||
           (!advisory && value < minimum);
}

static inline const char *yew_perf_throughput_verdict(double value, double minimum,
                                               bool advisory)
{
    if (!yew_perf_throughput_sane(value, minimum))
        return " SANITY-FAIL";
    if (value < minimum)
        return advisory ? " WARN" : " REGRESSION";
    return " ok";
}

#endif
