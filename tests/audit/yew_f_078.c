/*
 * YEW-F-078 — crash-journal identity admits a replaced inode.
 *
 * Correct behavior: invariant 1's prescribed symlink/three-hardlink race
 * requires recovery to reproduce the exact unsaved post-edit image.  A file
 * installed at the same canonical path with the same size and nanosecond
 * mtime is not the file whose journal yew recorded.
 *
 * Baseline failure: the journal header authenticates only canonical path,
 * size, and mtime.  After another process replaces the target inode with
 * same-metadata bytes, probe and replay both accept the journal and splice
 * the old file's edit into the replacement instead of recovering the exact
 * post-edit image.  The two remaining hardlinks retain the original base,
 * making the mismatch independently observable and recoverable.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "text/journal.h"

static bool f078_write(const char *path, const u8 *bytes, size_t len)
{
    size_t done = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    if (fd < 0)
        return false;
    while (done < len) {
        ssize_t wrote = write(fd, bytes + done, len - done);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            (void)close(fd);
            return false;
        }
        done += (size_t)wrote;
    }
    return close(fd) == 0;
}

static bool f078_text_is(const TextBuf *tb, const u8 *want, size_t want_len)
{
    TextIter it;
    size_t done = 0U;

    if (tb == NULL || yew_textbuf_len(tb) != (u64)want_len)
        return false;
    if (want_len == 0U)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return false;
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        size_t take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            return false;
        take = len < (u64)(want_len - done) ? (size_t)len
                                             : want_len - done;
        if (memcmp(bytes, want + done, take) != 0)
            return false;
        done += take;
        if (done < want_len && !yew_textiter_advance(&it, tb))
            return false;
    }
    return true;
}

static void f078_remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir;
        struct dirent *entry;

        (void)chmod(path, 0700);
        dir = opendir(path);
        if (dir == NULL)
            return;
        while ((entry = readdir(dir)) != NULL) {
            char child[PATH_MAX];
            int count;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            count = snprintf(child, sizeof(child), "%s/%s", path,
                             entry->d_name);
            if (count > 0 && (size_t)count < sizeof(child))
                f078_remove_tree(child);
        }
        (void)closedir(dir);
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static char *f078_copy_env(const char *value)
{
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value) + 1U;
    copy = malloc(len);
    if (copy != NULL)
        (void)memcpy(copy, value, len);
    return copy;
}

bool test_yew_f_078(char *why, size_t why_cap)
{
    static const u8 original[] = "alpha\n";
    static const u8 replacement[] = "omega\n";
    static const u8 recovered[] = "Xalpha\n";
    char root[] = "/tmp/yew-f078-XXXXXX";
    char work[PATH_MAX] = {0};
    char state[PATH_MAX];
    char target[PATH_MAX];
    char twin_a[PATH_MAX];
    char twin_b[PATH_MAX];
    char link_path[PATH_MAX];
    char incoming[PATH_MAX];
    char *old_state = f078_copy_env(getenv("XDG_STATE_HOME"));
    FileMeta before;
    FileMeta after;
    TextBuf *edited = NULL;
    TextBuf *loaded = NULL;
    Journal *journal = NULL;
    struct timespec times[2];
    struct stat old_st;
    struct stat new_st;
    bool initialized_before = false;
    bool initialized_after = false;
    bool topology = false;
    bool probe = false;
    bool replay = false;
    bool correct = false;
    int count;

    if (mkdtemp(root) == NULL)
        goto done;
    count = snprintf(work, sizeof(work), "%s/work", root);
    if (count <= 0 || (size_t)count >= sizeof(work) ||
        mkdir(work, 0700) != 0)
        goto done;
    count = snprintf(state, sizeof(state), "%s/state", root);
    if (count <= 0 || (size_t)count >= sizeof(state) ||
        mkdir(state, 0700) != 0 || setenv("XDG_STATE_HOME", state, 1) != 0)
        goto done;
    count = snprintf(target, sizeof(target), "%s/target", work);
    if (count <= 0 || (size_t)count >= sizeof(target))
        goto done;
    count = snprintf(twin_a, sizeof(twin_a), "%s/twin-a", work);
    if (count <= 0 || (size_t)count >= sizeof(twin_a))
        goto done;
    count = snprintf(twin_b, sizeof(twin_b), "%s/twin-b", work);
    if (count <= 0 || (size_t)count >= sizeof(twin_b))
        goto done;
    count = snprintf(link_path, sizeof(link_path), "%s/entry", root);
    if (count <= 0 || (size_t)count >= sizeof(link_path))
        goto done;
    count = snprintf(incoming, sizeof(incoming), "%s/incoming", root);
    if (count <= 0 || (size_t)count >= sizeof(incoming))
        goto done;
    if (!f078_write(target, original, sizeof(original) - 1U) ||
        link(target, twin_a) != 0 || link(target, twin_b) != 0 ||
        symlink("work/target", link_path) != 0 ||
        yew_file_load(link_path, &edited, &before) != YEW_LOAD_OK)
        goto done;
    initialized_before = true;
    if (!before.via_symlink || before.nlink != 3U)
        goto done;
    journal = yew_journal_open(before.realpath, &before);
    if (journal == NULL)
        goto done;
    yew_journal_record(journal, YEW_JOURNAL_INS, 0U, (const u8 *)"X", 1U);
    yew_textbuf_insert(edited, BYTEOFF(0U), (const u8 *)"X", 1U);
    yew_journal_sync(journal);
    if (!yew_journal_ok(journal))
        goto done;
    yew_journal_close(journal);
    journal = NULL;

    if (!f078_write(incoming, replacement, sizeof(replacement) - 1U))
        goto done;
    times[0] = before.mtime;
    times[1] = before.mtime;
    if (utimensat(AT_FDCWD, incoming, times, 0) != 0 ||
        rename(incoming, target) != 0 || chmod(work, 0500) != 0 ||
        stat(target, &new_st) != 0 || stat(twin_a, &old_st) != 0)
        goto done;
    topology = new_st.st_ino != old_st.st_ino && old_st.st_nlink == 2U &&
               new_st.st_size == (off_t)(sizeof(replacement) - 1U) &&
               new_st.st_mtime == before.mtime.tv_sec;
    if (!topology ||
        yew_file_load(link_path, &loaded, &after) != YEW_LOAD_OK)
        goto done;
    initialized_after = true;
    topology = after.ino != before.ino &&
               after.size_on_disk == before.size_on_disk &&
               after.mtime.tv_sec == before.mtime.tv_sec &&
               after.mtime.tv_nsec == before.mtime.tv_nsec;
    if (!topology)
        goto done;
    probe = yew_journal_probe(link_path, &after);
    replay = yew_journal_replay(link_path, loaded, &after);
    correct = probe && replay &&
              f078_text_is(loaded, recovered, sizeof(recovered) - 1U);

done:
    if (!correct)
        (void)snprintf(why, why_cap,
                       "topology=%u probe=%u replay=%u recovered=%s; "
                       "same-metadata replacement accepted as journal base",
                       topology ? 1U : 0U, probe ? 1U : 0U,
                       replay ? 1U : 0U,
                       loaded == NULL ? "<none>" :
                       f078_text_is(loaded, (const u8 *)"Xomega\n", 7U)
                           ? "Xomega"
                           : "other");
    yew_journal_close(journal);
    yew_textbuf_free(loaded);
    yew_textbuf_free(edited);
    if (initialized_after)
        yew_filemeta_dispose(&after);
    if (initialized_before)
        yew_filemeta_dispose(&before);
    (void)chmod(work, 0700);
    f078_remove_tree(root);
    if (old_state != NULL)
        (void)setenv("XDG_STATE_HOME", old_state, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    free(old_state);
    return correct;
}
