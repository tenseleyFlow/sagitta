/*
 * Sprint 26 §3.  See walk.h for why this is bespoke and why it is not
 * recursive.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ws/walk.h"

#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"
#include "util/sort.h"
#include "ws/gitignore.h"

/* ---------------------------------------------------------------- */
/* Test hooks                                                       */
/* ---------------------------------------------------------------- */

static u64 g_opendirs;
static u64 g_statats;

u64 yew_walk_opendir_count(void)
{
    return g_opendirs;
}

u64 yew_walk_statat_count(void)
{
    return g_statats;
}

void yew_walk_counts_reset(void)
{
    g_opendirs = 0U;
    g_statats = 0U;
}

/* ---------------------------------------------------------------- */
/* The visited set                                                  */
/* ---------------------------------------------------------------- */

/*
 * Every directory ENTERED, by (st_dev, st_ino).
 *
 * Maintained unconditionally rather than only under follow_symlinks.
 * Bind mounts and several network filesystems produce cycles with no
 * symlink anywhere in sight, so a guard that is optional by
 * construction is switched off in exactly the cases that need it.  It
 * costs 16 bytes per directory.
 *
 * Open addressing with linear probing: no per-entry allocation, and the
 * load factor is held under 1/2 so probe chains stay short.
 */
typedef struct SeenSlot {
    u64 dev;
    u64 ino;
    bool used;
} SeenSlot;

typedef struct SeenSet {
    SeenSlot *slot;
    u64 cap;
    u64 len;
} SeenSet;

static u64 seen_hash(u64 dev, u64 ino)
{
    /* Mixed, not concatenated: inode numbers on one device are dense and
     * sequential, so the low bits alone would collide constantly. */
    u64 h = dev * 0x9E3779B97F4A7C15ULL;

    h ^= ino + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

static void seen_init(SeenSet *s)
{
    s->cap = 256U;
    s->len = 0U;
    s->slot = yew_xcalloc((size_t)s->cap, sizeof(*s->slot));
}

static void seen_free(SeenSet *s)
{
    free(s->slot);
    s->slot = NULL;
    s->cap = 0U;
    s->len = 0U;
}

static void seen_grow(SeenSet *s);

/* True when (dev, ino) was already present; inserts it either way. */
static bool seen_add(SeenSet *s, u64 dev, u64 ino)
{
    u64 mask;
    u64 i;

    if ((s->len + 1U) * 2U > s->cap)
        seen_grow(s);
    mask = s->cap - 1U;
    i = seen_hash(dev, ino) & mask;
    while (s->slot[i].used) {
        if (s->slot[i].dev == dev && s->slot[i].ino == ino)
            return true;
        i = (i + 1U) & mask;
    }
    s->slot[i].used = true;
    s->slot[i].dev = dev;
    s->slot[i].ino = ino;
    s->len++;
    return false;
}

static void seen_grow(SeenSet *s)
{
    SeenSlot *old = s->slot;
    u64 old_cap = s->cap;
    u64 i;

    s->cap *= 2U;
    s->len = 0U;
    s->slot = yew_xcalloc((size_t)s->cap, sizeof(*s->slot));
    for (i = 0U; i < old_cap; i++) {
        if (old[i].used)
            (void)seen_add(s, old[i].dev, old[i].ino);
    }
    free(old);
}

/* ---------------------------------------------------------------- */
/* The explicit stack                                               */
/* ---------------------------------------------------------------- */

/*
 * One frame per directory we have descended into.  `rel_len` is the
 * length of the relative prefix for this directory, so pushing and
 * popping a path component is arithmetic on one buffer rather than a
 * string per level.
 */
typedef struct WalkFrame {
    /* Names still to visit in this directory, already sorted. */
    char **names;
    u32 n;
    u32 at;
    u32 rel_len;
    u32 depth;
    /* The ignore rules in scope HERE — this directory's .gitignore
     * chained under everything above it.  Carried per frame because a
     * nested ignore file applies only inside its own subtree. */
    const GiSet *gi;
    /*
     * This directory was itself ignored, and we descended anyway
     * because a negation might re-include something under it.
     *
     * git's rule is that an excluded directory's contents are excluded
     * — it simply never descends.  We do descend, so the exclusion has
     * to be carried down explicitly: without this, `node_modules/`
     * (a DIRECTORY-only rule) never matches the file
     * `node_modules/other.js`, and every file in a supposedly ignored
     * tree came back in the list.
     */
    bool ignored;
} WalkFrame;

struct WalkState {
    char root[PATH_MAX];
    /* The path being built, absolute; `rel_base` is where the
     * workspace-relative part starts. */
    char path[PATH_MAX];
    u32 rel_base;
    u32 path_len;

    WalkFrame *stack;
    u32 depth;
    u32 stack_cap;

    SeenSet seen;
    WalkOpts opts;
    FileList *out;
    /* Compiled ignore rules live here and die with the walk; FileList's
     * arena outlives it and holds only paths. */
    Arena gi_arena;
    bool done;
};

void yew_filelist_init(FileList *fl)
{
    if (fl == NULL)
        return;
    (void)memset(fl, 0, sizeof(*fl));
    arena_init(&fl->a);
}

void yew_filelist_free(FileList *fl)
{
    if (fl == NULL)
        return;
    YewPathVec_free(&fl->paths);
    arena_free_all(&fl->a);
    (void)memset(fl, 0, sizeof(*fl));
}

/* Byte order, through the stable sort — never strcoll, whose answer
 * depends on LC_COLLATE and would make the list locale-dependent. */
static int name_cmp(const void *a, const void *b, void *ctx)
{
    (void)ctx;
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool frame_push(WalkState *w, char **names, u32 n, u32 rel_len,
                       u32 depth, const GiSet *gi, bool ignored)
{
    if (w->depth == w->stack_cap) {
        u32 cap = w->stack_cap == 0U ? 16U : w->stack_cap * 2U;

        w->stack = yew_xreallocarray(w->stack, cap, sizeof(*w->stack));
        w->stack_cap = cap;
    }
    w->stack[w->depth].names = names;
    w->stack[w->depth].n = n;
    w->stack[w->depth].at = 0U;
    w->stack[w->depth].rel_len = rel_len;
    w->stack[w->depth].depth = depth;
    w->stack[w->depth].gi = gi;
    w->stack[w->depth].ignored = ignored;
    w->depth++;
    return true;
}

static void frame_pop(WalkState *w)
{
    WalkFrame *f;
    u32 i;

    if (w->depth == 0U)
        return;
    w->depth--;
    f = &w->stack[w->depth];
    for (i = 0U; i < f->n; i++)
        free(f->names[i]);
    free(f->names);
    f->names = NULL;
    f->n = 0U;
}

/*
 * Reads a directory into a sorted name array.
 *
 * `.git` is dropped here, unconditionally and before any ignore
 * matching: it is never a source file, it is enormous, and walking it
 * is pure cost in every workspace that has one.
 */
static bool read_sorted(WalkState *w, const char *abs, char ***out_names,
                        u32 *out_n)
{
    DIR *d;
    struct dirent *ent;
    char **names = NULL;
    u32 n = 0U;
    u32 cap = 0U;

    /*
     * Returns SUCCESS separately from the array.
     *
     * An empty directory yields no names, and a directory that could
     * not be opened also yields no names — collapsing the two into a
     * NULL return made an empty workspace root indistinguishable from
     * an unreadable one, so yew_walk_begin refused to walk it at all.
     */
    *out_names = NULL;
    *out_n = 0U;
    g_opendirs++;
    d = opendir(abs);
    if (d == NULL)
        return false;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strcmp(ent->d_name, ".git") == 0)
            continue;
        if (!w->opts.hidden && ent->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap = cap == 0U ? 32U : cap * 2U;
            names = yew_xreallocarray(names, cap, sizeof(*names));
        }
        names[n] = yew_xmalloc(len + 1U);
        (void)memcpy(names[n], ent->d_name, len + 1U);
        n++;
    }
    (void)closedir(d);
    /* THE determinism step.  See walk.h. */
    yew_sort_stable(names, n, sizeof(*names), name_cmp, NULL);
    *out_names = names;
    *out_n = n;
    return true;
}

static i64 now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000000 + (i64)ts.tv_nsec / 1000;
}

WalkState *yew_walk_begin(const char *root, const WalkOpts *o, FileList *out)
{
    WalkState *w;
    char **names;
    u32 n = 0U;
    size_t rlen;
    struct stat st;

    if (root == NULL || out == NULL)
        return NULL;
    rlen = strlen(root);
    if (rlen == 0U || rlen + 2U >= PATH_MAX)
        return NULL;
    w = yew_xcalloc(1U, sizeof(*w));
    (void)memcpy(w->root, root, rlen + 1U);
    w->opts = o == NULL ? (WalkOpts){0} : *o;
    if (w->opts.max_depth == 0U)
        w->opts.max_depth = (u32)YEW_WALK_DEFAULT_DEPTH;
    if (w->opts.max_entries == 0U)
        w->opts.max_entries = (u64)YEW_WALK_DEFAULT_ENTRIES;
    w->out = out;

    yew_filelist_free(out);
    yew_filelist_init(out);

    (void)memcpy(w->path, root, rlen + 1U);
    w->path_len = (u32)rlen;
    /* Strip a trailing slash so the relative part never starts with
     * one — "src/x.c", never "/src/x.c". */
    while (w->path_len > 1U && w->path[w->path_len - 1U] == '/')
        w->path[--w->path_len] = '\0';
    w->rel_base = w->path_len + 1U;

    seen_init(&w->seen);
    arena_init(&w->gi_arena);
    g_statats++;
    if (stat(w->path, &st) == 0)
        (void)seen_add(&w->seen, (u64)st.st_dev, (u64)st.st_ino);

    if (!read_sorted(w, w->path, &names, &n)) {
        seen_free(&w->seen);
        free(w);
        return NULL;
    }
    {
        const GiSet *root_gi = NULL;

        if (w->opts.use_gitignore) {
            char excl[PATH_MAX];

            /*
             * .git/info/exclude first, then the root .gitignore on top
             * of it — the repository's own rules are the outermost
             * layer, and a .gitignore may negate them.
             */
            if (snprintf(excl, sizeof(excl), "%s/.git/info", w->path) <
                (int)sizeof(excl))
                root_gi = yew_gi_load(&w->gi_arena, excl, NULL);
            root_gi = yew_gi_load(&w->gi_arena, w->path, root_gi);
        }
        (void)frame_push(w, names, n, w->path_len, 0U, root_gi, false);
    }
    return w;
}

/* Appends "/name" to the built path.  False when it would not fit —
 * a truncated path is a different path, never a shorter one. */
static bool path_push(WalkState *w, const char *name)
{
    size_t len = strlen(name);

    if ((size_t)w->path_len + 1U + len + 1U > sizeof(w->path))
        return false;
    w->path[w->path_len] = '/';
    (void)memcpy(w->path + w->path_len + 1U, name, len + 1U);
    w->path_len += (u32)len + 1U;
    return true;
}

static void path_truncate(WalkState *w, u32 to)
{
    w->path_len = to;
    w->path[to] = '\0';
}

static void record_file(WalkState *w)
{
    FileList *out = w->out;
    char *rel;

    if ((u64)out->paths.len >= w->opts.max_entries) {
        out->truncated = true;
        return;
    }
    rel = arena_strdup(&out->a, w->path + w->rel_base);
    YewPathVec_push(&out->paths, rel);
    out->n_files++;
}

bool yew_walk_step(WalkState *w, i64 budget_us)
{
    i64 started = budget_us > 0 ? now_us() : 0;

    if (w == NULL || w->done)
        return false;
    while (w->depth > 0U) {
        WalkFrame *f = &w->stack[w->depth - 1U];
        const char *name;
        struct stat st;
        u32 saved;

        /* read_sorted has already closed DIR* and retained an immutable
         * name vector in the frame, so an entry boundary is a safe yield
         * point.  Checking only when a whole directory finished let a
         * flat source tree monopolize the editor for several milliseconds. */
        if (budget_us > 0 && now_us() - started >= budget_us)
            return true;

        if (f->at >= f->n) {
            path_truncate(w, f->rel_len);
            frame_pop(w);
            continue;
        }
        name = f->names[f->at++];
        saved = w->path_len;
        if (!path_push(w, name))
            continue;
        g_statats++;
        if (fstatat(AT_FDCWD, w->path, &st,
                    w->opts.follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
            path_truncate(w, saved);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char **names;
            u32 n = 0U;
            const GiSet *child_gi = f->gi;
            bool child_ignored = f->ignored;

            if (f->depth + 1U >= w->opts.max_depth) {
                path_truncate(w, saved);
                continue;
            }
            /*
             * §4.1: PRUNE before opening.  An ignored directory that
             * nothing could re-include from is skipped without an
             * opendir, which is what turns node_modules from 90 000
             * entries into a single stat.  yew_gi_prunable is
             * conservative, so when it says no we descend and filter
             * per file instead.
             */
            if (w->opts.use_gitignore && f->gi != NULL &&
                yew_gi_match(f->gi, w->path + w->rel_base, true)) {
                w->out->n_ignored++;
                if (yew_gi_prunable(f->gi, w->path + w->rel_base)) {
                    path_truncate(w, saved);
                    continue;
                }
                child_ignored = true;
            }
            /*
             * The loop guard, checked BEFORE opening: a directory we
             * have already entered is skipped and counted, which is what
             * makes `a/b -> ../a` terminate instead of descending
             * forever.
             */
            if (seen_add(&w->seen, (u64)st.st_dev, (u64)st.st_ino)) {
                w->out->n_loops++;
                path_truncate(w, saved);
                continue;
            }
            if (!read_sorted(w, w->path, &names, &n)) {
                path_truncate(w, saved);
                continue;
            }
            w->out->n_dirs++;
            /* This directory's own .gitignore, if it has one, layered
             * over everything above it. */
            if (w->opts.use_gitignore)
                child_gi = yew_gi_load(&w->gi_arena, w->path, f->gi);
            (void)frame_push(w, names, n, saved, f->depth + 1U, child_gi,
                             child_ignored);
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            bool skip = false;

            if (w->opts.use_gitignore && f->gi != NULL) {
                /*
                 * Inside an ignored directory the default flips: a file
                 * is out unless a rule explicitly RE-INCLUDES it, which
                 * is what makes `!node_modules/keep.js` the only thing
                 * that survives `node_modules/`.
                 */
                if (f->ignored)
                    skip = yew_gi_match(f->gi, w->path + w->rel_base,
                                        false) ||
                           !yew_gi_negated(f->gi, w->path + w->rel_base);
                else
                    skip = yew_gi_match(f->gi, w->path + w->rel_base,
                                        false);
            }
            if (skip)
                w->out->n_ignored++;
            else
                record_file(w);
        }
        path_truncate(w, saved);
        if (w->out->truncated) {
            w->done = true;
            return false;
        }
    }
    w->done = true;
    return false;
}

void yew_walk_end(WalkState *w)
{
    if (w == NULL)
        return;
    while (w->depth > 0U)
        frame_pop(w);
    free(w->stack);
    seen_free(&w->seen);
    arena_free_all(&w->gi_arena);
    free(w);
}
