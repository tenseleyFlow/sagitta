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
#include "util/arena.h"

static void rmtree_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size_t len = strlen(text);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, len), (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

void test_plug_pkg_rmtree_never_follows_symlinks(void)
{
    char root[] = "/tmp/yew-pkg-rmtree-XXXXXX";
    char outside[] = "/tmp/yew-pkg-outside-XXXXXX";
    char tree[320];
    char nested[384];
    char file[448];
    char link[448];
    char loop[448];
    char sentinel[320];
    Arena a;
    DiagCtx dc;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    YEW_ASSERT_NOT_NULL(mkdtemp(outside));
    (void)snprintf(tree, sizeof(tree), "%s/plugin", root);
    (void)snprintf(nested, sizeof(nested), "%s/nested", tree);
    (void)snprintf(file, sizeof(file), "%s/file", nested);
    (void)snprintf(link, sizeof(link), "%s/outside", nested);
    (void)snprintf(loop, sizeof(loop), "%s/loop", nested);
    (void)snprintf(sentinel, sizeof(sentinel), "%s/keep", outside);
    YEW_ASSERT_EQ_I64(mkdir(tree, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(nested, 0700), 0);
    rmtree_write(file, "x");
    rmtree_write(sentinel, "keep");
    YEW_ASSERT_EQ_I64(symlink(outside, link), 0);
    YEW_ASSERT_EQ_I64(symlink("..", loop), 0);
    YEW_ASSERT_EQ_I64(chmod(nested, 0500), 0);
    arena_init(&a);
    fl_diag_init(&dc, &a);
    YEW_ASSERT(yew_rmtree(tree, root, &dc));
    YEW_ASSERT(access(tree, F_OK) != 0);
    YEW_ASSERT_EQ_I64(access(sentinel, F_OK), 0);
    YEW_ASSERT(!yew_rmtree(outside, root, &dc));
    YEW_ASSERT_EQ_I64(access(sentinel, F_OK), 0);
    YEW_ASSERT_EQ_I64(unlink(sentinel), 0);
    YEW_ASSERT_EQ_I64(rmdir(outside), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    arena_free_all(&a);
}

void test_plug_pkg_rmtree_refuses_root_and_missing_paths(void)
{
    char root[] = "/tmp/yew-pkg-rmtree-guard-XXXXXX";
    char missing[320];
    Arena a;
    DiagCtx dc;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(missing, sizeof(missing), "%s/missing", root);
    arena_init(&a);
    fl_diag_init(&dc, &a);
    YEW_ASSERT(!yew_rmtree(root, root, &dc));
    YEW_ASSERT_EQ_I64(access(root, F_OK), 0);
    YEW_ASSERT(!yew_rmtree(missing, root, &dc));
    YEW_ASSERT_EQ_I64(access(root, F_OK), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    YEW_ASSERT(fl_diag_errors(&dc) >= 2U);
    arena_free_all(&a);
}
