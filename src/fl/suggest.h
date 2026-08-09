#ifndef SAG_FL_SUGGEST_H
#define SAG_FL_SUGGEST_H

/*
 * Sprint 32 §7: "did you mean".
 *
 * THE CANDIDATE SET IS PER-SITE BY CONSTRUCTION.  The easy
 * implementation scores against every interned string in the process --
 * the Interner is right there -- and suggests private helpers,
 * module-internal names and string literals.  Callers build the set
 * from what is actually in scope; nothing here can reach the interner,
 * and DoD 7 greps to confirm it.
 *
 * THE METRIC IS PINNED: Damerau-Levenshtein, optimal string alignment.
 * Transposition matters because `lenght`/`length` is the most common
 * real typo, and OSA is the cheap variant -- no alphabet table.  Costs
 * are scaled by two so the case-only case can be a half in integers:
 *
 *     insert / delete / substitute      2
 *     adjacent transposition            2
 *     substitution differing in case    1
 *
 * THE LENGTH FLOOR IS ON THE CANDIDATE, not the typo.  At length two
 * everything is within one edit of everything, and "did you mean 'if'?"
 * for `in` is noise that teaches users to ignore the feature.
 */

#include "util/base.h"
#include "util/buf.h"

/*
 * Scope proximity, and the ORDER MATTERS: it is the second sort key,
 * because a shadowed local is what the user most likely meant.
 */
typedef enum {
    FL_SCOPE_LOCAL = 0,   /* innermost locals                          */
    FL_SCOPE_UPVAL,       /* enclosing-function locals, as upvalues    */
    FL_SCOPE_GLOBAL,      /* module globals                            */
    FL_SCOPE_BUILTIN      /* the seven builtin module names            */
} FlScopeBand;

enum {
    /* Above this the set is prefix-filtered, and if it is still above,
     * nothing is suggested: a suggestion is a courtesy, not a reason to
     * spend milliseconds. */
    FL_SUGGEST_MAX_CANDIDATES = 512,
    /* Names longer than this are not scored.  A 64-byte typo does not
     * want a suggestion, and the DP is quadratic. */
    FL_SUGGEST_MAX_LEN = 64,
    FL_SUGGEST_MAX_SHOWN = 3
};

typedef struct FlCand {
    const char *name;
    u32 len;
    u8 band;              /* FlScopeBand                               */
    u32 dist;             /* filled by the scorer                      */
} FlCand;

typedef struct FlSuggest {
    FlCand v[FL_SUGGEST_MAX_CANDIDATES];
    u32 n;
    /* Set once the cap is passed, so a caller can stop adding. */
    bool full;
} FlSuggest;

void fl_suggest_reset(FlSuggest *s);
void fl_suggest_add(FlSuggest *s, const char *name, u32 len, FlScopeBand band);

/*
 * Appends `did you mean 'total'? (or 'totals', 'tally')` to `out`, or
 * appends NOTHING when no candidate clears the threshold.  Returns how
 * many were named.
 *
 * Deterministic: ascending distance, then scope band, then
 * byte-lexicographic through sag_sort_stable.  Sprint 33's determinism
 * lane byte-compares two runs, and suggestion ordering is the part that
 * rots first.
 */
u32 fl_suggest_render(FlSuggest *s, const char *typo, u32 typolen,
                      Bytebuf *out);

/* The scaled distance, exposed for the metric's own tests.  Returns
 * U32_MAX when either name is too long to score. */
u32 fl_suggest_distance(const char *a, u32 alen, const char *b, u32 blen);

#endif /* SAG_FL_SUGGEST_H */
