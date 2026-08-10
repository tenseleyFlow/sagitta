#ifndef YEW_TEST_RE_REF_H
#define YEW_TEST_RE_REF_H

/*
 * A deliberately naive reference matcher for the differential fuzzer.
 *
 * This is a RECURSIVE BACKTRACKER — the exact thing yew's engine
 * refuses to be.  That is the point: an independent implementation with
 * different failure modes is a real oracle, whereas a second Thompson
 * construction would share every bug with the first.  It is allowed to
 * be exponential because it is never shipped; a step budget stops it,
 * and budget-exceeded cases are SKIPPED rather than failed, since the
 * oracle running out of road says nothing about the engine.
 *
 * It covers only the overlapping semantic subset: literals, '.',
 * classes, anchors, groups, alternation, and greedy/lazy quantifiers
 * with leftmost-first semantics.
 */

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

enum { YEW_REF_MAX_GROUPS = 8 };

typedef enum {
    YEW_REF_NO_MATCH = 0,
    YEW_REF_MATCH = 1,
    /* Step budget exhausted, or the pattern used a construct outside the
     * shared subset: the caller must skip, not fail. */
    YEW_REF_UNKNOWN = -1
} YewRefResult;

typedef struct YewRefMatch {
    u64 lo[YEW_REF_MAX_GROUPS];
    u64 hi[YEW_REF_MAX_GROUPS];
    bool set[YEW_REF_MAX_GROUPS];
    u32 ngroups;
} YewRefMatch;

/* Leftmost-first search over `hay`, starting at or after `from`. */
YewRefResult yew_ref_search(const char *pat, size_t patlen, const u8 *hay,
                            size_t haylen, u64 from, YewRefMatch *out);

#endif
