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
    f->buffers[0].tb = sag_textbuf_from_bytes(bytes, len);
    f->buffers[0].undo = sag_undo_new(f->buffers[0].tb);
    sag_undo_mark_saved(f->buffers[0].undo);
    f->buffers[0].path = (char *)path;
    f->buffers[0].meta.eol = SAG_EOL_LF;
    f->win.buf = &f->buffers[0];
    sag_cset_init(&f->win.cs, cursor);
    sag_vp_init(&f->win);
    f->win.vp.rows = 10U;
    f->win.vp.cols = 80U;
    f->ed.mode = SAG_MODE_L;
    f->ed.prev_unit = SAG_MODE_L;
    f->ed.win = &f->win;
    f->bufptrs[0] = &f->buffers[0];
    f->bufptrs[1] = &f->buffers[1];
    f->ed.ws.bufs = f->bufptrs;
    f->ed.ws.nbufs = 1U;
}

static void status_fixture_free(StatusFixture *f)
{
    sag_vp_free(&f->win);
    sag_cset_free(&f->win.cs);
    sag_undo_free(f->buffers[0].undo);
    sag_textbuf_free(f->buffers[0].tb);
}

static bool contains(const StatuslineText *text, const char *needle)
{
    return strstr(text->body, needle) != NULL;
}

void test_gutter_number_styles_width_and_continuations(void)
{
    char number[32];

    SAG_ASSERT_EQ_U64(sag_gutter_width_for(1U, SAG_NUM_NONE), 6U);
    SAG_ASSERT_EQ_U64(sag_gutter_width_for(1U, SAG_NUM_ABS), 6U);
    SAG_ASSERT_EQ_U64(sag_gutter_width_for(99U, SAG_NUM_HYBRID), 6U);
    SAG_ASSERT_EQ_U64(sag_gutter_width_for(100U, SAG_NUM_HYBRID), 6U);
    SAG_ASSERT_EQ_U64(sag_gutter_width_for(999U, SAG_NUM_REL), 6U);
    SAG_ASSERT_EQ_U64(sag_gutter_width_for(1000U, SAG_NUM_REL), 7U);

    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_ABS, LINENO(99U),
                                          LINENO(99U), false), 3U);
    SAG_ASSERT_EQ_STR(number, "100");
    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_REL, LINENO(99U),
                                          LINENO(99U), false), 1U);
    SAG_ASSERT_EQ_STR(number, "0");
    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_REL, LINENO(91U),
                                          LINENO(99U), false), 1U);
    SAG_ASSERT_EQ_STR(number, "8");
    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_HYBRID, LINENO(99U),
                                          LINENO(99U), false), 3U);
    SAG_ASSERT_EQ_STR(number, "100");
    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_HYBRID, LINENO(102U),
                                          LINENO(99U), false), 1U);
    SAG_ASSERT_EQ_STR(number, "3");
    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_ABS, LINENO(99U),
                                          LINENO(99U), true), 0U);
    SAG_ASSERT_EQ_STR(number, "");
    SAG_ASSERT_EQ_U64(sag_gutter_number(number, sizeof(number),
                                          SAG_NUM_NONE, LINENO(99U),
                                          LINENO(99U), false), 0U);
    SAG_ASSERT_EQ_STR(number, "");
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
    f.buffers[0].meta.eol = SAG_EOL_MIXED;
    f.buffers[0].meta.had_bom = true;
    f.buffers[0].meta.binary = true;
    f.buffers[0].meta.had_invalid_utf8 = true;
    f.win.cs.curs.data[0].pos = BYTEOFF(7U);
    f.win.cs.curs.data[0].anchor = BYTEOFF(7U);
    f.win.vp.rows = 5U;
    f.win.vp.top = LINENO(2U);
    sag_statusline_build(&f.ed, &f.win, 200U, &out);
    SAG_ASSERT_EQ_STR(out.chip, " L ");
    SAG_ASSERT(contains(&out, "main.c"));
    SAG_ASSERT(contains(&out, "[ro]"));
    SAG_ASSERT(contains(&out, "utf-8 bin"));
    SAG_ASSERT(contains(&out, "mixed!"));
    SAG_ASSERT_EQ_U64(out.warn_len, strlen("mixed!"));
    SAG_ASSERT_EQ_MEM(out.body + out.warn_at, "mixed!", out.warn_len);
    SAG_ASSERT(contains(&out, "bom"));
    SAG_ASSERT(contains(&out, "!utf8"));
    SAG_ASSERT(contains(&out, "1:4"));
    SAG_ASSERT(contains(&out, "25%"));
    SAG_ASSERT_EQ_U64(out.chip_cells + out.body_cells, 200U);

    f.win.cs.curs.data[0].anchor = BYTEOFF(0U);
    f.win.cs.selstacks.data[f.win.cs.primary].n = 1U;
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 200U, &out);
    SAG_ASSERT(contains(&out, "1:4@1:1"));
    f.win.cs.curs.data[0].anchor = f.win.cs.curs.data[0].pos;
    f.win.cs.selstacks.data[f.win.cs.primary].n = 0U;

    f.win.vp.top = LINENO(0U);
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 200U, &out);
    SAG_ASSERT(contains(&out, "top"));
    f.win.vp.top = LINENO(8U);
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 200U, &out);
    SAG_ASSERT(contains(&out, "bot"));
    f.win.vp.rows = 20U;
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 200U, &out);
    SAG_ASSERT(contains(&out, "all"));

    f.ed.mode = SAG_MODE_H;
    f.ed.prev_unit = SAG_MODE_B;
    f.win.h.from = SAG_MODE_B;
    f.win.h.unit = &sag_unit_block;
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 200U, &out);
    SAG_ASSERT_EQ_STR(out.chip, " H\xC2\xB7" "B ");
    SAG_ASSERT_EQ_U64(out.chip_cells, 5U);
    {
        Cursor extra = {BYTEOFF(11U), {0U}, BYTEOFF(11U)};

        SAG_ASSERT(sag_cset_add(&f.win.cs, extra));
        sag_statusline_text_free(&out);
        sag_statusline_build(&f.ed, &f.win, 200U, &out);
        SAG_ASSERT(contains(&out, "\xC3\x97" "2"));
    }
    sag_statusline_text_free(&out);
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
    f.buffers[0].meta.eol = SAG_EOL_MIXED;
    arena_init(&f.ed.arena);
    interner_init(&f.ed.interner, &f.ed.arena);
    SAG_ASSERT(sag_grid_init(&f.ed.grid, &f.ed.interner, 2U, 80U));
    f.ed.footer_rect = (Rect){0U, 1U, 80U, 1U};
    sag_statusline_build(&f.ed, &f.win, 80U, &out);
    prefix_cells = sag_str_width((const u8 *)out.body, out.warn_at, 4U);
    SAG_ASSERT(prefix_cells >= 0);
    col = (u16)(out.chip_cells + (u16)prefix_cells);

    sag_statusline_draw(&f.ed, &f.win);
    for (i = 0U; i < strlen("mixed!"); i++) {
        const Cell *cell = &f.ed.grid.back[f.ed.grid.cols + col + (u16)i];

        SAG_ASSERT_EQ_U64(cell->fg.tag, SAG_COLOR_INDEXED);
        SAG_ASSERT_EQ_U64(cell->fg.r, TEST_STATUS_WARN_COLOR);
        SAG_ASSERT((cell->attrs & SAG_ATTR_BOLD) != 0U);
    }
    SAG_ASSERT((f.ed.grid.back[f.ed.grid.cols + col - 1U].attrs &
                SAG_ATTR_BOLD) == 0U);

    sag_statusline_text_free(&out);
    sag_grid_free(&f.ed.grid);
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

    sag_statusline_build(&f.ed, &f.win, 48U, &out);
    SAG_ASSERT(!contains(&out, "top"));
    SAG_ASSERT(contains(&out, "[ro]"));
    SAG_ASSERT(contains(&out, "utf-8"));
    SAG_ASSERT(contains(&out, "bom"));
    SAG_ASSERT(contains(&out, "!utf8"));
    SAG_ASSERT(contains(&out, "1:1"));

    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 43U, &out);
    SAG_ASSERT(!contains(&out, "top"));
    SAG_ASSERT(!contains(&out, "[ro]"));
    SAG_ASSERT(contains(&out, "utf-8"));
    SAG_ASSERT(contains(&out, "bom"));
    SAG_ASSERT(contains(&out, "!utf8"));
    SAG_ASSERT(contains(&out, "1:1"));

    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 37U, &out);
    SAG_ASSERT(!contains(&out, "utf-8"));
    SAG_ASSERT(!contains(&out, " bom"));
    SAG_ASSERT(contains(&out, "!utf8"));
    SAG_ASSERT(contains(&out, "1:1"));

    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 21U, &out);
    SAG_ASSERT(!contains(&out, "!utf8"));
    SAG_ASSERT(contains(&out, "1:1"));
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 14U, &out);
    SAG_ASSERT(!contains(&out, "1:1"));
    SAG_ASSERT(contains(&out, "main.c"));
    sag_statusline_text_free(&out);
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
    sag_statusline_build(&f.ed, &f.win, 15U, &out);
    width = sag_str_width((const u8 *)out.body, out.body_len, 4U);
    SAG_ASSERT(width >= 0);
    SAG_ASSERT_EQ_U64((u64)width, out.body_cells);
    SAG_ASSERT(out.chip_cells + out.body_cells <= 15U);
    SAG_ASSERT(contains(&out, "\xE2\x80\xA6"));
    SAG_ASSERT(contains(&out, "\xE6\xBC\xA2"));
    SAG_ASSERT(contains(&out, "\xF0\x9F\x99\x82"));

    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 10U, &out);
    width = sag_str_width((const u8 *)out.body, out.body_len, 4U);
    SAG_ASSERT(width >= 0);
    SAG_ASSERT(out.chip_cells + (u16)width <= 10U);
    SAG_ASSERT(contains(&out, "\xE2\x80\xA6"));

    f.buffers[0].path = "/a/alpha_long/edit/keymap.c";
    f.buffers[1].path = "/b/beta_long/edit/keymap.c";
    f.ed.ws.nbufs = 2U;
    sag_statusline_text_free(&out);
    sag_statusline_build(&f.ed, &f.win, 22U, &out);
    SAG_ASSERT(contains(&out, "\xE2\x80\xA6/edit/keymap.c"));

    sag_statusline_text_free(&out);
    (void)memset(long_path, 'p', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    f.buffers[0].path = long_path;
    f.ed.ws.nbufs = 1U;
    sag_statusline_build(&f.ed, &f.win, 2000U, &out);
    SAG_ASSERT_EQ_U64(out.chip_cells + out.body_cells, 2000U);
    SAG_ASSERT(out.body_len > 1024U);

    sag_statusline_text_free(&out);
    status_fixture_free(&f);
}

void test_statusline_mode_roles_are_fixed_and_distinct(void)
{
    SagUiStyle styles[SAG_MODE__N];
    size_t i;
    size_t j;

    for (i = 0U; i < SAG_MODE__N; i++) {
        styles[i] = sag_statusline_mode_style((Mode)i);
        SAG_ASSERT_EQ_U64(styles[i].chip_bg.tag, SAG_COLOR_RGB);
        SAG_ASSERT_EQ_U64(styles[i].row_bg.tag, SAG_COLOR_RGB);
        SAG_ASSERT(styles[i].attrs & SAG_ATTR_BOLD);
        SAG_ASSERT(memcmp(&styles[i].chip_bg, &styles[i].row_bg,
                          sizeof(SagColor)) != 0);
    }
    for (i = 0U; i < SAG_MODE__N; i++) {
        for (j = i + 1U; j < SAG_MODE__N; j++) {
            SAG_ASSERT(memcmp(&styles[i].chip_bg, &styles[j].chip_bg,
                              sizeof(SagColor)) != 0);
            SAG_ASSERT(memcmp(&styles[i].row_bg, &styles[j].row_bg,
                              sizeof(SagColor)) != 0);
        }
    }
}
