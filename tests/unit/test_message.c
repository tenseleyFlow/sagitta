#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "term/grid.h"
#include "text/undo.h"
#include "ui/message.h"
#include "ui/viewport.h"
#include "unicode/width.h"
#include "util/arena.h"
#include "util/intern.h"

static void message_ed_init(Ed *ed)
{
    (void)memset(ed, 0, sizeof(*ed));
    ed->mode = SAG_MODE_L;
    sag_timers_init(&ed->timers);
}

static void message_ed_free(Ed *ed)
{
    sag_msg_clear(ed);
    sag_timers_free(&ed->timers);
}

void test_message_expiry_and_replacement_rules(void)
{
    Ed ed;

    message_ed_init(&ed);
    sag_msg_at(&ed, SAG_MSG_INFO, 1000, "info %d", 7);
    SAG_ASSERT(ed.msg.active);
    SAG_ASSERT_EQ_STR(ed.msg.text, "info 7");
    SAG_ASSERT(ed.msg.expiry != SAG_TIMER_NONE);
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&ed.timers, 1000), 4000);
    sag_timers_fire(&ed.timers, &ed, 4999);
    SAG_ASSERT(ed.msg.active);
    sag_timers_fire(&ed.timers, &ed, 5000);
    SAG_ASSERT(!ed.msg.active);
    SAG_ASSERT_EQ_U64(ed.msg.expiry, SAG_TIMER_NONE);
    SAG_ASSERT(!ed.full_damage);

    ed.full_damage = false;
    sag_msg_at(&ed, SAG_MSG_WARN, 2000, "careful");
    SAG_ASSERT_EQ_U64(ed.msg.sev, SAG_MSG_WARN);
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&ed.timers, 2000), 8000);
    sag_timers_fire(&ed.timers, &ed, 9999);
    SAG_ASSERT(ed.msg.active);
    sag_timers_fire(&ed.timers, &ed, 10000);
    SAG_ASSERT(!ed.msg.active);

    sag_msg_at(&ed, SAG_MSG_ERROR, 3000, "disk error");
    SAG_ASSERT(ed.msg.active);
    SAG_ASSERT_EQ_U64(ed.msg.sev, SAG_MSG_ERROR);
    SAG_ASSERT_EQ_U64(ed.msg.expiry, SAG_TIMER_NONE);
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&ed.timers, 3000), -1);
    sag_timers_fire(&ed.timers, &ed, INT64_MAX);
    SAG_ASSERT(ed.msg.active);

    sag_msg_at(&ed, SAG_MSG_INFO, 4000, "wrote file");
    SAG_ASSERT_EQ_U64(ed.msg.sev, SAG_MSG_ERROR);
    SAG_ASSERT_EQ_STR(ed.msg.text, "disk error");
    sag_msg_at(&ed, SAG_MSG_WARN, 4000, "warning replaces error");
    SAG_ASSERT_EQ_U64(ed.msg.sev, SAG_MSG_WARN);
    SAG_ASSERT_EQ_STR(ed.msg.text, "warning replaces error");
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&ed.timers, 4000), 8000);

    sag_msg_clear(&ed);
    SAG_ASSERT(!ed.msg.active);
    SAG_ASSERT_EQ_I64(sag_timers_deadline(&ed.timers, 4000), -1);
    message_ed_free(&ed);
}

void test_message_prompt_and_expand_lifetime(void)
{
    Ed ed;
    char long_message[700];

    message_ed_init(&ed);
    ed.prompt = SAG_PROMPT_QUIT_DIRTY;
    sag_msg_at(&ed, SAG_MSG_ERROR, 100, "save? [w] [d]");
    SAG_ASSERT(ed.msg.active);
    SAG_ASSERT(ed.msg.prompt);
    SAG_ASSERT_EQ_U64(ed.msg.expiry, SAG_TIMER_NONE);
    sag_timers_fire(&ed.timers, &ed, INT64_MAX);
    SAG_ASSERT(ed.msg.active);

    ed.prompt = SAG_PROMPT_NONE;
    sag_msg_at(&ed, SAG_MSG_WARN, 200, "background warning");
    SAG_ASSERT(ed.msg.active);
    SAG_ASSERT(ed.msg.prompt);
    SAG_ASSERT_EQ_STR(ed.msg.text, "save? [w] [d]");

    SAG_ASSERT(!sag_msg_expand(&ed));
    ed.msg.truncated = true;
    ed.full_damage = false;
    SAG_ASSERT(sag_msg_expand(&ed));
    SAG_ASSERT(ed.msg.expanded);
    SAG_ASSERT(ed.full_damage);
    SAG_ASSERT(!sag_msg_expand(&ed));
    ed.full_damage = false;
    SAG_ASSERT(sag_msg_dismiss_overlay(&ed));
    SAG_ASSERT(!ed.msg.expanded);
    SAG_ASSERT(ed.full_damage);
    SAG_ASSERT(!sag_msg_dismiss_overlay(&ed));

    sag_msg_clear(&ed);
    SAG_ASSERT(!ed.msg.prompt);
    SAG_ASSERT(!ed.msg.expanded);

    (void)memset(long_message, 'x', sizeof(long_message) - 1U);
    long_message[sizeof(long_message) - 1U] = '\0';
    sag_msg_at(&ed, SAG_MSG_WARN, 300, "%s", long_message);
    SAG_ASSERT_NOT_NULL(ed.msg.full);
    SAG_ASSERT_EQ_U64(ed.msg.len, sizeof(long_message) - 1U);
    SAG_ASSERT_EQ_STR(ed.msg.full, long_message);
    message_ed_free(&ed);
}

void test_message_unicode_cell_truncation(void)
{
    char out[128];
    bool cut;
    size_t n;
    static const char cjk[] = "A\xe6\xbc\xa2" "B";
    static const char combining[] = "e\xcc\x81x";
    static const char family[] =
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
        "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6x";

    n = sag_message_clip("abcdefg", 7U, 6U, out, sizeof(out), &cut);
    SAG_ASSERT(cut);
    SAG_ASSERT_EQ_U64(n, 8U);
    SAG_ASSERT_EQ_STR(out, "abcde\xe2\x80\xa6");
    SAG_ASSERT_EQ_I64(sag_str_width((const u8 *)out, n, 1U), 6);

    n = sag_message_clip(cjk, sizeof(cjk) - 1U, 4U, out, sizeof(out), &cut);
    SAG_ASSERT(!cut);
    SAG_ASSERT_EQ_U64(n, sizeof(cjk) - 1U);
    SAG_ASSERT_EQ_STR(out, cjk);
    n = sag_message_clip(cjk, sizeof(cjk) - 1U, 3U, out, sizeof(out), &cut);
    SAG_ASSERT(cut);
    SAG_ASSERT_EQ_STR(out, "A\xe2\x80\xa6");
    SAG_ASSERT_EQ_I64(sag_str_width((const u8 *)out, n, 1U), 2);

    n = sag_message_clip(combining, sizeof(combining) - 1U, 1U,
                         out, sizeof(out), &cut);
    SAG_ASSERT(cut);
    SAG_ASSERT_EQ_STR(out, "\xe2\x80\xa6");
    SAG_ASSERT_EQ_I64(sag_str_width((const u8 *)out, n, 1U), 1);
    n = sag_message_clip(combining, sizeof(combining) - 1U, 2U,
                         out, sizeof(out), &cut);
    SAG_ASSERT(!cut);
    SAG_ASSERT_EQ_STR(out, combining);

    n = sag_message_clip(family, sizeof(family) - 1U, 2U,
                         out, sizeof(out), &cut);
    SAG_ASSERT(cut);
    SAG_ASSERT_EQ_STR(out, "\xe2\x80\xa6");
    SAG_ASSERT_EQ_I64(sag_str_width((const u8 *)out, n, 1U), 1);

    n = sag_message_clip("x", 1U, 0U, out, sizeof(out), &cut);
    SAG_ASSERT(cut);
    SAG_ASSERT_EQ_U64(n, 0U);
    SAG_ASSERT_EQ_STR(out, "");
}

void test_message_severity_and_prompt_styles(void)
{
    Ed ed;
    SagUiStyle info;
    SagUiStyle warn;
    SagUiStyle error;
    SagUiStyle prompt;

    message_ed_init(&ed);
    ed.msg.sev = SAG_MSG_INFO;
    info = sag_message_style(&ed);
    SAG_ASSERT_EQ_U64(info.attrs, 0U);

    ed.msg.sev = SAG_MSG_WARN;
    warn = sag_message_style(&ed);
    SAG_ASSERT((warn.attrs & SAG_ATTR_BOLD) != 0U);
    SAG_ASSERT_EQ_U64(warn.row_fg.tag, SAG_COLOR_INDEXED);
    SAG_ASSERT(warn.row_fg.r != info.row_fg.r ||
               warn.row_fg.tag != info.row_fg.tag);

    ed.msg.sev = SAG_MSG_ERROR;
    error = sag_message_style(&ed);
    SAG_ASSERT((error.attrs & SAG_ATTR_BOLD) != 0U);
    SAG_ASSERT_EQ_U64(error.row_fg.tag, SAG_COLOR_INDEXED);
    SAG_ASSERT(error.row_fg.r != warn.row_fg.r);

    ed.msg.prompt = true;
    prompt = sag_message_style(&ed);
    SAG_ASSERT((prompt.attrs & SAG_ATTR_BOLD) != 0U);
    SAG_ASSERT_EQ_U64(prompt.row_fg.tag, SAG_COLOR_INDEXED);
    SAG_ASSERT(prompt.row_fg.r != error.row_fg.r);
    SAG_ASSERT(prompt.row_fg.r != warn.row_fg.r);
    SAG_ASSERT_EQ_U64(prompt.row_bg.tag, info.row_bg.tag);
    SAG_ASSERT_EQ_U64(prompt.row_bg.r, info.row_bg.r);
    message_ed_free(&ed);
}

void test_message_draw_retains_chip_and_caps_overlay(void)
{
    static const u8 bytes[] = "x\n";
    Ed ed;
    Buffer buffer;
    Buffer *bufptrs[1];
    Win win;
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    Cell *cell;

    memset(&ed, 0, sizeof(ed));
    memset(&buffer, 0, sizeof(buffer));
    memset(&win, 0, sizeof(win));
    arena_init(&ed.arena);
    interner_init(&ed.interner, &ed.arena);
    sag_timers_init(&ed.timers);
    SAG_ASSERT(sag_grid_init(&ed.grid, &ed.interner, 6U, 12U));
    buffer.tb = sag_textbuf_from_bytes(bytes, sizeof(bytes) - 1U);
    buffer.undo = sag_undo_new(buffer.tb);
    sag_undo_mark_saved(buffer.undo);
    buffer.meta.eol = SAG_EOL_LF;
    win.buf = &buffer;
    sag_cset_init(&win.cs, cursor);
    sag_vp_init(&win);
    win.vp.rows = 5U;
    win.vp.cols = 12U;
    ed.mode = SAG_MODE_L;
    ed.prev_unit = SAG_MODE_L;
    ed.win = &win;
    bufptrs[0] = &buffer;
    ed.ws.bufs = bufptrs;
    ed.ws.nbufs = 1U;
    ed.footer_rect = (Rect){0U, 5U, 12U, 1U};

    sag_msg_at(&ed, SAG_MSG_WARN, 0,
               "abcdefghijklmnopqrstuvwxyz");
    sag_message_draw(&ed, &win);
    SAG_ASSERT(ed.msg.truncated);
    cell = &ed.grid.back[5U * ed.grid.cols];
    SAG_ASSERT_EQ_U64(cell[0].utf8[0], (u8)' ');
    SAG_ASSERT_EQ_U64(cell[1].utf8[0], (u8)'L');
    SAG_ASSERT_EQ_U64(cell[2].utf8[0], (u8)' ');
    SAG_ASSERT_EQ_U64(cell[3].utf8[0], (u8)'a');
    SAG_ASSERT_EQ_U64(cell[10].utf8[0], (u8)'h');
    SAG_ASSERT_EQ_U64(cell[11].utf8[0], 0xe2U);
    SAG_ASSERT_EQ_U64(cell[3].fg.tag, SAG_COLOR_INDEXED);
    SAG_ASSERT((cell[3].attrs & SAG_ATTR_BOLD) != 0U);

    SAG_ASSERT(sag_msg_expand(&ed));
    sag_message_draw(&ed, &win);
    SAG_ASSERT_EQ_U64(ed.grid.back[3U * ed.grid.cols + 3U].utf8[0],
                      (u8)'a');
    SAG_ASSERT_EQ_U64(ed.grid.back[4U * ed.grid.cols + 3U].utf8[0],
                      (u8)'j');
    SAG_ASSERT_EQ_U64(ed.grid.back[5U * ed.grid.cols + 3U].utf8[0],
                      (u8)'s');
    SAG_ASSERT_EQ_U64(ed.grid.back[5U * ed.grid.cols + 1U].utf8[0],
                      (u8)'L');
    SAG_ASSERT_EQ_U64(ed.grid.back[5U * ed.grid.cols].bg.tag,
                      SAG_COLOR_RGB);

    sag_msg_clear(&ed);
    sag_vp_free(&win);
    sag_cset_free(&win.cs);
    sag_undo_free(buffer.undo);
    sag_textbuf_free(buffer.tb);
    sag_grid_free(&ed.grid);
    sag_timers_free(&ed.timers);
    interner_free(&ed.interner);
    arena_free_all(&ed.arena);
}
