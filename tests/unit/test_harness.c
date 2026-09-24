#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "util/buf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* `env` is NULL or NAME, VALUE pairs ending in NULL, set in the child. */
static int run_unit_child_env(char *const argv[], const char *log_path,
                              bool selfcheck, const char *const *env,
                              Bytebuf *output)
{
    int pipefd[2];
    pid_t pid;
    pid_t waited;
    int status;
    char chunk[1024];
    ssize_t count;

    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    pid = fork();
    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        if (selfcheck) {
            (void)setenv("YEW_TEST_SELFCHECK", "1", 1);
            (void)setenv("YEW_LOG_LEVEL", "debug", 1);
        } else {
            (void)unsetenv("YEW_TEST_SELFCHECK");
        }
        if (log_path != NULL)
            (void)setenv("YEW_LOG", log_path, 1);
        for (; env != NULL && env[0] != NULL; env += 2) {
            if (setenv(env[0], env[1], 1) != 0)
                _exit(126);
        }
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
    YEW_ASSERT_EQ_I64(count, 0);
    (void)close(pipefd[0]);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, pid);
    if (!WIFEXITED(status))
        return 128;
    return WEXITSTATUS(status);
}

static int run_unit_child(char *const argv[], const char *log_path,
                          bool selfcheck, Bytebuf *output)
{
    return run_unit_child_env(argv, log_path, selfcheck, NULL, output);
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

    YEW_ASSERT(++condition_calls == 1);
    YEW_ASSERT_EQ_U64(uvalue++, 4U);
    YEW_ASSERT_EQ_I64(ivalue++, -2);
    YEW_ASSERT_EQ_STR(strings[0], "same");
    YEW_ASSERT_EQ_MEM(bytes, bytes, sizeof(bytes));
    YEW_ASSERT_NULL(NULL);
    YEW_ASSERT_NOT_NULL(&pointer_value);
    YEW_ASSERT_EQ_U64(uvalue, 5U);
    YEW_ASSERT_EQ_I64(ivalue, -1);
}

void test_harness_filter_selects(void)
{
    Bytebuf output;
    char *argv[] = {(char *)yew_test_program_path(), "--filter",
                    "args_parse_", NULL};
    int rc;

    bytebuf_init(&output);
    rc = run_unit_child(argv, NULL, false, &output);
    YEW_ASSERT_EQ_I64(rc, 0);
    YEW_ASSERT_EQ_U64(substring_count(&output, "PASS args_parse_"), 20U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "PASS arena_"), 0U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "unit: 20 tests,"), 1U);
    bytebuf_free(&output);
}

void test_harness_list_order(void)
{
    Bytebuf output;
    Bytebuf expected;
    char *argv[] = {(char *)yew_test_program_path(), "--list", NULL};
    size_t i;
    int rc;

    bytebuf_init(&output);
    bytebuf_init(&expected);
    rc = run_unit_child(argv, NULL, false, &output);
    for (i = 0U; i < yew_tests_len; i++)
        bytebuf_printf(&expected, "%s\n", yew_tests[i].name);
    YEW_ASSERT_EQ_I64(rc, 0);
    YEW_ASSERT_EQ_U64(output.len, expected.len);
    YEW_ASSERT_EQ_MEM(output.data, expected.data, expected.len);
    bytebuf_free(&expected);
    bytebuf_free(&output);
}

void test_harness_failure_isolated(void)
{
    static const char expected[] =
        "FAIL harness_intentional_failure at tests/unit/test_harness.c:404: "
        "YEW_ASSERT_EQ_U64 left=1 right=2\n"
        "unit: 1 tests, 1 assertions, 1 failure\n";
    Bytebuf output;
    char log_path[] = "/tmp/yew-unit-log-XXXXXX";
    char *argv[] = {(char *)yew_test_program_path(), "--filter",
                    "harness_intentional_failure", NULL};
    int fd;
    int rc;

    bytebuf_init(&output);
    fd = mkstemp(log_path);
    YEW_ASSERT(fd >= 0);
    (void)close(fd);
    (void)unlink(log_path);
    rc = run_unit_child(argv, log_path, true, &output);
    (void)unlink(log_path);

    YEW_ASSERT_EQ_I64(rc, 1);
    YEW_ASSERT_EQ_U64(output.len, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_MEM(output.data, expected, sizeof(expected) - 1U);
    bytebuf_free(&output);
}

void test_harness_intentional_failure(void)
{
    if (getenv("YEW_TEST_SELFCHECK") != NULL) {
        yew_test_capture_log();
        yew_log(YEW_LOG_ERROR, "captured before intentional failure");
#line 404 "tests/unit/test_harness.c"
        YEW_ASSERT_EQ_U64(1U, 2U);
#line 192 "tests/unit/test_harness.c"
    }
    YEW_ASSERT(true);
}

void test_harness_only_selects_one_exact_name(void)
{
    Bytebuf output;
    char *exact[] = {(char *)yew_test_program_path(), "--only",
                     "args_parse_batch_misuse", NULL};
    char *prefix[] = {(char *)yew_test_program_path(), "--only",
                      "args_parse_", NULL};
    int rc;

    bytebuf_init(&output);
    rc = run_unit_child(exact, NULL, false, &output);
    YEW_ASSERT_EQ_I64(rc, 0);
    YEW_ASSERT_EQ_U64(substring_count(&output, "PASS "), 1U);
    YEW_ASSERT_EQ_U64(substring_count(&output,
                                      "PASS args_parse_batch_misuse\n"), 1U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "unit: 1 tests,"), 1U);
    bytebuf_free(&output);

    /* A substring is --filter's job: --only takes a whole name. */
    bytebuf_init(&output);
    rc = run_unit_child(prefix, NULL, false, &output);
    YEW_ASSERT_EQ_I64(rc, 1);
    YEW_ASSERT_EQ_U64(substring_count(&output, "matched zero tests"), 1U);
    bytebuf_free(&output);
}

/* Set by the parent before it spawns; a fresh process never sees it. */
static int harness_parent_only_state;
static char harness_prep_value[] = "prepared";

static bool harness_child_prep(void *user)
{
    return setenv("YEW_HARNESS_PREP", user, 1) == 0;
}

void test_harness_spawned_child_is_fresh(void)
{
    const char *role = yew_test_child_role();
    YewTestChild child;
    const char *home;
    char expected[256];
    int n;

    if (role != NULL) {
        const char *prep = getenv("YEW_HARNESS_PREP");

        home = getenv("HOME");
        if (strcmp(role, "probe") != 0)
            _exit(3);
        if (harness_parent_only_state != 0)
            _exit(4);
        /* The marker is consumed, so the child's own children are not
         * mistaken for children of this test. */
        if (getenv("YEW_TEST_CHILD") != NULL ||
            getenv("YEW_TEST_CHILD_ROLE") != NULL)
            _exit(5);
        if (prep == NULL || strcmp(prep, harness_prep_value) != 0)
            _exit(6);
        (void)fprintf(stderr, "child home=%s\n", home == NULL ? "" : home);
        _exit(7);
    }
    harness_parent_only_state = 1;
    yew_test_spawn_child("probe", harness_child_prep, harness_prep_value,
                         &child);
    harness_parent_only_state = 0;
    YEW_ASSERT_CHILD_EXIT(&child, 7);
    /* It shares this run's isolated HOME rather than making its own. */
    home = getenv("HOME");
    YEW_ASSERT_NOT_NULL(home);
    n = snprintf(expected, sizeof(expected), "child home=%s\n", home);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(expected));
    YEW_ASSERT_EQ_U64(child.err_len, (u64)n);
    YEW_ASSERT_EQ_STR(child.err, expected);
    yew_test_child_free(&child);
    YEW_ASSERT_NULL(getenv("YEW_HARNESS_PREP"));
}

/*
 * The leak is blamed on the test that made it.  The child runs with
 * LeakSanitizer's exit-time check off, so only the harness's batch
 * check (a batch of one: the child runs this test alone) can turn its
 * exit status to 1 and name the test; its stdout
 * joins the captured stderr so the FAIL line can be read.
 */
static void *volatile harness_leak_sink;
static char harness_leak_options[1024];

/* Called through a volatile pointer, so no copy of the lost pointer
 * outlives this frame in the caller's registers. */
static void harness_leak_once(void)
{
    harness_leak_sink = malloc(48U);
    harness_leak_sink = NULL;
}

static void (*volatile harness_leak)(void) = harness_leak_once;

static bool harness_leak_prep(void *user)
{
    (void)user;
    return dup2(STDERR_FILENO, STDOUT_FILENO) >= 0 &&
           setenv("ASAN_OPTIONS", harness_leak_options, 1) == 0;
}

void test_harness_leak_is_blamed_on_the_leaking_test(void)
{
    const char *options;
    YewTestChild child;
    int n;

    if (yew_test_child_role() != NULL) {
        harness_leak();
        return;
    }
    if (!yew_test_leak_check_enabled())
        yew_test_skip("leak-check: no LeakSanitizer in this build");
    options = getenv("ASAN_OPTIONS");
    n = snprintf(harness_leak_options, sizeof(harness_leak_options),
                 "%s%sdetect_leaks=1:leak_check_at_exit=0",
                 options == NULL ? "" : options,
                 options == NULL || options[0] == '\0' ? "" : ":");
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(harness_leak_options));
    yew_test_spawn_child("leak", harness_leak_prep, NULL, &child);
    YEW_ASSERT_CHILD_EXIT(&child, 1);
    YEW_ASSERT_NOT_NULL(strstr(child.err,
                               "FAIL harness_leak_is_blamed_on_the_leaking_"
                               "test: LeakSanitizer found a leak"));
    yew_test_child_free(&child);
}

/*
 * A leak found at the end of a batch is narrowed to its test by
 * rerunning each of the batch's tests alone.  The two members do
 * nothing unless YEW_TEST_LEAK_DEMO is set, which only this test's
 * child run sets.
 */
void test_harness_leak_batch_member_clean(void)
{
    YEW_ASSERT(true);
}

void test_harness_leak_batch_member_leaks(void)
{
    if (getenv("YEW_TEST_LEAK_DEMO") != NULL)
        harness_leak();
    YEW_ASSERT(true);
}

void test_harness_leak_in_a_batch_is_narrowed_to_its_test(void)
{
    const char *options;
    const char *env[] = {"YEW_TEST_LEAK_DEMO", "1", "YEW_TEST_LEAK_EVERY",
                         "64", "ASAN_OPTIONS", harness_leak_options, NULL};
    char *argv[] = {(char *)yew_test_program_path(), "--filter",
                    "harness_leak_batch_member_", NULL};
    Bytebuf output;
    int n;
    int rc;

    if (!yew_test_leak_check_enabled())
        yew_test_skip("leak-check: no LeakSanitizer in this build");
    options = getenv("ASAN_OPTIONS");
    n = snprintf(harness_leak_options, sizeof(harness_leak_options),
                 "%s%sdetect_leaks=1:leak_check_at_exit=0",
                 options == NULL ? "" : options,
                 options == NULL || options[0] == '\0' ? "" : ":");
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(harness_leak_options));
    bytebuf_init(&output);
    rc = run_unit_child_env(argv, NULL, false, env, &output);
    YEW_ASSERT_EQ_I64(rc, 1);
    YEW_ASSERT_EQ_U64(substring_count(&output, "PASS harness_leak_batch_"
                                               "member_"), 2U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "found a leak in the last "
                                               "2 tests"), 1U);
    YEW_ASSERT_EQ_U64(substring_count(&output,
                                      "FAIL harness_leak_batch_member_leaks: "
                                      "LeakSanitizer found a leak when it "
                                      "ran alone\n"), 1U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "FAIL harness_leak_batch_"
                                               "member_clean"), 0U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "leak checks are off"), 1U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "unit: 2 tests,"), 1U);
    bytebuf_free(&output);
}

/* The batch size is a count from 1 to 4096; anything else stops the run
 * before a test starts. */
void test_harness_leak_every_rejects_a_bad_count(void)
{
    static const char *const bad[] = {"0", "4097", "-1", "", "8x", "abc"};
    char *argv[] = {(char *)yew_test_program_path(), "--only",
                    "args_parse_batch_misuse", NULL};
    const char *env[] = {"YEW_TEST_LEAK_EVERY", NULL, NULL};
    Bytebuf output;
    size_t i;
    int rc;

    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        env[1] = bad[i];
        bytebuf_init(&output);
        rc = run_unit_child_env(argv, NULL, false, env, &output);
        YEW_ASSERT_EQ_I64(rc, 1);
        YEW_ASSERT_EQ_U64(substring_count(&output, "YEW_TEST_LEAK_EVERY "
                                                   "must be a count from 1 "
                                                   "to 4096"), 1U);
        YEW_ASSERT_EQ_U64(substring_count(&output, "PASS "), 0U);
        bytebuf_free(&output);
    }
    env[1] = "4096";
    bytebuf_init(&output);
    rc = run_unit_child_env(argv, NULL, false, env, &output);
    YEW_ASSERT_EQ_I64(rc, 0);
    YEW_ASSERT_EQ_U64(substring_count(&output,
                                      "PASS args_parse_batch_misuse\n"), 1U);
    bytebuf_free(&output);
}

/*
 * SHELL is put back before every test.  The first member fails after
 * pointing SHELL elsewhere; the second sees the run's own SHELL.  Both
 * do nothing unless YEW_TEST_SHELL_DEMO (the SHELL the run started
 * with) is set, which only this test's child run sets.
 */
void test_harness_shell_member_a_fails_mid_change(void)
{
    if (getenv("YEW_TEST_SHELL_DEMO") == NULL)
        return;
    YEW_ASSERT_EQ_I64(setenv("SHELL", "/nonexistent/fixture-sh", 1), 0);
    YEW_ASSERT(false);
}

void test_harness_shell_member_b_sees_the_run_shell(void)
{
    const char *want = getenv("YEW_TEST_SHELL_DEMO");
    const char *shell = getenv("SHELL");

    if (want == NULL)
        return;
    YEW_ASSERT_NOT_NULL(shell);
    YEW_ASSERT_EQ_STR(shell, want);
}

void test_harness_shell_is_restored_between_tests(void)
{
    const char *env[] = {"SHELL", "/bin/sh", "YEW_TEST_SHELL_DEMO",
                         "/bin/sh", NULL};
    char *argv[] = {(char *)yew_test_program_path(), "--filter",
                    "harness_shell_member_", NULL};
    Bytebuf output;
    int rc;

    bytebuf_init(&output);
    rc = run_unit_child_env(argv, NULL, false, env, &output);
    YEW_ASSERT_EQ_I64(rc, 1);
    YEW_ASSERT_EQ_U64(substring_count(&output, "FAIL harness_shell_member_"
                                               "a_fails_mid_change"), 1U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "PASS harness_shell_member_"
                                               "b_sees_the_run_shell\n"),
                      1U);
    YEW_ASSERT_EQ_U64(substring_count(&output, "unit: 2 tests,"), 1U);
    bytebuf_free(&output);
}
