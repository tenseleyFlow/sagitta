#ifndef YEW_TESTS_PERF_POLICY_H
#define YEW_TESTS_PERF_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { YEW_PERF_ADVISORY_SANITY_MULTIPLIER = 100 };

static bool yew_perf_advisory(void)
{
    const char *value = getenv("YEW_PERF_ADVISORY");

    return value != NULL && strcmp(value, "0") != 0;
}

static bool yew_perf_timing_sane(uint64_t value, uint64_t budget)
{
    uint64_t ceiling;

    if (value == 0U || budget == 0U)
        return false;
    if (budget > UINT64_MAX / YEW_PERF_ADVISORY_SANITY_MULTIPLIER)
        ceiling = UINT64_MAX;
    else
        ceiling = budget * YEW_PERF_ADVISORY_SANITY_MULTIPLIER;
    return value <= ceiling;
}

static bool yew_perf_timing_failed(uint64_t value, uint64_t budget,
                                   bool advisory)
{
    return !yew_perf_timing_sane(value, budget) ||
           (!advisory && value > budget);
}

static const char *yew_perf_timing_verdict(uint64_t value, uint64_t budget,
                                           bool advisory)
{
    if (!yew_perf_timing_sane(value, budget))
        return " SANITY-FAIL";
    if (value > budget)
        return advisory ? " WARN" : " REGRESSION";
    return " ok";
}

#endif
