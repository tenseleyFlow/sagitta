#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "util/rss.h"

#include <stdlib.h>
#include <string.h>

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

static char *copy_env(const char *name)
{
    const char *value = getenv(name);
    size_t size;
    char *copy;

    if (value == NULL)
        return NULL;
    size = strlen(value) + 1U;
    copy = malloc(size);
    if (copy != NULL)
        (void)memcpy(copy, value, size);
    return copy;
}

static void restore_env(const char *name, char *value)
{
    if (value == NULL)
        (void)unsetenv(name);
    else {
        (void)setenv(name, value, 1);
        free(value);
    }
}

void test_rss_checkpoint_requires_exact_prof_opt_in(void)
{
    char *saved_prof = copy_env("YEW_PROF");
    char *saved_level = copy_env("YEW_LOG_LEVEL");
    bool disabled_is_silent;
    bool near_match_is_silent;
    bool enabled_emits_row;

    yew_test_capture_log();
    (void)setenv("YEW_LOG_LEVEL", "info", 1);
    (void)unsetenv("YEW_PROF");
    yew_rss_checkpoint("argv");
    disabled_is_silent = yew_test_log_count() == 0U;
    (void)setenv("YEW_PROF", "true", 1);
    yew_rss_checkpoint("argv");
    near_match_is_silent = yew_test_log_count() == 0U;
    (void)setenv("YEW_PROF", "1", 1);
    yew_rss_checkpoint("argv");
    enabled_emits_row = yew_test_log_count() == 1U &&
        yew_test_log_contains(YEW_LOG_INFO, "rss checkpoint=argv ") &&
        yew_test_log_contains(YEW_LOG_INFO, "current_bytes=") &&
        yew_test_log_contains(YEW_LOG_INFO, "peak_bytes=");
    restore_env("YEW_LOG_LEVEL", saved_level);
    restore_env("YEW_PROF", saved_prof);

    YEW_ASSERT(disabled_is_silent);
    YEW_ASSERT(near_match_is_silent);
    YEW_ASSERT(enabled_emits_row);
}
