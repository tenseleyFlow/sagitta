#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "text/edit.h"
#include "text/undo.h"
#include "ui/gutter.h"
#include "ui/statusline.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/width.h"

enum { TEST_STATUS_WARN_COLOR = 214 };

typedef struct StatusFixture {
    Ed ed;
    Buffer buffers[2];
    /* ws.bufs holds pointers so the list cannot relocate under a Win. */
    Buffer *bufptrs[2];
    Win win;
} StatusFixture;

static void status_fixture_init(StatusFixture *f, const u8 *bytes,
                                size_t len, const char *path)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    memset(f, 0, sizeof(*f));
    f->buffers[0].tb = yew_textbuf_from_bytes(bytes, len);
    f->buffers[0].undo = yew_undo_new(f->buffers[0].tb);
    yew_undo_mark_saved(f->buffers[0].undo);
    f->buffers[0].path = (char *)path;
    f->buffers[0].meta.eol = YEW_EOL_LF;
    f->win.buf = &f->buffers[0];
    yew_cset_init(&f->win.cs, cursor);
    yew_vp_init(&f->win);
    f->win.vp.rows = 10U;
    f->win.vp.cols = 80U;
    f->ed.mode = YEW_MODE_L;
    f->ed.prev_unit = YEW_MODE_L;
    f->ed.win = &f->win;
    f->bufptrs[0] = &f->buffers[0];
    f->bufptrs[1] = &f->buffers[1];
    f->ed.ws.bufs = f->bufptrs;
    f->ed.ws.nbufs = 1U;
}

static void status_fixture_free(StatusFixture *f)
{
    yew_vp_free(&f->win);
    yew_cset_free(&f->win.cs);
    yew_undo_free(f->buffers[0].undo);
    yew_textbuf_free(f->buffers[0].tb);
}

static bool contains(const StatuslineText *text, const char *needle)
{
    return strstr(text->body, needle) != NULL;
}

void test_gutter_number_styles_width_and_continuations(void)
{
    char number[32];

    YEW_ASSERT_EQ_U64(yew_gutter_width_for(1U, YEW_NUM_NONE), 6U);
    YEW_ASSERT_EQ_U64(yew_gutter_width_for(1U, YEW_NUM_ABS), 6U);
    YEW_ASSERT_EQ_U64(yew_gutter_width_for(99U, YEW_NUM_HYBRID), 6U);
    YEW_ASSERT_EQ_U64(yew_gutter_width_for(100U, YEW_NUM_HYBRID), 6U);
    YEW_ASSERT_EQ_U64(yew_gutter_width_for(999U, YEW_NUM_REL), 6U);
    YEW_ASSERT_EQ_U64(yew_gutter_width_for(1000U, YEW_NUM_REL), 7U);

    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_ABS, LINENO(99U),
                                          LINENO(99U), false), 3U);
    YEW_ASSERT_EQ_STR(number, "100");
    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_REL, LINENO(99U),
                                          LINENO(99U), false), 1U);
    YEW_ASSERT_EQ_STR(number, "0");
    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_REL, LINENO(91U),
                                          LINENO(99U), false), 1U);
    YEW_ASSERT_EQ_STR(number, "8");
    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_HYBRID, LINENO(99U),
                                          LINENO(99U), false), 3U);
    YEW_ASSERT_EQ_STR(number, "100");
    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_HYBRID, LINENO(102U),
                                          LINENO(99U), false), 1U);
    YEW_ASSERT_EQ_STR(number, "3");
    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_ABS, LINENO(99U),
                                          LINENO(99U), true), 0U);
    YEW_ASSERT_EQ_STR(number, "");
    YEW_ASSERT_EQ_U64(yew_gutter_number(number, sizeof(number),
                                          YEW_NUM_NONE, LINENO(99U),
                                          LINENO(99U), false), 0U);
    YEW_ASSERT_EQ_STR(number, "");
}

void test_statusline_metadata_position_percent_and_mode_chip(void)
{
    static const u8 text[] = "\xE6\xBC\xA2\xE5\xAD\x97\tx\n"
                             "two\nthree\nfour\nfive\nsix\nseven\n"
                             "eight\nnine\nten\neleven\ntwelve\n";
    StatusFixture f;
    StatuslineText out;

    status_fixture_init(&f, text, sizeof(text) - 1U, "/work/src/main.c");
    f.buffers[0].meta.exists = true;
    f.buffers[0].meta.mode = 0444;
    f.buffers[0].meta.eol = YEW_EOL_MIXED;
    f.buffers[0].meta.had_bom = true;
    f.buffers[0].meta.binary = true;
    f.buffers[0].meta.had_invalid_utf8 = true;
    f.win.cs.curs.data[0].pos = BYTEOFF(7U);
    f.win.cs.curs.data[0].anchor = BYTEOFF(7U);
    f.win.vp.rows = 5U;
    f.win.vp.top = LINENO(2U);
    yew_statusline_build(&f.ed, &f.win, 200U, &out);
    YEW_ASSERT_EQ_STR(out.chip, " L ");
    YEW_ASSERT(contains(&out, "main.c"));
    YEW_ASSERT(contains(&out, "[ro]"));
    YEW_ASSERT(contains(&out, "utf-8 bin"));
    YEW_ASSERT(contains(&out, "mixed!"));
    YEW_ASSERT_EQ_U64(out.warn_len, strlen("mixed!"));
    YEW_ASSERT_EQ_MEM(out.body + out.warn_at, "mixed!", out.warn_len);
    YEW_ASSERT(contains(&out, "bom"));
    YEW_ASSERT(contains(&out, "!utf8"));
    YEW_ASSERT(contains(&out, "1:4"));
    YEW_ASSERT(contains(&out, "25%"));
    YEW_ASSERT_EQ_U64(out.chip_cells + out.body_cells, 200U);

    f.win.cs.curs.data[0].anchor = BYTEOFF(0U);
    f.win.cs.selstacks.data[f.win.cs.primary].n = 1U;
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 200U, &out);
    YEW_ASSERT(contains(&out, "1:4@1:1"));
    f.win.cs.curs.data[0].anchor = f.win.cs.curs.data[0].pos;
    f.win.cs.selstacks.data[f.win.cs.primary].n = 0U;

    f.win.vp.top = LINENO(0U);
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 200U, &out);
    YEW_ASSERT(contains(&out, "top"));
    f.win.vp.top = LINENO(8U);
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 200U, &out);
    YEW_ASSERT(contains(&out, "bot"));
    f.win.vp.rows = 20U;
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 200U, &out);
    YEW_ASSERT(contains(&out, "all"));

    f.ed.mode = YEW_MODE_H;
    f.ed.prev_unit = YEW_MODE_B;
    f.win.h.from = YEW_MODE_B;
    f.win.h.unit = &yew_unit_block;
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 200U, &out);
    YEW_ASSERT_EQ_STR(out.chip, " H\xC2\xB7" "B ");
    YEW_ASSERT_EQ_U64(out.chip_cells, 5U);
    {
        Cursor extra = {BYTEOFF(11U), {0U}, BYTEOFF(11U)};

        YEW_ASSERT(yew_cset_add(&f.win.cs, extra));
        yew_statusline_text_free(&out);
        yew_statusline_build(&f.ed, &f.win, 200U, &out);
        YEW_ASSERT(contains(&out, "\xC3\x97" "2"));
    }
    yew_statusline_text_free(&out);
    status_fixture_free(&f);
}

void test_statusline_mixed_eol_uses_warning_attributes(void)
{
    static const u8 text[] = "x\n";
    StatusFixture f;
    StatuslineText out;
    int prefix_cells;
    u16 col;
    size_t i;

    status_fixture_init(&f, text, sizeof(text) - 1U, "main.c");
    f.buffers[0].meta.eol = YEW_EOL_MIXED;
    arena_init(&f.ed.arena);
    interner_init(&f.ed.interner, &f.ed.arena);
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 2U, 80U));
    f.ed.footer_rect = (Rect){0U, 1U, 80U, 1U};
    yew_statusline_build(&f.ed, &f.win, 80U, &out);
    prefix_cells = yew_str_width((const u8 *)out.body, out.warn_at, 4U);
    YEW_ASSERT(prefix_cells >= 0);
    col = (u16)(out.chip_cells + (u16)prefix_cells);

    yew_statusline_draw(&f.ed, &f.win);
    for (i = 0U; i < strlen("mixed!"); i++) {
        const Cell *cell = &f.ed.grid.back[f.ed.grid.cols + col + (u16)i];

        YEW_ASSERT_EQ_U64(cell->fg.tag, YEW_COLOR_INDEXED);
        YEW_ASSERT_EQ_U64(cell->fg.r, TEST_STATUS_WARN_COLOR);
        YEW_ASSERT((cell->attrs & YEW_ATTR_BOLD) != 0U);
    }
    YEW_ASSERT((f.ed.grid.back[f.ed.grid.cols + col - 1U].attrs &
                YEW_ATTR_BOLD) == 0U);

    yew_statusline_text_free(&out);
    yew_grid_free(&f.ed.grid);
    interner_free(&f.ed.interner);
    arena_free_all(&f.ed.arena);
    status_fixture_free(&f);
}

void test_statusline_priority_drop_order_is_tiered(void)
{
    static const u8 text[] = "x\n";
    StatusFixture f;
    StatuslineText out;

    status_fixture_init(&f, text, sizeof(text) - 1U, "main.c");
    f.buffers[0].meta.exists = true;
    f.buffers[0].meta.mode = 0444;
    f.buffers[0].meta.had_bom = true;
    f.buffers[0].meta.had_invalid_utf8 = true;
    f.win.vp.rows = 0U;

    yew_statusline_build(&f.ed, &f.win, 48U, &out);
    YEW_ASSERT(!contains(&out, "top"));
    YEW_ASSERT(contains(&out, "[ro]"));
    YEW_ASSERT(contains(&out, "utf-8"));
    YEW_ASSERT(contains(&out, "bom"));
    YEW_ASSERT(contains(&out, "!utf8"));
    YEW_ASSERT(contains(&out, "1:1"));

    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 43U, &out);
    YEW_ASSERT(!contains(&out, "top"));
    YEW_ASSERT(!contains(&out, "[ro]"));
    YEW_ASSERT(contains(&out, "utf-8"));
    YEW_ASSERT(contains(&out, "bom"));
    YEW_ASSERT(contains(&out, "!utf8"));
    YEW_ASSERT(contains(&out, "1:1"));

    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 37U, &out);
    YEW_ASSERT(!contains(&out, "utf-8"));
    YEW_ASSERT(!contains(&out, " bom"));
    YEW_ASSERT(contains(&out, "!utf8"));
    YEW_ASSERT(contains(&out, "1:1"));

    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 21U, &out);
    YEW_ASSERT(!contains(&out, "!utf8"));
    YEW_ASSERT(contains(&out, "1:1"));
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 14U, &out);
    YEW_ASSERT(!contains(&out, "1:1"));
    YEW_ASSERT(contains(&out, "main.c"));
    yew_statusline_text_free(&out);
    status_fixture_free(&f);
}

void test_statusline_recording_indicator_is_pinned_and_counted(void)
{
    static const u8 text[] = "x\n";
    StatusFixture f;
    StatuslineText out;

    status_fixture_init(&f, text, sizeof(text) - 1U,
                        "/a/very/long/path/that/must/lose/to/recording.c");
    f.ed.rec.active = true;
    f.ed.rec.reg = (u8)'a';
    f.ed.rec.ev.len = 9U;
    yew_statusline_build(&f.ed, &f.win, 80U, &out);
    YEW_ASSERT_EQ_STR(out.recording, "\xE2\x97\x8FREC a");
    YEW_ASSERT_EQ_U64(out.recording_cells, 6U);
    YEW_ASSERT_EQ_U64(out.chip_cells + out.recording_cells + out.body_cells,
                      80U);
    yew_statusline_text_free(&out);

    f.ed.rec.ev.len = 12U;
    yew_statusline_build(&f.ed, &f.win, 40U, &out);
    YEW_ASSERT_EQ_STR(out.recording, "\xE2\x97\x8FREC a 12");
    YEW_ASSERT(strstr(out.recording, "a") != NULL);
    YEW_ASSERT_EQ_U64(
        (u64)(out.chip_cells + out.recording_cells + out.body_cells <= 40),
        1U);
    yew_statusline_text_free(&out);

    yew_statusline_build(&f.ed, &f.win, 5U, &out);
    YEW_ASSERT_EQ_STR(out.recording, "\xE2\x97\x8F" "a");
    YEW_ASSERT_EQ_U64(out.recording_cells, 2U);
    yew_statusline_text_free(&out);
    status_fixture_free(&f);
}

void test_statusline_unicode_path_elision_uses_cells(void)
{
    static const u8 text[] = "x\n";
    static const char first[] = "/work/alpha/\xE6\xBC\xA2\xF0\x9F\x99\x82.c";
    static const char second[] = "/other/beta/\xE6\xBC\xA2\xF0\x9F\x99\x82.c";
    StatusFixture f;
    StatuslineText out;
    char long_path[1501];
    int width;

    status_fixture_init(&f, text, sizeof(text) - 1U, first);
    f.buffers[1].path = (char *)second;
    f.ed.ws.nbufs = 2U;
    yew_statusline_build(&f.ed, &f.win, 15U, &out);
    width = yew_str_width((const u8 *)out.body, out.body_len, 4U);
    YEW_ASSERT(width >= 0);
    YEW_ASSERT_EQ_U64((u64)width, out.body_cells);
    YEW_ASSERT(out.chip_cells + out.body_cells <= 15U);
    YEW_ASSERT(contains(&out, "\xE2\x80\xA6"));
    YEW_ASSERT(contains(&out, "\xE6\xBC\xA2"));
    YEW_ASSERT(contains(&out, "\xF0\x9F\x99\x82"));

    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 10U, &out);
    width = yew_str_width((const u8 *)out.body, out.body_len, 4U);
    YEW_ASSERT(width >= 0);
    YEW_ASSERT(out.chip_cells + (u16)width <= 10U);
    YEW_ASSERT(contains(&out, "\xE2\x80\xA6"));

    f.buffers[0].path = "/a/alpha_long/edit/keymap.c";
    f.buffers[1].path = "/b/beta_long/edit/keymap.c";
    f.ed.ws.nbufs = 2U;
    yew_statusline_text_free(&out);
    yew_statusline_build(&f.ed, &f.win, 22U, &out);
    YEW_ASSERT(contains(&out, "\xE2\x80\xA6/edit/keymap.c"));

    yew_statusline_text_free(&out);
    (void)memset(long_path, 'p', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    f.buffers[0].path = long_path;
    f.ed.ws.nbufs = 1U;
    yew_statusline_build(&f.ed, &f.win, 2000U, &out);
    YEW_ASSERT_EQ_U64(out.chip_cells + out.body_cells, 2000U);
    YEW_ASSERT(out.body_len > 1024U);

    yew_statusline_text_free(&out);
    status_fixture_free(&f);
}

void test_statusline_mode_roles_are_fixed_and_distinct(void)
{
    YewUiStyle styles[YEW_MODE__N];
    size_t i;
    size_t j;

    for (i = 0U; i < YEW_MODE__N; i++) {
        styles[i] = yew_statusline_mode_style((Mode)i);
        YEW_ASSERT_EQ_U64(styles[i].chip_bg.tag, YEW_COLOR_RGB);
        YEW_ASSERT_EQ_U64(styles[i].row_bg.tag, YEW_COLOR_RGB);
        YEW_ASSERT(styles[i].attrs & YEW_ATTR_BOLD);
        YEW_ASSERT(memcmp(&styles[i].chip_bg, &styles[i].row_bg,
                          sizeof(YewColor)) != 0);
    }
    for (i = 0U; i < YEW_MODE__N; i++) {
        for (j = i + 1U; j < YEW_MODE__N; j++) {
            YEW_ASSERT(memcmp(&styles[i].chip_bg, &styles[j].chip_bg,
                              sizeof(YewColor)) != 0);
            YEW_ASSERT(memcmp(&styles[i].row_bg, &styles[j].row_bg,
                              sizeof(YewColor)) != 0);
        }
    }
}
