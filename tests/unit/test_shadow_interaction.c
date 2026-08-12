/* Sprint 43: every non-accepting interaction has one pinned ghost rule. */
#include "harness.h"

#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/completion.h"
#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/shadow.h"
#include "term/grid.h"
#include "text/edit.h"
#include "ui/layout.h"
#include "ui/shadowdraw.h"
#include "ui/tabs.h"

void yew_test_shadow_providers_register(void);

static void interaction_fixture(Ed *ed, const u8 *bytes, size_t len,
                                u64 cursor)
{
    yew_test_shadow_providers_register();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "shadow-interaction"));
    ed->win->cs.curs.data[ed->win->cs.primary].pos = BYTEOFF(cursor);
    ed->win->cs.curs.data[ed->win->cs.primary].anchor = BYTEOFF(cursor);
}

static u32 interaction_deliver(Ed *ed, const char *text)
{
    ShadowSug suggestion = {0};
    Shadow *shadow = &ed->win->shadow;

    suggestion.seq = shadow->seq_next[YEW_SHADOW_INDEX]++;
    suggestion.prov = YEW_SHADOW_INDEX;
    suggestion.buf_id = ed->win->buf->id;
    suggestion.buf_gen = ed->win->buf->tb->gen;
    suggestion.pos = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    suggestion.text = (const u8 *)text;
    suggestion.len = (u32)strlen(text);
    yew_shadow_deliver(ed, &suggestion);
    YEW_ASSERT(ed->win->shadow.live);
    return suggestion.seq;
}

static CmdStatus interaction_invoke(Ed *ed, const char *name,
                                    CmdSource source, const char *sarg)
{
    CmdCtx command = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    command.win = ed->win;
    command.source = source;
    command.count = 1U;
    command.sarg = sarg;
    command.sarg_len = sarg == NULL ? 0U : (u32)strlen(sarg);
    return yew_ed_invoke(ed, id, &command);
}

void test_shadow_fully_typed_and_off_cursor_insert_follow_table(void)
{
    Ed ed;
    EditCtx edit;

    interaction_fixture(&ed, NULL, 0U, 0U);
    (void)interaction_deliver(&ed, "go");
    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"go", 2U));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    yew_ed_free(&ed);

    interaction_fixture(&ed, (const u8 *)"a", 1U, 1U);
    (void)interaction_deliver(&ed, "ghost");
    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"z", 1U));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_ed_free(&ed);
}

void test_shadow_backspace_motion_and_mode_change_follow_table(void)
{
    Ed ed;

    interaction_fixture(&ed, (const u8 *)"a", 1U, 1U);
    ed.mode = YEW_MODE_I;
    (void)interaction_deliver(&ed, "ghost");
    YEW_ASSERT_EQ_I64(
        interaction_invoke(&ed, "ed.edit.delete.grapheme_left",
                           YEW_SRC_TEST, NULL),
        YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_ed_free(&ed);

    interaction_fixture(&ed, (const u8 *)"a", 1U, 0U);
    (void)interaction_deliver(&ed, "ghost");
    YEW_ASSERT_EQ_I64(interaction_invoke(&ed, "ed.move.char.next",
                                         YEW_SRC_TEST, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_ed_free(&ed);

    interaction_fixture(&ed, NULL, 0U, 0U);
    (void)interaction_deliver(&ed, "ghost");
    YEW_ASSERT_EQ_I64(interaction_invoke(&ed, "ed.mode.enter",
                                         YEW_SRC_TEST, "I"),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    (void)interaction_deliver(&ed, "again");
    YEW_ASSERT_EQ_I64(yew_mode_enter_highlight(&ed, YEW_MODE_I, false),
                      YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    yew_ed_free(&ed);
}

void test_shadow_undo_redo_paste_replay_and_save_stay_quiet(void)
{
    Ed ed;
    EditCtx edit;

    interaction_fixture(&ed, NULL, 0U, 0U);
    edit = yew_ed_edit_ctx(&ed);
    yew_undo_begin(&edit, YEW_TXN_TYPE);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"a", 1U));
    yew_undo_end(&edit);
    yew_ed_finish_edit(&ed, &edit);
    yew_shadow_dismiss(&ed, ed.win);

    (void)interaction_deliver(&ed, "undo");
    YEW_ASSERT_EQ_I64(interaction_invoke(&ed, "ed.edit.undo",
                                         YEW_SRC_TEST, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    (void)interaction_deliver(&ed, "redo");
    YEW_ASSERT_EQ_I64(interaction_invoke(&ed, "ed.edit.redo",
                                         YEW_SRC_TEST, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    yew_ed_free(&ed);

    interaction_fixture(&ed, NULL, 0U, 0U);
    ed.mode = YEW_MODE_I;
    (void)interaction_deliver(&ed, "paste");
    yew_ed_handle_paste(&ed, NULL, 0U, false);
    YEW_ASSERT(ed.shadow_holdoff);
    yew_ed_handle_paste(&ed, (const u8 *)"p", 1U, false);
    yew_ed_handle_paste(&ed, NULL, 0U, true);
    YEW_ASSERT(!ed.shadow_holdoff);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);

    (void)interaction_deliver(&ed, "replay");
    YEW_ASSERT_EQ_I64(interaction_invoke(&ed, "ed.edit.insert.text",
                                         YEW_SRC_REPLAY, "r"),
                      YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);

    (void)interaction_deliver(&ed, "save");
    YEW_ASSERT(interaction_invoke(&ed, "ed.file.save", YEW_SRC_TEST,
                                  NULL) != YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    yew_ed_free(&ed);
}

void test_shadow_focus_and_buffer_switch_dismiss(void)
{
    Ed ed;
    Win *before;
    int next;

    interaction_fixture(&ed, NULL, 0U, 0U);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    before = ed.win;
    (void)interaction_deliver(&ed, "pane");
    YEW_ASSERT_EQ_I64(interaction_invoke(&ed, "ed.pane.split_h",
                                         YEW_SRC_TEST, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.win != before);
    YEW_ASSERT(!before->shadow.live);

    next = yew_tab_open(&ed, NULL);
    YEW_ASSERT(next >= 0);
    before = ed.win;
    (void)interaction_deliver(&ed, "tab");
    yew_tab_switch(&ed, next);
    YEW_ASSERT(ed.win != before);
    YEW_ASSERT(!before->shadow.live);
    yew_ed_free(&ed);
}

void test_shadow_completion_menu_suppresses_and_rearms_fresh(void)
{
    Ed ed;
    ShadowSug late = {0};
    u32 stale_seq;

    interaction_fixture(&ed, NULL, 0U, 0U);
    stale_seq = interaction_deliver(&ed, "old");
    yew_compl_open(&ed, ed.win);
    YEW_ASSERT(ed.win->compl.open);
    YEW_ASSERT(ed.win->shadow.suppressed);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);

    late.seq = stale_seq;
    late.prov = YEW_SHADOW_INDEX;
    late.buf_id = ed.win->buf->id;
    late.buf_gen = ed.win->buf->tb->gen;
    late.pos = BYTEOFF(0U);
    late.text = (const u8 *)"late";
    late.len = 4U;
    yew_shadow_deliver(&ed, &late);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.dropped_stale, 1U);

    yew_compl_close(&ed, ed.win);
    YEW_ASSERT(!ed.win->compl.open);
    YEW_ASSERT(!ed.win->shadow.suppressed);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_ed_free(&ed);
}

void test_shadow_menu_ghost_conflict_is_a_bug(void)
{
    pid_t child;
    pid_t waited;
    int status;

    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        Ed ed = {0};
        Win win = {0};
        Grid grid = {0};
        ShadowLayout layout = {0};

        (void)close(STDERR_FILENO);
        win.shadow.live = true;
        win.compl.open = true;
        layout.nlines = 1U;
        yew_shadow_draw(&ed, &win, &layout, &grid);
        _exit(99);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
}
