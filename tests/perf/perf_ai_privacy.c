#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod/ai/redact.h"
#include "util/base.h"

enum {
    AI_PRIVACY_CONTEXT_BYTES = 4096,
    AI_PRIVACY_TRIALS = 17,
    AI_PRIVACY_REDACT_BATCH = 1,
    AI_PRIVACY_PATH_BATCH = 256,
    AI_PRIVACY_REDACT_LIMIT_NS = 2000000,
    AI_PRIVACY_PATH_LIMIT_NS = 20000
};

static volatile u64 privacy_sink;

static u64 now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        (void)fprintf(stderr, "perf_ai_privacy: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)now.tv_sec * UINT64_C(1000000000) + (u64)now.tv_nsec;
}

static void sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t j = i;

        while (j > 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
}

static void fill_context(u8 bytes[AI_PRIVACY_CONTEXT_BYTES])
{
    static const char row[] =
        "static int compute_value(int left, int right) { return left + right; }\n";
    size_t at = 0U;

    while (at < AI_PRIVACY_CONTEXT_BYTES) {
        size_t take = sizeof(row) - 1U;

        if (take > AI_PRIVACY_CONTEXT_BYTES - at)
            take = AI_PRIVACY_CONTEXT_BYTES - at;
        (void)memcpy(bytes + at, row, take);
        at += take;
    }
}

static bool measure_redact(const AiRedactPolicy *policy, u64 *result)
{
    u8 bytes[AI_PRIVACY_CONTEXT_BYTES];
    u64 trials[AI_PRIVACY_TRIALS];
    AiCtx context;
    size_t trial;

    fill_context(bytes);
    (void)memset(&context, 0, sizeof(context));
    context.prefix = bytes;
    context.plen = AI_PRIVACY_CONTEXT_BYTES / 2U;
    context.suffix = bytes + context.plen;
    context.slen = AI_PRIVACY_CONTEXT_BYTES - context.plen;
    context.line_1based = 31U;
    if (yew_ai_redact_scan(policy, &context, NULL))
        return false;
    for (trial = 0U; trial < AI_PRIVACY_TRIALS; trial++) {
        u64 started = now_ns();
        u64 ended;
        size_t i;

        if (started == 0U)
            return false;
        for (i = 0U; i < AI_PRIVACY_REDACT_BATCH; i++)
            privacy_sink ^= yew_ai_redact_scan(policy, &context, NULL);
        ended = now_ns();
        if (ended < started)
            return false;
        trials[trial] = (ended - started) / AI_PRIVACY_REDACT_BATCH;
    }
    sort_u64(trials, YEW_ARRAY_LEN(trials));
    /* The gate is isolated scan cost, not scheduler delay.  The best trial
     * follows the existing perf_unicode convention and removes involuntary
     * descheduling. */
    *result = trials[0];
    return true;
}

static bool measure_path(const AiPathPolicy *policy, u64 *result)
{
    static const char path[] =
        "src/generated/vendor/component/subcomponent/editor_privacy_probe.c";
    u64 trials[AI_PRIVACY_TRIALS];
    size_t trial;

    if (yew_ai_path_excluded(policy, path, NULL))
        return false;
    for (trial = 0U; trial < AI_PRIVACY_TRIALS; trial++) {
        u64 started = now_ns();
        u64 ended;
        size_t i;

        if (started == 0U)
            return false;
        for (i = 0U; i < AI_PRIVACY_PATH_BATCH; i++)
            privacy_sink ^= yew_ai_path_excluded(policy, path, NULL);
        ended = now_ns();
        if (ended < started)
            return false;
        trials[trial] = (ended - started) / AI_PRIVACY_PATH_BATCH;
    }
    sort_u64(trials, YEW_ARRAY_LEN(trials));
    *result = trials[0];
    return true;
}

int main(void)
{
    AiRedactPolicy *redact =
        yew_ai_redact_policy_new(NULL, 0U, false, NULL);
    AiPathPolicy *paths = yew_ai_path_policy_new(NULL, 0U, false, NULL);
    u64 redact_ns = 0U;
    u64 path_ns = 0U;
    int status = 0;

    if (redact == NULL || paths == NULL ||
        yew_ai_redact_policy_len(redact) == 0U ||
        yew_ai_path_policy_len(paths) == 0U) {
        (void)fprintf(stderr,
                      "perf_ai_privacy: shipped policy construction failed\n");
        status = 2;
        goto done;
    }
    if (!measure_redact(redact, &redact_ns) ||
        !measure_path(paths, &path_ns)) {
        (void)fprintf(stderr,
                      "perf_ai_privacy: measurement invariant failed\n");
        status = 2;
        goto done;
    }
    (void)printf("ai_redact_scan_ns %llu limit=%u%s\n",
                 (unsigned long long)redact_ns,
                 (unsigned)AI_PRIVACY_REDACT_LIMIT_NS,
                 redact_ns <= AI_PRIVACY_REDACT_LIMIT_NS ? " ok" : " FAIL");
    (void)printf("ai_path_match_ns %llu limit=%u%s\n",
                 (unsigned long long)path_ns,
                 (unsigned)AI_PRIVACY_PATH_LIMIT_NS,
                 path_ns <= AI_PRIVACY_PATH_LIMIT_NS ? " ok" : " FAIL");
    if (redact_ns > AI_PRIVACY_REDACT_LIMIT_NS ||
        path_ns > AI_PRIVACY_PATH_LIMIT_NS)
        status = 1;
done:
    yew_ai_path_policy_free(paths);
    yew_ai_redact_policy_free(redact);
    return status;
}
