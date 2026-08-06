#ifndef SAG_WS_WALK_H
#define SAG_WS_WALK_H

/*
 * Sprint 26 §3: the workspace file walk.
 *
 * BESPOKE, and the sprint says why (invariant 7).  The libc tree-walk
 * helpers cannot be interrupted on a budget, carry their callback state
 * through globals, and hand back an order that is not ours to pin; the
 * directory-slurping one allocates per entry and sorts with a
 * comparator that cannot see our tie rules.  All of them are available
 * and all of them are rejected.
 *
 * DoD 4 greps src/ws/ for those function names and expects nothing, so
 * this paragraph deliberately spells none of them — a tripwire a
 * comment can trip is one a reviewer learns to wave through.
 *
 * RECURSION IS OUT TOO.  A 400-deep tree is a stack hazard in a program
 * that must never crash (invariant 1), so the descent is an explicit
 * stack of open directories.
 *
 * DETERMINISM IS THE PROPERTY (invariant 5).  readdir returns filesystem
 * order: it differs between ext4 and xfs, and between two checkouts of
 * the same repository made in different orders.  Every directory's
 * entries are therefore sorted before we descend, so the same tree
 * always produces the same list — which is what keeps ranking ties, and
 * every pty golden that shows a file list, from depending on the
 * filesystem underneath.
 *
 * BUDGETED.  sag_walk_step does roughly `budget_us` of work and returns,
 * so a UI-initiated walk of 100 000 files runs in slices on the idle
 * timer and never blocks a keystroke (invariant 4).
 */

#include "util/arena.h"
#include "util/base.h"
#include "util/vec.h"

VEC_DECL(SagPathVec, char *);

typedef struct WalkOpts {
    bool follow_symlinks; /* default false                    */
    bool hidden;          /* include dotfiles; default false  */
    u32 max_depth;        /* 0 = SAG_WALK_DEFAULT_DEPTH       */
    u64 max_entries;      /* 0 = SAG_WALK_DEFAULT_ENTRIES     */
} WalkOpts;

enum {
    SAG_WALK_DEFAULT_DEPTH = 64,
    SAG_WALK_DEFAULT_ENTRIES = 200000
};

/*
 * Paths are workspace-RELATIVE: that is what the user reads, what
 * scoring sees, and what keeps 100 000 strings small.  One arena, one
 * vector, no per-path malloc.
 */
typedef struct FileList {
    Arena a;
    SagPathVec paths;
    u64 n_dirs;
    u64 n_files;
    u64 n_ignored;
    /* Directories skipped because we had already entered them — see the
     * device+inode set in walk.c. */
    u64 n_loops;
    /* max_entries was reached.  The caller says so once; a silently
     * short list is the failure this flag exists to prevent. */
    bool truncated;
} FileList;

void sag_filelist_init(FileList *fl);
void sag_filelist_free(FileList *fl);

typedef struct WalkState WalkState;

/*
 * Begins a walk of `root`, filling `out`.  NULL when `root` cannot be
 * opened — which is not an error the caller must handle beyond showing
 * an empty list.
 *
 * `out` is reset by this call and owns its results until
 * sag_filelist_free.
 */
WalkState *sag_walk_begin(const char *root, const WalkOpts *o,
                          FileList *out);

/*
 * Does roughly `budget_us` of work.  Returns true while there is more to
 * do, false when the walk is complete.  A budget of 0 means "run to
 * completion", which is what tests and the perf harness use.
 *
 * The budget is checked between DIRECTORIES, not between entries: a
 * single directory of 100 000 files is one indivisible unit of work, and
 * pretending otherwise would mean holding a half-read DIR* across
 * frames while the filesystem changes underneath it.
 */
bool sag_walk_step(WalkState *w, i64 budget_us);
void sag_walk_end(WalkState *w);

/*
 * Test hooks (DoD 5 counts them): opendir and fstatat calls performed by
 * the walk so far.  Pruning a 5 000-file node_modules must cost one stat
 * and zero opendirs, and counting is the only way to know.
 */
u64 sag_walk_opendir_count(void);
u64 sag_walk_statat_count(void);
void sag_walk_counts_reset(void);

#endif
