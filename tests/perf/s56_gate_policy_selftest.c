/* Sprint 56 deterministic gate-policy proof.
 *
 * This is deliberately a self-contained policy test: it injects observations
 * instead of sleeping, so scheduler noise cannot make the gate's own test
 * flaky.  The production harness supplies p99 and frame counts; this test
 * pins how three such observations become a verdict.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum GateMode {
    GATE_ADVISORY,
    GATE_DESIGNATED
} GateMode;

typedef struct GateVerdict {
    uint64_t median_p99_ns;
    unsigned over_count;
    bool fail;
    bool warn;
    bool rebaseline;
} GateVerdict;

typedef bool (*TestFn)(void);

typedef struct TestCase {
    const char *name;
    TestFn run;
} TestCase;

static uint64_t median3(const uint64_t samples[3])
{
    uint64_t a = samples[0];
    uint64_t b = samples[1];
    uint64_t c = samples[2];

    if (a > b) {
        uint64_t t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        uint64_t t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        uint64_t t = a;
        a = b;
        b = t;
    }
    return b;
}

static bool exceeds_percent(uint64_t value, uint64_t baseline,
                            unsigned percent)
{
    uint64_t whole = baseline / 100U;
    uint64_t remainder = baseline % 100U;
    uint64_t limit = baseline + whole * percent +
                     (remainder * percent + 99U) / 100U;

    return value > limit;
}

static bool below_percent(uint64_t value, uint64_t baseline,
                          unsigned percent)
{
    uint64_t whole = baseline / 100U;
    uint64_t remainder = baseline % 100U;
    uint64_t reduction = whole * percent +
                         (remainder * percent) / 100U;

    return value < baseline - reduction;
}

static GateVerdict assess(const uint64_t p99_ns[3], uint64_t baseline_ns,
                          uint64_t absolute_budget_ns, uint64_t frames,
                          uint64_t keys, GateMode mode)
{
    GateVerdict out = {0};
    unsigned i;

    out.median_p99_ns = median3(p99_ns);
    for (i = 0U; i < 3U; i++) {
        bool absolute_over = p99_ns[i] > absolute_budget_ns;
        bool relative_over = exceeds_percent(p99_ns[i], baseline_ns, 10U);

        if (absolute_over || relative_over)
            out.over_count++;
    }
    if (frames > keys) {
        out.fail = true;
    } else if (out.over_count >= 2U) {
        if (mode == GATE_DESIGNATED)
            out.fail = true;
        else
            out.warn = true;
    }
    out.rebaseline = below_percent(out.median_p99_ns, baseline_ns, 20U);
    return out;
}

static bool permits_one_frame_per_key(void)
{
    static const uint64_t samples[3] = {1000U, 1000U, 1000U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return !v.fail;
}

static bool rejects_extra_frame_on_designated_runner(void)
{
    static const uint64_t samples[3] = {1000U, 1000U, 1000U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10001U, 10000U,
                           GATE_DESIGNATED);

    return v.fail;
}

static bool rejects_extra_frame_on_advisory_runner(void)
{
    static const uint64_t samples[3] = {1000U, 1000U, 1000U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10001U, 10000U,
                           GATE_ADVISORY);

    return v.fail;
}

static bool seeded_two_ms_slowdown_fails_designated_runner(void)
{
    static const uint64_t samples[3] = {7000000U, 7000000U, 7000000U};
    GateVerdict v = assess(samples, 5000000U, 5000000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return v.fail && !v.warn;
}

static bool seeded_two_ms_slowdown_warns_on_advisory_runner(void)
{
    static const uint64_t samples[3] = {7000000U, 7000000U, 7000000U};
    GateVerdict v = assess(samples, 5000000U, 5000000U, 10000U, 10000U,
                           GATE_ADVISORY);

    return !v.fail && v.warn;
}

static bool one_slow_observation_does_not_fail(void)
{
    static const uint64_t samples[3] = {1000U, 1000U, 1200U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return v.median_p99_ns == 1000U && v.over_count == 1U && !v.fail;
}

static bool two_slow_observations_fail(void)
{
    static const uint64_t samples[3] = {1200U, 1000U, 1200U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return v.median_p99_ns == 1200U && v.over_count == 2U && v.fail;
}

static bool five_percent_slowdown_does_not_fail(void)
{
    static const uint64_t samples[3] = {1050U, 1050U, 1050U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return !v.fail && !v.warn;
}

static bool twenty_percent_slowdown_fails(void)
{
    static const uint64_t samples[3] = {1200U, 1200U, 1200U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return v.fail;
}

static bool exact_twenty_percent_improvement_does_not_request_rebaseline(void)
{
    static const uint64_t samples[3] = {800U, 800U, 800U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return !v.fail && !v.rebaseline;
}

static bool greater_than_twenty_percent_improvement_requests_rebaseline(void)
{
    static const uint64_t samples[3] = {790U, 790U, 790U};
    GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                           GATE_DESIGNATED);

    return !v.fail && v.rebaseline;
}

static bool five_unchanged_runs_have_zero_failures(void)
{
    static const uint64_t samples[3] = {1000U, 1000U, 1000U};
    unsigned failures = 0U;
    unsigned run;

    for (run = 0U; run < 5U; run++) {
        GateVerdict v = assess(samples, 1000U, 5000U, 10000U, 10000U,
                               GATE_DESIGNATED);

        if (v.fail)
            failures++;
    }
    return failures == 0U;
}

int main(void)
{
    static const TestCase tests[] = {
        {"permits exactly one frame per key", permits_one_frame_per_key},
        {"rejects an extra frame on a designated runner",
         rejects_extra_frame_on_designated_runner},
        {"rejects an extra frame on an advisory runner",
         rejects_extra_frame_on_advisory_runner},
        {"seeded 2 ms slowdown fails a designated runner",
         seeded_two_ms_slowdown_fails_designated_runner},
        {"seeded 2 ms slowdown only warns on an advisory runner",
         seeded_two_ms_slowdown_warns_on_advisory_runner},
        {"one slow observation does not fail",
         one_slow_observation_does_not_fail},
        {"two slow observations fail", two_slow_observations_fail},
        {"5 percent slowdown does not fail",
         five_percent_slowdown_does_not_fail},
        {"20 percent slowdown fails", twenty_percent_slowdown_fails},
        {"exact 20 percent improvement does not request rebaseline",
         exact_twenty_percent_improvement_does_not_request_rebaseline},
        {"greater than 20 percent improvement requests rebaseline",
         greater_than_twenty_percent_improvement_requests_rebaseline},
        {"five unchanged runs have zero failures",
         five_unchanged_runs_have_zero_failures},
    };
    size_t i;
    unsigned failed = 0U;

    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i++) {
        bool ok = tests[i].run();

        (void)printf("%s %zu - %s\n", ok ? "ok" : "not ok", i + 1U,
                     tests[i].name);
        if (!ok)
            failed++;
    }
    (void)printf("1..%zu\n", sizeof(tests) / sizeof(tests[0]));
    return failed == 0U ? 0 : 1;
}
