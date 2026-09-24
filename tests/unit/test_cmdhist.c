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
    char *copy = yew_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void hist_fixture_init(HistFixture *f)
{
    const char *old = getenv("XDG_STATE_HOME");

    f->saved_state = old == NULL ? NULL : test_dup(old);
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-hist-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->root, 1), 0);
}

static void hist_fixture_free(HistFixture *f)
{
    char path[160];
    static const char *const names[] = {"cmd", "cmd.lock", "search",
                                        "search.lock", "shell", "shell.lock"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        (void)snprintf(path, sizeof(path), "%s/yew/history/%s", f->root,
                       names[i]);
        if (unlink(path) != 0)
            YEW_ASSERT(errno == ENOENT);
    }
    (void)snprintf(path, sizeof(path), "%s/yew/history", f->root);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
    (void)snprintf(path, sizeof(path), "%s/yew", f->root);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    if (f->saved_state != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->saved_state, 1), 0);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    free(f->saved_state);
}

static void write_exact(const char *path, const void *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    ssize_t wrote;

    YEW_ASSERT(fd >= 0);
    do {
        wrote = write(fd, bytes, len);
    } while (wrote < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(wrote, (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

void test_cmdhist_memory_rules(void)
{
    CmdHist *h = yew_hist_open_memory();
    char long_line[YEW_HIST_LINE_MAX + 128U];
    char value[24];
    size_t i;

    YEW_ASSERT(yew_hist_is_memory(h));
    yew_hist_add(h, "alpha");
    yew_hist_add(h, "beta");
    yew_hist_add(h, "alpha");
    YEW_ASSERT_EQ_U64(yew_hist_len(h), 2U);
    YEW_ASSERT_EQ_STR(yew_hist_at(h, 0U), "beta");
    YEW_ASSERT_EQ_STR(yew_hist_at(h, 1U), "alpha");
    yew_hist_add(h, "alpha");
    YEW_ASSERT_EQ_U64(yew_hist_len(h), 2U);
    yew_hist_add(h, " hidden");
    yew_hist_add(h, "");
    YEW_ASSERT_EQ_U64(yew_hist_len(h), 2U);

    (void)memset(long_line, 'x', sizeof(long_line) - 1U);
    long_line[sizeof(long_line) - 1U] = '\0';
    yew_hist_add(h, long_line);
    YEW_ASSERT_EQ_U64(strlen(yew_hist_at(h, 2U)), YEW_HIST_LINE_MAX);

    for (i = 0U; i < YEW_HIST_MAX + 5U; i++) {
        (void)snprintf(value, sizeof(value), "value-%04zu", i);
        yew_hist_add(h, value);
    }
    YEW_ASSERT_EQ_U64(yew_hist_len(h), YEW_HIST_MAX);
    YEW_ASSERT_EQ_STR(yew_hist_at(h, 0U), "value-0005");
    YEW_ASSERT_EQ_STR(yew_hist_at(h, YEW_HIST_MAX - 1U), "value-1004");
    yew_hist_flush(h);
    yew_hist_close(h);
}

void test_cmdhist_frozen_stem_navigation(void)
{
    CmdHist *h = yew_hist_open_memory();
    HistCur cur = {.idx = -1};

    yew_hist_add(h, "write old");
    yew_hist_add(h, "other");
    yew_hist_add(h, "write new");
    yew_hist_cur_reset(&cur, "wr");
    YEW_ASSERT_EQ_STR(yew_hist_prev(h, &cur), "write new");
    YEW_ASSERT_EQ_STR(cur.stem, "wr");
    YEW_ASSERT_EQ_STR(yew_hist_prev(h, &cur), "write old");
    YEW_ASSERT_NULL(yew_hist_prev(h, &cur));
    YEW_ASSERT_EQ_STR(yew_hist_next(h, &cur), "write new");
    YEW_ASSERT_EQ_STR(yew_hist_next(h, &cur), "wr");
    YEW_ASSERT_EQ_I64(cur.idx, -1);
    YEW_ASSERT_NULL(yew_hist_next(h, &cur));

    yew_hist_cur_reset(&cur, "other draft");
    YEW_ASSERT_NULL(yew_hist_prev(h, &cur));
    YEW_ASSERT_EQ_STR(cur.stem, "other draft");
    YEW_ASSERT_EQ_STR(cur.draft, "other draft");
    yew_hist_cur_dispose(&cur);
    yew_hist_close(h);
}

/*
 * Sprint 57.30 §2: the smart walk -- substring, newest first, a frozen
 * term, duplicates skipped, Down past the newest back at the draft.
 */
void test_cmdhist_smart_walk_substring_and_draft(void)
{
    CmdHist *h = yew_hist_open_memory();
    YewHistView v = {h, NULL};
    YewHistWalk w;
    bool at_draft = true;

    (void)memset(&w, 0, sizeof(w));
    yew_hist_add(h, "set wrap");
    yew_hist_add(h, "write old");
    yew_hist_add(h, "other");
    yew_hist_add(h, "rewrite");
    YEW_ASSERT_EQ_U64(yew_hist_view_len(&v), 4U);
    YEW_ASSERT_EQ_STR(yew_hist_view_at(&v, 0U), "rewrite");
    YEW_ASSERT_NULL(yew_hist_view_at(&v, 4U));

    /* Nothing walked yet: no current entry, nothing newer. */
    yew_hist_walk_begin(&w, "wr", "wr", 2U);
    YEW_ASSERT_NULL(yew_hist_walk_current(&w, &v));
    YEW_ASSERT_NULL(yew_hist_walk_newer(&w, &v, &at_draft));
    YEW_ASSERT(!at_draft);
    /* A SUBSTRING, not a prefix: `rewrite` and `set wrap` match. */
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "rewrite");
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "write old");
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "set wrap");
    YEW_ASSERT_EQ_STR(yew_hist_walk_current(&w, &v), "set wrap");
    /* Past the oldest it stays put. */
    YEW_ASSERT_NULL(yew_hist_walk_older(&w, &v));
    YEW_ASSERT_EQ_STR(yew_hist_walk_current(&w, &v), "set wrap");
    YEW_ASSERT_EQ_STR(w.term, "wr");
    YEW_ASSERT_EQ_STR(yew_hist_walk_newer(&w, &v, &at_draft), "write old");
    YEW_ASSERT_EQ_STR(yew_hist_walk_newer(&w, &v, &at_draft), "rewrite");
    YEW_ASSERT_NULL(yew_hist_walk_newer(&w, &v, &at_draft));
    YEW_ASSERT(at_draft);
    YEW_ASSERT_EQ_STR(w.draft, "wr");
    YEW_ASSERT_NULL(yew_hist_walk_current(&w, &v));

    /* Case-sensitive, and an empty term walks everything. */
    yew_hist_walk_begin(&w, "", "", 0U);
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "rewrite");
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "other");
    yew_hist_walk_begin(&w, "WR", "WR", 2U);
    YEW_ASSERT_NULL(yew_hist_walk_older(&w, &v));
    yew_hist_walk_end(&w);
    YEW_ASSERT(!w.on);
    yew_hist_walk_end(&w);
    yew_hist_close(h);
}

/*
 * An entry equal to one this walk already showed is skipped.  Both real
 * sources dedupe on the way in, so the view is built by hand here: the
 * walk's rule must not lean on theirs.
 */
void test_cmdhist_smart_walk_skips_duplicates(void)
{
    static char pool[] = "git log\0ls\0git log\0git status";
    static u32 off[] = {0U, 8U, 11U, 19U};
    YewHistSuggest s;
    YewHistView v = {NULL, NULL};
    YewHistWalk w;
    bool at_draft = false;
    size_t at = 99U;

    yew_hist_suggest_init(&s);
    s.pool = pool;
    s.off = off;
    s.n = 4U;
    v.suggest = &s;
    (void)memset(&w, 0, sizeof(w));
    yew_hist_walk_begin(&w, "!g", "g", 1U);
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "git log");
    /* `ls` does not match; the second `git log` was already shown. */
    YEW_ASSERT_EQ_STR(yew_hist_walk_older(&w, &v), "git status");
    YEW_ASSERT_EQ_U64(w.n_seen, 2U);
    YEW_ASSERT_EQ_U64(w.seen[1], 3U);
    YEW_ASSERT_NULL(yew_hist_walk_older(&w, &v));
    YEW_ASSERT_EQ_STR(yew_hist_walk_newer(&w, &v, &at_draft), "git log");
    YEW_ASSERT(!at_draft);
    yew_hist_walk_end(&w);

    YEW_ASSERT(yew_hist_find("abcabc", 6U, "ca", 2U, &at));
    YEW_ASSERT_EQ_U64(at, 2U);
    YEW_ASSERT(!yew_hist_find("abc", 3U, "abcd", 4U, &at));
    YEW_ASSERT(!yew_hist_find("abc", 3U, "C", 1U, &at));
    YEW_ASSERT(yew_hist_find("abc", 3U, "", 0U, &at));
    YEW_ASSERT_EQ_U64(at, 0U);
    YEW_ASSERT(yew_hist_find("xxab", 4U, "ab", 2U, &at));
    YEW_ASSERT_EQ_U64(at, 2U);
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
    h = yew_hist_open_memory();
    yew_hist_add(h, "never-written");
    yew_hist_flush(h);
    yew_hist_close(h);
    (void)snprintf(path, sizeof(path), "%s/yew", fixture.root);
    YEW_ASSERT_EQ_I64(stat(path, &st), -1);
    YEW_ASSERT_EQ_I64(errno, ENOENT);

    h = yew_hist_open("cmd");
    YEW_ASSERT(!yew_hist_is_memory(h));
    yew_hist_add(h, odd);
    yew_hist_add(h, "plain");
    yew_hist_flush(h);
    yew_hist_close(h);

    h = yew_hist_open("cmd");
    YEW_ASSERT_EQ_U64(yew_hist_len(h), 2U);
    YEW_ASSERT_EQ_MEM(yew_hist_at(h, 0U), odd, sizeof(odd));
    YEW_ASSERT_EQ_STR(yew_hist_at(h, 1U), "plain");
    yew_hist_close(h);

    (void)snprintf(path, sizeof(path), "%s/yew/history/cmd",
                   fixture.root);
    write_exact(path, corrupt, sizeof(corrupt) - 1U);
    yew_test_capture_log();
    h = yew_hist_open("cmd");
    YEW_ASSERT_EQ_U64(yew_hist_len(h), 2U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "dropping corrupt history entry"));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "dropping incomplete history entry"));
    yew_hist_close(h);
    hist_fixture_free(&fixture);
}

void test_cmdhist_concurrent_handles_preserve_appends(void)
{
    HistFixture fixture;
    CmdHist *first;
    CmdHist *second;
    CmdHist *check;

    hist_fixture_init(&fixture);
    first = yew_hist_open("search");
    second = yew_hist_open("search");
    yew_hist_add(first, "from-first");
    yew_hist_add(second, "from-second");
    yew_hist_flush(first);
    yew_hist_flush(second);
    yew_hist_close(second);
    yew_hist_close(first);

    check = yew_hist_open("search");
    YEW_ASSERT_EQ_U64(yew_hist_len(check), 2U);
    YEW_ASSERT_EQ_STR(yew_hist_at(check, 0U), "from-first");
    YEW_ASSERT_EQ_STR(yew_hist_at(check, 1U), "from-second");
    yew_hist_close(check);
    hist_fixture_free(&fixture);
}

void test_cmdhist_flush_read_failure_preserves_state(void)
{
    HistFixture fixture;
    char path[160];
    struct stat st;
    CmdHist *h;

    hist_fixture_init(&fixture);
    h = yew_hist_open("shell");
    yew_hist_add(h, "preserve");
    (void)snprintf(path, sizeof(path), "%s/yew/history/shell",
                   fixture.root);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    yew_test_capture_log();
    yew_hist_flush(h);
    YEW_ASSERT_EQ_I64(stat(path, &st), 0);
    YEW_ASSERT(S_ISDIR(st.st_mode));
    YEW_ASSERT_EQ_U64(yew_hist_len(h), 1U);
    YEW_ASSERT_EQ_STR(yew_hist_at(h, 0U), "preserve");
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "cannot read command history"));
    yew_hist_close(h);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
    hist_fixture_free(&fixture);
}
