/*
 * Sprint 26 §3: the workspace walk.
 *
 * DETERMINISM IS THE PROPERTY, and it is the one that fails silently.
 * readdir hands back filesystem order, which differs between ext4 and
 * xfs and between two checkouts of one repository made in different
 * orders.  A walk that inherited it would produce a different list on
 * every machine, and the symptom would not be an error — it would be a
 * ranking tie breaking the other way, and a pty golden that passes for
 * whoever recorded it.  So the fixtures here create their entries in a
 * DELIBERATELY SCRAMBLED order and demand the same bytes out.
 *
 * The loop guard is the other one worth stating: `a/b -> ../a` is not a
 * hypothetical, and a walk that follows it does not fail, it runs until
 * something else stops it.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/buf.h"
#include "ws/walk.h"

/* ---------------------------------------------------------------- */
/* Fixtures                                                         */
/* ---------------------------------------------------------------- */

typedef struct WkFix {
    char root[128];
} WkFix;

static void wk_rm_rf(const char *path)
{
    char cmd[512];

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void wk_make(WkFix *f)
{
    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-walk-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->root));
}

static void wk_remove(WkFix *f)
{
    wk_rm_rf(f->root);
}

static void wk_join(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", dir, leaf);

    SAG_ASSERT(n > 0 && (size_t)n < cap);
}

static void wk_file(const WkFix *f, const char *rel)
{
    char path[512];
    FILE *fp;

    wk_join(path, sizeof(path), f->root, rel);
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("x\n", fp);
    (void)fclose(fp);
}

static void wk_dir(const WkFix *f, const char *rel)
{
    char path[512];

    wk_join(path, sizeof(path), f->root, rel);
    SAG_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

/* Runs a walk to completion and returns the list. */
static void wk_walk(const WkFix *f, const WalkOpts *o, FileList *out)
{
    WalkState *w;

    sag_filelist_init(out);
    w = sag_walk_begin(f->root, o, out);
    SAG_ASSERT_NOT_NULL(w);
    while (sag_walk_step(w, 0))
        ;
    sag_walk_end(w);
}

/* The whole list as one newline-joined string, for byte comparison. */
static void wk_join_paths(const FileList *fl, Bytebuf *out)
{
    size_t i;

    for (i = 0U; i < fl->paths.len; i++) {
        bytebuf_append(out, (const u8 *)fl->paths.data[i],
                       strlen(fl->paths.data[i]));
        bytebuf_push_u8(out, (u8)'\n');
    }
    bytebuf_push_u8(out, 0U);
    out->len--;
}

static bool wk_has(const FileList *fl, const char *rel)
{
    size_t i;

    for (i = 0U; i < fl->paths.len; i++) {
        if (strcmp(fl->paths.data[i], rel) == 0)
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* Basics                                                           */
/* ---------------------------------------------------------------- */

/* Paths are workspace-RELATIVE: that is what the user reads, what
 * scoring sees, and what keeps 100 000 strings small. */
void test_walk_lists_files_relative_to_the_root(void)
{
    WkFix f;
    FileList fl;

    wk_make(&f);
    wk_file(&f, "a.txt");
    wk_dir(&f, "src");
    wk_file(&f, "src/main.c");
    wk_dir(&f, "src/ui");
    wk_file(&f, "src/ui/draw.c");

    wk_walk(&f, NULL, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 3U);
    SAG_ASSERT(wk_has(&fl, "a.txt"));
    SAG_ASSERT(wk_has(&fl, "src/main.c"));
    SAG_ASSERT(wk_has(&fl, "src/ui/draw.c"));
    /* No leading slash, and no absolute prefix. */
    SAG_ASSERT(fl.paths.data[0][0] != '/');
    SAG_ASSERT_NULL(strstr(fl.paths.data[0], "/tmp/"));
    SAG_ASSERT_EQ_U64(fl.n_files, 3U);
    SAG_ASSERT_EQ_U64(fl.n_dirs, 2U);
    SAG_ASSERT(!fl.truncated);
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* Directories are not files: only regular files are listed. */
void test_walk_lists_only_regular_files(void)
{
    WkFix f;
    FileList fl;
    char path[512];

    wk_make(&f);
    wk_file(&f, "real.txt");
    wk_dir(&f, "adir");
    /* A fifo is neither, and must not appear. */
    wk_join(path, sizeof(path), f.root, "afifo");
    if (mkfifo(path, 0600) != 0) {
        sag_filelist_init(&fl);
        sag_filelist_free(&fl);
        wk_remove(&f);
        return;
    }
    wk_walk(&f, NULL, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 1U);
    SAG_ASSERT_EQ_STR(fl.paths.data[0], "real.txt");
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Determinism (DoD 4)                                              */
/* ---------------------------------------------------------------- */

/*
 * Two walks of one tree produce byte-identical lists.
 *
 * The fixture creates entries in DELIBERATELY SCRAMBLED order — `m`
 * before `a`, `z` before `b` — because on most filesystems readdir
 * returns creation order for a small directory.  A walk that skipped
 * the sort would pass a test whose fixture happened to be created
 * alphabetically and fail on a real checkout.
 */
void test_walk_is_deterministic_regardless_of_creation_order(void)
{
    WkFix f;
    FileList first;
    FileList second;
    Bytebuf a;
    Bytebuf b;

    wk_make(&f);
    wk_file(&f, "mmm.txt");
    wk_file(&f, "aaa.txt");
    wk_file(&f, "zzz.txt");
    wk_file(&f, "bbb.txt");
    wk_dir(&f, "zdir");
    wk_file(&f, "zdir/two.c");
    wk_file(&f, "zdir/one.c");
    wk_dir(&f, "adir");
    wk_file(&f, "adir/nine.c");
    wk_file(&f, "adir/eight.c");

    wk_walk(&f, NULL, &first);
    wk_walk(&f, NULL, &second);
    bytebuf_init(&a);
    bytebuf_init(&b);
    wk_join_paths(&first, &a);
    wk_join_paths(&second, &b);
    SAG_ASSERT_EQ_U64(a.len, b.len);
    SAG_ASSERT_EQ_MEM(a.data, b.data, a.len);

    /* And the order is SORTED, not creation order — which is the claim
     * the byte comparison alone cannot make, since two identical wrong
     * answers are still identical. */
    SAG_ASSERT_EQ_STR(first.paths.data[0], "aaa.txt");
    SAG_ASSERT_EQ_STR(first.paths.data[1], "adir/eight.c");
    SAG_ASSERT_EQ_STR(first.paths.data[2], "adir/nine.c");
    SAG_ASSERT_EQ_STR(first.paths.data[3], "bbb.txt");
    SAG_ASSERT_EQ_STR(first.paths.data[4], "mmm.txt");
    SAG_ASSERT_EQ_STR(first.paths.data[5], "zdir/one.c");
    SAG_ASSERT_EQ_STR(first.paths.data[6], "zdir/two.c");
    SAG_ASSERT_EQ_STR(first.paths.data[7], "zzz.txt");

    bytebuf_free(&a);
    bytebuf_free(&b);
    sag_filelist_free(&first);
    sag_filelist_free(&second);
    wk_remove(&f);
}

/*
 * A walk sliced across many sag_walk_step calls equals a single-call
 * walk, byte for byte.
 *
 * This is what makes the budget safe to use: if slicing could change
 * the result, a walk that happened to finish in one frame would list
 * different files than the same walk under load.
 */
void test_walk_sliced_equals_unsliced(void)
{
    WkFix f;
    FileList whole;
    FileList sliced;
    WalkState *w;
    Bytebuf a;
    Bytebuf b;
    u32 steps = 0U;
    u32 i;

    wk_make(&f);
    for (i = 0U; i < 12U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "d%02u", (unsigned)i);
        wk_dir(&f, name);
        {
            char rel[64];
            u32 k;

            for (k = 0U; k < 6U; k++) {
                (void)snprintf(rel, sizeof(rel), "d%02u/f%02u.c",
                               (unsigned)i, (unsigned)k);
                wk_file(&f, rel);
            }
        }
    }
    wk_walk(&f, NULL, &whole);

    /* A 1 us budget forces a return after essentially every
     * directory. */
    sag_filelist_init(&sliced);
    w = sag_walk_begin(f.root, NULL, &sliced);
    SAG_ASSERT_NOT_NULL(w);
    while (sag_walk_step(w, 1)) {
        steps++;
        SAG_ASSERT(steps < 10000U); /* never loops */
    }
    sag_walk_end(w);
    /* It really was sliced, or this proves nothing. */
    SAG_ASSERT(steps > 1U);

    bytebuf_init(&a);
    bytebuf_init(&b);
    wk_join_paths(&whole, &a);
    wk_join_paths(&sliced, &b);
    SAG_ASSERT_EQ_U64(whole.paths.len, 72U);
    SAG_ASSERT_EQ_U64(a.len, b.len);
    SAG_ASSERT_EQ_MEM(a.data, b.data, a.len);

    bytebuf_free(&a);
    bytebuf_free(&b);
    sag_filelist_free(&whole);
    sag_filelist_free(&sliced);
    wk_remove(&f);
}

/* ---------------------------------------------------------------- */
/* The symlink loop guard (DoD 4)                                   */
/* ---------------------------------------------------------------- */

/*
 * `a/b -> ../a` terminates, and says it did.
 *
 * With follow_symlinks the descent would revisit `a` forever; the
 * device+inode set stops it at the first repeat and counts one loop.
 */
void test_walk_symlink_loop_terminates_and_is_counted(void)
{
    WkFix f;
    FileList fl;
    WalkOpts o;
    char target[512];
    char link[512];

    wk_make(&f);
    wk_dir(&f, "a");
    wk_file(&f, "a/inside.txt");
    wk_join(target, sizeof(target), f.root, "a");
    wk_join(link, sizeof(link), f.root, "a/b");
    if (symlink(target, link) != 0) {
        wk_remove(&f);
        return;
    }
    (void)memset(&o, 0, sizeof(o));
    o.follow_symlinks = true;

    wk_walk(&f, &o, &fl);
    SAG_ASSERT_EQ_U64(fl.n_loops, 1U);
    SAG_ASSERT(wk_has(&fl, "a/inside.txt"));
    /* The file is listed ONCE, not once per trip around the loop. */
    SAG_ASSERT_EQ_U64(fl.paths.len, 1U);
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/*
 * The guard is maintained even WITHOUT follow_symlinks.
 *
 * A guard that only exists under an option is switched off in exactly
 * the cases that need it: bind mounts and several network filesystems
 * produce cycles with no symlink in sight.  Here the symlink is not
 * followed at all, so no loop is possible — and the walk still
 * terminates with the file listed once, which is the observable half of
 * the claim.
 */
void test_walk_does_not_follow_symlinks_by_default(void)
{
    WkFix f;
    FileList fl;
    char target[512];
    char link[512];

    wk_make(&f);
    wk_dir(&f, "a");
    wk_file(&f, "a/inside.txt");
    wk_join(target, sizeof(target), f.root, "a");
    wk_join(link, sizeof(link), f.root, "a/b");
    if (symlink(target, link) != 0) {
        wk_remove(&f);
        return;
    }
    wk_walk(&f, NULL, &fl);
    SAG_ASSERT_EQ_U64(fl.n_loops, 0U);
    SAG_ASSERT_EQ_U64(fl.paths.len, 1U);
    SAG_ASSERT(wk_has(&fl, "a/inside.txt"));
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Options and caps                                                 */
/* ---------------------------------------------------------------- */

/* Dotfiles are out by default and in on request. */
void test_walk_hidden_files_are_opt_in(void)
{
    WkFix f;
    FileList fl;
    WalkOpts o;

    wk_make(&f);
    wk_file(&f, "visible.txt");
    wk_file(&f, ".hidden");
    wk_dir(&f, ".hiddendir");
    wk_file(&f, ".hiddendir/inside.txt");

    wk_walk(&f, NULL, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 1U);
    SAG_ASSERT(wk_has(&fl, "visible.txt"));
    sag_filelist_free(&fl);

    (void)memset(&o, 0, sizeof(o));
    o.hidden = true;
    wk_walk(&f, &o, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 3U);
    SAG_ASSERT(wk_has(&fl, ".hidden"));
    SAG_ASSERT(wk_has(&fl, ".hiddendir/inside.txt"));
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/*
 * `.git` is skipped unconditionally — even with hidden files on.
 *
 * It is never a source file, it is enormous, and walking it is pure
 * cost in every workspace that has one.
 */
void test_walk_always_skips_dot_git(void)
{
    WkFix f;
    FileList fl;
    WalkOpts o;

    wk_make(&f);
    wk_file(&f, "src.c");
    wk_dir(&f, ".git");
    wk_file(&f, ".git/config");
    wk_dir(&f, ".git/objects");
    wk_file(&f, ".git/objects/abc");

    (void)memset(&o, 0, sizeof(o));
    o.hidden = true;
    wk_walk(&f, &o, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 1U);
    SAG_ASSERT_EQ_STR(fl.paths.data[0], "src.c");
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* max_entries truncation is FLAGGED, never a silently short list. */
void test_walk_truncation_is_flagged(void)
{
    WkFix f;
    FileList fl;
    WalkOpts o;
    u32 i;

    wk_make(&f);
    for (i = 0U; i < 20U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "f%02u.txt", (unsigned)i);
        wk_file(&f, name);
    }
    (void)memset(&o, 0, sizeof(o));
    o.max_entries = 5U;
    wk_walk(&f, &o, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 5U);
    SAG_ASSERT(fl.truncated);
    sag_filelist_free(&fl);

    /* And without the cap, nothing is flagged. */
    wk_walk(&f, NULL, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 20U);
    SAG_ASSERT(!fl.truncated);
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* max_depth stops the descent without stopping the walk. */
void test_walk_max_depth_bounds_the_descent(void)
{
    WkFix f;
    FileList fl;
    WalkOpts o;

    wk_make(&f);
    wk_file(&f, "top.txt");
    wk_dir(&f, "one");
    wk_file(&f, "one/a.txt");
    wk_dir(&f, "one/two");
    wk_file(&f, "one/two/b.txt");
    wk_dir(&f, "one/two/three");
    wk_file(&f, "one/two/three/c.txt");

    (void)memset(&o, 0, sizeof(o));
    o.max_depth = 2U;
    wk_walk(&f, &o, &fl);
    SAG_ASSERT(wk_has(&fl, "top.txt"));
    SAG_ASSERT(wk_has(&fl, "one/a.txt"));
    /* Past the bound, and therefore absent. */
    SAG_ASSERT(!wk_has(&fl, "one/two/b.txt"));
    SAG_ASSERT(!wk_has(&fl, "one/two/three/c.txt"));
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/*
 * A deep tree does not blow the stack.
 *
 * The descent is an explicit stack precisely so that a pathological
 * tree is a slow walk rather than a crash (invariant 1) — and the
 * default depth cap stops it well before either.
 */
void test_walk_deep_tree_does_not_recurse(void)
{
    WkFix f;
    FileList fl;
    WalkOpts o;
    char rel[1024];
    u32 i;
    size_t at = 0U;

    wk_make(&f);
    rel[0] = '\0';
    for (i = 0U; i < 120U; i++) {
        int n;

        n = snprintf(rel + at, sizeof(rel) - at, "%sd", at == 0U ? "" : "/");
        SAG_ASSERT(n > 0);
        at += (size_t)n;
        {
            char path[1200];

            wk_join(path, sizeof(path), f.root, rel);
            if (mkdir(path, 0700) != 0)
                break;
        }
    }
    (void)memset(&o, 0, sizeof(o));
    o.max_depth = 200U;
    wk_walk(&f, &o, &fl);
    /* No crash, and the walk completed. */
    SAG_ASSERT(fl.n_dirs > 100U);
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* An unreadable root is an empty list, not an error the caller has to
 * handle. */
void test_walk_missing_root_is_null(void)
{
    FileList fl;
    WalkState *w;

    sag_filelist_init(&fl);
    w = sag_walk_begin("/tmp/sag-walk-does-not-exist-xyz", NULL, &fl);
    SAG_ASSERT_NULL(w);
    sag_filelist_free(&fl);
    /* And degenerate inputs do not crash. */
    SAG_ASSERT_NULL(sag_walk_begin(NULL, NULL, &fl));
    SAG_ASSERT_NULL(sag_walk_begin("/tmp", NULL, NULL));
    SAG_ASSERT(!sag_walk_step(NULL, 0));
    sag_walk_end(NULL);
}

/* An empty directory is an empty list, not a failure. */
void test_walk_empty_root_is_empty(void)
{
    WkFix f;
    FileList fl;

    wk_make(&f);
    wk_walk(&f, NULL, &fl);
    SAG_ASSERT_EQ_U64(fl.paths.len, 0U);
    SAG_ASSERT_EQ_U64(fl.n_files, 0U);
    SAG_ASSERT(!fl.truncated);
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/*
 * The syscall counters move, which is what DoD 5's pruning assertion
 * will stand on.  A hook that silently counted nothing would make that
 * assertion vacuous.
 */
void test_walk_counts_syscalls(void)
{
    WkFix f;
    FileList fl;
    u64 opendirs;
    u64 statats;

    wk_make(&f);
    wk_file(&f, "a.txt");
    wk_dir(&f, "sub");
    wk_file(&f, "sub/b.txt");

    sag_walk_counts_reset();
    SAG_ASSERT_EQ_U64(sag_walk_opendir_count(), 0U);
    SAG_ASSERT_EQ_U64(sag_walk_statat_count(), 0U);
    wk_walk(&f, NULL, &fl);
    opendirs = sag_walk_opendir_count();
    statats = sag_walk_statat_count();
    /* Root plus one subdirectory. */
    SAG_ASSERT_EQ_U64(opendirs, 2U);
    /* One per entry, plus the root. */
    SAG_ASSERT(statats >= 3U);
    sag_filelist_free(&fl);
    wk_remove(&f);
}

/* A trailing slash on the root does not leak into the paths. */
void test_walk_root_trailing_slash_is_normalized(void)
{
    WkFix f;
    FileList fl;
    WalkState *w;
    char rooted[192];

    wk_make(&f);
    wk_file(&f, "a.txt");
    (void)snprintf(rooted, sizeof(rooted), "%s/", f.root);
    sag_filelist_init(&fl);
    w = sag_walk_begin(rooted, NULL, &fl);
    SAG_ASSERT_NOT_NULL(w);
    while (sag_walk_step(w, 0))
        ;
    sag_walk_end(w);
    SAG_ASSERT_EQ_U64(fl.paths.len, 1U);
    SAG_ASSERT_EQ_STR(fl.paths.data[0], "a.txt");
    sag_filelist_free(&fl);
    wk_remove(&f);
}
