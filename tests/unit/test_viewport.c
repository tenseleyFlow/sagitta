#include "unit/harness.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/layout.h"
#include "ui/viewport.h"

typedef struct {
    Buffer buffer;
    Win win;
} VpFixture;

static Cursor vp_test_cursor(u64 off)
{
    Cursor c;
    c.pos = BYTEOFF(off);
    c.anchor = c.pos;
    c.goal_col = (GCol){0U};
    return c;
}

static void vp_fixture_init(VpFixture *f, const char *text, u16 rows,
                            u16 cols)
{
    memset(f, 0, sizeof(*f));
    f->buffer.tb = yew_textbuf_from_bytes((const u8 *)text, strlen(text));
    f->win.buf = &f->buffer;
    yew_cset_init(&f->win.cs, vp_test_cursor(0U));
    yew_vp_init(&f->win);
    YEW_ASSERT_EQ_U64(f->win.number_style, YEW_NUM_HYBRID);
    f->win.vp.rows = rows;
    f->win.vp.cols = cols;
}

static void vp_fixture_free(VpFixture *f)
{
    yew_vp_free(&f->win);
    yew_cset_free(&f->win.cs);
    yew_textbuf_free(f->buffer.tb);
}

static void vp_set_line(VpFixture *f, u64 line)
{
    Cursor *c = &f->win.cs.curs.data[0];
    c->pos = yew_textbuf_line_start(f->buffer.tb, LINENO(line));
    c->anchor = c->pos;
}

static char *vp_make_lines(size_t count, size_t line_cells)
{
    size_t line_bytes = line_cells + 1U;
    size_t len = count * line_bytes;
    char *text = yew_xmalloc(len + 1U);
    size_t line;

    for (line = 0U; line < count; line++) {
        (void)memset(text + line * line_bytes, 'x', line_cells);
        text[line * line_bytes + line_cells] = '\n';
    }
    text[len - 1U] = 'x';
    text[len] = '\0';
    return text;
}

void test_viewport_follow_vertical_rules(void)
{
    VpFixture f;
    u64 expected[] = {0U, 0U, 0U, 0U, 1U, 2U, 3U, 4U, 5U, 5U};
    size_t i;

    vp_fixture_init(&f, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9", 5U, 20U);
    f.win.vp.scrolloff = 1U;
    for (i = 0U; i < YEW_ARRAY_LEN(expected); i++) {
        vp_set_line(&f, i);
        yew_vp_follow(&f.win);
        YEW_ASSERT_EQ_U64(f.win.vp.top.v, expected[i]);
    }
    vp_fixture_free(&f);
}

void test_viewport_short_document_and_scrolloff_degrade(void)
{
    VpFixture f;
    u64 line;

    vp_fixture_init(&f, "a\nb\nc\nd\ne", 24U, 20U);
    for (line = 0U; line < 5U; line++) {
        vp_set_line(&f, line);
        yew_vp_follow(&f.win);
        YEW_ASSERT_EQ_U64(f.win.vp.top.v, 0U);
    }
    f.win.vp.rows = 3U;
    f.win.vp.scrolloff = 9U;
    vp_set_line(&f, 3U);
    yew_vp_follow(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 2U);
    vp_fixture_free(&f);
}

void test_viewport_horizontal_follow_wide_glyph(void)
{
    static const char text[] = "abcdef\xE6\xBC\xA2z";
    VpFixture f;
    Cursor *c;

    vp_fixture_init(&f, text, 4U, 8U);
    f.win.vp.sidescrolloff = 0U;
    c = &f.win.cs.curs.data[0];
    c->pos = BYTEOFF(6U);
    c->anchor = c->pos;
    yew_vp_follow(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.left.v, 0U);
    f.win.vp.cols = 7U;
    yew_vp_follow(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.left.v, 1U);
    c->pos = BYTEOFF(0U);
    c->anchor = c->pos;
    yew_vp_follow(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.left.v, 0U);
    vp_fixture_free(&f);
}

void test_viewport_large_utf8_follow_keeps_deferred_index(void)
{
    const size_t len = 17U * 1024U * 1024U;
    u8 *bytes = yew_xmalloc(len);
    VpFixture f;
    Span row;

    (void)memset(bytes, 'x', len);
    bytes[0] = 0xE4U;
    bytes[1] = 0xB8U;
    bytes[2] = 0xADU;
    bytes[3] = '\n';
    bytes[4] = 0xE4U;
    bytes[5] = 0xB8U;
    bytes[6] = 0xADU;
    bytes[7] = '\r';
    bytes[8] = '\n';

    memset(&f, 0, sizeof(f));
    f.buffer.tb = yew_textbuf_from_owned_bytes_simple(bytes, len, false);
    YEW_ASSERT_NOT_NULL(f.buffer.tb);
    f.win.buf = &f.buffer;
    yew_cset_init(&f.win.cs, vp_test_cursor(0U));
    yew_vp_init(&f.win);
    f.win.vp.rows = 8U;
    f.win.vp.cols = 80U;
    YEW_ASSERT(!f.buffer.tb->graphemes.initialized);

    yew_vp_follow(&f.win);
    YEW_ASSERT(!f.buffer.tb->graphemes.initialized);
    row = yew_wrap_row(&f.win, LINENO(0U), 0U);
    YEW_ASSERT_EQ_U64(row.lo, 0U);
    YEW_ASSERT_EQ_U64(row.hi, 3U);
    row = yew_wrap_row(&f.win, LINENO(1U), 0U);
    YEW_ASSERT_EQ_U64(row.lo, 4U);
    YEW_ASSERT_EQ_U64(row.hi, 7U);
    YEW_ASSERT(!f.buffer.tb->graphemes.initialized);
    vp_fixture_free(&f);
}

void test_viewport_wrap_forces_left_and_conversions(void)
{
    VpFixture f;
    u16 row;
    LineNo line;
    u32 sub;

    vp_fixture_init(&f, "abcdef\nghijkl", 4U, 3U);
    f.win.vp.wrap = true;
    f.win.vp.left = (CCol){99U};
    yew_vp_follow(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.left.v, 0U);
    YEW_ASSERT(yew_vp_row_of_line(&f.win, LINENO(1U), 0U, &row));
    YEW_ASSERT_EQ_U64(row, 2U);
    YEW_ASSERT(yew_vp_line_of_row(&f.win, 3U, &line, &sub));
    YEW_ASSERT_EQ_U64(line.v, 1U);
    YEW_ASSERT_EQ_U64(sub, 1U);
    /*
     * The two conversions are absolute, and their origin is
     * `rect.x` — which yew_layout_win sets to leaf->rect.x + gutter, so
     * the gutter is already inside it.  This fixture sets rect.x the
     * same way rather than leaving it zero: with a zero origin the two
     * possible implementations agree, which is exactly how a
     * gutter-relative version survived from Sprint 14 until panes gave
     * a window a non-zero x and the cursor vanished from every split.
     */
    f.win.gutter_width = 6U;
    f.win.rect = (Rect){6U, 0U, 3U, 4U};
    YEW_ASSERT_EQ_U64(yew_vp_gridx_of_ccol(&f.win, (CCol){2U}), 8U);
    YEW_ASSERT_EQ_U64(yew_vp_ccol_of_gridx(&f.win, 9U).v, 3U);

    /* And with the window somewhere other than column 0, which is the
     * case that was broken: a pane at x=47 puts content column 2 at
     * absolute 49, and a click at 49 comes back as column 2. */
    f.win.rect = (Rect){47U, 0U, 33U, 4U};
    YEW_ASSERT_EQ_U64(yew_vp_gridx_of_ccol(&f.win, (CCol){2U}), 49U);
    YEW_ASSERT_EQ_U64(yew_vp_ccol_of_gridx(&f.win, 49U).v, 2U);
    /*
     * Round trip across the window's own columns.  Bounded by vp.cols
     * rather than by the screen: past the last visible cell a WRAPPED
     * window deliberately projects onto it, so a wider sweep would be
     * asserting that the clamp does not happen.
     */
    {
        u16 gx;

        for (gx = 48U; gx < (u16)(47U + f.win.vp.cols); gx++) {
            CCol c = yew_vp_ccol_of_gridx(&f.win, gx);

            YEW_ASSERT_EQ_U64(yew_vp_gridx_of_ccol(&f.win, c), gx);
        }
    }
    vp_fixture_free(&f);
}

void test_viewport_scroll_page_center_top_bottom_and_clamp(void)
{
    VpFixture f;
    ByteOff delete_from;

    vp_fixture_init(&f, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9", 5U, 20U);
    yew_vp_scroll(&f.win, 100);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 5U);
    yew_vp_scroll(&f.win, -100);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 0U);
    yew_vp_page(&f.win, 1);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 3U);
    vp_set_line(&f, 7U);
    yew_vp_center(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 5U);
    yew_vp_top(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 7U);
    vp_set_line(&f, 3U);
    yew_vp_bottom(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 0U);
    f.win.vp.top = LINENO(99U);
    yew_vp_clamp(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 5U);
    delete_from = yew_textbuf_line_start(f.buffer.tb, LINENO(4U));
    yew_textbuf_delete(f.buffer.tb,
                       (Span){delete_from.v, yew_textbuf_len(f.buffer.tb)});
    yew_vp_invalidate_from(&f.win, LINENO(4U));
    yew_vp_clamp(&f.win);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(f.buffer.tb), 5U);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 0U);
    vp_fixture_free(&f);
}

void test_viewport_display_row_motion(void)
{
    VpFixture f;
    Cursor *c;

    vp_fixture_init(&f, "abcdef\nxy", 5U, 3U);
    f.win.vp.wrap = true;
    c = &f.win.cs.curs.data[0];
    c->pos = BYTEOFF(1U);
    c->anchor = c->pos;
    YEW_ASSERT(yew_vp_move_display(&f.win, 1));
    YEW_ASSERT_EQ_U64(c->pos.v, 4U);
    YEW_ASSERT(yew_vp_move_display(&f.win, 1));
    YEW_ASSERT_EQ_U64(c->pos.v, 8U);
    YEW_ASSERT(yew_vp_move_display(&f.win, -2));
    YEW_ASSERT_EQ_U64(c->pos.v, 1U);
    vp_fixture_free(&f);
}

void test_viewport_display_col_obeys_wrap_mode(void)
{
    VpFixture f;

    vp_fixture_init(&f, "abcdef", 5U, 3U);
    f.win.vp.wrap = false;
    YEW_ASSERT_EQ_U64(yew_vp_display_col(&f.win, BYTEOFF(6U)).v, 6U);
    f.win.vp.wrap = true;
    YEW_ASSERT_EQ_U64(yew_vp_display_col(&f.win, BYTEOFF(6U)).v, 3U);
    vp_fixture_free(&f);
}

void test_viewport_display_motion_stays_on_short_and_tab_rows(void)
{
    VpFixture f;
    Cursor *c;

    vp_fixture_init(&f, "abcdef\na-bbbbb", 6U, 4U);
    f.win.vp.wrap = true;
    c = &f.win.cs.curs.data[0];
    c->pos = BYTEOFF(5U);
    c->anchor = c->pos;
    YEW_ASSERT(yew_vp_move_display(&f.win, 1));
    YEW_ASSERT_EQ_U64(c->pos.v, 8U);
    YEW_ASSERT_EQ_U64(yew_vp_cursor_subrow(&f.win), 0U);
    vp_fixture_free(&f);

    vp_fixture_init(&f, "abc\tz", 5U, 3U);
    f.win.vp.wrap = true;
    c = &f.win.cs.curs.data[0];
    c->pos = BYTEOFF(2U);
    c->anchor = c->pos;
    YEW_ASSERT(yew_vp_move_display(&f.win, 1));
    YEW_ASSERT_EQ_U64(c->pos.v, 3U);
    YEW_ASSERT_EQ_U64(yew_vp_cursor_subrow(&f.win), 1U);
    vp_fixture_free(&f);
}

void test_viewport_cursor_push_preserves_nowrap_goal(void)
{
    VpFixture f;
    Cursor *c;

    vp_fixture_init(&f, "abcdef\nx\nabcdef", 1U, 20U);
    c = &f.win.cs.curs.data[0];
    c->pos = BYTEOFF(5U);
    c->anchor = c->pos;
    c->goal_col = (GCol){5U};
    f.win.vp.top = LINENO(1U);
    yew_vp_push_cursor(&f.win);
    YEW_ASSERT_EQ_U64(c->pos.v, 8U);
    YEW_ASSERT_EQ_U64(c->goal_col.v, 5U);
    f.win.vp.top = LINENO(2U);
    yew_vp_push_cursor(&f.win);
    YEW_ASSERT_EQ_U64(c->pos.v, 14U);
    YEW_ASSERT_EQ_U64(c->goal_col.v, 5U);
    vp_fixture_free(&f);
}

void test_viewport_scroll_pushes_cursor_without_follow_snapback(void)
{
    VpFixture f;

    vp_fixture_init(&f, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9", 5U, 20U);
    f.win.vp.scrolloff = 1U;
    vp_set_line(&f, 1U);
    yew_vp_scroll(&f.win, 2);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 2U);
    yew_vp_push_cursor(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 2U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(f.buffer.tb,
                                           f.win.cs.curs.data[0].pos).v,
                      3U);
    yew_vp_scroll(&f.win, -2);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 0U);
    yew_vp_push_cursor(&f.win);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, 0U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(f.buffer.tb,
                                           f.win.cs.curs.data[0].pos).v,
                      3U);
    vp_fixture_free(&f);
}

void test_viewport_resize_roundtrip_and_gutter_digit_damage(void)
{
    VpFixture f;
    Viewport before;
    Ed ed;
    char *text = vp_make_lines(999U, 20U);
    Cursor *cursor;
    ByteOff end;

    vp_fixture_init(&f, text, 23U, 74U);
    free(text);
    memset(&ed, 0, sizeof(ed));
    arena_init(&ed.arena);
    interner_init(&ed.interner, &ed.arena);
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 24U, 80U));
    ed.win = &f.win;
    f.win.number_style = YEW_NUM_HYBRID;
    cursor = &f.win.cs.curs.data[0];
    cursor->pos = BYTEOFF(
        yew_textbuf_line_start(f.buffer.tb, LINENO(35U)).v + 10U);
    cursor->anchor = cursor->pos;
    f.win.vp.top = LINENO(30U);
    f.win.vp.left = (CCol){5U};
    f.win.vp.scrolloff = 3U;
    f.win.vp.sidescrolloff = 1U;
    yew_layout(&ed);
    before = f.win.vp;

    YEW_ASSERT(yew_grid_resize(&ed.grid, 12U, 40U));
    yew_layout(&ed);
    YEW_ASSERT(yew_grid_resize(&ed.grid, 24U, 80U));
    yew_layout(&ed);
    YEW_ASSERT_EQ_U64(f.win.vp.top.v, before.top.v);
    YEW_ASSERT_EQ_U64(f.win.vp.top_sub, before.top_sub);
    YEW_ASSERT_EQ_U64(f.win.vp.left.v, before.left.v);
    YEW_ASSERT_EQ_U64(f.win.vp.rows, before.rows);
    YEW_ASSERT_EQ_U64(f.win.vp.cols, before.cols);
    YEW_ASSERT_EQ_U64(f.win.vp.scrolloff, before.scrolloff);
    YEW_ASSERT_EQ_U64(f.win.vp.sidescrolloff, before.sidescrolloff);
    YEW_ASSERT(f.win.vp.wrap == before.wrap);

    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(f.buffer.tb), 999U);
    YEW_ASSERT_EQ_U64(f.win.gutter_width, 6U);
    ed.full_damage = false;
    end = BYTEOFF(yew_textbuf_len(f.buffer.tb));
    yew_textbuf_insert(f.buffer.tb, end, (const u8 *)"\nx", 2U);
    yew_ed_damage_line(&ed, LINENO(998U), true);
    yew_layout(&ed);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(f.buffer.tb), 1000U);
    YEW_ASSERT_EQ_U64(f.win.gutter_width, 7U);
    YEW_ASSERT(ed.full_damage);
    ed.full_damage = false;
    yew_layout(&ed);
    YEW_ASSERT(!ed.full_damage);

    yew_grid_free(&ed.grid);
    interner_free(&ed.interner);
    arena_free_all(&ed.arena);
    vp_fixture_free(&f);
}

void test_viewport_relative_cursor_render_touches_only_gutter(void)
{
    static const u8 text[] = "alpha\nbeta\ngamma\n";
    Ed ed;
    TtyCaps caps = {0};
    Cursor *cursor;
    Cell *content;
    size_t at;
    int sink;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "relative-gutter.txt"));
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 8U, 40U));
    ed.grid_ready = true;
    yew_render_init(&ed.render, &caps, NULL);
    ed.render_ready = true;
    yew_ed_layout(&ed);
    sink = open("/dev/null", O_WRONLY);
    YEW_ASSERT(sink >= 0);
    ed.tty.wfd = sink;

    yew_ed_render(&ed);
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT(ed.drawn_cursor_line_valid);
    YEW_ASSERT(ed.win->gutter_width != 0U);
    YEW_ASSERT(ed.win->rect.w > 1U);
    at = (size_t)ed.win->rect.y * ed.grid.cols + ed.win->rect.x + 1U;
    content = &ed.grid.back[at];
    YEW_ASSERT_EQ_U64(content->utf8[0], (u8)'l');

    /* Leave an unmarked sentinel in a document cell.  A cursor-line change
     * may repaint the relative-number gutter and footer, but must not revisit
     * document text while the viewport itself remains stable. */
    content->utf8[0] = (u8)'#';
    cursor = yew_ed_cursor(&ed);
    cursor->pos = yew_textbuf_line_start(ed.buffer.tb, LINENO(1U));
    cursor->anchor = cursor->pos;
    ed.cursor_follow_pending = true;
    yew_ed_render(&ed);
    YEW_ASSERT_EQ_U64(ed.drawn_cursor_line.v, 1U);
    YEW_ASSERT_EQ_U64(content->utf8[0], (u8)'#');

    YEW_ASSERT(close(sink) == 0);
    ed.tty.wfd = -1;
    yew_ed_free(&ed);
}
