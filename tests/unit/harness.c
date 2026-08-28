#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "fl/flruntime.h"
#include "util/buf.h"

typedef struct {
    YewLogLevel level;
    char *message;
} CapturedLog;

static jmp_buf *failure_target;
static const char *failure_file;
static int failure_line;
static char failure_detail[1024];
static size_t assertion_count;
static CapturedLog *captured_logs;
static size_t captured_logs_len;
static size_t captured_logs_cap;
static const char *program_path;

static char *env_copy(const char *name)
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

static void env_restore(const char *name, const char *saved)
{
    if (saved != NULL) {
        if (setenv(name, saved, 1) != 0)
            abort();
    } else if (unsetenv(name) != 0) {
        abort();
    }
}

static void capture_write(void *user, YewLogLevel level, const char *message)
{
    CapturedLog *entry;
    size_t len;

    (void)user;
    if (captured_logs_len == captured_logs_cap) {
        size_t cap = captured_logs_cap == 0U ? 8U : captured_logs_cap * 2U;
        captured_logs = yew_xreallocarray(captured_logs, cap,
                                          sizeof(*captured_logs));
        captured_logs_cap = cap;
    }
    entry = &captured_logs[captured_logs_len++];
    len = strlen(message);
    entry->message = yew_xmalloc(len + 1U);
    (void)memcpy(entry->message, message, len + 1U);
    entry->level = level;
}

static void capture_reset(void)
{
    size_t i;

    for (i = 0U; i < captured_logs_len; i++)
        yew_xfree(captured_logs[i].message);
    yew_xfree(captured_logs);
    captured_logs = NULL;
    captured_logs_len = 0U;
    captured_logs_cap = 0U;
}

void yew_test_capture_log(void)
{
    static const YewLogSink sink = {capture_write, NULL};

    capture_reset();
    yew_log_set_sink(&sink);
}

size_t yew_test_log_count(void)
{
    return captured_logs_len;
}

bool yew_test_log_contains(YewLogLevel level, const char *substr)
{
    size_t i;

    for (i = 0U; i < captured_logs_len; i++) {
        if (captured_logs[i].level == level &&
            strstr(captured_logs[i].message, substr) != NULL)
            return true;
    }
    return false;
}

void yew_test_teardown(void)
{
    yew_log_set_sink(NULL);
    capture_reset();
}

void yew_test_count_assertion(void)
{
    assertion_count++;
}

static _Noreturn void fail_at(const char *file, int line, const char *detail)
{
    failure_file = file;
    failure_line = line;
    (void)snprintf(failure_detail, sizeof(failure_detail), "%s", detail);
    if (failure_target != NULL)
        longjmp(*failure_target, 1);
    abort();
}

_Noreturn void yew_test_fail(const char *file, int line, const char *detail)
{
    fail_at(file, line, detail);
}

_Noreturn void yew_test_fail_i64(const char *file, int line,
                                 i64 left, i64 right)
{
    char detail[160];

    (void)snprintf(detail, sizeof(detail),
                   "YEW_ASSERT_EQ_I64 left=%lld right=%lld",
                   (long long)left, (long long)right);
    fail_at(file, line, detail);
}

_Noreturn void yew_test_fail_u64(const char *file, int line,
                                 u64 left, u64 right)
{
    char detail[160];

    (void)snprintf(detail, sizeof(detail),
                   "YEW_ASSERT_EQ_U64 left=%llu right=%llu",
                   (unsigned long long)left, (unsigned long long)right);
    fail_at(file, line, detail);
}

_Noreturn void yew_test_fail_str(const char *file, int line,
                                 const char *left, const char *right)
{
    char detail[768];

    (void)snprintf(detail, sizeof(detail),
                   "YEW_ASSERT_EQ_STR left=\"%s\" right=\"%s\"",
                   left == NULL ? "(null)" : left,
                   right == NULL ? "(null)" : right);
    fail_at(file, line, detail);
}

_Noreturn void yew_test_fail_mem(const char *file, int line,
                                 size_t offset, u8 left, u8 right)
{
    char detail[192];

    (void)snprintf(detail, sizeof(detail),
                   "YEW_ASSERT_EQ_MEM offset=%zu left=0x%02x right=0x%02x",
                   offset, (unsigned int)left, (unsigned int)right);
    fail_at(file, line, detail);
}

_Noreturn void yew_test_fail_pointer(const char *file, int line,
                                     const char *macro, bool is_null)
{
    char detail[128];

    (void)snprintf(detail, sizeof(detail), "%s pointer is %s", macro,
                   is_null ? "NULL" : "non-NULL");
    fail_at(file, line, detail);
}

bool yew_test_name_matches(const char *name, const char *filter)
{
    return filter == NULL || strstr(name, filter) != NULL;
}

static bool test_is_excluded(const char *name, const char **excluded,
                             size_t excluded_len)
{
    size_t i;

    for (i = 0U; i < excluded_len; i++) {
        if (strcmp(name, excluded[i]) == 0)
            return true;
    }
    return false;
}

const char *yew_test_program_path(void)
{
    return program_path;
}

void yew_test_load_runtime(Ed *ed)
{
    FILE *fp;
    Bytebuf source;
    u8 chunk[4096];
    size_t n;

    fp = fopen("runtime/init.fl", "rb");
    YEW_ASSERT_NOT_NULL(fp);
    bytebuf_init(&source);
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) != 0U)
        bytebuf_append(&source, chunk, n);
    YEW_ASSERT(!ferror(fp));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    yew_bind_batch_begin(ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(ed, (const char *)source.data,
                                  (u32)source.len), YEW_CMD_OK);
    yew_bind_batch_end(ed);
    bytebuf_free(&source);
}

static bool run_one_test(const YewTest *test)
{
    jmp_buf target;

    failure_target = &target;
    failure_file = NULL;
    failure_line = 0;
    failure_detail[0] = '\0';
    if (setjmp(target) == 0) {
        test->fn();
        yew_test_teardown();
        failure_target = NULL;
        (void)printf("PASS %s\n", test->name);
        (void)fflush(stdout);
        return true;
    }
    yew_test_teardown();
    failure_target = NULL;
    (void)printf("FAIL %s at %s:%d: %s\n", test->name,
                 failure_file, failure_line, failure_detail);
    (void)fflush(stdout);
    if (getenv("YEW_TEST_SELFCHECK") != NULL) {
        yew_log(YEW_LOG_INFO, "harness teardown restored default sink");
        if (yew_test_log_count() != 0U)
            (void)printf("FAIL harness teardown left capture sink installed\n");
    }
    return false;
}

int yew_test_run(int argc, char **argv)
{
    const char *filter = NULL;
    const char *excluded[32];
    size_t excluded_len = 0U;
    bool list = false;
    size_t selected = 0U;
    size_t failures = 0U;
    size_t i;
    int argi;
    char *xdg_state;

    program_path = argv[0];
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--list") == 0) {
            list = true;
        } else if (strcmp(argv[argi], "--filter") == 0) {
            if (++argi >= argc) {
                (void)fprintf(stderr, "unit: --filter requires a substring\n");
                return 1;
            }
            filter = argv[argi];
        } else if (strcmp(argv[argi], "--exclude") == 0) {
            if (++argi >= argc || excluded_len ==
                                      sizeof(excluded) / sizeof(excluded[0])) {
                (void)fprintf(stderr,
                              "unit: --exclude requires a test name\n");
                return 1;
            }
            excluded[excluded_len++] = argv[argi];
        } else {
            (void)fprintf(stderr, "unit: unknown option '%s'\n", argv[argi]);
            return 1;
        }
    }

    for (i = 0U; i < yew_tests_len; i++) {
        if (yew_test_name_matches(yew_tests[i].name, filter) &&
            !test_is_excluded(yew_tests[i].name, excluded, excluded_len))
            selected++;
    }
    if (selected == 0U) {
        (void)fprintf(stderr, "unit: filter matched zero tests\n");
        return 1;
    }
    if (list) {
        for (i = 0U; i < yew_tests_len; i++) {
            if (yew_test_name_matches(yew_tests[i].name, filter) &&
                !test_is_excluded(yew_tests[i].name, excluded,
                                  excluded_len))
                (void)printf("%s\n", yew_tests[i].name);
        }
        return 0;
    }

    xdg_state = env_copy("XDG_STATE_HOME");
    for (i = 0U; i < yew_tests_len; i++) {
        if (!yew_test_name_matches(yew_tests[i].name, filter) ||
            test_is_excluded(yew_tests[i].name, excluded, excluded_len))
            continue;
        env_restore("XDG_STATE_HOME", xdg_state);
        if (!run_one_test(&yew_tests[i]))
            failures++;
    }
    env_restore("XDG_STATE_HOME", xdg_state);
    yew_xfree(xdg_state);
    (void)printf("unit: %zu tests, %zu assertions, %zu failure%s\n",
                 selected, assertion_count, failures,
                 failures == 1U ? "" : "s");
    (void)fflush(stdout);
    return failures == 0U ? 0 : 1;
}
