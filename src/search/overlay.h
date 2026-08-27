#ifndef YEW_SEARCH_OVERLAY_H
#define YEW_SEARCH_OVERLAY_H

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
    YEW_OVERLAY_BUDGET_US = 1000,
    YEW_OVERLAY_BUDGET_BYTES = 256U * 1024U,
    /* Look-ahead beyond the viewport, so an ordinary scroll reuses the
     * scanned range instead of rescanning. */
    YEW_SEARCH_LOOKAHEAD_MAX = 64U * 1024U,
    /* Counting every match IS a whole-file scan, so an interactive count
     * is bounded by bytes as well as matches and wall time. */
    YEW_SEARCH_COUNT_MAX = 10000,
    /* The regex engine cannot preempt one search call.  Keep that call
     * small enough that even the slow anchored/alternation smoke rows stay
     * below one keypress frame on the designated class of machine. */
    YEW_SEARCH_COUNT_BUDGET_BYTES = 16U * 1024U
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

void yew_overlay_init(MatchOverlay *ov);
void yew_overlay_free(MatchOverlay *ov);
/* Drops the spans; the next refresh starts from nothing. */
void yew_overlay_invalidate(MatchOverlay *ov);

/*
 * Brings the overlay up to date for `w`'s current viewport, damaging
 * exactly the lines whose highlight set changed.  `budget_us` <= 0
 * means "no limit", which only the idle-timer path and tests pass.
 */
void yew_overlay_refresh(Ed *ed, Win *w, const YewRe *re, u32 pat_gen,
                         i64 budget_us);

/* The bounded count behind the `[3/17]` indicator.  A positive time budget
 * enables the byte and time ceilings for buffers larger than the byte
 * ceiling; small buffers count exactly so instrumentation and machine speed
 * cannot change the badge.  A non-positive budget is exact too. */
void yew_overlay_count(MatchOverlay *ov, const YewRe *re, const TextBuf *tb,
                       i64 budget_us);

#endif
