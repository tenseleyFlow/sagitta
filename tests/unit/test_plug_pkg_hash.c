#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/diag.h"
#include "mod/plug/pkg.h"
#include "unit/stat_time.h"
#include "util/arena.h"

static void hash_write(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    size_t n = strlen(text);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, n), (i64)n);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

void test_plug_pkg_hash_tracks_git_content_model(void)
{
    char root[] = "/tmp/yew-pkg-hash-XXXXXX";
    char a[320];
    char b[320];
    char git[320];
    char gitfile[384];
    char link[320];
    char h0[17];
    char h1[17];
    Arena arena;
    DiagCtx dc;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(a, sizeof(a), "%s/a", root);
    (void)snprintf(b, sizeof(b), "%s/b", root);
    (void)snprintf(git, sizeof(git), "%s/.git", root);
    (void)snprintf(gitfile, sizeof(gitfile), "%s/HEAD", git);
    (void)snprintf(link, sizeof(link), "%s/link", root);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    hash_write(a, "one", 0600);
    YEW_ASSERT(yew_pkg_tree_hash(root, h0, &dc));
    YEW_ASSERT_EQ_U64(strlen(h0), 16U);
    YEW_ASSERT(yew_pkg_tree_hash(root, h1, &dc));
    YEW_ASSERT_EQ_STR(h0, h1);
    hash_write(a, "two", 0600);
    YEW_ASSERT(yew_pkg_tree_hash(root, h1, &dc));
    YEW_ASSERT(strcmp(h0, h1) != 0);
    (void)memcpy(h0, h1, sizeof(h0));
    YEW_ASSERT_EQ_I64(rename(a, b), 0);
    YEW_ASSERT(yew_pkg_tree_hash(root, h1, &dc));
    YEW_ASSERT(strcmp(h0, h1) != 0);
    (void)memcpy(h0, h1, sizeof(h0));
    YEW_ASSERT_EQ_I64(chmod(b, 0700), 0);
    YEW_ASSERT(yew_pkg_tree_hash(root, h1, &dc));
    YEW_ASSERT(strcmp(h0, h1) != 0);
    (void)memcpy(h0, h1, sizeof(h0));
    YEW_ASSERT_EQ_I64(mkdir(git, 0700), 0);
    hash_write(gitfile, "churn", 0600);
    YEW_ASSERT(yew_pkg_tree_hash(root, h1, &dc));
    YEW_ASSERT_EQ_STR(h0, h1);
    YEW_ASSERT_EQ_I64(symlink("/", link), 0);
    YEW_ASSERT(yew_pkg_tree_hash(root, h1, &dc));
    YEW_ASSERT(strcmp(h0, h1) != 0);
    YEW_ASSERT_EQ_I64(unlink(link), 0);
    YEW_ASSERT_EQ_I64(unlink(gitfile), 0);
    YEW_ASSERT_EQ_I64(rmdir(git), 0);
    YEW_ASSERT_EQ_I64(unlink(b), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&dc), 0U);
    arena_free_all(&arena);
}

void test_plug_pkg_hash_directory_independence_and_scale(void)
{
    char left[] = "/tmp/yew-pkg-hash-left-XXXXXX";
    char right[] = "/tmp/yew-pkg-hash-right-XXXXXX";
    char path[384];
    char link[384];
    char h0[17];
    char h1[17];
    struct stat st;
    struct timespec times[2];
    Arena arena;
    DiagCtx dc;
    size_t i;

    YEW_ASSERT_NOT_NULL(mkdtemp(left));
    YEW_ASSERT_NOT_NULL(mkdtemp(right));
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    (void)snprintf(path, sizeof(path), "%s/same", left);
    hash_write(path, "content", 0600);
    (void)snprintf(path, sizeof(path), "%s/same", right);
    hash_write(path, "content", 0600);
    YEW_ASSERT(yew_pkg_tree_hash(left, h0, &dc));
    YEW_ASSERT(yew_pkg_tree_hash(right, h1, &dc));
    YEW_ASSERT_EQ_STR(h0, h1);

    (void)snprintf(path, sizeof(path), "%s/added", left);
    hash_write(path, "new", 0600);
    YEW_ASSERT(yew_pkg_tree_hash(left, h1, &dc));
    YEW_ASSERT(strcmp(h0, h1) != 0);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT(yew_pkg_tree_hash(left, h1, &dc));
    YEW_ASSERT_EQ_STR(h0, h1);

    (void)snprintf(link, sizeof(link), "%s/link", left);
    YEW_ASSERT_EQ_I64(symlink("one", link), 0);
    YEW_ASSERT(yew_pkg_tree_hash(left, h0, &dc));
    YEW_ASSERT_EQ_I64(unlink(link), 0);
    YEW_ASSERT_EQ_I64(symlink("two", link), 0);
    YEW_ASSERT(yew_pkg_tree_hash(left, h1, &dc));
    YEW_ASSERT(strcmp(h0, h1) != 0);
    YEW_ASSERT_EQ_I64(unlink(link), 0);

    YEW_ASSERT(yew_pkg_tree_hash(left, h0, &dc));

    (void)snprintf(path, sizeof(path), "%s/same", left);
    YEW_ASSERT_EQ_I64(stat(path, &st), 0);
    times[0] = yew_test_stat_atime(&st);
    times[0].tv_sec += 17;
    times[1] = yew_test_stat_mtime(&st);
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, path, times, 0), 0);
    YEW_ASSERT(yew_pkg_tree_hash(left, h1, &dc));
    YEW_ASSERT_EQ_STR(h0, h1);

    for (i = 0U; i < 5000U; i++) {
        (void)snprintf(path, sizeof(path), "%s/f%04zu", right, i);
        hash_write(path, "x", 0600);
    }
    YEW_ASSERT(yew_pkg_tree_hash(right, h1, &dc));
    for (i = 0U; i < 5000U; i++) {
        (void)snprintf(path, sizeof(path), "%s/f%04zu", right, i);
        YEW_ASSERT_EQ_I64(unlink(path), 0);
    }

    (void)snprintf(path, sizeof(path), "%s/same", left);
    YEW_ASSERT_EQ_I64(chmod(path, 0000), 0);
    YEW_ASSERT(!yew_pkg_tree_hash(left, h1, &dc));
    YEW_ASSERT_EQ_I64(chmod(path, 0600), 0);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    (void)snprintf(path, sizeof(path), "%s/same", right);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(left), 0);
    YEW_ASSERT_EQ_I64(rmdir(right), 0);
    YEW_ASSERT(fl_diag_errors(&dc) >= 1U);
    arena_free_all(&arena);
}
