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
    copy = sag_xmalloc(len + 1U);
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
    char *old_level = copy_env("SAG_LOG_LEVEL");
    size_t count;
    bool has_debug;
    bool has_info;
    bool has_warn;
    bool has_error;
    bool rejects_wrong_level;

    (void)setenv("SAG_LOG_LEVEL", "debug", 1);
    sag_test_capture_log();
    sag_log(SAG_LOG_DEBUG, "debug number %d", 1);
    sag_log(SAG_LOG_INFO, "info number %d", 2);
    sag_log(SAG_LOG_WARN, "warn number %d", 3);
    sag_log(SAG_LOG_ERROR, "error number %d", 4);
    count = sag_test_log_count();
    has_debug = sag_test_log_contains(SAG_LOG_DEBUG, "number 1");
    has_info = sag_test_log_contains(SAG_LOG_INFO, "info number");
    has_warn = sag_test_log_contains(SAG_LOG_WARN, "number 3");
    has_error = sag_test_log_contains(SAG_LOG_ERROR, "error number 4");
    rejects_wrong_level = !sag_test_log_contains(SAG_LOG_INFO, "number 4");
    restore_env("SAG_LOG_LEVEL", old_level);

    SAG_ASSERT_EQ_U64(count, 4U);
    SAG_ASSERT(has_debug);
    SAG_ASSERT(has_info);
    SAG_ASSERT(has_warn);
    SAG_ASSERT(has_error);
    SAG_ASSERT(rejects_wrong_level);
}

void test_log_levels(void)
{
    char *old_level = copy_env("SAG_LOG_LEVEL");
    size_t count;
    bool has_error;
    bool has_info;

    (void)setenv("SAG_LOG_LEVEL", "error", 1);
    sag_test_capture_log();
    sag_log(SAG_LOG_INFO, "filtered info");
    sag_log(SAG_LOG_ERROR, "visible error");
    count = sag_test_log_count();
    has_error = sag_test_log_contains(SAG_LOG_ERROR, "visible");
    has_info = sag_test_log_contains(SAG_LOG_INFO, "filtered");
    restore_env("SAG_LOG_LEVEL", old_level);

    SAG_ASSERT_EQ_U64(count, 1U);
    SAG_ASSERT(has_error);
    SAG_ASSERT(!has_info);
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
    static const char report[] = "sagitta: internal error at";
    Bytebuf output;
    int pipefd[2];
    char chunk[512];
    pid_t child;
    pid_t waited;
    ssize_t count;
    int status;

    bytebuf_init(&output);
    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("SAG_LOG", "/dev/null", 1);
        sag_bug_set_prehook(bug_prehook_marker);
        sag_bug("prehook-test", 7, "ordered");
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    SAG_ASSERT_EQ_I64(count, 0);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    SAG_ASSERT(output.len >= sizeof(marker) - 1U + sizeof(report) - 1U);
    SAG_ASSERT_EQ_MEM(output.data, marker, sizeof(marker) - 1U);
    SAG_ASSERT(memcmp(output.data + sizeof(marker) - 1U,
                      report, sizeof(report) - 1U) == 0);
    bytebuf_free(&output);
}
