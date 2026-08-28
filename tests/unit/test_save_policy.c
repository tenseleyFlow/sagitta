#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/option.h"
#include "text/edit.h"
#include "text/file.h"
#include "unit/stat_time.h"

static void policy_write(const char *path, const char *text)
{
    size_t len = strlen(text);
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    YEW_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, text + at, len - at);

        YEW_ASSERT(n > 0);
        at += (size_t)n;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void policy_assert_bytes(const char *path, const char *want)
{
    char bytes[128];
    size_t len = strlen(want);
    int fd = open(path, O_RDONLY);
    ssize_t n;

    YEW_ASSERT(fd >= 0);
    n = read(fd, bytes, sizeof(bytes));
    YEW_ASSERT_EQ_I64(n, (ssize_t)len);
    YEW_ASSERT_EQ_MEM(bytes, want, len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static u64 policy_fnv64(const char *text)
{
    u64 hash = UINT64_C(14695981039346656037);

    while (*text != '\0') {
        hash ^= (u8)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void policy_content_accepts_bom_crlf_mtime_change(void);
static void policy_atomic_result_reports_commit(void);
static void policy_backup_failure_commits_edit_state(void);

void test_save_policy_defaults_match_option_table(void)
{
    const OptDesc *desc;
    YewSaveOpts opts;

    yew_file_save_opts_default(&opts);
    YEW_ASSERT_EQ_U64(opts.strategy, YEW_SAVE_STRATEGY_DEFAULT);
    YEW_ASSERT_EQ_U64(opts.check_disk, YEW_SAVE_CHECK_DISK_DEFAULT);
    YEW_ASSERT_EQ_U64(opts.check_disk_max,
                      YEW_SAVE_CHECK_DISK_MAX_DEFAULT);
    YEW_ASSERT_EQ_U64(opts.backup_keep, YEW_SAVE_BACKUP_KEEP_DEFAULT);
    YEW_ASSERT_EQ_MEM(opts.backup_dir, YEW_SAVE_BACKUP_DIR_DEFAULT, 1U);
    desc = yew_opt_desc("save.check_disk_max", 19U);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_EQ_I64(desc->dflt.as.i,
                      (i64)YEW_SAVE_CHECK_DISK_MAX_DEFAULT);
    desc = yew_opt_desc("save.backup_keep", 16U);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_EQ_I64(desc->dflt.as.i, YEW_SAVE_BACKUP_KEEP_DEFAULT);
    desc = yew_opt_desc("plug.verify_on_load", 19U);
#if YEW_WITH_PLUGINS
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT(desc->dflt.as.b);
#else
    YEW_ASSERT_NULL(desc);
#endif
    policy_atomic_result_reports_commit();
}

static void policy_atomic_result_reports_commit(void)
{
    static const u8 bytes[] = "state\n";
    char dir[] = "/tmp/yew-atomic-result-XXXXXX";
    char path[128];
    char missing[160];
    YewAtomicWriteResult result;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/state", dir);
    result = yew_file_write_atomic_result(path, bytes, sizeof(bytes) - 1U,
                                          0600U);
    YEW_ASSERT_EQ_U64(result.error, YEW_SAVE_OK);
    YEW_ASSERT(result.committed);
    policy_assert_bytes(path, "state\n");
    (void)snprintf(missing, sizeof(missing), "%s/missing/state", dir);
    result = yew_file_write_atomic_result(missing, bytes,
                                          sizeof(bytes) - 1U, 0600U);
    YEW_ASSERT_EQ_U64(result.error, YEW_SAVE_IO);
    YEW_ASSERT(!result.committed);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}

void test_save_policy_content_accepts_identical_mtime_change(void)
{
    char dir[] = "/tmp/yew-save-content-XXXXXX";
    char path[128];
    struct stat st;
    struct timespec times[2];
    FileMeta meta;
    TextBuf *tb = NULL;
    YewSaveOpts opts;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/file.txt", dir);
    policy_write(path, "old\n");
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_EQ_I64(stat(path, &st), 0);
    times[0] = yew_test_stat_atime(&st);
    times[1] = yew_test_stat_mtime(&st);
    times[1].tv_sec++;
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, path, times, 0), 0);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)),
                       (const u8 *)"edit\n", 5U);
    yew_file_save_opts_default(&opts);
    opts.check_disk = YEW_SAVE_CHECK_CONTENT;
    YEW_ASSERT_EQ_U64(yew_file_save_opts(tb, &meta, path, &opts),
                      YEW_SAVE_OK);
    policy_assert_bytes(path, "old\nedit\n");
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    policy_content_accepts_bom_crlf_mtime_change();
}

static void policy_content_accepts_bom_crlf_mtime_change(void)
{
    static const char disk_text[] = "\xEF\xBB\xBFold\r\n";
    char dir[] = "/tmp/yew-save-content-crlf-XXXXXX";
    char path[128];
    struct stat st;
    struct timespec times[2];
    FileMeta meta;
    TextBuf *tb = NULL;
    YewSaveOpts opts;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/file.txt", dir);
    policy_write(path, disk_text);
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT(meta.had_bom);
    YEW_ASSERT_EQ_U64(meta.eol, YEW_EOL_CRLF);
    YEW_ASSERT_EQ_I64(stat(path, &st), 0);
    times[0] = yew_test_stat_atime(&st);
    times[1] = yew_test_stat_mtime(&st);
    times[1].tv_sec++;
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, path, times, 0), 0);
    yew_file_save_opts_default(&opts);
    opts.check_disk = YEW_SAVE_CHECK_CONTENT;
    YEW_ASSERT_EQ_U64(yew_file_save_opts(tb, &meta, path, &opts),
                      YEW_SAVE_OK);
    policy_assert_bytes(path, disk_text);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}

void test_save_policy_content_rejects_different_bytes(void)
{
    char dir[] = "/tmp/yew-save-conflict-XXXXXX";
    char path[128];
    FileMeta meta;
    TextBuf *tb = NULL;
    YewSaveOpts opts;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/file.txt", dir);
    policy_write(path, "old\n");
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    policy_write(path, "bad\n");
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)),
                       (const u8 *)"edit\n", 5U);
    yew_file_save_opts_default(&opts);
    opts.check_disk = YEW_SAVE_CHECK_CONTENT;
    YEW_ASSERT_EQ_U64(yew_file_save_opts(tb, &meta, path, &opts),
                      YEW_SAVE_CHANGED_ON_DISK);
    policy_assert_bytes(path, "bad\n");
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}

void test_save_policy_inplace_rotates_custom_backups(void)
{
    char dir[] = "/tmp/yew-save-rotate-XXXXXX";
    char path[128];
    char backup_dir[128];
    char resolved[PATH_MAX];
    char backup[192];
    char pending[224];
    char recovery[224];
    FileMeta meta;
    TextBuf *tb = NULL;
    YewSaveOpts opts;
    u32 i;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/file.txt", dir);
    (void)snprintf(backup_dir, sizeof(backup_dir), "%s/backups", dir);
    policy_write(path, "0");
    YEW_ASSERT_NOT_NULL(realpath(path, resolved));
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    yew_file_save_opts_default(&opts);
    opts.strategy = YEW_SAVE_STRATEGY_INPLACE;
    opts.backup_keep = 10U;
    opts.backup_dir = backup_dir;
    for (i = 1U; i <= 10U; i++) {
        char value[16];
        int len = snprintf(value, sizeof(value), "%u", i);

        YEW_ASSERT(len > 0);
        yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
        yew_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)value, (size_t)len);
        YEW_ASSERT_EQ_U64(yew_file_save_opts(tb, &meta, path, &opts),
                          YEW_SAVE_OK);
    }
    (void)snprintf(backup, sizeof(backup), "%s/%016" PRIx64 ".bak",
                   backup_dir, policy_fnv64(resolved));
    policy_assert_bytes(backup, "9");
    for (i = 1U; i < 10U; i++) {
        char want[16];

        (void)snprintf(backup, sizeof(backup),
                       "%s/%016" PRIx64 ".%u.bak", backup_dir,
                       policy_fnv64(resolved), i);
        (void)snprintf(want, sizeof(want), "%u", 9U - i);
        policy_assert_bytes(backup, want);
    }
    (void)snprintf(backup, sizeof(backup), "%s/%016" PRIx64 ".bak",
                   backup_dir, policy_fnv64(resolved));
    (void)snprintf(pending, sizeof(pending), "%s/%016" PRIx64
                   ".pending.bak", backup_dir, policy_fnv64(resolved));
    (void)snprintf(recovery, sizeof(recovery), "%s.recover.bak", backup);
    policy_write(pending, "stale pending");
    policy_write(recovery, "stale recovery");
    yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
    yew_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)"11", 2U);
    YEW_ASSERT_EQ_U64(yew_file_save_opts(tb, &meta, path, &opts),
                      YEW_SAVE_OK);
    policy_assert_bytes(path, "11");
    policy_assert_bytes(backup, "10");
    policy_assert_bytes(pending, "stale pending");
    policy_assert_bytes(recovery, "stale recovery");
    YEW_ASSERT_EQ_I64(unlink(pending), 0);
    YEW_ASSERT_EQ_I64(unlink(recovery), 0);
    (void)snprintf(backup, sizeof(backup), "%s/%016" PRIx64 ".bak",
                   backup_dir, policy_fnv64(resolved));
    YEW_ASSERT_EQ_I64(unlink(backup), 0);
    for (i = 1U; i < 10U; i++) {
        (void)snprintf(backup, sizeof(backup),
                       "%s/%016" PRIx64 ".%u.bak", backup_dir,
                       policy_fnv64(resolved), i);
        YEW_ASSERT_EQ_I64(unlink(backup), 0);
    }
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(backup_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    policy_backup_failure_commits_edit_state();
}

static void policy_backup_failure_commits_edit_state(void)
{
    char dir[] = "/tmp/yew-save-backup-fail-XXXXXX";
    char path[160];
    char backup_dir[160];
    char resolved[PATH_MAX];
    char backup[224];
    char pending[224];
    FileMeta meta;
    TextBuf *tb = NULL;
    UndoTree *undo;
    EditCtx edit;
    YewSaveOpts opts;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/file.txt", dir);
    (void)snprintf(backup_dir, sizeof(backup_dir), "%s/backups", dir);
    YEW_ASSERT_EQ_I64(mkdir(backup_dir, 0700U), 0);
    policy_write(path, "old");
    YEW_ASSERT_NOT_NULL(realpath(path, resolved));
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
    yew_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)"new", 3U);
    undo = yew_undo_new(tb);
    undo->saved = UINT32_MAX;
    (void)memset(&edit, 0, sizeof(edit));
    edit.tb = tb;
    edit.meta = &meta;
    edit.undo = undo;
    edit.jrnl = yew_journal_open(meta.realpath, &meta);
    YEW_ASSERT_NOT_NULL(edit.jrnl);
    yew_file_save_opts_default(&opts);
    opts.strategy = YEW_SAVE_STRATEGY_INPLACE;
    opts.backup_dir = backup_dir;
    (void)snprintf(backup, sizeof(backup), "%s/%016" PRIx64 ".bak",
                   backup_dir, policy_fnv64(resolved));
    YEW_ASSERT_EQ_I64(mkdir(backup, 0700U), 0);
    YEW_ASSERT_EQ_U64(yew_edit_save_opts(&edit, path, &opts),
                      YEW_SAVE_BACKUP_FAILED);
    policy_assert_bytes(path, "new");
    YEW_ASSERT_EQ_U64(meta.size_on_disk, 3U);
    YEW_ASSERT(meta.disk_snapshot_valid);
    YEW_ASSERT_EQ_U64(undo->saved, undo->cur);
    YEW_ASSERT_NULL(edit.jrnl);
    (void)snprintf(pending, sizeof(pending), "%.*s.pending.bak",
                   (int)(strlen(backup) - strlen(".bak")), backup);
    policy_assert_bytes(pending, "old");
    YEW_ASSERT_EQ_I64(unlink(pending), 0);
    YEW_ASSERT_EQ_I64(rmdir(backup), 0);
    yew_undo_free(undo);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(backup_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}
