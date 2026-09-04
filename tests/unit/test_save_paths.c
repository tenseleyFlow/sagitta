#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/file.h"
#include "util/buf.h"

typedef struct {
    char root[64];
    char state[96];
} SaveFixture;

static void save_fixture_make(SaveFixture *fixture)
{
    int count;

    (void)snprintf(fixture->root, sizeof(fixture->root),
                   "/tmp/yew-save-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fixture->root));
    count = snprintf(fixture->state, sizeof(fixture->state), "%s/state",
                     fixture->root);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->state));
    YEW_ASSERT_EQ_I64(mkdir(fixture->state, 0700), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->state, 1), 0);
}

static void remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir;
        struct dirent *entry;

        (void)chmod(path, 0700);
        dir = opendir(path);
        YEW_ASSERT_NOT_NULL(dir);
        while ((entry = readdir(dir)) != NULL) {
            char child[PATH_MAX];
            int count;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            count = snprintf(child, sizeof(child), "%s/%s", path,
                             entry->d_name);
            YEW_ASSERT(count > 0 && (size_t)count < sizeof(child));
            remove_tree(child);
        }
        YEW_ASSERT_EQ_I64(closedir(dir), 0);
        YEW_ASSERT_EQ_I64(rmdir(path), 0);
    } else {
        YEW_ASSERT_EQ_I64(unlink(path), 0);
    }
}

static void path_in(char *out, size_t cap, const char *dir, const char *name)
{
    int count = snprintf(out, cap, "%s/%s", dir, name);

    YEW_ASSERT(count > 0 && (size_t)count < cap);
}

static void save_write(const char *path, const u8 *bytes, size_t len,
                       mode_t mode)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);

    YEW_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        YEW_ASSERT(n > 0);
        at += (size_t)n;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static Bytebuf save_read(const char *path)
{
    Bytebuf out;
    u8 block[256];
    int fd = open(path, O_RDONLY);

    bytebuf_init(&out);
    YEW_ASSERT(fd >= 0);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        YEW_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(&out, block, (size_t)n);
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
    return out;
}

static void assert_saved_bytes(const char *path, const u8 *expected,
                               size_t len)
{
    Bytebuf actual = save_read(path);

    YEW_ASSERT_EQ_U64(actual.len, len);
    YEW_ASSERT_EQ_MEM(actual.data, expected, len);
    bytebuf_free(&actual);
}

static u64 save_fnv64(const char *text)
{
    u64 hash = UINT64_C(14695981039346656037);

    while (*text != '\0') {
        hash ^= (u8)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void save_sibling_path(char *out, size_t cap, const char *name)
{
    const char *program = yew_test_program_path();
    const char *slash = strrchr(program, '/');
    int count;

    if (slash == NULL)
        count = snprintf(out, cap, "./%s", name);
    else if (slash == program)
        count = snprintf(out, cap, "/%s", name);
    else
        count = snprintf(out, cap, "%.*s/%s", (int)(slash - program),
                         program, name);
    YEW_ASSERT(count > 0 && (size_t)count < cap);
}

void test_save_fault_shim_contract(void)
{
    char driver[PATH_MAX];
    char child[PATH_MAX];
    char shim[PATH_MAX];
    pid_t pid;
    pid_t waited;
    int status;

    save_sibling_path(driver, sizeof(driver), "kill9");
    save_sibling_path(child, sizeof(child), "yew-torture");
    save_sibling_path(shim, sizeof(shim), "tests/torture/faultshim.so");
    pid = fork();
    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        if (setenv("YEW_TORTURE_SIGKILL_ITERS", "0", 1) != 0)
            _exit(126);
        execl(driver, driver, child, shim, (char *)NULL);
        _exit(126);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, pid);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
}

void test_save_symlink_preserves_link_and_updates_target(void)
{
    static const u8 original[] = "old";
    static const u8 expected[] = "old-new!";
    SaveFixture fixture;
    char target[128];
    char link_path[128];
    struct stat st;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(target, sizeof(target), fixture.root, "target.txt");
    path_in(link_path, sizeof(link_path), fixture.root, "link.txt");
    save_write(target, original, sizeof(original) - 1U, 0600);
    YEW_ASSERT_EQ_I64(symlink("target.txt", link_path), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(link_path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT(meta.via_symlink);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)),
                       (const u8 *)"-new", 4U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, link_path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(lstat(link_path, &st), 0);
    YEW_ASSERT(S_ISLNK(st.st_mode));
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)), (const u8 *)"!", 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, link_path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(lstat(link_path, &st), 0);
    YEW_ASSERT(S_ISLNK(st.st_mode));
    assert_saved_bytes(target, expected, sizeof(expected) - 1U);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_symlink_into_read_only_directory_stays_in_place(void)
{
    static const u8 original[] = "old";
    static const u8 expected[] = "old-new";
    SaveFixture fixture;
    char work[128];
    char target[160];
    char link_path[128];
    struct stat before;
    struct stat after;
    struct stat link_st;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(work, sizeof(work), fixture.root, "readonly");
    YEW_ASSERT_EQ_I64(mkdir(work, 0700), 0);
    path_in(target, sizeof(target), work, "target.txt");
    path_in(link_path, sizeof(link_path), fixture.root, "link.txt");
    save_write(target, original, sizeof(original) - 1U, 0600);
    YEW_ASSERT_EQ_I64(symlink("readonly/target.txt", link_path), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(link_path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_EQ_I64(stat(target, &before), 0);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)),
                       (const u8 *)"-new", 4U);
    YEW_ASSERT_EQ_I64(chmod(work, 0500), 0);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, link_path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(target, &after), 0);
    YEW_ASSERT_EQ_I64(lstat(link_path, &link_st), 0);
    YEW_ASSERT_EQ_U64(before.st_ino, after.st_ino);
    YEW_ASSERT(S_ISLNK(link_st.st_mode));
    assert_saved_bytes(target, expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_I64(chmod(work, 0700), 0);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_dangling_symlink_preserves_link_and_creates_target(void)
{
    static const u8 expected[] = "created through link\n";
    SaveFixture fixture;
    char target[128];
    char link_path[128];
    struct stat st;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(target, sizeof(target), fixture.root, "future.txt");
    path_in(link_path, sizeof(link_path), fixture.root, "link.txt");
    YEW_ASSERT_EQ_I64(symlink("future.txt", link_path), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(link_path, &tb, &meta), YEW_LOAD_ENOENT);
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT(meta.via_symlink);
    yew_textbuf_insert(tb, BYTEOFF(0U), expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, link_path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(lstat(link_path, &st), 0);
    YEW_ASSERT(S_ISLNK(st.st_mode));
    assert_saved_bytes(target, expected, sizeof(expected) - 1U);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_hardlink_preserves_shared_inode(void)
{
    static const u8 original[] = "old";
    static const u8 expected[] = "old-new!";
    SaveFixture fixture;
    char first[128];
    char second[128];
    char third[128];
    struct stat first_st;
    struct stat second_st;
    struct stat third_st;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(first, sizeof(first), fixture.root, "first.txt");
    path_in(second, sizeof(second), fixture.root, "second.txt");
    path_in(third, sizeof(third), fixture.root, "third.txt");
    save_write(first, original, sizeof(original) - 1U, 0600);
    YEW_ASSERT_EQ_I64(link(first, second), 0);
    YEW_ASSERT_EQ_I64(link(first, third), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(first, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_EQ_U64(meta.nlink, 3U);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)),
                       (const u8 *)"-new", 4U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, first), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(first, &first_st), 0);
    YEW_ASSERT_EQ_I64(stat(second, &second_st), 0);
    YEW_ASSERT_EQ_I64(stat(third, &third_st), 0);
    YEW_ASSERT_EQ_U64(first_st.st_ino, second_st.st_ino);
    YEW_ASSERT_EQ_U64(first_st.st_ino, third_st.st_ino);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)), (const u8 *)"!", 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, first), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(first, &first_st), 0);
    YEW_ASSERT_EQ_I64(stat(second, &second_st), 0);
    YEW_ASSERT_EQ_I64(stat(third, &third_st), 0);
    YEW_ASSERT_EQ_U64(first_st.st_ino, second_st.st_ino);
    YEW_ASSERT_EQ_U64(first_st.st_ino, third_st.st_ino);
    assert_saved_bytes(first, expected, sizeof(expected) - 1U);
    assert_saved_bytes(second, expected, sizeof(expected) - 1U);
    assert_saved_bytes(third, expected, sizeof(expected) - 1U);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_backup_path_symlink_cannot_overwrite_victim(void)
{
    static const u8 original[] = "original";
    static const u8 expected[] = "replacement";
    static const u8 victim_bytes[] = "do not overwrite";
    SaveFixture fixture;
    char first[128];
    char second[128];
    char victim[128];
    char yew_dir[128];
    char backup_dir[160];
    char backup[224];
    char resolved[PATH_MAX];
    struct stat backup_st;
    FileMeta meta;
    TextBuf *tb = NULL;
    int count;

    save_fixture_make(&fixture);
    path_in(first, sizeof(first), fixture.root, "first.txt");
    path_in(second, sizeof(second), fixture.root, "second.txt");
    path_in(victim, sizeof(victim), fixture.root, "victim.txt");
    path_in(yew_dir, sizeof(yew_dir), fixture.state, "yew");
    path_in(backup_dir, sizeof(backup_dir), yew_dir, "backup");
    save_write(first, original, sizeof(original) - 1U, 0600);
    YEW_ASSERT_EQ_I64(link(first, second), 0);
    save_write(victim, victim_bytes, sizeof(victim_bytes) - 1U, 0600);
    YEW_ASSERT_NOT_NULL(realpath(first, resolved));
    YEW_ASSERT_EQ_I64(mkdir(yew_dir, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(backup_dir, 0700), 0);
    count = snprintf(backup, sizeof(backup), "%s/%016" PRIx64 ".bak",
                     backup_dir, save_fnv64(resolved));
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(backup));
    YEW_ASSERT_EQ_I64(symlink(victim, backup), 0);

    YEW_ASSERT_EQ_U64(yew_file_load(first, &tb, &meta), YEW_LOAD_OK);
    yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
    yew_textbuf_insert(tb, BYTEOFF(0U), expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, first), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(lstat(backup, &backup_st), 0);
    YEW_ASSERT(S_ISREG(backup_st.st_mode));
    assert_saved_bytes(backup, original, sizeof(original) - 1U);
    assert_saved_bytes(victim, victim_bytes, sizeof(victim_bytes) - 1U);
    assert_saved_bytes(first, expected, sizeof(expected) - 1U);
    assert_saved_bytes(second, expected, sizeof(expected) - 1U);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_read_only_directory_uses_in_place_path(void)
{
    static const u8 original[] = "old";
    static const u8 expected[] = "new";
    SaveFixture fixture;
    char work[128];
    char path[160];
    struct stat before;
    struct stat after;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(work, sizeof(work), fixture.root, "readonly");
    YEW_ASSERT_EQ_I64(mkdir(work, 0700), 0);
    path_in(path, sizeof(path), work, "file.txt");
    save_write(path, original, sizeof(original) - 1U, 0600);
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_EQ_I64(stat(path, &before), 0);
    yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
    yew_textbuf_insert(tb, BYTEOFF(0U), expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_I64(chmod(work, 0500), 0);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(path, &after), 0);
    YEW_ASSERT_EQ_U64(before.st_ino, after.st_ino);
    assert_saved_bytes(path, expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_I64(chmod(work, 0700), 0);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_existing_file_preserves_mode_owner_and_group(void)
{
    static const u8 original[] = "old";
    static const u8 expected[] = "old!?";
    SaveFixture fixture;
    char path[128];
    struct stat before;
    struct stat after;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(path, sizeof(path), fixture.root, "mode.txt");
    save_write(path, original, sizeof(original) - 1U, 0640);
    YEW_ASSERT_EQ_I64(stat(path, &before), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)), (const u8 *)"!", 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(path, &after), 0);
    YEW_ASSERT_EQ_U64(after.st_mode & 07777U, before.st_mode & 07777U);
    YEW_ASSERT_EQ_U64(after.st_uid, before.st_uid);
    YEW_ASSERT_EQ_U64(after.st_gid, before.st_gid);
    YEW_ASSERT(meta.exists);
    YEW_ASSERT_EQ_U64(meta.dev, after.st_dev);
    YEW_ASSERT_EQ_U64(meta.ino, after.st_ino);
    YEW_ASSERT_EQ_U64(meta.nlink, after.st_nlink);
    YEW_ASSERT_EQ_U64(meta.size_on_disk, (u64)after.st_size);
    YEW_ASSERT_NOT_NULL(meta.realpath);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)), (const u8 *)"?", 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(path, &after), 0);
    YEW_ASSERT_EQ_U64(meta.dev, after.st_dev);
    YEW_ASSERT_EQ_U64(meta.ino, after.st_ino);
    YEW_ASSERT_EQ_U64(meta.size_on_disk, (u64)after.st_size);
    assert_saved_bytes(path, expected, sizeof(expected) - 1U);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_new_file_creates_requested_content(void)
{
    static const u8 expected[] = "brand new\n";
    SaveFixture fixture;
    char path[128];
    struct stat st;
    FileMeta meta;
    TextBuf *tb = NULL;

    save_fixture_make(&fixture);
    path_in(path, sizeof(path), fixture.root, "new.txt");
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_ENOENT);
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT(!meta.exists);
    yew_textbuf_insert(tb, BYTEOFF(0U), expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, path), YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(path, &st), 0);
    YEW_ASSERT(S_ISREG(st.st_mode));
    YEW_ASSERT(meta.exists);
    YEW_ASSERT_EQ_U64(meta.dev, st.st_dev);
    YEW_ASSERT_EQ_U64(meta.ino, st.st_ino);
    YEW_ASSERT_EQ_U64(meta.size_on_disk, (u64)st.st_size);
    assert_saved_bytes(path, expected, sizeof(expected) - 1U);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    remove_tree(fixture.root);
}

void test_save_atomic_bytes_replaces_file_and_cleans_temp(void)
{
    static const u8 original[] = "old";
    static const u8 replacement[] = {0U, 1U, 2U, 0xffU, '\n'};
    SaveFixture fixture;
    char path[128];
    DIR *dir;
    struct dirent *entry;
    struct stat before;
    struct stat after;

    save_fixture_make(&fixture);
    path_in(path, sizeof(path), fixture.root, "tree.yewu");
    save_write(path, original, sizeof(original) - 1U, 0600);
    YEW_ASSERT_EQ_I64(stat(path, &before), 0);
    YEW_ASSERT_EQ_U64(yew_file_write_atomic(path, replacement,
                                            sizeof(replacement), 0600),
                      YEW_SAVE_OK);
    YEW_ASSERT_EQ_I64(stat(path, &after), 0);
    YEW_ASSERT(before.st_ino != after.st_ino);
    YEW_ASSERT_EQ_U64(after.st_mode & 07777U, 0600U);
    assert_saved_bytes(path, replacement, sizeof(replacement));

    dir = opendir(fixture.root);
    YEW_ASSERT_NOT_NULL(dir);
    while ((entry = readdir(dir)) != NULL)
        YEW_ASSERT(strncmp(entry->d_name, ".yew-tree.yewu-", 15U) != 0);
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    remove_tree(fixture.root);
}
