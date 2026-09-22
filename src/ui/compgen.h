#ifndef YEW_UI_COMPGEN_H
#define YEW_UI_COMPGEN_H

/*
 * Sprint 57.24 §5: completion generators.
 *
 * Built-ins (hosts, signals, users, make_targets) are C and synchronous:
 * they read a file or the password database and never run anything.
 * `make_targets` in particular PARSES the Makefile -- `make -p` would
 * evaluate it, and `$(shell ...)` in a Makefile runs arbitrary commands.
 *
 * Spec generators run as subprocesses through the job layer, never a
 * shell, and ASYNCHRONOUSLY: a keystroke never waits on one.  The async
 * contract (invariants 4 and 5):
 *
 *   - at most ONE job in flight per key, at most FOUR in total;
 *   - results are cached per key; fresh ones are served synchronously,
 *     stale ones are served AND refreshed;
 *   - an arrival only refilters an open menu whose key still matches --
 *     it NEVER edits the prompt's text;
 *   - a timeout, a non-zero exit or a spawn failure caches an empty
 *     answer for cache_ms and logs once; the footer says nothing.
 *
 * Generator jobs are internal (hidden from the jobs table) and
 * `evictable`: a user's own `:!` command that finds the job table full
 * takes a generator's slot rather than failing (job.c).
 */

#include <stdbool.h>

#include "ui/cmdcomp.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct Ed Ed;

enum {
    YEW_COMPGEN_MAX_INFLIGHT = 4,
    YEW_COMPGEN_TIMEOUT_MS = 1500,
    YEW_COMPGEN_MAX_LINES = 5000,
    YEW_COMPGEN_DEFAULT_CACHE_MS = 2000
};

#define YEW_COMPGEN_COLLECT_MAX (1024U * 1024U)

typedef struct YewCompGenKey {
    const char *name;         /* for logs: "git.fl:branches", "pids"   */
    const char *const *argv;  /* NULL-terminated; pass flags included  */
    const char *cwd;          /* where `:!` commands run               */
    i64 cache_ms;
    /* Parse `ps -o pid=,comm=` columns (the `pids` built-in) instead of
     * `candidate<TAB>description` lines. */
    bool ps_columns;
} YewCompGenKey;

/* The key's identity: name, argv and cwd.  Heap-owned. */
char *yew_compgen_key_string(const YewCompGenKey *key);

/*
 * Returns the rows for `key` now (possibly stale, possibly empty) and
 * whether a job is in flight for it.  Never blocks.  Rows are
 * YEW_COMP_GEN items with `text` == `match` == the candidate, in the
 * generator's order, strings in `a`.
 */
bool yew_compgen_rows(Ed *ed, const YewCompGenKey *key, i64 now_ms,
                      Arena *a, Vec_CompItem *out, bool *pending);

/* In flight AND no cached answer yet: the pager's `…` marker (§5.5). */
bool yew_compgen_awaiting(const char *key_string);

/* The synchronous built-ins.  `cwd` and `makefile` matter only to
 * make_targets (`makefile` NULL: GNUmakefile, makefile, Makefile).
 * Returns false for a name that is not a built-in. */
bool yew_compgen_builtin(const char *name, const char *cwd,
                         const char *makefile, Arena *a, Vec_CompItem *out);

u32 yew_compgen_inflight(void);          /* test seam */
void yew_compgen_cache_clear(void);      /* test seam; prompt close */
/* Test seam: the generator timeout (0 restores YEW_COMPGEN_TIMEOUT_MS). */
void yew_compgen_test_set_timeout_ms(i64 ms);
/* Test seam: spawns attempted since the last cache clear. */
u32 yew_compgen_test_spawns(void);

#endif
