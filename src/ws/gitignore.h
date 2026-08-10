#ifndef YEW_WS_GITIGNORE_H
#define YEW_WS_GITIGNORE_H

/*
 * Sprint 26 §4: the .gitignore matcher — bespoke, and a SUBSET.
 *
 * WHY BESPOKE.  No dependency (invariant 7) and no `git` subprocess: the
 * finder must work in a directory that is not a repository at all, and
 * shelling out per walk would put process-spawn latency inside the file
 * list.  The POSIX pattern matcher is rejected separately — it has no
 * `**`, its path-mode semantics differ across libcs (a determinism
 * hazard, invariant 5), and negation and anchoring have to be built
 * around it anyway.
 *
 * WHAT IS SUPPORTED
 *
 *   blank lines, `#` comments        leading `\#` escapes a literal #
 *   trailing whitespace stripped     unless escaped as `\ `
 *   leading `/`                      anchored to the ignore file's dir
 *   trailing `/`                     directory only
 *   `*`                              any run not crossing `/`
 *   `?`                              one byte, not `/`
 *   `[abc]` `[a-z]` `[!a-z]` `[^a-z]`  byte ranges
 *   double-star prefix, suffix and interior segments
 *   `!` negation                     last matching rule wins
 *   nested .gitignore                deepest matched last
 *   .git/info/exclude                loaded once at the root
 *
 * WHAT IS NOT, AND WHAT HAPPENS INSTEAD.  Every row here has a test
 * pinning the DOCUMENTED behaviour, not an aspiration — an unsupported
 * feature that silently half-works is worse than one that is absent,
 * because it looks like it works until someone's file goes missing.
 *
 *   POSIX classes `[[:alpha:]]`   The bracket set closes at the FIRST
 *                                 `]`, so it reads as a set of the
 *                                 bytes `[ : a l p h` followed by a
 *                                 LITERAL `]`.  `f[[:alpha:]].txt`
 *                                 therefore matches `fa].txt` and not
 *                                 `fa.txt` or `fz.txt`.  git does none
 *                                 of this.  Vanishingly rare in
 *                                 checked-in ignore files, and pinned
 *                                 by a test so implementing classes
 *                                 later is a deliberate change.
 *   core.excludesFile             Needs a `git config` subprocess per
 *                                 walk.  Global ignores are not read.
 *   sparse-checkout, skip-worktree,
 *   assume-unchanged              Index features; we have no index.
 *   INDEX AWARENESS               git never ignores a TRACKED file.  We
 *                                 have no index reader, so a
 *                                 tracked-but-ignored file DOES appear
 *                                 in the finder.  This is the most
 *                                 user-visible divergence and it is in
 *                                 the manual.
 *   core.ignorecase               Matching is always case-sensitive,
 *                                 even on a case-insensitive
 *                                 filesystem.
 *
 * Patterns compile ONCE at load into a small segment program (literal
 * runs, wildcards, bracket sets).  Matching never re-parses, which is
 * what makes 200 000 calls cheap.
 */

#include "util/arena.h"
#include "util/base.h"

typedef struct GiSet GiSet;

/*
 * Compiles `dir`/.gitignore, chained under `parent` (which may be NULL).
 * Returns `parent` unchanged when there is no ignore file here, so a
 * deep tree of directories without one costs nothing.
 *
 * Everything is allocated in `a` and dies with it.
 */
GiSet *yew_gi_load(Arena *a, const char *dir, const GiSet *parent);

/*
 * Compiles one in-memory ignore file, for the root's
 * `.git/info/exclude` and for tests that would rather not touch a
 * filesystem.  `base` is the set's directory, workspace-relative ("" at
 * the root).
 */
GiSet *yew_gi_compile(Arena *a, const char *base, const char *text,
                      u64 len, const GiSet *parent);

/*
 * True when `rel` — a workspace-RELATIVE path — is ignored.
 *
 * `is_dir` matters: a trailing-slash rule matches directories only.
 * The whole chain is consulted, deepest set last, and within a set the
 * LAST matching rule wins, which is what makes `!` work.
 */
bool yew_gi_match(const GiSet *g, const char *rel, bool is_dir);

/*
 * §4.1: may the walk skip this ignored directory without opening it?
 *
 * git's own rule — "it is not possible to re-include a file if a parent
 * directory of that file is excluded" — is what makes pruning legal at
 * all.  `node_modules/` then costs one stat instead of 90 000 entries,
 * which is the difference between a usable and an unusable finder on a
 * JavaScript checkout.
 *
 * Deliberately CONSERVATIVE: a negation is "in scope" when any active
 * pattern begins with `!` and its literal prefix, up to its first
 * wildcard, lies inside the excluded directory.  When in doubt,
 * descend — a slow correct walk beats a fast one that hides a file the
 * user can see in `git status`.
 */
bool yew_gi_prunable(const GiSet *g, const char *rel);

/*
 * Did a `!` rule name this path?
 *
 * Asked separately from yew_gi_match because inside an ignored
 * directory the default reverses: git never descends into one, so
 * everything below is excluded, and only an explicit re-inclusion
 * survives.  yew_gi_match cannot express that on its own — a file under
 * `node_modules/` matches no rule at all, because a directory-only rule
 * does not match files.
 */
bool yew_gi_negated(const GiSet *g, const char *rel);

#endif
