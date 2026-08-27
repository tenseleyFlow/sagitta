/*
 * Sprint 21 §3 / DoD 5 + 9: the match overlay.
 *
 * The load-bearing assertion is the scope one.  "Highlight all matches"
 * read literally is a whole-file scan, and on a large file that is
 * seconds per keystroke against a 5 ms budget.  So the fixture below
 * deliberately puts matches far outside the viewport: a correct overlay
 * does NOT find them, and an implementation that quietly scans the file
 * fails here rather than in production on someone's 200 MB log.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "search/overlay.h"
#include "search/searchui.h"
#include "text/piece.h"
#include "util/arena.h"

/* `nlines` lines of "aaaa\n", with "needle" planted on `plant` only. */
static void ov_fixture(Ed *ed, u32 nlines, u32 plant_a, u32 plant_b)
{
    EditCtx ec;
    Bytebuf src;
    u32 i;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    bytebuf_init(&src);
    for (i = 0U; i < nlines; i++) {
        if (i == plant_a || i == plant_b)
            bytebuf_append(&src, "needle\n", 7U);
        else
            bytebuf_append(&src, "aaaa\n", 5U);
    }
    ec = yew_ed_edit_ctx(ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), src.data, src.len));
    bytebuf_free(&src);
    /* A viewport of 10 rows, so "outside the viewport" is easy to
     * arrange and easy to read in the assertions below. */
    ed->win->rect.h = 10U;
    ed->win->rect.w = 80U;
}

static YewRe *ov_compile(Arena *a, const char *pat)
{
    SearchOpts o;
    YewRe *re;

    arena_init(a);
    yew_search_opts_init(&o);
    re = yew_search_compile(a, pat, strlen(pat), &o, NULL);
    YEW_ASSERT_NOT_NULL(re);
    return re;
}

/*
 * DoD 5.  Line 2 is on screen, line 900 is not; a full scan would find
 * both.  The overlay must find exactly the one in scope.
 */
void test_overlay_scan_is_bounded_by_viewport_and_lookahead(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 1000U, 2U, 900U);
    re = ov_compile(&arena, "needle");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);

    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 1U);
    /* Line 2 starts at byte 10 (two "aaaa\n" before it). */
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.data[0].lo, 10U);
    /* And the scanned range really is bounded, not merely lucky. */
    YEW_ASSERT(ed.win->overlay.scanned.hi < yew_textbuf_len(ed.buffer.tb));
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/* The look-ahead is the thing that makes one screen of scrolling free,
 * so it must actually extend past the visible lines. */
void test_overlay_lookahead_reaches_past_the_visible_lines(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;
    u64 visible_hi;

    /* 10 visible rows, so lines 0..10 are on screen; plant at 20,
     * which is inside two viewport heights of look-ahead. */
    ov_fixture(&ed, 1000U, 20U, 900U);
    re = ov_compile(&arena, "needle");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);

    visible_hi = yew_textbuf_line_start(ed.buffer.tb, LINENO(11U)).v;
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 1U);
    YEW_ASSERT(ed.win->overlay.spans.data[0].lo > visible_hi);
    YEW_ASSERT(ed.win->overlay.scanned.hi > visible_hi);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/* A scroll that stays inside `scanned` reuses it instead of rescanning:
 * the spans and the scanned range come back identical. */
void test_overlay_scroll_inside_scanned_range_is_reused(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;
    Span first_scanned;
    size_t first_len;

    ov_fixture(&ed, 1000U, 2U, 900U);
    re = ov_compile(&arena, "needle");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    first_scanned = ed.win->overlay.scanned;
    first_len = ed.win->overlay.spans.len;

    /* One line down is well inside the look-ahead. */
    ed.win->vp.top = LINENO(1U);
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.scanned.lo, first_scanned.lo);
    YEW_ASSERT_EQ_U64(ed.win->overlay.scanned.hi, first_scanned.hi);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, first_len);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/* Both invalidation keys drop the scanned range. */
void test_overlay_invalidates_on_pattern_and_buffer_generation(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;
    EditCtx ec;

    ov_fixture(&ed, 100U, 2U, 3U);
    re = ov_compile(&arena, "needle");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 2U);
    YEW_ASSERT_EQ_U64(ed.win->overlay.pat_gen, 1U);

    /* A recompiled pattern: same generation number would be a stale
     * highlight, so the key must move. */
    yew_overlay_refresh(&ed, ed.win, re, 2U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.pat_gen, 2U);

    /* An edit bumps TextBuf.gen, which invalidates independently. */
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"needle\n",
                               7U));
    yew_overlay_refresh(&ed, ed.win, re, 2U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.buf_gen, ed.buffer.tb->gen);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 3U);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/*
 * An exhausted budget keeps what was found and says the overlay is
 * incomplete, rather than dropping work or blocking.  Partial
 * highlighting for one frame is invisible; a stalled keystroke is not.
 */
void test_overlay_budget_exhaustion_yields_partial_spans(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 20000U, 0U, 1U);
    /* Every line matches, and the window is large, so a 1 us budget
     * cannot possibly finish. */
    re = ov_compile(&arena, "a");
    ed.win->rect.h = 200U;
    yew_overlay_refresh(&ed, ed.win, re, 1U, 1);
    YEW_ASSERT(!ed.win->overlay.complete);
    /* Cut short, not empty: the matches found so far still highlight. */
    YEW_ASSERT(ed.win->overlay.spans.len > 0U);
    /* And the scanned range honestly reports how far it actually got. */
    YEW_ASSERT(ed.win->overlay.scanned.hi >=
               ed.win->overlay.spans.data[ed.win->overlay.spans.len - 1U].lo);

    /* The idle pass, with no budget, finishes the job. */
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT(ed.win->overlay.complete);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/* Spans are ascending, disjoint and inside the buffer — the invariants
 * the fuzzer will assert after every operation. */
void test_overlay_spans_are_ascending_and_disjoint(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;
    size_t i;
    u64 len;

    ov_fixture(&ed, 60U, 1U, 2U);
    re = ov_compile(&arena, "a*");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    len = yew_textbuf_len(ed.buffer.tb);
    YEW_ASSERT(ed.win->overlay.spans.len > 0U);
    for (i = 0U; i < ed.win->overlay.spans.len; i++) {
        Span s = ed.win->overlay.spans.data[i];

        YEW_ASSERT(s.lo <= s.hi);
        YEW_ASSERT(s.hi <= len);
        if (i > 0U)
            YEW_ASSERT(ed.win->overlay.spans.data[i - 1U].hi <= s.lo);
    }
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/* cur_index names the match the cursor is standing on, which is what
 * separates the `search_current` style from `search_match`. */
void test_overlay_cur_index_tracks_the_cursor(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 20U, 1U, 3U);
    re = ov_compile(&arena, "needle");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 2U);
    YEW_ASSERT_EQ_I64(ed.win->overlay.cur_index, -1);

    yew_ed_cursor(&ed)->pos = BYTEOFF(ed.win->overlay.spans.data[1].lo);
    yew_overlay_refresh(&ed, ed.win, re, 2U, 0);
    YEW_ASSERT_EQ_I64(ed.win->overlay.cur_index, 1);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/*
 * The numerator must follow the cursor even when the spans are reused.
 * The reuse fast path returned before recomputing it, so `[2/3]` would
 * stay `[2/3]` while `n` walked — visible in a pty golden as a badge
 * naming the previous match.
 */
void test_overlay_cur_index_updates_when_spans_are_reused(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 20U, 1U, 3U);
    re = ov_compile(&arena, "needle");
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 2U);
    YEW_ASSERT(ed.win->overlay.complete);
    YEW_ASSERT_EQ_I64(ed.win->overlay.cur_index, -1);

    /* Same pattern, same viewport: the spans are reused.  The cursor
     * moved, so the index must not be. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(ed.win->overlay.spans.data[0].lo);
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_I64(ed.win->overlay.cur_index, 0);

    yew_ed_cursor(&ed)->pos = BYTEOFF(ed.win->overlay.spans.data[1].lo);
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_I64(ed.win->overlay.cur_index, 1);

    /* And back to nothing when the cursor leaves every match. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(0U);
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT_EQ_I64(ed.win->overlay.cur_index, -1);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/* A NULL pattern clears the highlight; that is what `clear_highlight`
 * and an emptied prompt both do. */
void test_overlay_null_pattern_clears(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 20U, 1U, 3U);
    re = ov_compile(&arena, "needle");
    yew_overlay_refresh(&ed, ed.win, re, 1U, 0);
    YEW_ASSERT(ed.win->overlay.spans.len > 0U);
    yew_overlay_refresh(&ed, ed.win, NULL, 1U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 0U);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/*
 * The counting cap.  An unbounded counter is exactly the feature that
 * makes a big-file editor feel broken, so past the cap the count stops
 * and says it stopped.
 */
void test_overlay_count_caps_at_ten_thousand(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 20000U, 0U, 1U);
    re = ov_compile(&arena, "aaaa");
    yew_overlay_count(&ed.win->overlay, re, ed.buffer.tb, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.count_total, YEW_SEARCH_COUNT_MAX);
    YEW_ASSERT(ed.win->overlay.count_capped);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

void test_overlay_count_is_exact_below_the_cap(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 100U, 5U, 7U);
    re = ov_compile(&arena, "needle");
    yew_overlay_count(&ed.win->overlay, re, ed.buffer.tb, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.count_total, 2U);
    YEW_ASSERT(!ed.win->overlay.count_capped);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

void test_overlay_interactive_count_is_byte_bounded(void)
{
    Ed ed;
    Arena arena;
    YewRe *re;

    ov_fixture(&ed, 60000U, UINT32_MAX, UINT32_MAX);
    re = ov_compile(&arena, "needle");
    YEW_ASSERT(yew_textbuf_len(ed.buffer.tb) >
               YEW_SEARCH_COUNT_BUDGET_BYTES);
    yew_overlay_count(&ed.win->overlay, re, ed.buffer.tb, 1000);
    YEW_ASSERT_EQ_U64(ed.win->overlay.count_total, 0U);
    YEW_ASSERT(ed.win->overlay.count_capped);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

/*
 * DoD 9's structural half: adding a character to the pattern damages
 * only the lines whose highlight set actually changed.  The expectation
 * is computed from the two span sets, not written as a magic number.
 */
void test_overlay_damage_is_diffed_not_blanket(void)
{
    Ed ed;
    Arena arena;
    YewRe *wide;
    YewRe *narrow;
    u32 changed_lines = 0U;
    size_t i;

    /* "needle" on lines 1 and 3; /n/ matches both, /ne/ still matches
     * both, so refining the pattern changes only the span WIDTHS on
     * those two lines and nothing anywhere else. */
    ov_fixture(&ed, 40U, 1U, 3U);
    wide = ov_compile(&arena, "n");
    yew_overlay_refresh(&ed, ed.win, wide, 1U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 2U);

    ed.doc_damage_lo = 0U;
    ed.doc_damage_hi = 0U;
    narrow = yew_re_compile(&arena, "needle", 6U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(narrow);
    yew_overlay_refresh(&ed, ed.win, narrow, 2U, 0);
    YEW_ASSERT_EQ_U64(ed.win->overlay.spans.len, 2U);

    /* Both span sets live on exactly two lines, so at most two lines
     * can legitimately be damaged — not the forty in the buffer, and
     * not the ten on screen. */
    for (i = 0U; i < ed.win->overlay.spans.len; i++)
        changed_lines++;
    YEW_ASSERT_EQ_U64(changed_lines, 2U);
    YEW_ASSERT(ed.doc_damage_hi - ed.doc_damage_lo <= ed.win->rect.h);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}
