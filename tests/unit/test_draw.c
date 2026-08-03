#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/sel_actions.h"
#include "edit/select.h"
#include "ui/draw.h"
#include "ui/viewport.h"

static Bytebuf draw_materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (sag_textiter_begin(&it, tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    return out;
}

static void draw_append_outside_spans(Bytebuf *out, const Bytebuf *before,
                                      const SagSelSpanVec *spans)
{
    u64 at = 0U;
    size_t i;

    for (i = 0U; i < spans->len; i++) {
        Span span = spans->data[i];

        SAG_ASSERT(span.lo >= at);
        SAG_ASSERT(span.hi <= before->len);
        bytebuf_append(out, before->data + at, (size_t)(span.lo - at));
        at = span.hi;
    }
    bytebuf_append(out, before->data + at, before->len - (size_t)at);
}

static size_t draw_count_bytes(const Bytebuf *out, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t count = 0U;
    size_t i;

    for (i = 0U; needle_len != 0U && i + needle_len <= out->len; i++) {
        if (memcmp(out->data + i, needle, needle_len) == 0)
            count++;
    }
    return count;
}

void test_draw_selection_then_secondary_cursor_preserves_glyphs(void)
{
    static const u8 text[] = "a\xE6\xBC\xA2" "b\n\ty\n";
    Ed ed;
    Buffer buffer;
    Win win;
    Cursor primary = {BYTEOFF(4U), {0U}, BYTEOFF(0U)};
    Cursor secondary = {BYTEOFF(7U), {0U}, BYTEOFF(7U)};
    const Cell *han;
    const Cell *tail;
    const Cell *painted;

    (void)memset(&ed, 0, sizeof(ed));
    (void)memset(&buffer, 0, sizeof(buffer));
    (void)memset(&win, 0, sizeof(win));
    arena_init(&ed.arena);
    interner_init(&ed.interner, &ed.arena);
    SAG_ASSERT(sag_grid_init(&ed.grid, &ed.interner, 3U, 12U));
    buffer.tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    buffer.tabwidth = 8U;
    win.buf = &buffer;
    win.h.kind = SAG_SEL_CHAR;
    win.rect = (Rect){0U, 0U, 12U, 2U};
    sag_cset_init(&win.cs, primary);
    SAG_ASSERT(sag_cset_add(&win.cs, secondary));
    sag_vp_init(&win);
    win.vp.rows = 2U;
    win.vp.cols = 12U;
    ed.mode = SAG_MODE_H;
    ed.render.tier = SAG_RENDER_TIER_16;

    sag_draw_document_rows(&ed, &win, 0U, 2U);
    han = &ed.grid.back[1U];
    tail = &ed.grid.back[2U];
    painted = &ed.grid.back[12U + 8U];
    SAG_ASSERT_EQ_MEM(han->utf8, text + 1U, 3U);
    SAG_ASSERT_EQ_U64(han->w, 2U);
    SAG_ASSERT_EQ_U64(tail->w, 0U);
    SAG_ASSERT_EQ_U64(han->bg.tag, SAG_COLOR_RGB);
    SAG_ASSERT_EQ_MEM(&han->bg, &tail->bg, sizeof(han->bg));
    SAG_ASSERT_EQ_U64(painted->utf8[0], (u8)'y');
    SAG_ASSERT((painted->attrs & SAG_ATTR_REVERSE) != 0U);

    sag_draw_cursor(&ed, &win);
    SAG_ASSERT_EQ_U64(ed.grid.cur_row, 0U);
    SAG_ASSERT_EQ_U64(ed.grid.cur_col, 3U);
    SAG_ASSERT(ed.grid.cur_vis);
    SAG_ASSERT_EQ_U64(ed.grid.cur_shape, SAG_CURSOR_BLOCK);
    SAG_ASSERT_EQ_U64(ed.grid.back[3U].bg.tag, SAG_COLOR_DEFAULT);

    sag_grid_flip(&ed.grid);
    win.cs.curs.data[1U].pos = BYTEOFF(6U);
    win.cs.curs.data[1U].anchor = BYTEOFF(6U);
    sag_draw_cursor(&ed, &win);
    SAG_ASSERT((ed.grid.back[12U + 8U].attrs & SAG_ATTR_REVERSE) == 0U);
    SAG_ASSERT((ed.grid.back[12U].attrs & SAG_ATTR_REVERSE) != 0U);

    ed.mode = SAG_MODE_L;
    sag_draw_document_rows(&ed, &win, 0U, 2U);
    sag_grid_flip(&ed.grid);
    win.cs.curs.data[0U].pos = BYTEOFF(1U);
    win.cs.curs.data[0U].anchor = BYTEOFF(1U);
    sag_draw_cursor(&ed, &win);
    SAG_ASSERT_EQ_U64(ed.grid.dmg_lo, ed.grid.rows);
    SAG_ASSERT_EQ_U64(ed.grid.dmg_hi, 0U);

    sag_vp_free(&win);
    sag_cset_free(&win.cs);
    sag_textbuf_free(buffer.tb);
    sag_grid_free(&ed.grid);
    interner_free(&ed.interner);
    arena_free_all(&ed.arena);
}

void test_draw_rect_selected_cells_equal_deleted_span_cells(void)
{
    static const u8 text[] =
        "  ab\n"
        "a\xE6\xBC\xA2" "b\n"
        "a\tb\n"
        "a\xFF" "b\n"
        "x\n"
        "   z\n";
    static const SagColor selected_bg = {
        SAG_COLOR_RGB, 52U, 72U, 108U
    };
    Ed ed;
    Cursor *cursor;
    SagSelSpanVec spans = {0};
    Bytebuf before;
    Bytebuf expected;
    Bytebuf after;
    CmdCtx cx = {0};
    EditCtx ec;
    size_t row;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    sag_undo_free(ed.buffer.undo);
    sag_textbuf_free(ed.buffer.tb);
    ed.buffer.tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    ed.buffer.undo = sag_undo_new(ed.buffer.tb);
    ed.buffer.tabwidth = 4U;
    ed.win->buf = &ed.buffer;
    ed.win->rect = (Rect){0U, 0U, 16U, 6U};
    ed.win->vp.rows = 6U;
    ed.win->vp.cols = 16U;
    ed.win->gutter_width = 0U;
    SAG_ASSERT(sag_grid_init(&ed.grid, &ed.interner, 7U, 16U));
    ed.grid_ready = true;
    ed.mode = SAG_MODE_H;
    ed.win->h.kind = SAG_SEL_RECT;
    cursor = &ed.win->cs.curs.data[ed.win->cs.primary];
    cursor->anchor = BYTEOFF(1U);  /* cell 1 on the first row */
    cursor->pos = BYTEOFF(24U);    /* cell 3 on the final row */

    sag_sel_rect_spans(ed.win, cursor, &spans);
    SAG_ASSERT_EQ_U64(spans.len, 6U);
    sag_draw_document_rows(&ed, ed.win, 0U, 6U);
    for (row = 0U; row < spans.len; row++) {
        Span line = sag_textbuf_line_span(ed.buffer.tb, LINENO(row));
        CCol painted_lo;
        CCol painted_hi;
        u16 col;

        if (line.hi > line.lo && text[line.hi - 1U] == '\n')
            line.hi--;
        painted_lo = sag_off_to_ccol(ed.buffer.tb, line,
                                     BYTEOFF(spans.data[row].lo), 4U);
        painted_hi = sag_off_to_ccol(ed.buffer.tb, line,
                                     BYTEOFF(spans.data[row].hi), 4U);
        for (col = 0U; col < ed.win->rect.w; col++) {
            const Cell *cell = &ed.grid.back[row * ed.grid.cols + col];
            bool selected = col >= painted_lo.v && col < painted_hi.v;

            SAG_ASSERT((memcmp(&cell->bg, &selected_bg,
                               sizeof(selected_bg)) == 0) == selected);
        }
    }

    before = draw_materialize(ed.buffer.tb);
    bytebuf_init(&expected);
    draw_append_outside_spans(&expected, &before, &spans);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = SAG_SRC_TEST;
    ec = sag_ed_edit_ctx(&ed);
    sag_undo_begin(&ec, SAG_TXN_TYPE);
    SAG_ASSERT_EQ_U64(sag_sel_cmd_delete(&cx), SAG_CMD_OK);
    sag_undo_end(&ec);
    after = draw_materialize(ed.buffer.tb);
    SAG_ASSERT_EQ_U64(after.len, expected.len);
    SAG_ASSERT_EQ_MEM(after.data, expected.data, expected.len);

    bytebuf_free(&after);
    bytebuf_free(&expected);
    bytebuf_free(&before);
    SagSelSpanVec_free(&spans);
    sag_ed_free(&ed);
}

void test_draw_primary_is_only_hardware_cursor_target_in_raw_frame(void)
{
    static const u8 text[] = "abc\ndef\n";
    Ed ed;
    Cursor secondary = {BYTEOFF(5U), {0U}, BYTEOFF(5U)};
    TtyCaps caps = {0};
    const Cell *primary_cell;
    const Cell *secondary_cell;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    sag_textbuf_insert(ed.buffer.tb, BYTEOFF(0U), text, sizeof(text) - 1U);
    ed.win->rect = (Rect){0U, 0U, 8U, 2U};
    ed.win->vp.rows = 2U;
    ed.win->vp.cols = 8U;
    SAG_ASSERT(sag_grid_init(&ed.grid, &ed.interner, 3U, 8U));
    ed.grid_ready = true;
    SAG_ASSERT(sag_cset_add(&ed.win->cs, secondary));
    ed.render.tier = SAG_RENDER_TIER_16;
    sag_render_init(&ed.render, &caps, NULL);
    ed.mode = SAG_MODE_L;

    sag_draw_document_rows(&ed, ed.win, 0U, 2U);
    sag_draw_cursor(&ed, ed.win);
    primary_cell = &ed.grid.back[0U];
    secondary_cell = &ed.grid.back[8U + 1U];
    SAG_ASSERT((primary_cell->attrs & SAG_ATTR_REVERSE) == 0U);
    SAG_ASSERT((secondary_cell->attrs & SAG_ATTR_REVERSE) != 0U);
    SAG_ASSERT_EQ_U64(ed.grid.cur_row, 0U);
    SAG_ASSERT_EQ_U64(ed.grid.cur_col, 0U);
    SAG_ASSERT_EQ_U64(ed.grid.cur_shape, SAG_CURSOR_BLOCK);

    ed.frame.len = 0U;
    SAG_ASSERT(sag_render_frame(&ed.render, &ed.grid, &ed.frame) != 0U);
    SAG_ASSERT_EQ_U64(draw_count_bytes(&ed.frame, "\033[?25h"), 1U);
    SAG_ASSERT_EQ_U64(draw_count_bytes(&ed.frame, "\033[2 q"), 1U);
    /* The renderer uses the canonical short CUP form (ESC [ H) for the
     * primary at row 0, column 0. Paint-time CUPs may precede this suffix;
     * only this final target is followed by hardware-cursor enable. */
    SAG_ASSERT(ed.frame.len >= sizeof("\033[H\033[?25h") - 1U);
    SAG_ASSERT_EQ_MEM(ed.frame.data + ed.frame.len -
                          (sizeof("\033[H\033[?25h") - 1U),
                      "\033[H\033[?25h",
                      sizeof("\033[H\033[?25h") - 1U);
    sag_ed_free(&ed);
}
