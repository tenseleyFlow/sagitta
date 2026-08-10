#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/fltxn.h"
#include "fl/handle.h"
#include "ui/layout.h"

static void cmd_fixture(Ed *ed)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
}

static CmdStatus invoke(Ed *ed, Win *win, const char *name, i64 iarg,
                        const char *sarg, u32 sarg_len, Span span, bool bang)
{
    CmdCtx cx = {0};
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));

    SAG_ASSERT(id.v != 0U);
    cx.ed = ed;
    cx.win = win;
    cx.count = 1U;
    cx.iarg = iarg;
    cx.sarg = sarg;
    cx.sarg_len = sarg_len;
    cx.range.given = span.lo != 0U || span.hi != 0U;
    cx.range.tok = span;
    cx.bang = bang;
    cx.source = SAG_SRC_TEST;
    return sag_ed_invoke(ed, id, &cx);
}

static void temp_file(char path[64])
{
    static const char contents[] = "alpha\nbeta\n";
    int fd;

    (void)strcpy(path, "/tmp/sag-fl-cmds-XXXXXX");
    fd = mkstemp(path);
    SAG_ASSERT(fd >= 0);
    SAG_ASSERT_EQ_I64(write(fd, contents, sizeof(contents) - 1U),
                      (i64)(sizeof(contents) - 1U));
    SAG_ASSERT_EQ_I64(close(fd), 0);
}

void test_fl_cmd_buf_open_preserves_model_and_reuses_stable_buffer(void)
{
    Ed ed;
    char path[64];
    u32 primary_id;
    Buffer *opened;

    cmd_fixture(&ed);
    temp_file(path);
    primary_id = ed.buffer.id;
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.open", 0, path,
                             (u32)strlen(path), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    opened = sag_ed_doc(&ed);
    SAG_ASSERT(opened != &ed.buffer);
    SAG_ASSERT_EQ_U64(ed.buffer.id, primary_id);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(opened->tb), 11U);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.open", 0, path,
                             (u32)strlen(path), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    SAG_ASSERT(sag_ed_doc(&ed) == opened);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 2U);
    sag_ed_free(&ed);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
}

void test_fl_cmd_buf_close_retargets_every_view_and_kills_handles(void)
{
    Ed ed;
    char path[64];
    Buffer *opened;
    u32 opened_id;
    FlValue bh;
    FlValue sh;
    Pane *leaves[SAG_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    cmd_fixture(&ed);
    temp_file(path);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.open", 0, path,
                             (u32)strlen(path), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    opened = sag_ed_doc(&ed);
    opened_id = opened->id;
    bh = fl_h_buf_make(&ed, opened);
    sh = fl_h_span_make(&ed, opened, 0U, 5U);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H));
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.close", 0, NULL, 0U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT(!fl_h_alive(&ed.handles, bh));
    SAG_ASSERT(!fl_h_alive(&ed.handles, sh));
    SAG_ASSERT_NULL(sag_ws_buf_by_id(&ed, opened_id));
    sag_pane_collect_leaves(ed.pane_root, leaves, SAG_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++)
        SAG_ASSERT(leaves[i]->win->buf == &ed.buffer);
    sag_ed_free(&ed);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
}

void test_fl_cmd_buf_close_dirty_requires_force_and_reports_io(void)
{
    Ed ed;
    char path[64];
    char state[] = "/tmp/sag-fl-state-XXXXXX";
    char journal_dir[96];
    char sagitta_dir[80];
    const char *old_state = getenv("XDG_STATE_HOME");
    char *saved_state = old_state == NULL ? NULL : strdup(old_state);
    Buffer *opened;

    SAG_ASSERT_NOT_NULL(mkdtemp(state));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state, 1), 0);
    cmd_fixture(&ed);
    temp_file(path);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.open", 0, path,
                             (u32)strlen(path), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    opened = sag_ed_doc(&ed);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.edit.insert.at", 0, "x", 1U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT(sag_buf_dirty(opened));
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.close", 0, NULL, 0U,
                             (Span){0U, 0U}, false), SAG_CMD_ERR_IO);
    SAG_ASSERT_NOT_NULL(sag_ws_buf_by_id(&ed, opened->id));
    SAG_ASSERT_NOT_NULL(opened->jrn);
    sag_journal_discard(opened->jrn);
    opened->jrn = NULL;
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.close", 0, NULL, 0U,
                             (Span){0U, 0U}, true), SAG_CMD_OK);
    sag_ed_free(&ed);
    if (saved_state != NULL) {
        SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_state, 1), 0);
        free(saved_state);
    } else {
        SAG_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/sagitta/journal",
                   state);
    (void)snprintf(sagitta_dir, sizeof(sagitta_dir), "%s/sagitta", state);
    SAG_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(sagitta_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(state), 0);
}

void test_fl_cmd_buf_close_primary_retires_id_without_alias(void)
{
    Ed ed;
    u32 old_id;
    FlValue old;

    cmd_fixture(&ed);
    old_id = ed.buffer.id;
    old = fl_h_buf_make(&ed, &ed.buffer);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.close", 0, NULL, 0U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT(!fl_h_alive(&ed.handles, old));
    SAG_ASSERT(ed.buffer.id > old_id);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 0U);
    SAG_ASSERT(sag_ws_buf_by_id(&ed, old_id) == NULL);
    SAG_ASSERT(sag_ws_buf_by_id(&ed, ed.buffer.id) == &ed.buffer);
    sag_ed_free(&ed);
}

void test_fl_cmd_edit_rows_preserve_binary_and_unit_semantics(void)
{
    static const char binary[] = {'A', '\0', 'B'};
    Ed ed;

    cmd_fixture(&ed);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.edit.insert.at", 0, binary,
                             sizeof(binary), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), sizeof(binary));
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.edit.replace.span", 0, "xy",
                             2U, (Span){1U, 2U}, false), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 4U);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.cursor.set", 1, NULL, 0U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, 1U);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.mode.enter", 0, "C", 1U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.edit.delete.unit", 0, NULL,
                             0U, (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 3U);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.edit.delete.span", 0, NULL,
                             0U, (Span){1U, 3U}, false), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 1U);
    sag_ed_free(&ed);
}

void test_fl_cmd_close_refuses_an_enlisted_buffer(void)
{
    Ed ed;
    FlVm *vm;
    EditCtx ec;

    cmd_fixture(&ed);
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    ec = sag_ed_edit_ctx_for(&ed, ed.win);
    SAG_ASSERT(vm->host->run_begin(vm));
    SAG_ASSERT(fl_txn_enlist(vm, &ec));
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.close", 0, NULL, 0U,
                             (Span){0U, 0U}, true), SAG_CMD_ERR_STATE);
    SAG_ASSERT(sag_ws_buf_by_id(&ed, ed.buffer.id) == &ed.buffer);
    SAG_ASSERT(vm->host->run_end(vm, true));
    sag_ed_free(&ed);
}

void test_fl_cmd_pane_close_targets_the_supplied_window(void)
{
    Ed ed;
    Pane *split;
    Win *first;
    Win *second;
    u32 first_id;

    cmd_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    split = ed.pane_root;
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, split, SAG_SPLIT_H));
    first = split->a->win;
    second = split->b->win;
    first_id = first->id;
    ed.focus = split->b;
    ed.win = second;
    {
        Tab *tab = sag_tab_at(&ed, ed.tabs.active);
        SAG_ASSERT_NOT_NULL(tab);
        tab->focus = ed.focus;
    }
    SAG_ASSERT_EQ_I64(invoke(&ed, first, "ed.pane.close", 0, NULL, 0U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT_NULL(sag_ed_win_by_id(&ed, first_id));
    SAG_ASSERT(ed.win == second);
    SAG_ASSERT(ed.focus == ed.pane_root);
    sag_ed_free(&ed);
}

void test_fl_cmd_path_commands_reject_embedded_nul(void)
{
    static const char path[] = {'a', '\0', 'b'};
    Ed ed;

    cmd_fixture(&ed);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.open", 0, path,
                             (u32)sizeof(path), (Span){0U, 0U}, false),
                      SAG_CMD_ERR_ARG);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.file.write", 0, path,
                             (u32)sizeof(path), (Span){0U, 0U}, false),
                      SAG_CMD_ERR_ARG);
    sag_ed_free(&ed);
}

void test_fl_cmd_save_targets_a_nonfocused_buffer(void)
{
    Ed ed;
    char first_path[64];
    char second_path[64];
    Pane *split;
    Win *first;
    Win *second;
    int fd;
    char got[16] = {0};

    cmd_fixture(&ed);
    temp_file(first_path);
    temp_file(second_path);
    SAG_ASSERT_EQ_I64(invoke(&ed, ed.win, "ed.buf.open", 0, first_path,
                             (u32)strlen(first_path), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    split = ed.pane_root;
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, split, SAG_SPLIT_H));
    first = split->a->win;
    second = split->b->win;
    ed.focus = split->b;
    ed.win = second;
    {
        Tab *tab = sag_tab_at(&ed, ed.tabs.active);
        SAG_ASSERT_NOT_NULL(tab);
        tab->focus = ed.focus;
    }
    SAG_ASSERT_EQ_I64(invoke(&ed, second, "ed.buf.open", 0, second_path,
                             (u32)strlen(second_path), (Span){0U, 0U}, false),
                      SAG_CMD_OK);
    sag_textbuf_insert(first->buf->tb, BYTEOFF(0U), (const u8 *)"X", 1U);
    SAG_ASSERT_EQ_I64(invoke(&ed, first, "ed.file.save", 0, NULL, 0U,
                             (Span){0U, 0U}, false), SAG_CMD_OK);
    SAG_ASSERT(ed.win == second);
    SAG_ASSERT(second->buf != first->buf);
    fd = open(first_path, O_RDONLY);
    SAG_ASSERT(fd >= 0);
    SAG_ASSERT_EQ_I64(read(fd, got, sizeof(got)), 12);
    SAG_ASSERT_EQ_MEM(got, "Xalpha\nbeta\n", 12U);
    SAG_ASSERT_EQ_I64(close(fd), 0);
    sag_ed_free(&ed);
    SAG_ASSERT_EQ_I64(unlink(second_path), 0);
    SAG_ASSERT_EQ_I64(unlink(first_path), 0);
}
