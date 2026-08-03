#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/select.h"

typedef struct {
    Buffer buffer;
    Win win;
} SelectFixture;

static void select_fixture_init(SelectFixture *f, const u8 *bytes, u64 len)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    (void)memset(f, 0, sizeof(*f));
    f->buffer.tb = sag_textbuf_from_bytes(bytes, len);
    f->buffer.tabwidth = 4U;
    f->win.buf = &f->buffer;
    f->win.h.kind = SAG_SEL_CHAR;
    sag_cset_init(&f->win.cs, cursor);
}

static void select_fixture_free(SelectFixture *f)
{
    sag_cset_free(&f->win.cs);
    sag_textbuf_free(f->buffer.tb);
}

static Cursor selection(u64 anchor, u64 pos)
{
    Cursor cursor = {BYTEOFF(pos), {0U}, BYTEOFF(anchor)};

    return cursor;
}

static void assert_span(Span span, u64 lo, u64 hi)
{
    SAG_ASSERT_EQ_U64(span.lo, lo);
    SAG_ASSERT_EQ_U64(span.hi, hi);
}

void test_select_char_span_is_half_open_and_direction_independent(void)
{
    static const u8 bytes[] = "a\xCC\x81\xE6\xBC\xA2z";
    SelectFixture f;
    Cursor forward = selection(0U, 6U);
    Cursor backward = selection(6U, 0U);
    Cursor caret = selection(3U, 3U);

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_CHAR;
    assert_span(sag_sel_span(&f.win, &forward), 0U, 6U);
    assert_span(sag_sel_span(&f.win, &backward), 0U, 6U);
    assert_span(sag_sel_span(&f.win, &caret), 3U, 3U);
    select_fixture_free(&f);
}

void test_select_line_span_includes_crlf_lf_and_final_line_bytes(void)
{
    static const u8 bytes[] = "aa\r\nb\nlast";
    SelectFixture f;
    Cursor first_two = selection(1U, 4U);
    Cursor through_final = selection(1U, 8U);
    Cursor final_only = selection(7U, 9U);

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_LINE;
    assert_span(sag_sel_span(&f.win, &first_two), 0U, 6U);
    assert_span(sag_sel_span(&f.win, &through_final), 0U, 10U);
    assert_span(sag_sel_span(&f.win, &final_only), 6U, 10U);
    select_fixture_free(&f);
}

void test_select_rows_counts_both_endpoint_lines_in_either_direction(void)
{
    static const u8 bytes[] = "a\nb\nc\n";
    SelectFixture f;
    Cursor forward = selection(0U, 4U);
    Cursor backward = selection(4U, 0U);

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    SAG_ASSERT_EQ_U64(sag_sel_rows(&f.win, &forward), 3U);
    SAG_ASSERT_EQ_U64(sag_sel_rows(&f.win, &backward), 3U);
    select_fixture_free(&f);
}

void test_select_rect_widens_cjk_and_tab_left_edges_and_clips_short_lines(void)
{
    static const u8 bytes[] =
        "  top\n"
        "a\xE6\xBC\xA2" "bc\n"
        "a\tbcde\n"
        "x\n"
        "  end\n";
    SelectFixture f;
    Cursor cursor = selection(2U, 27U);
    SagSelSpanVec spans = {0};
    Span row;
    CCol c0;
    CCol c1;

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_RECT;
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(1U),
                                &row, &c0, &c1));
    assert_span(row, 7U, 12U);
    SAG_ASSERT_EQ_U64(c0.v, 2U);
    SAG_ASSERT_EQ_U64(c1.v, 5U);
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(2U),
                                &row, &c0, &c1));
    assert_span(row, 14U, 16U);
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(3U),
                                &row, &c0, &c1));
    assert_span(row, 21U, 21U);

    sag_sel_rect_spans(&f.win, &cursor, &spans);
    SAG_ASSERT_EQ_U64(spans.len, 5U);
    assert_span(spans.data[1], 7U, 12U);
    assert_span(spans.data[2], 14U, 16U);
    assert_span(spans.data[3], 21U, 21U);
    SagSelSpanVec_free(&spans);
    select_fixture_free(&f);
}

void test_select_rect_widens_cjk_emoji_and_tab_right_edges(void)
{
    static const u8 bytes[] =
        "a x\n"
        "a\xE6\xBC\xA2z\n"
        "a\xF0\x9F\x98\x80z\n"
        "a\tz\n"
        "a y\n";
    SelectFixture f;
    Cursor cursor = selection(0U, 23U);
    Span row;
    CCol c0;
    CCol c1;

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_RECT;
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(1U),
                                &row, &c0, &c1));
    assert_span(row, 4U, 8U);
    SAG_ASSERT_EQ_U64(c0.v, 0U);
    SAG_ASSERT_EQ_U64(c1.v, 2U);
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(2U),
                                &row, &c0, &c1));
    assert_span(row, 10U, 15U);
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(3U),
                                &row, &c0, &c1));
    assert_span(row, 17U, 19U);
    select_fixture_free(&f);
}

void test_select_rect_zero_width_carets_land_on_whole_cluster_boundaries(void)
{
    static const u8 bytes[] =
        "  x\n"
        "a\xE6\xBC\xA2z\n"
        "a\tz\n"
        "x\n"
        "  y\n";
    SelectFixture f;
    Cursor cursor = selection(2U, 18U);
    Span row;
    CCol c0;
    CCol c1;

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_RECT;
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(1U),
                                &row, &c0, &c1));
    assert_span(row, 5U, 5U);
    SAG_ASSERT_EQ_U64(c0.v, 2U);
    SAG_ASSERT_EQ_U64(c1.v, 2U);
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(2U),
                                &row, &c0, &c1));
    assert_span(row, 11U, 11U);
    SAG_ASSERT(sag_sel_rect_row(&f.win, &cursor, LINENO(3U),
                                &row, &c0, &c1));
    assert_span(row, 15U, 15U);
    select_fixture_free(&f);
}

void test_select_rect_row_rejects_lines_outside_selection(void)
{
    static const u8 bytes[] = "aa\nbb\ncc";
    SelectFixture f;
    Cursor cursor = selection(3U, 4U);
    Span row = {99U, 99U};
    CCol c0 = {99U};
    CCol c1 = {99U};

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_RECT;
    SAG_ASSERT(!sag_sel_rect_row(&f.win, &cursor, LINENO(0U),
                                 &row, &c0, &c1));
    SAG_ASSERT(!sag_sel_rect_row(&f.win, &cursor, LINENO(2U),
                                 &row, &c0, &c1));
    select_fixture_free(&f);
}

void test_select_rect_spans_exclude_crlf_and_reach_final_unterminated_line(void)
{
    static const u8 bytes[] = " ab\r\nx\r\nyz";
    SelectFixture f;
    Cursor cursor = selection(1U, 10U);
    SagSelSpanVec spans = {0};

    select_fixture_init(&f, bytes, sizeof(bytes) - 1U);
    f.win.h.kind = SAG_SEL_RECT;
    sag_sel_rect_spans(&f.win, &cursor, &spans);
    SAG_ASSERT_EQ_U64(spans.len, 3U);
    assert_span(spans.data[0], 1U, 2U);
    assert_span(spans.data[1], 6U, 6U);
    assert_span(spans.data[2], 9U, 10U);
    SagSelSpanVec_free(&spans);
    select_fixture_free(&f);
}
