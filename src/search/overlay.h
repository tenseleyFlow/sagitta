#ifndef SAG_SEARCH_OVERLAY_H
#define SAG_SEARCH_OVERLAY_H

/*
 * Sprint 21 §3: viewport-scoped match highlighting.
 *
 * This is the sprint's load-bearing performance decision.  Highlighting
 * "all matches" sounds like a whole-file scan, and on a 1 GB file a
 * whole-file scan is seconds — per keystroke, against a 5 ms
 * keypress→paint budget (invariant 4).  So the scan is bounded twice
 * over: by SCOPE (the viewport plus a look-ahead, never the file) and
 * by TIME (a per-keystroke budget, with the remainder finished on the
 * idle timer).  Partial highlighting for one frame is invisible; a
 * stalled keystroke is not.
 *
 * The type is deliberately pattern-agnostic — it speaks byte spans and
 * nothing else — because Sprint 47's LSP document-highlight reuses this
 * span/damage machinery.  No width or cluster math lives here; the draw
 * pass owns that.
 */

#include "search/regex.h"
#include "text/coords.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;
typedef struct Win Win;

VEC_DECL(SpanVec, Span);

enum {
    /* Per-keystroke scan ceiling, whichever comes first. */
    SAG_OVERLAY_BUDGET_US = 1000,
    SAG_OVERLAY_BUDGET_BYTES = 256U * 1024U,
    /* Look-ahead beyond the viewport, so an ordinary scroll reuses the
     * scanned range instead of rescanning. */
    SAG_SEARCH_LOOKAHEAD_MAX = 64U * 1024U,
    /* Counting every match IS a whole-file scan, so it is capped. */
    SAG_SEARCH_COUNT_MAX = 10000
};

typedef struct MatchOverlay {
    SpanVec spans;   /* buffer byte spans, ascending and disjoint */
    Span scanned;    /* the byte range those spans are complete for */
    u32 pat_gen;     /* bumped per recompile */
    u64 buf_gen;     /* TextBuf.gen */
    i32 cur_index;   /* the match the cursor is on, or -1 */
    u32 count_total;
    bool count_capped;
    bool complete;   /* false when the budget cut a scan short */
} MatchOverlay;

void sag_overlay_init(MatchOverlay *ov);
void sag_overlay_free(MatchOverlay *ov);
/* Drops the spans; the next refresh starts from nothing. */
void sag_overlay_invalidate(MatchOverlay *ov);

/*
 * Brings the overlay up to date for `w`'s current viewport, damaging
 * exactly the lines whose highlight set changed.  `budget_us` <= 0
 * means "no limit", which only the idle-timer path and tests pass.
 */
void sag_overlay_refresh(Ed *ed, Win *w, const SagRe *re, u32 pat_gen,
                         i64 budget_us);

/* The bounded count behind the `[3/17]` indicator.  Stops at
 * SAG_SEARCH_COUNT_MAX and says so. */
void sag_overlay_count(MatchOverlay *ov, const SagRe *re, const TextBuf *tb,
                       i64 budget_us);

#endif
