#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "util/buf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char *copy_env(const char *name)
{
    const char *value = getenv(name);
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value);
    copy = yew_xmalloc(len + 1U);
    (void)memcpy(copy, value, len + 1U);
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

void test_log_capture(void)
{
    char *old_level = copy_env("YEW_LOG_LEVEL");
    size_t count;
    bool has_debug;
    bool has_info;
    bool has_warn;
    bool has_error;
    bool rejects_wrong_level;

    (void)setenv("YEW_LOG_LEVEL", "debug", 1);
    yew_test_capture_log();
    yew_log(YEW_LOG_DEBUG, "debug number %d", 1);
    yew_log(YEW_LOG_INFO, "info number %d", 2);
    yew_log(YEW_LOG_WARN, "warn number %d", 3);
    yew_log(YEW_LOG_ERROR, "error number %d", 4);
    count = yew_test_log_count();
    has_debug = yew_test_log_contains(YEW_LOG_DEBUG, "number 1");
    has_info = yew_test_log_contains(YEW_LOG_INFO, "info number");
    has_warn = yew_test_log_contains(YEW_LOG_WARN, "number 3");
    has_error = yew_test_log_contains(YEW_LOG_ERROR, "error number 4");
    rejects_wrong_level = !yew_test_log_contains(YEW_LOG_INFO, "number 4");
    restore_env("YEW_LOG_LEVEL", old_level);

    YEW_ASSERT_EQ_U64(count, 4U);
    YEW_ASSERT(has_debug);
    YEW_ASSERT(has_info);
    YEW_ASSERT(has_warn);
    YEW_ASSERT(has_error);
    YEW_ASSERT(rejects_wrong_level);
}

void test_log_levels(void)
{
    char *old_level = copy_env("YEW_LOG_LEVEL");
    size_t count;
    bool has_error;
    bool has_info;

    (void)setenv("YEW_LOG_LEVEL", "error", 1);
    yew_test_capture_log();
    yew_log(YEW_LOG_INFO, "filtered info");
    yew_log(YEW_LOG_ERROR, "visible error");
    count = yew_test_log_count();
    has_error = yew_test_log_contains(YEW_LOG_ERROR, "visible");
    has_info = yew_test_log_contains(YEW_LOG_INFO, "filtered");
    restore_env("YEW_LOG_LEVEL", old_level);

    YEW_ASSERT_EQ_U64(count, 1U);
    YEW_ASSERT(has_error);
    YEW_ASSERT(!has_info);
}

static void bug_prehook_marker(void)
{
    static const char marker[] = "prehook\n";
    ssize_t written = write(STDERR_FILENO, marker, sizeof(marker) - 1U);

    (void)written;
}

void test_log_bug_prehook(void)
{
    static const char marker[] = "prehook\n";
    static const char report[] = "yew: internal error at";
    YewTestChild child;

    if (yew_test_child_role() != NULL) {
        (void)setenv("YEW_LOG", "/dev/null", 1);
        yew_bug_set_prehook(bug_prehook_marker);
        yew_bug("prehook-test", 7, "ordered");
    }
    yew_test_spawn_child("bug", NULL, NULL, &child);
    YEW_ASSERT_CHILD_EXIT(&child, YEW_EXIT_BUG);
    YEW_ASSERT(child.err_len >= sizeof(marker) - 1U + sizeof(report) - 1U);
    YEW_ASSERT_EQ_MEM(child.err, marker, sizeof(marker) - 1U);
    YEW_ASSERT(memcmp(child.err + sizeof(marker) - 1U,
                      report, sizeof(report) - 1U) == 0);
    yew_test_child_free(&child);
}
