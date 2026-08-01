#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "util/buf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_unit_child(char *const argv[], const char *log_path,
                          bool selfcheck, Bytebuf *output)
{
    int pipefd[2];
    pid_t pid;
    pid_t waited;
    int status;
    char chunk[1024];
    ssize_t count;

    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    pid = fork();
    SAG_ASSERT(pid >= 0);
    if (pid == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        if (selfcheck) {
            (void)setenv("SAG_TEST_SELFCHECK", "1", 1);
            (void)setenv("SAG_LOG_LEVEL", "debug", 1);
        } else {
            (void)unsetenv("SAG_TEST_SELFCHECK");
        }
        if (log_path != NULL)
            (void)setenv("SAG_LOG", log_path, 1);
        execv(argv[0], argv);
        _exit(127);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    SAG_ASSERT_EQ_I64(count, 0);
    (void)close(pipefd[0]);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, pid);
    if (!WIFEXITED(status))
        return 128;
    return WEXITSTATUS(status);
}

static size_t substring_count(const Bytebuf *buf, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t count = 0U;
    size_t i;

    if (needle_len == 0U)
        return 0U;
    for (i = 0U; i + needle_len <= buf->len; i++) {
        if (memcmp(buf->data + i, needle, needle_len) == 0)
            count++;
    }
    return count;
}

void test_harness_assert_once(void)
{
    u64 uvalue = 4U;
    i64 ivalue = -2;
    const char *strings[] = {"same", "unused"};
    const u8 bytes[] = {0x00U, 0x7fU, 0xffU};
    int pointer_value = 1;
    int condition_calls = 0;

    SAG_ASSERT(++condition_calls == 1);
    SAG_ASSERT_EQ_U64(uvalue++, 4U);
    SAG_ASSERT_EQ_I64(ivalue++, -2);
    SAG_ASSERT_EQ_STR(strings[0], "same");
    SAG_ASSERT_EQ_MEM(bytes, bytes, sizeof(bytes));
    SAG_ASSERT_NULL(NULL);
    SAG_ASSERT_NOT_NULL(&pointer_value);
    SAG_ASSERT_EQ_U64(uvalue, 5U);
    SAG_ASSERT_EQ_I64(ivalue, -1);
}

void test_harness_filter_selects(void)
{
    Bytebuf output;
    char *argv[] = {(char *)sag_test_program_path(), "--filter",
                    "args_parse_", NULL};
    int rc;

    bytebuf_init(&output);
    rc = run_unit_child(argv, NULL, false, &output);
    SAG_ASSERT_EQ_I64(rc, 0);
    SAG_ASSERT_EQ_U64(substring_count(&output, "PASS args_parse_"), 7U);
    SAG_ASSERT_EQ_U64(substring_count(&output, "PASS arena_"), 0U);
    SAG_ASSERT_EQ_U64(substring_count(&output, "unit: 7 tests,"), 1U);
    bytebuf_free(&output);
}

void test_harness_list_order(void)
{
    Bytebuf output;
    Bytebuf expected;
    char *argv[] = {(char *)sag_test_program_path(), "--list", NULL};
    size_t i;
    int rc;

    bytebuf_init(&output);
    bytebuf_init(&expected);
    rc = run_unit_child(argv, NULL, false, &output);
    for (i = 0U; i < sag_tests_len; i++)
        bytebuf_printf(&expected, "%s\n", sag_tests[i].name);
    SAG_ASSERT_EQ_I64(rc, 0);
    SAG_ASSERT_EQ_U64(output.len, expected.len);
    SAG_ASSERT_EQ_MEM(output.data, expected.data, expected.len);
    bytebuf_free(&expected);
    bytebuf_free(&output);
}

void test_harness_failure_isolated(void)
{
    static const char expected[] =
        "FAIL harness_intentional_failure at tests/unit/test_harness.c:404: "
        "SAG_ASSERT_EQ_U64 left=1 right=2\n"
        "unit: 1 tests, 1 assertions, 1 failure\n";
    Bytebuf output;
    char log_path[] = "/tmp/sagitta-unit-log-XXXXXX";
    char *argv[] = {(char *)sag_test_program_path(), "--filter",
                    "harness_intentional_failure", NULL};
    int fd;
    int rc;

    bytebuf_init(&output);
    fd = mkstemp(log_path);
    SAG_ASSERT(fd >= 0);
    (void)close(fd);
    (void)unlink(log_path);
    rc = run_unit_child(argv, log_path, true, &output);
    (void)unlink(log_path);

    SAG_ASSERT_EQ_I64(rc, 1);
    SAG_ASSERT_EQ_U64(output.len, sizeof(expected) - 1U);
    SAG_ASSERT_EQ_MEM(output.data, expected, sizeof(expected) - 1U);
    bytebuf_free(&output);
}

void test_harness_intentional_failure(void)
{
    if (getenv("SAG_TEST_SELFCHECK") != NULL) {
        sag_test_capture_log();
        sag_log(SAG_LOG_ERROR, "captured before intentional failure");
#line 404 "tests/unit/test_harness.c"
        SAG_ASSERT_EQ_U64(1U, 2U);
#line 192 "tests/unit/test_harness.c"
    }
    SAG_ASSERT(true);
}
