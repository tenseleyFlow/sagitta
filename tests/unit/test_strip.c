/*
 * Sprint 23 §3: the strip layout engine.
 *
 * The engine has one job and two ways to get it wrong: measure labels
 * with strlen (so a CJK tab name shifts every span to its right), or
 * scroll further than needed (so the strip jumps under the user).  Both
 * get explicit rows.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "ui/strip.h"

static void st_fill(StripEntry *e, int n, const char *fmt)
{
    int i;

    for (i = 0; i < n; i++) {
        (void)memset(&e[i], 0, sizeof(e[i]));
        (void)snprintf(e[i].label, sizeof(e[i].label), fmt, i);
        e[i].payload = i;
    }
}

void test_strip_lays_entries_left_to_right_without_gaps(void)
{
    StripEntry e[4];
    StripSpan spans[8];
    int n_spans = 0;
    int scroll = 0;
    bool more_l = true;
    bool more_r = true;
    int i;

    st_fill(e, 4, "[%d: a]");
    sag_strip_layout(e, 4, 80U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(n_spans, 4);
    SAG_ASSERT(!more_l);
    SAG_ASSERT(!more_r);
    SAG_ASSERT_EQ_U64(spans[0].col0, 0U);
    for (i = 0; i < n_spans; i++) {
        SAG_ASSERT_EQ_I64(spans[i].idx, i);
        SAG_ASSERT(spans[i].col1 > spans[i].col0);
        /* Contiguous: no gap and no overlap, so every cell of the strip
         * belongs to exactly one entry. */
        if (i > 0)
            SAG_ASSERT_EQ_U64(spans[i].col0, spans[i - 1].col1);
    }
}

/* Spans are measured in CELLS.  A CJK label is two cells per glyph, and
 * a strlen-based engine would place the next span six cells early. */
void test_strip_measures_cjk_labels_in_cells(void)
{
    StripEntry e[2];
    StripSpan spans[4];
    int n_spans = 0;
    int scroll = 0;
    u16 ascii_w;
    u16 cjk_w;

    (void)memset(e, 0, sizeof(e));
    /* "ab" is 2 cells; the two ideographs are 4 cells but 6 bytes. */
    (void)snprintf(e[0].label, sizeof(e[0].label), "ab");
    (void)snprintf(e[1].label, sizeof(e[1].label),
                   "\xE6\xBC\xA2\xE5\xAD\x97");
    ascii_w = sag_strip_label_cells(e[0].label);
    cjk_w = sag_strip_label_cells(e[1].label);
    SAG_ASSERT_EQ_U64(ascii_w, 2U);
    SAG_ASSERT_EQ_U64(cjk_w, 4U);
    SAG_ASSERT_EQ_U64(strlen(e[1].label), 6U);

    sag_strip_layout(e, 2, 80U, 0, &scroll, spans, &n_spans, NULL, NULL);
    SAG_ASSERT_EQ_I64(n_spans, 2);
    SAG_ASSERT_EQ_U64(spans[1].col0, 2U);
    /* Four cells wide, not six bytes wide. */
    SAG_ASSERT_EQ_U64(spans[1].col1, 6U);
}

/* Labels clip at 24 cells, so one long path cannot eat the strip. */
void test_strip_clips_long_labels(void)
{
    StripEntry e[1];

    (void)memset(e, 0, sizeof(e));
    (void)snprintf(e[0].label, sizeof(e[0].label),
                   "[1: a-very-long-file-name-that-keeps-going]");
    SAG_ASSERT_EQ_U64(sag_strip_label_cells(e[0].label),
                      SAG_STRIP_LABEL_CELLS);
}

/*
 * Overflow: entries past the edge are reported, not silently dropped,
 * so the renderer can draw `<` and `>N`.
 */
void test_strip_reports_overflow_on_both_sides(void)
{
    StripEntry e[10];
    StripSpan spans[16];
    int n_spans = 0;
    int scroll = 0;
    bool more_l = false;
    bool more_r = false;

    st_fill(e, 10, "[%d: name]");
    /* Each label is 10 cells; 40 columns holds four. */
    sag_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(n_spans, 4);
    SAG_ASSERT(!more_l);
    SAG_ASSERT(more_r);

    /* Scrolled into the middle: both sides have more. */
    scroll = 3;
    sag_strip_layout(e, 10, 40U, 3, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT(more_l);
    SAG_ASSERT(more_r);
    SAG_ASSERT_EQ_I64(spans[0].idx, 3);
}

/*
 * Scroll minimality: an active entry just past the right edge scrolls
 * by EXACTLY enough to show it.  Scrolling further is not wrong on
 * screen, it just moves entries the user was reading.
 */
void test_strip_scrolls_by_exactly_enough(void)
{
    StripEntry e[10];
    StripSpan spans[16];
    int n_spans = 0;
    int scroll = 0;
    bool more_l = false;
    bool more_r = false;

    st_fill(e, 10, "[%d: name]");
    /* 40 cells holds four 10-cell entries: 0..3 visible. */
    sag_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(scroll, 0);

    /* Entry 4 is one past the edge: scrolling by one shows it. */
    sag_strip_layout(e, 10, 40U, 4, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(scroll, 1);
    SAG_ASSERT_EQ_I64(spans[n_spans - 1].idx, 4);

    /* Jumping to entry 9 scrolls to show it and no further. */
    sag_strip_layout(e, 10, 40U, 9, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(scroll, 6);
    SAG_ASSERT_EQ_I64(spans[n_spans - 1].idx, 9);
    SAG_ASSERT(!more_r);

    /* And back to entry 0 scrolls left to exactly 0. */
    sag_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(scroll, 0);
}

/* An already-visible active entry does not move the strip at all. */
void test_strip_does_not_scroll_when_active_is_visible(void)
{
    StripEntry e[10];
    StripSpan spans[16];
    int n_spans = 0;
    int scroll = 2;

    st_fill(e, 10, "[%d: name]");
    sag_strip_layout(e, 10, 40U, 3, &scroll, spans, &n_spans, NULL, NULL);
    SAG_ASSERT_EQ_I64(scroll, 2);
    SAG_ASSERT_EQ_I64(spans[0].idx, 2);
}

/* An entry wider than the whole strip still places, clipped — placing
 * nothing would leave the strip blank and the caller spinning. */
void test_strip_places_an_oversized_entry_clipped(void)
{
    StripEntry e[2];
    StripSpan spans[4];
    int n_spans = 0;
    int scroll = 0;

    (void)memset(e, 0, sizeof(e));
    (void)snprintf(e[0].label, sizeof(e[0].label), "[0: aaaaaaaaaaaa]");
    (void)snprintf(e[1].label, sizeof(e[1].label), "[1: b]");
    sag_strip_layout(e, 2, 6U, 0, &scroll, spans, &n_spans, NULL, NULL);
    SAG_ASSERT_EQ_I64(n_spans, 1);
    SAG_ASSERT_EQ_U64(spans[0].col0, 0U);
    SAG_ASSERT_EQ_U64(spans[0].col1, 6U);
}

/* Degenerate inputs answer with nothing rather than reading past an
 * array. */
void test_strip_handles_empty_and_zero_width(void)
{
    StripEntry e[2];
    StripSpan spans[4];
    int n_spans = 7;
    int scroll = 0;
    bool more_l = true;
    bool more_r = true;

    st_fill(e, 2, "[%d]");
    sag_strip_layout(e, 0, 80U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    SAG_ASSERT_EQ_I64(n_spans, 0);
    SAG_ASSERT(!more_l);
    SAG_ASSERT(!more_r);

    n_spans = 7;
    sag_strip_layout(e, 2, 0U, 0, &scroll, spans, &n_spans, NULL, NULL);
    SAG_ASSERT_EQ_I64(n_spans, 0);
}

/*
 * A clipped label must report the BYTES that fit, not just the cells.
 *
 * Measuring the clipped width and then drawing the whole string writes
 * the tail past the span, over whatever the layout placed next — the
 * tab strip golden caught exactly that, as `[2: b.txt]do.txt]`.
 */
void test_strip_label_bytes_match_the_clipped_cells(void)
{
    static const char long_label[] =
        "[1: a-name-long-enough-to-be-clipped.txt]";
    size_t fit = sag_strip_label_bytes(long_label);

    SAG_ASSERT_EQ_U64(sag_strip_label_cells(long_label),
                      SAG_STRIP_LABEL_CELLS);
    /* Fewer bytes than the whole label, and exactly the clipped cells
     * worth for an ASCII label. */
    SAG_ASSERT(fit < strlen(long_label));
    SAG_ASSERT_EQ_U64(fit, SAG_STRIP_LABEL_CELLS);

    /* A short label is not clipped at all. */
    SAG_ASSERT_EQ_U64(sag_strip_label_bytes("[1: a]"), 6U);

    /* CJK: four cells is six BYTES, so a byte-count clip would cut a
     * sequence in half. */
    {
        const char *cjk = "\xE6\xBC\xA2\xE5\xAD\x97";

        SAG_ASSERT_EQ_U64(sag_strip_label_cells(cjk), 4U);
        SAG_ASSERT_EQ_U64(sag_strip_label_bytes(cjk), 6U);
    }
}
