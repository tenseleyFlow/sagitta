#ifndef SAG_WS_FINDER_H
#define SAG_WS_FINDER_H

/*
 * Sprint 18.5 §1: the fuss-derived fuzzy scorer, pulled forward from
 * Sprint 26 §1/§2 so the command line can rank and HIGHLIGHT this sprint
 * rather than waiting for the finder.  Sprint 26 inherits these as landed
 * APIs; its walk, ignore matcher, and list picker are still its own.
 *
 * Ported from `fuzzy_match_score` / `fuzzy_jump_to_match` in
 * ~/GithubOrgs/FortranGoingOnForty/fuss/src/fuss_main.f90.
 *
 * This file is a BYTE scan with ASCII case folding.  It does no width
 * math and no Unicode work: the moment ranking consults width tables it
 * is doing display work, and the two answers diverge under emoji.
 */

#include <stdbool.h>
#include <stdint.h>

#include "util/base.h"

/*
 * INT32_MIN, not 0.  fuss returns 0 both for "the pattern was not
 * exhausted" and, arithmetically, for a long path whose gap and length
 * penalties cancelled its bonuses -- a real match then vanishes from the
 * list.  The sentinel separates the two, and sag_fz_score clamps a
 * genuine score away from it so the two can never collide.
 */
#define SAG_FZ_NO_MATCH INT32_MIN

enum {
    /* Matched positions beyond this are unhighlighted, never rescored. */
    SAG_FZ_MAX_POS = 64,
    /* Lifts every exact-or-prefix basename match above every fuzzy path
     * match, globally.  See sag_fz_rank. */
    SAG_FZ_BASENAME_TIER = 100000
};

/* Byte offsets into the scored text, ascending. */
typedef struct FzMatch {
    u16 n_pos;
    u16 pos[SAG_FZ_MAX_POS];
} FzMatch;

/*
 * Score `text` against `pat`; SAG_FZ_NO_MATCH when `pat` is not a
 * subsequence of `text`.  `m` may be NULL when positions are not wanted.
 *
 * The table this implements (every row is a unit case):
 *
 *   empty pattern    1            plen == 0; source order preserved
 *   exact            10000        case-folded equal; returns immediately
 *   prefix           5000         case-folded prefix; returns immediately
 *   matched char     +100         each char matched by the greedy scan
 *   consecutive run  +50 * k      k = 2, 3, ...; first of a run gets none
 *   start of text    +200         the match landed at byte 0
 *   word boundary    +150         preceding byte is one of / _ - .
 *   gap              -1           each unmatched byte after the first match
 *   length penalty   -tlen        once, at the end
 *
 * Greedy first-match, not an optimal alignment: `abc` against
 * `axbxc_abc` scores the early, worse occurrence.  That is fuss's
 * behaviour and it is O(n) rather than O(n*m); sag_fz_rank's basename
 * tier covers the case users actually complain about.  Do not promote it
 * to a dynamic program without re-pinning the table, the parity corpus,
 * and every golden that depends on the ordering.
 */
i32 sag_fz_score(const char *pat, u32 plen, const char *text, u32 tlen,
                 FzMatch *m);

typedef struct FzRanked {
    u32 idx; /* index into the caller's `text` array */
    i32 score;
    FzMatch m; /* positions are relative to text[idx], always */
} FzRanked;

/*
 * Rank `n` candidates into `out` (which the caller sizes to `n`);
 * returns how many MATCHED.  Non-matching candidates are excluded rather
 * than sorted last, so the count means "matches" and not "rows".
 *
 * fuss runs pass 1 over basenames, takes the answer if it scored >= 5000,
 * and only then runs pass 2 over full paths -- which selects one row.  A
 * filtering list needs an ordering of ALL rows, so the same intent
 * becomes one key:
 *
 *   sb  = score(pat, basename(text))
 *   sp  = score(pat, text)                       (path_mode only)
 *   key = (sb >= 5000) ? sb + SAG_FZ_BASENAME_TIER : max(sb, sp)
 *
 * which is why `src` selects the directory `src/` and not `src/file.c`.
 *
 * `path_mode == false` (command names, buffer labels) scores the label
 * only.  Ties break by shorter text, then memcmp, through
 * sag_sort_stable -- never qsort, whose tie order differs between glibc
 * and musl and would make the pty goldens libc-dependent (invariant 5).
 */
u32 sag_fz_rank(const char *pat, u32 plen, const char *const *text, u32 n,
                bool path_mode, FzRanked *out);

#endif
