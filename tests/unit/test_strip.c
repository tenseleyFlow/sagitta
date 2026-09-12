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

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/region.h"
#include "ui/strip.h"
#include "ui/tabs.h"

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
    yew_strip_layout(e, 4, 80U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(n_spans, 4);
    YEW_ASSERT(!more_l);
    YEW_ASSERT(!more_r);
    YEW_ASSERT_EQ_U64(spans[0].col0, 0U);
    for (i = 0; i < n_spans; i++) {
        YEW_ASSERT_EQ_I64(spans[i].idx, i);
        YEW_ASSERT(spans[i].col1 > spans[i].col0);
        /* Contiguous: no gap and no overlap, so every cell of the strip
         * belongs to exactly one entry. */
        if (i > 0)
            YEW_ASSERT_EQ_U64(spans[i].col0, spans[i - 1].col1);
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
    ascii_w = yew_strip_label_cells(e[0].label);
    cjk_w = yew_strip_label_cells(e[1].label);
    YEW_ASSERT_EQ_U64(ascii_w, 2U);
    YEW_ASSERT_EQ_U64(cjk_w, 4U);
    YEW_ASSERT_EQ_U64(strlen(e[1].label), 6U);

    yew_strip_layout(e, 2, 80U, 0, &scroll, spans, &n_spans, NULL, NULL);
    YEW_ASSERT_EQ_I64(n_spans, 2);
    YEW_ASSERT_EQ_U64(spans[1].col0, 2U);
    /* Four cells wide, not six bytes wide. */
    YEW_ASSERT_EQ_U64(spans[1].col1, 6U);
}

/* Labels clip at 24 cells, so one long path cannot eat the strip. */
void test_strip_clips_long_labels(void)
{
    StripEntry e[1];

    (void)memset(e, 0, sizeof(e));
    (void)snprintf(e[0].label, sizeof(e[0].label),
                   "[1: a-very-long-file-name-that-keeps-going]");
    YEW_ASSERT_EQ_U64(yew_strip_label_cells(e[0].label),
                      YEW_STRIP_LABEL_CELLS);
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
    yew_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(n_spans, 4);
    YEW_ASSERT(!more_l);
    YEW_ASSERT(more_r);

    /* Scrolled into the middle: both sides have more. */
    scroll = 3;
    yew_strip_layout(e, 10, 40U, 3, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT(more_l);
    YEW_ASSERT(more_r);
    YEW_ASSERT_EQ_I64(spans[0].idx, 3);
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
    yew_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 0);

    /* Entry 4 is one past the edge: scrolling by one shows it. */
    yew_strip_layout(e, 10, 40U, 4, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 1);
    YEW_ASSERT_EQ_I64(spans[n_spans - 1].idx, 4);

    /* Jumping to entry 9 scrolls to show it and no further. */
    yew_strip_layout(e, 10, 40U, 9, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 6);
    YEW_ASSERT_EQ_I64(spans[n_spans - 1].idx, 9);
    YEW_ASSERT(!more_r);

    /* And back to entry 0 scrolls left to exactly 0. */
    yew_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 0);
}

/* An already-visible active entry does not move the strip at all. */
void test_strip_does_not_scroll_when_active_is_visible(void)
{
    StripEntry e[10];
    StripSpan spans[16];
    int n_spans = 0;
    int scroll = 2;

    st_fill(e, 10, "[%d: name]");
    yew_strip_layout(e, 10, 40U, 3, &scroll, spans, &n_spans, NULL, NULL);
    YEW_ASSERT_EQ_I64(scroll, 2);
    YEW_ASSERT_EQ_I64(spans[0].idx, 2);
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
    yew_strip_layout(e, 2, 6U, 0, &scroll, spans, &n_spans, NULL, NULL);
    YEW_ASSERT_EQ_I64(n_spans, 1);
    YEW_ASSERT_EQ_U64(spans[0].col0, 0U);
    YEW_ASSERT_EQ_U64(spans[0].col1, 6U);
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
    yew_strip_layout(e, 0, 80U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(n_spans, 0);
    YEW_ASSERT(!more_l);
    YEW_ASSERT(!more_r);

    n_spans = 7;
    yew_strip_layout(e, 2, 0U, 0, &scroll, spans, &n_spans, NULL, NULL);
    YEW_ASSERT_EQ_I64(n_spans, 0);
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
    size_t fit = yew_strip_label_bytes(long_label);

    YEW_ASSERT_EQ_U64(yew_strip_label_cells(long_label),
                      YEW_STRIP_LABEL_CELLS);
    /* Fewer bytes than the whole label, and exactly the clipped cells
     * worth for an ASCII label. */
    YEW_ASSERT(fit < strlen(long_label));
    YEW_ASSERT_EQ_U64(fit, YEW_STRIP_LABEL_CELLS);

    /* A short label is not clipped at all. */
    YEW_ASSERT_EQ_U64(yew_strip_label_bytes("[1: a]"), 6U);

    /* CJK: four cells is six BYTES, so a byte-count clip would cut a
     * sequence in half. */
    {
        const char *cjk = "\xE6\xBC\xA2\xE5\xAD\x97";

        YEW_ASSERT_EQ_U64(yew_strip_label_cells(cjk), 4U);
        YEW_ASSERT_EQ_U64(yew_strip_label_bytes(cjk), 6U);
    }
}

/* ---------------------------------------------------------------- */
/* Sprint 57.15 §1: whose offset is this                            */
/* ---------------------------------------------------------------- */

/*
 * THE REGRESSION THIS SPRINT EXISTS FOR, at the engine.
 *
 * `*scroll` is written back, so the follow-the-active walk used to
 * overwrite an offset the user had just chosen — on the very next
 * render.  The chevron therefore appeared to do nothing, and appeared
 * to work only when the active entry already sat beside it.
 *
 * `active < 0` is how the caller says "the offset is the user's".  The
 * engine leaves it alone, and reports the overflow honestly so the
 * chevrons still draw.
 */
void test_strip_user_owned_offset_survives_an_offscreen_active(void)
{
    StripEntry e[10];
    StripSpan spans[16];
    int n_spans = 0;
    int scroll = 0;
    bool more_l = false;
    bool more_r = false;

    st_fill(e, 10, "[%d: name]");
    /* Entry 0 is active and visible; the user scrolls one right. */
    scroll = 1;
    yew_strip_layout(e, 10, 40U, -1, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 1);
    YEW_ASSERT_EQ_I64(spans[0].idx, 1);
    YEW_ASSERT(more_l);
    YEW_ASSERT(more_r);

    /* Rendering again changes nothing — the frame before is the frame
     * the bug ate. */
    yew_strip_layout(e, 10, 40U, -1, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 1);

    /* All the way to the far end, with entry 0 long gone.  An
     * off-screen active entry is a legitimate view. */
    scroll = 6;
    yew_strip_layout(e, 10, 40U, -1, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 6);
    YEW_ASSERT_EQ_I64(spans[0].idx, 6);
    YEW_ASSERT(!more_r);

    /* Following again reveals it, by exactly the minimal walk. */
    yew_strip_layout(e, 10, 40U, 0, &scroll, spans, &n_spans, &more_l,
                     &more_r);
    YEW_ASSERT_EQ_I64(scroll, 0);
    YEW_ASSERT_EQ_I64(spans[0].idx, 0);
}

/*
 * Not following is not the same as not clamping.  An offset past the
 * end of a list that shrank under it is not a view anybody chose, so it
 * still comes back into range — and the strip still shows something.
 */
void test_strip_user_owned_offset_is_still_clamped_into_range(void)
{
    StripEntry e[10];
    StripSpan spans[16];
    int n_spans = 0;
    int scroll = 9;

    st_fill(e, 10, "[%d: name]");
    /* The list shrinks to three entries with the offset out past them. */
    yew_strip_layout(e, 3, 40U, -1, &scroll, spans, &n_spans, NULL, NULL);
    YEW_ASSERT_EQ_I64(scroll, 2);
    YEW_ASSERT_EQ_I64(n_spans, 1);
    YEW_ASSERT_EQ_I64(spans[0].idx, 2);

    scroll = -4;
    yew_strip_layout(e, 3, 40U, -1, &scroll, spans, &n_spans, NULL, NULL);
    YEW_ASSERT_EQ_I64(scroll, 0);
    YEW_ASSERT_EQ_I64(spans[0].idx, 0);
}

/*
 * The same law one level up, where the flag actually lives.
 *
 * A narrow grid so both chevrons exist, and the render is the whole
 * test: the bug was that a render UNDID the scroll, so nothing short of
 * drawing a frame can prove it does not.
 */
typedef struct StFixture {
    Ed ed;
} StFixture;

static void st_fixture(StFixture *f, u32 extra_tabs, u16 cols)
{
    u32 i;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_grid_init(&f->ed.grid, &f->ed.interner, 24U, cols));
    f->ed.grid_ready = true;
    for (i = 0U; i < extra_tabs; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-stripscroll-%u.txt",
                       (unsigned)i);
        YEW_ASSERT(yew_tab_open(&f->ed, path) >= 0);
    }
    yew_ed_layout(&f->ed);
    f->ed.now_ms = 1000;
}

static void st_paint(StFixture *f)
{
    if (f->ed.layout_dirty)
        yew_ed_layout(&f->ed);
    yew_region_frame_begin();
    yew_tab_strip_draw(&f->ed, f->ed.tab_strip_rect);
}

void test_strip_explicit_row1_offset_survives_the_render(void)
{
    StFixture f;

    st_fixture(&f, 7U, 40U);
    yew_tab_switch(&f.ed, 0);
    st_paint(&f);
    /* Following: entry 0 is active, so the strip sits at the left. */
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 0);
    YEW_ASSERT(!f.ed.tabs.scroll_user);

    /* The user scrolls.  Without the flag this lasted one frame. */
    f.ed.tabs.scroll = 3;
    yew_tabs_scroll_owned(&f.ed.tabs, false);
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 3);
    /* And a second frame, because the clamp ran once per render. */
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 3);
    /* The active entry is genuinely off-screen and that is allowed. */
    YEW_ASSERT_EQ_I64(f.ed.tabs.active, 0);

    /* Changing the active entry hands the strip back to the follow. */
    yew_tab_switch(&f.ed, 0);
    YEW_ASSERT(!f.ed.tabs.scroll_user);
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 0);
    yew_ed_free(&f.ed);
}

/*
 * s24 made the two rows' offsets independent and this sprint makes
 * their OWNERSHIP independent for the same reason: scrolling row 2
 * must not hand row 1 back to the follow, or the member strip's
 * chevron would move the row above it.
 */
void test_strip_rows_own_their_offsets_independently(void)
{
    StFixture f;
    u32 g;

    st_fixture(&f, 7U, 40U);
    g = yew_group_create(&f.ed, "/src", NULL);
    yew_group_add_member(&f.ed, g, 2);
    yew_group_add_member(&f.ed, g, 3);
    yew_group_add_member(&f.ed, g, 4);
    yew_group_add_member(&f.ed, g, 5);
    yew_tab_switch(&f.ed, 2);
    st_paint(&f);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&f.ed), g);

    yew_tabs_scroll_owned(&f.ed.tabs, true);
    f.ed.tabs.member_scroll = 2;
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.member_scroll, 2);
    /* Row 1 never became the user's, so it still follows. */
    YEW_ASSERT(!f.ed.tabs.scroll_user);
    YEW_ASSERT(f.ed.tabs.member_scroll_user);

    /* And the other way round. */
    yew_tabs_follow_active(&f.ed.tabs);
    f.ed.tabs.member_scroll = 0;
    yew_tabs_scroll_owned(&f.ed.tabs, false);
    f.ed.tabs.scroll = 2;
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 2);
    YEW_ASSERT(!f.ed.tabs.member_scroll_user);
    YEW_ASSERT_EQ_I64(f.ed.tabs.member_scroll, 0);
    yew_ed_free(&f.ed);
}

/*
 * A switch is not the only thing that moves the active entry: a CLOSE
 * renumbers every index above it and an OPEN grows the list.  Both must
 * resume the follow, or a strip left scrolled would keep showing a
 * window of entries that no longer means what it did.
 */
void test_strip_close_and_open_resume_following(void)
{
    StFixture f;

    st_fixture(&f, 7U, 40U);
    yew_tab_switch(&f.ed, 0);
    yew_tabs_scroll_owned(&f.ed.tabs, false);
    f.ed.tabs.scroll = 4;
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 4);

    YEW_ASSERT(yew_tab_close(&f.ed, 7));
    YEW_ASSERT(!f.ed.tabs.scroll_user);
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 0);

    yew_tabs_scroll_owned(&f.ed.tabs, false);
    f.ed.tabs.scroll = 3;
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 3);
    YEW_ASSERT(yew_tab_open(&f.ed, "/tmp/yew-stripscroll-new.txt") >= 0);
    YEW_ASSERT(!f.ed.tabs.scroll_user);
    st_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 0);
    yew_ed_free(&f.ed);
}
