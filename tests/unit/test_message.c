#include "harness.h"

#include <string.h>
#include <unistd.h>

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
    ed->mode = YEW_MODE_L;
    yew_timers_init(&ed->timers);
}

static void message_ed_free(Ed *ed)
{
    yew_msg_clear(ed);
    yew_timers_free(&ed->timers);
}

void test_message_expiry_and_replacement_rules(void)
{
    Ed ed;

    message_ed_init(&ed);
    yew_msg_at(&ed, YEW_MSG_INFO, 1000, "info %d", 7);
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_STR(ed.msg.text, "info 7");
    YEW_ASSERT(ed.msg.expiry != YEW_TIMER_NONE);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, 1000), 4000);
    yew_timers_fire(&ed.timers, &ed, 4999);
    YEW_ASSERT(ed.msg.active);
    yew_timers_fire(&ed.timers, &ed, 5000);
    YEW_ASSERT(!ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.expiry, YEW_TIMER_NONE);
    YEW_ASSERT(!ed.full_damage);

    ed.full_damage = false;
    yew_msg_at(&ed, YEW_MSG_WARN, 2000, "careful");
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_WARN);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, 2000), 8000);
    yew_timers_fire(&ed.timers, &ed, 9999);
    YEW_ASSERT(ed.msg.active);
    yew_timers_fire(&ed.timers, &ed, 10000);
    YEW_ASSERT(!ed.msg.active);

    yew_msg_at(&ed, YEW_MSG_ERROR, 3000, "disk error");
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT_EQ_U64(ed.msg.expiry, YEW_TIMER_NONE);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, 3000), -1);
    yew_timers_fire(&ed.timers, &ed, INT64_MAX);
    YEW_ASSERT(ed.msg.active);

    yew_msg_at(&ed, YEW_MSG_INFO, 4000, "wrote file");
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT_EQ_STR(ed.msg.text, "disk error");
    yew_msg_at(&ed, YEW_MSG_WARN, 4000, "warning replaces error");
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_WARN);
    YEW_ASSERT_EQ_STR(ed.msg.text, "warning replaces error");
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, 4000), 8000);

    yew_msg_clear(&ed);
    YEW_ASSERT(!ed.msg.active);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, 4000), -1);
    message_ed_free(&ed);
}

void test_message_errorbells_write_only_for_errors(void)
{
    Ed ed;
    int pipefd[2];
    char bell = '\0';

    message_ed_init(&ed);
    YEW_ASSERT(pipe(pipefd) == 0);
    ed.tty.wfd = pipefd[1];
    ed.tty_ready = true;
    ed.errorbells = true;
    yew_msg_at(&ed, YEW_MSG_WARN, 1000, "warning");
    yew_msg_at(&ed, YEW_MSG_ERROR, 1000, "error");
    YEW_ASSERT(read(pipefd[0], &bell, 1U) == 1);
    YEW_ASSERT_EQ_U64((u8)bell, (u8)'\a');
    YEW_ASSERT(close(pipefd[1]) == 0);
    YEW_ASSERT(close(pipefd[0]) == 0);
    message_ed_free(&ed);
}

void test_message_prompt_and_expand_lifetime(void)
{
    Ed ed;
    char long_message[700];

    message_ed_init(&ed);
    ed.prompt = YEW_PROMPT_QUIT_DIRTY;
    yew_msg_at(&ed, YEW_MSG_ERROR, 100, "save? [w] [d]");
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT(ed.msg.prompt);
    YEW_ASSERT_EQ_U64(ed.msg.expiry, YEW_TIMER_NONE);
    yew_timers_fire(&ed.timers, &ed, INT64_MAX);
    YEW_ASSERT(ed.msg.active);

    ed.prompt = YEW_PROMPT_NONE;
    yew_msg_at(&ed, YEW_MSG_WARN, 200, "background warning");
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT(ed.msg.prompt);
    YEW_ASSERT_EQ_STR(ed.msg.text, "save? [w] [d]");

    YEW_ASSERT(!yew_msg_expand(&ed));
    ed.msg.truncated = true;
    ed.full_damage = false;
    YEW_ASSERT(yew_msg_expand(&ed));
    YEW_ASSERT(ed.msg.expanded);
    YEW_ASSERT(ed.full_damage);
    YEW_ASSERT(!yew_msg_expand(&ed));
    ed.full_damage = false;
    YEW_ASSERT(yew_msg_dismiss_overlay(&ed));
    YEW_ASSERT(!ed.msg.expanded);
    YEW_ASSERT(ed.full_damage);
    YEW_ASSERT(!yew_msg_dismiss_overlay(&ed));

    yew_msg_clear(&ed);
    YEW_ASSERT(!ed.msg.prompt);
    YEW_ASSERT(!ed.msg.expanded);

    (void)memset(long_message, 'x', sizeof(long_message) - 1U);
    long_message[sizeof(long_message) - 1U] = '\0';
    yew_msg_at(&ed, YEW_MSG_WARN, 300, "%s", long_message);
    YEW_ASSERT_NOT_NULL(ed.msg.full);
    YEW_ASSERT_EQ_U64(ed.msg.len, sizeof(long_message) - 1U);
    YEW_ASSERT_EQ_STR(ed.msg.full, long_message);
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

    n = yew_message_clip("abcdefg", 7U, 6U, out, sizeof(out), &cut);
    YEW_ASSERT(cut);
    YEW_ASSERT_EQ_U64(n, 8U);
    YEW_ASSERT_EQ_STR(out, "abcde\xe2\x80\xa6");
    YEW_ASSERT_EQ_I64(yew_str_width((const u8 *)out, n, 1U), 6);

    n = yew_message_clip(cjk, sizeof(cjk) - 1U, 4U, out, sizeof(out), &cut);
    YEW_ASSERT(!cut);
    YEW_ASSERT_EQ_U64(n, sizeof(cjk) - 1U);
    YEW_ASSERT_EQ_STR(out, cjk);
    n = yew_message_clip(cjk, sizeof(cjk) - 1U, 3U, out, sizeof(out), &cut);
    YEW_ASSERT(cut);
    YEW_ASSERT_EQ_STR(out, "A\xe2\x80\xa6");
    YEW_ASSERT_EQ_I64(yew_str_width((const u8 *)out, n, 1U), 2);

    n = yew_message_clip(combining, sizeof(combining) - 1U, 1U,
                         out, sizeof(out), &cut);
    YEW_ASSERT(cut);
    YEW_ASSERT_EQ_STR(out, "\xe2\x80\xa6");
    YEW_ASSERT_EQ_I64(yew_str_width((const u8 *)out, n, 1U), 1);
    n = yew_message_clip(combining, sizeof(combining) - 1U, 2U,
                         out, sizeof(out), &cut);
    YEW_ASSERT(!cut);
    YEW_ASSERT_EQ_STR(out, combining);

    n = yew_message_clip(family, sizeof(family) - 1U, 2U,
                         out, sizeof(out), &cut);
    YEW_ASSERT(cut);
    YEW_ASSERT_EQ_STR(out, "\xe2\x80\xa6");
    YEW_ASSERT_EQ_I64(yew_str_width((const u8 *)out, n, 1U), 1);

    n = yew_message_clip("x", 1U, 0U, out, sizeof(out), &cut);
    YEW_ASSERT(cut);
    YEW_ASSERT_EQ_U64(n, 0U);
    YEW_ASSERT_EQ_STR(out, "");
}

void test_message_severity_and_prompt_styles(void)
{
    Ed ed;
    YewUiStyle info;
    YewUiStyle warn;
    YewUiStyle error;
    YewUiStyle prompt;

    message_ed_init(&ed);
    ed.msg.sev = YEW_MSG_INFO;
    info = yew_message_style(&ed);
    YEW_ASSERT_EQ_U64(info.attrs, 0U);

    ed.msg.sev = YEW_MSG_WARN;
    warn = yew_message_style(&ed);
    YEW_ASSERT((warn.attrs & YEW_ATTR_BOLD) != 0U);
    YEW_ASSERT_EQ_U64(warn.row_fg.tag, YEW_COLOR_INDEXED);
    YEW_ASSERT(warn.row_fg.r != info.row_fg.r ||
               warn.row_fg.tag != info.row_fg.tag);

    ed.msg.sev = YEW_MSG_ERROR;
    error = yew_message_style(&ed);
    YEW_ASSERT((error.attrs & YEW_ATTR_BOLD) != 0U);
    YEW_ASSERT_EQ_U64(error.row_fg.tag, YEW_COLOR_INDEXED);
    YEW_ASSERT(error.row_fg.r != warn.row_fg.r);

    ed.msg.prompt = true;
    prompt = yew_message_style(&ed);
    YEW_ASSERT((prompt.attrs & YEW_ATTR_BOLD) != 0U);
    YEW_ASSERT_EQ_U64(prompt.row_fg.tag, YEW_COLOR_INDEXED);
    YEW_ASSERT(prompt.row_fg.r != error.row_fg.r);
    YEW_ASSERT(prompt.row_fg.r != warn.row_fg.r);
    YEW_ASSERT_EQ_U64(prompt.row_bg.tag, info.row_bg.tag);
    YEW_ASSERT_EQ_U64(prompt.row_bg.r, info.row_bg.r);
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
    yew_timers_init(&ed.timers);
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 6U, 12U));
    buffer.tb = yew_textbuf_from_bytes(bytes, sizeof(bytes) - 1U);
    buffer.undo = yew_undo_new(buffer.tb);
    yew_undo_mark_saved(buffer.undo);
    buffer.meta.eol = YEW_EOL_LF;
    win.buf = &buffer;
    yew_cset_init(&win.cs, cursor);
    yew_vp_init(&win);
    win.vp.rows = 5U;
    win.vp.cols = 12U;
    ed.mode = YEW_MODE_L;
    ed.prev_unit = YEW_MODE_L;
    ed.win = &win;
    bufptrs[0] = &buffer;
    ed.ws.bufs = bufptrs;
    ed.ws.nbufs = 1U;
    ed.footer_rect = (Rect){0U, 5U, 12U, 1U};

    yew_msg_at(&ed, YEW_MSG_WARN, 0,
               "abcdefghijklmnopqrstuvwxyz");
    yew_message_draw(&ed, &win);
    YEW_ASSERT(ed.msg.truncated);
    cell = &ed.grid.back[5U * ed.grid.cols];
    YEW_ASSERT_EQ_U64(cell[0].utf8[0], (u8)' ');
    YEW_ASSERT_EQ_U64(cell[1].utf8[0], (u8)'L');
    YEW_ASSERT_EQ_U64(cell[2].utf8[0], (u8)' ');
    YEW_ASSERT_EQ_U64(cell[3].utf8[0], (u8)'a');
    YEW_ASSERT_EQ_U64(cell[10].utf8[0], (u8)'h');
    YEW_ASSERT_EQ_U64(cell[11].utf8[0], 0xe2U);
    YEW_ASSERT_EQ_U64(cell[3].fg.tag, YEW_COLOR_INDEXED);
    YEW_ASSERT((cell[3].attrs & YEW_ATTR_BOLD) != 0U);

    YEW_ASSERT(yew_msg_expand(&ed));
    yew_message_draw(&ed, &win);
    YEW_ASSERT_EQ_U64(ed.grid.back[3U * ed.grid.cols + 3U].utf8[0],
                      (u8)'a');
    YEW_ASSERT_EQ_U64(ed.grid.back[4U * ed.grid.cols + 3U].utf8[0],
                      (u8)'j');
    YEW_ASSERT_EQ_U64(ed.grid.back[5U * ed.grid.cols + 3U].utf8[0],
                      (u8)'s');
    YEW_ASSERT_EQ_U64(ed.grid.back[5U * ed.grid.cols + 1U].utf8[0],
                      (u8)'L');
    YEW_ASSERT_EQ_U64(ed.grid.back[5U * ed.grid.cols].bg.tag,
                      YEW_COLOR_RGB);

    ed.rec.active = true;
    ed.rec.reg = (u8)'a';
    yew_message_draw(&ed, &win);
    cell = &ed.grid.back[5U * ed.grid.cols];
    YEW_ASSERT_EQ_U64(cell[3].utf8[0], 0xe2U);
    YEW_ASSERT_EQ_U64(cell[4].utf8[0], (u8)'R');
    YEW_ASSERT_EQ_U64(cell[5].utf8[0], (u8)'E');
    YEW_ASSERT_EQ_U64(cell[6].utf8[0], (u8)'C');
    YEW_ASSERT_EQ_U64(cell[7].utf8[0], (u8)' ');
    YEW_ASSERT_EQ_U64(cell[8].utf8[0], (u8)'a');
    YEW_ASSERT((cell[3].attrs & YEW_ATTR_BLINK) == 0U);

    yew_msg_clear(&ed);
    yew_vp_free(&win);
    yew_cset_free(&win.cs);
    yew_undo_free(buffer.undo);
    yew_textbuf_free(buffer.tb);
    yew_grid_free(&ed.grid);
    yew_timers_free(&ed.timers);
    interner_free(&ed.interner);
    arena_free_all(&ed.arena);
}
