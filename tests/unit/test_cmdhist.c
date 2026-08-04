#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui/cmdhist.h"

typedef struct HistFixture {
    char root[64];
    char *saved_state;
} HistFixture;

static char *test_dup(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = sag_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void hist_fixture_init(HistFixture *f)
{
    const char *old = getenv("XDG_STATE_HOME");

    f->saved_state = old == NULL ? NULL : test_dup(old);
    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-hist-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->root));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->root, 1), 0);
}

static void hist_fixture_free(HistFixture *f)
{
    char path[160];
    static const char *const names[] = {"cmd", "cmd.lock", "search",
                                        "search.lock", "shell", "shell.lock"};
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(names); i++) {
        (void)snprintf(path, sizeof(path), "%s/sagitta/history/%s", f->root,
                       names[i]);
        if (unlink(path) != 0)
            SAG_ASSERT(errno == ENOENT);
    }
    (void)snprintf(path, sizeof(path), "%s/sagitta/history", f->root);
    SAG_ASSERT_EQ_I64(rmdir(path), 0);
    (void)snprintf(path, sizeof(path), "%s/sagitta", f->root);
    SAG_ASSERT_EQ_I64(rmdir(path), 0);
    SAG_ASSERT_EQ_I64(rmdir(f->root), 0);
    if (f->saved_state != NULL) {
        SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->saved_state, 1), 0);
    } else {
        SAG_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    free(f->saved_state);
}

static void write_exact(const char *path, const void *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    ssize_t wrote;

    SAG_ASSERT(fd >= 0);
    do {
        wrote = write(fd, bytes, len);
    } while (wrote < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(wrote, (i64)len);
    SAG_ASSERT_EQ_I64(close(fd), 0);
}

void test_cmdhist_memory_rules(void)
{
    CmdHist *h = sag_hist_open_memory();
    char long_line[SAG_HIST_LINE_MAX + 128U];
    char value[24];
    size_t i;

    SAG_ASSERT(sag_hist_is_memory(h));
    sag_hist_add(h, "alpha");
    sag_hist_add(h, "beta");
    sag_hist_add(h, "alpha");
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 0U), "beta");
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 1U), "alpha");
    sag_hist_add(h, "alpha");
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    sag_hist_add(h, " hidden");
    sag_hist_add(h, "");
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);

    (void)memset(long_line, 'x', sizeof(long_line) - 1U);
    long_line[sizeof(long_line) - 1U] = '\0';
    sag_hist_add(h, long_line);
    SAG_ASSERT_EQ_U64(strlen(sag_hist_at(h, 2U)), SAG_HIST_LINE_MAX);

    for (i = 0U; i < SAG_HIST_MAX + 5U; i++) {
        (void)snprintf(value, sizeof(value), "value-%04zu", i);
        sag_hist_add(h, value);
    }
    SAG_ASSERT_EQ_U64(sag_hist_len(h), SAG_HIST_MAX);
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 0U), "value-0005");
    SAG_ASSERT_EQ_STR(sag_hist_at(h, SAG_HIST_MAX - 1U), "value-1004");
    sag_hist_flush(h);
    sag_hist_close(h);
}

void test_cmdhist_frozen_stem_navigation(void)
{
    CmdHist *h = sag_hist_open_memory();
    HistCur cur = {.idx = -1};

    sag_hist_add(h, "write old");
    sag_hist_add(h, "other");
    sag_hist_add(h, "write new");
    sag_hist_cur_reset(&cur, "wr");
    SAG_ASSERT_EQ_STR(sag_hist_prev(h, &cur), "write new");
    SAG_ASSERT_EQ_STR(cur.stem, "wr");
    SAG_ASSERT_EQ_STR(sag_hist_prev(h, &cur), "write old");
    SAG_ASSERT_NULL(sag_hist_prev(h, &cur));
    SAG_ASSERT_EQ_STR(sag_hist_next(h, &cur), "write new");
    SAG_ASSERT_EQ_STR(sag_hist_next(h, &cur), "wr");
    SAG_ASSERT_EQ_I64(cur.idx, -1);
    SAG_ASSERT_NULL(sag_hist_next(h, &cur));

    sag_hist_cur_reset(&cur, "other draft");
    SAG_ASSERT_NULL(sag_hist_prev(h, &cur));
    SAG_ASSERT_EQ_STR(cur.stem, "other draft");
    SAG_ASSERT_EQ_STR(cur.draft, "other draft");
    sag_hist_cur_dispose(&cur);
    sag_hist_close(h);
}

void test_cmdhist_escape_corruption_and_xdg(void)
{
    static const char odd[] = {'a', '\\', '\n', '\t', '\r', (char)0xff,
                               'z', '\0'};
    static const char corrupt[] = "bad\\q\nshort\\xG0\nincomplete\\";
    HistFixture fixture;
    char path[160];
    struct stat st;
    CmdHist *h;

    hist_fixture_init(&fixture);
    h = sag_hist_open_memory();
    sag_hist_add(h, "never-written");
    sag_hist_flush(h);
    sag_hist_close(h);
    (void)snprintf(path, sizeof(path), "%s/sagitta", fixture.root);
    SAG_ASSERT_EQ_I64(stat(path, &st), -1);
    SAG_ASSERT_EQ_I64(errno, ENOENT);

    h = sag_hist_open("cmd");
    SAG_ASSERT(!sag_hist_is_memory(h));
    sag_hist_add(h, odd);
    sag_hist_add(h, "plain");
    sag_hist_flush(h);
    sag_hist_close(h);

    h = sag_hist_open("cmd");
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    SAG_ASSERT_EQ_MEM(sag_hist_at(h, 0U), odd, sizeof(odd));
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 1U), "plain");
    sag_hist_close(h);

    (void)snprintf(path, sizeof(path), "%s/sagitta/history/cmd",
                   fixture.root);
    write_exact(path, corrupt, sizeof(corrupt) - 1U);
    sag_test_capture_log();
    h = sag_hist_open("cmd");
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN,
                                     "dropping corrupt history entry"));
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN,
                                     "dropping incomplete history entry"));
    sag_hist_close(h);
    hist_fixture_free(&fixture);
}

void test_cmdhist_concurrent_handles_preserve_appends(void)
{
    HistFixture fixture;
    CmdHist *first;
    CmdHist *second;
    CmdHist *check;

    hist_fixture_init(&fixture);
    first = sag_hist_open("search");
    second = sag_hist_open("search");
    sag_hist_add(first, "from-first");
    sag_hist_add(second, "from-second");
    sag_hist_flush(first);
    sag_hist_flush(second);
    sag_hist_close(second);
    sag_hist_close(first);

    check = sag_hist_open("search");
    SAG_ASSERT_EQ_U64(sag_hist_len(check), 2U);
    SAG_ASSERT_EQ_STR(sag_hist_at(check, 0U), "from-first");
    SAG_ASSERT_EQ_STR(sag_hist_at(check, 1U), "from-second");
    sag_hist_close(check);
    hist_fixture_free(&fixture);
}

void test_cmdhist_flush_read_failure_preserves_state(void)
{
    HistFixture fixture;
    char path[160];
    struct stat st;
    CmdHist *h;

    hist_fixture_init(&fixture);
    h = sag_hist_open("shell");
    sag_hist_add(h, "preserve");
    (void)snprintf(path, sizeof(path), "%s/sagitta/history/shell",
                   fixture.root);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
    SAG_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    sag_test_capture_log();
    sag_hist_flush(h);
    SAG_ASSERT_EQ_I64(stat(path, &st), 0);
    SAG_ASSERT(S_ISDIR(st.st_mode));
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 1U);
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 0U), "preserve");
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN,
                                     "cannot read command history"));
    sag_hist_close(h);
    SAG_ASSERT_EQ_I64(rmdir(path), 0);
    hist_fixture_free(&fixture);
}
