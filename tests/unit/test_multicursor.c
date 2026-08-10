#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "text/journal.h"
#include "text/mark.h"
#include "ui/statusline.h"
#include "util/buf.h"

static Cursor test_cursor(u64 pos, u64 anchor, u64 goal)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(pos);
    cursor.anchor = BYTEOFF(anchor);
    cursor.goal_col = (GCol){goal};
    return cursor;
}

static void assert_cursor(const Cursor *cursor, u64 pos, u64 anchor,
                          u64 goal)
{
    YEW_ASSERT_EQ_U64(cursor->pos.v, pos);
    YEW_ASSERT_EQ_U64(cursor->anchor.v, anchor);
    YEW_ASSERT_EQ_U64(cursor->goal_col.v, goal);
}

static void assert_mc_text(const TextBuf *tb, const u8 *want, u64 want_len)
{
    TextIter it;
    u64 done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

void test_multicursor_lifecycle_and_point_dedupe(void)
{
    CursorSet set;

    yew_cset_init(&set, test_cursor(10U, 10U, 4U));
    YEW_ASSERT_EQ_U64(set.curs.len, 1U);
    YEW_ASSERT_EQ_U64(set.primary, 0U);
    YEW_ASSERT(yew_cset_add(&set, test_cursor(5U, 5U, 2U)));
    YEW_ASSERT_EQ_U64(set.curs.len, 2U);
    YEW_ASSERT_EQ_U64(set.primary, 1U);
    assert_cursor(&set.curs.data[0], 5U, 5U, 2U);
    assert_cursor(&set.curs.data[1], 10U, 10U, 4U);

    YEW_ASSERT(!yew_cset_add(&set, test_cursor(10U, 10U, 99U)));
    YEW_ASSERT_EQ_U64(set.curs.len, 2U);
    YEW_ASSERT_EQ_U64(set.primary, 1U);
    assert_cursor(&set.curs.data[1], 10U, 10U, 4U);
    YEW_ASSERT(!yew_cset_add(&set, test_cursor(5U, 5U, 77U)));
    assert_cursor(&set.curs.data[0], 5U, 5U, 2U);
    yew_cset_check(&set);

    yew_cset_free(&set);
    YEW_ASSERT_NULL(set.curs.data);
    YEW_ASSERT_EQ_U64(set.curs.len, 0U);
    YEW_ASSERT_EQ_U64(set.curs.cap, 0U);
}

void test_multicursor_latest_cap_and_active_tracking(void)
{
    CursorSet set;
    Cursor refused = test_cursor(20U, 20U, 20U);

    yew_cset_init(&set, test_cursor(10U, 10U, 10U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(5U, 5U, 5U)));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(15U, 15U, 15U)));
    YEW_ASSERT(yew_cset_drop_latest(&set));
    YEW_ASSERT_EQ_U64(set.curs.len, 2U);
    assert_cursor(&set.curs.data[0], 5U, 5U, 5U);
    assert_cursor(&set.curs.data[1], 10U, 10U, 10U);
    YEW_ASSERT(!yew_cset_add_many(&set, &refused, YEW_MC_MAX));
    YEW_ASSERT_EQ_U64(set.curs.len, 2U);

    YEW_ASSERT(yew_cset_add(&set, test_cursor(6U, 6U, 6U)));
    set.active = 1U;
    yew_cset_adjust(&set, YEW_JOURNAL_DEL, BYTEOFF(5U), 2U);
    YEW_ASSERT_EQ_U64(set.curs.len, 2U);
    YEW_ASSERT_EQ_U64(set.active, 0U);
    assert_cursor(&set.curs.data[0], 5U, 5U, 5U);
    assert_cursor(&set.curs.data[1], 8U, 8U, 10U);
    set.active = YEW_MC_ACTIVE_NONE;
    yew_cset_check(&set);
    yew_cset_free(&set);
}

static CmdStatus mc_probe_insert(CmdCtx *cx)
{
    EditCtx ec;
    Cursor *cursor;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        (size_t)cx->cursor_index >= cx->win->cs.curs.len)
        return YEW_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->cursor_index];
    ec = yew_ed_edit_ctx(cx->ed);
    return yew_edit_insert(&ec, cursor->pos, (const u8 *)"X", 1U)
               ? YEW_CMD_OK
               : YEW_CMD_ERR_IO;
}

static CmdStatus mc_probe_fail_second(CmdCtx *cx)
{
    if (cx != NULL && cx->cursor_index == 1U)
        return YEW_CMD_ERR_STATE;
    return mc_probe_insert(cx);
}

static CmdId mc_probe_id(void)
{
    static CmdId id;

    if (id.v == 0U) {
        static const CmdDesc desc = {
            "ed.edit.mc.insert", mc_probe_insert, YEW_ARITY_NONE,
            YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
            "Insert one multicursor test byte", NULL
        };

        id = yew_cmd_register(&desc);
    }
    return id;
}

static CmdId mc_probe_fail_id(void)
{
    static CmdId id;

    if (id.v == 0U) {
        static const CmdDesc desc = {
            "ed.edit.mc.cancel", mc_probe_fail_second, YEW_ARITY_NONE,
            YEW_CMD_NEEDS_WIN | YEW_CMD_CHANGES_BUFFER,
            "Fail the second multicursor test edit", NULL
        };

        id = yew_cmd_register(&desc);
    }
    return id;
}

static void mc_ed_init(Ed *ed, Win *win, const u8 *bytes, u64 len)
{
    Cursor cursor = test_cursor(0U, 0U, 0U);

    (void)memset(ed, 0, sizeof(*ed));
    (void)memset(win, 0, sizeof(*win));
    ed->buffer.tb = yew_textbuf_from_bytes(bytes, len);
    ed->buffer.undo = yew_undo_new(ed->buffer.tb);
    ed->buffer.marks = yew_marks_new();
    yew_timers_init(&ed->timers);
    win->buf = &ed->buffer;
    yew_cset_init(&win->cs, cursor);
    ed->win = win;
    ed->model_ready = true;
}

static void mc_ed_free(Ed *ed, Win *win)
{
    yew_msg_clear(ed);
    yew_timers_free(&ed->timers);
    yew_cset_free(&win->cs);
    yew_marks_free(ed->buffer.marks);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
}

static CmdId mc_delete_grapheme_id(void)
{
    CmdId id = yew_cmd_lookup("ed.edit.delete.grapheme",
                              (u32)strlen("ed.edit.delete.grapheme"));

    YEW_ASSERT(id.v != 0U);
    return id;
}

void test_multicursor_count_repeats_per_cursor(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};

    mc_ed_init(&ed, &win, (const u8 *)"abcdefghij", 10U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(5U, 5U, 5U)));
    cx.win = &win;
    cx.count = 2U;
    cx.count_given = true;
    cx.source = YEW_SRC_TEST;

    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, mc_delete_grapheme_id(), &cx),
                      YEW_CMD_OK);
    assert_mc_text(ed.buffer.tb, (const u8 *)"cdehij", 6U);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 2U);
    assert_cursor(&win.cs.curs.data[0], 0U, 0U, 0U);
    assert_cursor(&win.cs.curs.data[1], 3U, 3U, 5U);
    mc_ed_free(&ed, &win);
}

void test_multicursor_adjacent_edit_ranges_both_run(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};

    mc_ed_init(&ed, &win, (const u8 *)"abcdef", 6U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(3U, 3U, 3U)));
    cx.win = &win;
    cx.count = 3U;
    cx.count_given = true;
    cx.source = YEW_SRC_TEST;

    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, mc_delete_grapheme_id(), &cx),
                      YEW_CMD_OK);
    assert_mc_text(ed.buffer.tb, (const u8 *)"", 0U);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 1U);
    assert_cursor(&win.cs.curs.data[0], 0U, 0U, 0U);
    YEW_ASSERT_EQ_STR(ed.msg.text, "1 cursor merged");
    mc_ed_free(&ed, &win);
}

void test_multicursor_overlapping_edit_skips_later_and_reports_merge(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};

    mc_ed_init(&ed, &win, (const u8 *)"abcdef", 6U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(1U, 1U, 1U)));
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(2U, 2U, 2U)));
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(3U, 3U, 3U)));
    cx.win = &win;
    cx.count = 4U;
    cx.count_given = true;
    cx.source = YEW_SRC_TEST;

    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, mc_delete_grapheme_id(), &cx),
                      YEW_CMD_OK);
    assert_mc_text(ed.buffer.tb, (const u8 *)"ef", 2U);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 1U);
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_STR(ed.msg.text, "3 cursors merged");
    mc_ed_free(&ed, &win);
}

void test_multicursor_backward_overlap_uses_original_edit_ranges(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};
    CmdId command = yew_cmd_lookup("ed.edit.delete.grapheme_left", 28U);

    mc_ed_init(&ed, &win, (const u8 *)"abcdef", 6U);
    win.cs.curs.data[0] = test_cursor(2U, 2U, 20U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(3U, 3U, 30U)));
    cx.win = &win;
    cx.count = 2U;
    cx.count_given = true;
    cx.source = YEW_SRC_TEST;

    YEW_ASSERT(command.v != 0U);
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, command, &cx), YEW_CMD_OK);
    assert_mc_text(ed.buffer.tb, (const u8 *)"cdef", 4U);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 1U);
    assert_cursor(&win.cs.curs.data[0], 0U, 0U, 20U);
    YEW_ASSERT_EQ_STR(ed.msg.text, "1 cursor merged");
    mc_ed_free(&ed, &win);
}

void test_multicursor_run_commits_one_undo_transaction(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};
    EditCtx ec;
    const UndoNode *node;

    mc_ed_init(&ed, &win, (const u8 *)"ab", 2U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(2U, 2U, 2U)));
    cx.ed = &ed;
    cx.win = &win;
    cx.count = 1U;
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT_EQ_I64(yew_mc_run(&win, mc_probe_id(), &cx),
                      YEW_CMD_ERR_STATE);
    assert_mc_text(ed.buffer.tb, (const u8 *)"ab", 2U);
    yew_undo_begin(&ec, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_I64(yew_mc_run(&win, mc_probe_id(), &cx), YEW_CMD_OK);
    assert_mc_text(ed.buffer.tb, (const u8 *)"XabX", 4U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->depth, 1U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 2U);
    YEW_ASSERT(ed.buffer.undo->open != 0U);
    node = &ed.buffer.undo->nodes.data[1];
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);
    yew_undo_end(&ec);
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 2U);
    node = &ed.buffer.undo->nodes.data[1];
    YEW_ASSERT_EQ_U64(node->reason, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);
    YEW_ASSERT(yew_undo(&ec));
    assert_mc_text(ed.buffer.tb, (const u8 *)"ab", 2U);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 2U);
    mc_ed_free(&ed, &win);
}

void test_multicursor_run_rolls_back_every_cursor(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};
    EditCtx ec;

    mc_ed_init(&ed, &win, (const u8 *)"ab", 2U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(2U, 2U, 2U)));
    cx.ed = &ed;
    cx.win = &win;
    cx.count = 1U;
    ec = yew_ed_edit_ctx(&ed);
    yew_undo_begin(&ec, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_I64(yew_mc_run(&win, mc_probe_fail_id(), &cx),
                      YEW_CMD_ERR_STATE);
    assert_mc_text(ed.buffer.tb, (const u8 *)"Xab", 3U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->depth, 1U);
    yew_undo_abort(&ec);
    yew_ed_finish_edit(&ed, &ec);
    assert_mc_text(ed.buffer.tb, (const u8 *)"ab", 2U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 2U);
    assert_cursor(&win.cs.curs.data[0], 0U, 0U, 0U);
    assert_cursor(&win.cs.curs.data[1], 2U, 2U, 2U);
    mc_ed_free(&ed, &win);
}

void test_multicursor_editor_invoke_rolls_back_on_per_cursor_failure(void)
{
    Ed ed;
    Win win;
    CmdCtx cx = {0};

    mc_ed_init(&ed, &win, (const u8 *)"ab", 2U);
    YEW_ASSERT(yew_cset_add(&win.cs, test_cursor(2U, 2U, 2U)));
    cx.ed = &ed;
    cx.win = &win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&ed, mc_probe_fail_id(), &cx),
                      YEW_CMD_ERR_STATE);
    assert_mc_text(ed.buffer.tb, (const u8 *)"ab", 2U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root);
    YEW_ASSERT_EQ_U64(win.cs.curs.len, 2U);
    assert_cursor(&win.cs.curs.data[0], 0U, 0U, 0U);
    assert_cursor(&win.cs.curs.data[1], 2U, 2U, 2U);
    mc_ed_free(&ed, &win);
}

static void mc_sibling_path(char *out, size_t cap, const char *name)
{
    const char *program = yew_test_program_path();
    const char *slash = strrchr(program, '/');
    int count;

    if (slash == NULL)
        count = snprintf(out, cap, "./%s", name);
    else if (slash == program)
        count = snprintf(out, cap, "/%s", name);
    else
        count = snprintf(out, cap, "%.*s/%s", (int)(slash - program),
                         program, name);
    YEW_ASSERT(count > 0 && (size_t)count < cap);
}

static size_t mc_log_count(const char *path, const char *needle)
{
    FILE *stream = fopen(path, "rb");
    char line[192];
    size_t count = 0U;

    YEW_ASSERT_NOT_NULL(stream);
    while (fgets(line, sizeof(line), stream) != NULL) {
        if (strstr(line, needle) != NULL)
            count++;
    }
    YEW_ASSERT_EQ_I64(fclose(stream), 0);
    return count;
}

static void mc_sync_child(void)
{
    const char *root = getenv("YEW_MC_SYNC_ROOT");
    u8 initial[400];
    char source[PATH_MAX];
    FileMeta meta;
    Ed ed;
    CmdCtx cx = {0};
    CmdId insert;
    EditCtx ec;
    Journal *journal;
    size_t i;
    int count;

    YEW_ASSERT_NOT_NULL(root);
    for (i = 0U; i < sizeof(initial); i += 2U) {
        initial[i] = (u8)'a';
        initial[i + 1U] = (u8)'\n';
    }
    count = snprintf(source, sizeof(source), "%s/source.txt", root);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(source));
    yew_filemeta_init(&meta);
    meta.exists = true;
    meta.size_on_disk = sizeof(initial);
    journal = yew_journal_open(source, &meta);
    YEW_ASSERT_NOT_NULL(journal);

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_undo_free(ed.buffer.undo);
    yew_textbuf_free(ed.buffer.tb);
    ed.buffer.tb = yew_textbuf_from_bytes(initial, sizeof(initial));
    ed.buffer.undo = yew_undo_new(ed.buffer.tb);
    ed.buffer.jrn = journal;
    ed.win->buf = &ed.buffer;
    ed.win->cs.curs.data[0] = test_cursor(0U, 0U, 0U);
    for (i = 1U; i < 200U; i++)
        YEW_ASSERT(yew_cset_add(&ed.win->cs,
                                test_cursor(i * 2U, i * 2U, 0U)));
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.len, 200U);

    insert = yew_cmd_lookup("ed.edit.insert.text", 19U);
    YEW_ASSERT(insert.v != 0U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.sarg = "X";
    cx.sarg_len = 1U;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(setenv("YEW_FAULT_ENABLE", "1", 1), 0);
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&ed, insert, &cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAULT_ENABLE", "0", 1), 0);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.buffer.tb), 600U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.data[1].reason, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->nodes.data[1].n_ops, 200U);

    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&ec));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.buffer.tb), sizeof(initial));
    assert_mc_text(ed.buffer.tb, initial, sizeof(initial));
    ed.buffer.jrn = NULL;
    yew_journal_discard(journal);
    yew_ed_free(&ed);
    yew_filemeta_dispose(&meta);
}

/*
 * LD_PRELOAD with the shim first puts it ahead of the ASan runtime in
 * the child's initial library list; ASan then installs no interceptors
 * and the fault log stays empty.  Same reason kill9 takes a prefix.
 */
static int mc_set_preload(const char *shim)
{
#ifdef YEW_ASAN_RUNTIME
    char joined[PATH_MAX * 2];
    int n = snprintf(joined, sizeof(joined), "%s:%s", YEW_ASAN_RUNTIME,
                     shim);

    if (n <= 0 || (size_t)n >= sizeof(joined))
        return -1;
    return setenv("LD_PRELOAD", joined, 1);
#else
    return setenv("LD_PRELOAD", shim, 1);
#endif
}

void test_multicursor_200_insert_is_one_undo_and_one_journal_sync(void)
{
    const char *child_mode = getenv("YEW_MC_SYNC_CHILD");
    char root[] = "/tmp/yew-mc-sync-XXXXXX";
    char state[PATH_MAX];
    char log[PATH_MAX];
    char journal_dir[PATH_MAX];
    char yew_dir[PATH_MAX];
    char shim[PATH_MAX];
    pid_t child;
    pid_t waited;
    int status;
    int count;

    if (child_mode != NULL && strcmp(child_mode, "1") == 0) {
        mc_sync_child();
        return;
    }
    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    count = snprintf(state, sizeof(state), "%s/state", root);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(state));
    count = snprintf(log, sizeof(log), "%s/intercept.log", root);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(log));
    YEW_ASSERT_EQ_I64(mkdir(state, 0700), 0);
    mc_sibling_path(shim, sizeof(shim), "tests/torture/faultshim.so");

    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        if (setenv("YEW_MC_SYNC_CHILD", "1", 1) != 0 ||
            setenv("YEW_MC_SYNC_ROOT", root, 1) != 0 ||
            setenv("XDG_STATE_HOME", state, 1) != 0 ||
            setenv("YEW_FAULT_LOG", log, 1) != 0 ||
            setenv("YEW_FAULT_ENABLE", "0", 1) != 0 ||
            setenv("YEW_LOG", "/dev/null", 1) != 0 ||
            mc_set_preload(shim) != 0)
            _exit(126);
        execl(yew_test_program_path(), yew_test_program_path(), "--filter",
              "multicursor_200_insert_is_one_undo_and_one_journal_sync",
              (char *)NULL);
        _exit(126);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    YEW_ASSERT_EQ_U64(mc_log_count(log, "fsync-file pass"), 1U);
    YEW_ASSERT_EQ_U64(mc_log_count(log, "fdatasync-file pass"), 0U);

    count = snprintf(journal_dir, sizeof(journal_dir),
                     "%s/yew/journal", state);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(journal_dir));
    count = snprintf(yew_dir, sizeof(yew_dir), "%s/yew", state);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(yew_dir));
    YEW_ASSERT_EQ_I64(unlink(log), 0);
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(state), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}

void test_multicursor_selection_union_semantics(void)
{
    CursorSet primary_union;
    CursorSet earlier_union;
    CursorSet chained_union;

    yew_cset_init(&primary_union, test_cursor(8U, 2U, 80U));
    YEW_ASSERT(!yew_cset_add(&primary_union,
                             test_cursor(5U, 10U, 50U)));
    YEW_ASSERT_EQ_U64(primary_union.curs.len, 1U);
    YEW_ASSERT_EQ_U64(primary_union.primary, 0U);
    assert_cursor(&primary_union.curs.data[0], 10U, 2U, 80U);

    yew_cset_init(&earlier_union, test_cursor(100U, 100U, 100U));
    YEW_ASSERT(yew_cset_add(&earlier_union, test_cursor(8U, 2U, 80U)));
    YEW_ASSERT(!yew_cset_add(&earlier_union,
                             test_cursor(5U, 12U, 50U)));
    YEW_ASSERT_EQ_U64(earlier_union.curs.len, 2U);
    YEW_ASSERT_EQ_U64(earlier_union.primary, 1U);
    assert_cursor(&earlier_union.curs.data[0], 2U, 12U, 50U);
    assert_cursor(&earlier_union.curs.data[1], 100U, 100U, 100U);

    /* This pins interval-component merging when a later union expands
     * backwards over a point that did not overlap the first selection. */
    yew_cset_init(&chained_union, test_cursor(200U, 200U, 200U));
    YEW_ASSERT(yew_cset_add(&chained_union, test_cursor(5U, 5U, 5U)));
    YEW_ASSERT(yew_cset_add(&chained_union, test_cursor(10U, 100U, 10U)));
    YEW_ASSERT(!yew_cset_add(&chained_union,
                             test_cursor(50U, 0U, 50U)));
    YEW_ASSERT_EQ_U64(chained_union.curs.len, 2U);
    assert_cursor(&chained_union.curs.data[0], 100U, 0U, 5U);
    assert_cursor(&chained_union.curs.data[1], 200U, 200U, 200U);

    yew_cset_free(&chained_union);
    yew_cset_free(&earlier_union);
    yew_cset_free(&primary_union);
}

void test_multicursor_touch_and_adjacency(void)
{
    CursorSet touching;
    CursorSet adjacent;
    CursorSet edge_point;

    yew_cset_init(&touching, test_cursor(2U, 0U, 2U));
    YEW_ASSERT(!yew_cset_add(&touching, test_cursor(4U, 2U, 4U)));
    assert_cursor(&touching.curs.data[0], 4U, 0U, 2U);

    yew_cset_init(&adjacent, test_cursor(1U, 0U, 1U));
    YEW_ASSERT(yew_cset_add(&adjacent, test_cursor(3U, 2U, 3U)));
    YEW_ASSERT_EQ_U64(adjacent.curs.len, 2U);
    assert_cursor(&adjacent.curs.data[0], 1U, 0U, 1U);
    assert_cursor(&adjacent.curs.data[1], 3U, 2U, 3U);

    yew_cset_init(&edge_point, test_cursor(0U, 0U, 7U));
    YEW_ASSERT(!yew_cset_add(&edge_point, test_cursor(4U, 0U, 4U)));
    YEW_ASSERT_EQ_U64(edge_point.curs.len, 1U);
    assert_cursor(&edge_point.curs.data[0], 4U, 0U, 7U);

    yew_cset_free(&edge_point);
    yew_cset_free(&adjacent);
    yew_cset_free(&touching);
}

void test_multicursor_deleted_cursor_collapses_and_merges(void)
{
    CursorSet set;

    yew_cset_init(&set, test_cursor(7U, 7U, 70U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(9U, 9U, 90U)));
    yew_cset_adjust(&set, YEW_JOURNAL_DEL, BYTEOFF(6U), 4U);

    YEW_ASSERT_EQ_U64(set.curs.len, 1U);
    assert_cursor(&set.curs.data[0], 6U, 6U, 70U);
    yew_cset_free(&set);
}

void test_multicursor_coincident_after_edit_keeps_lower_goal(void)
{
    CursorSet set;

    yew_cset_init(&set, test_cursor(20U, 20U, 200U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(2U, 2U, 20U)));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(4U, 4U, 40U)));
    yew_cset_adjust(&set, YEW_JOURNAL_DEL, BYTEOFF(2U), 3U);

    YEW_ASSERT_EQ_U64(set.curs.len, 2U);
    YEW_ASSERT_EQ_U64(set.primary, 1U);
    assert_cursor(&set.curs.data[0], 2U, 2U, 20U);
    assert_cursor(&set.curs.data[1], 17U, 17U, 200U);
    yew_cset_free(&set);
}

void test_multicursor_coincident_after_edit_primary_wins(void)
{
    CursorSet set;

    yew_cset_init(&set, test_cursor(4U, 4U, 40U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(2U, 2U, 20U)));
    yew_cset_adjust(&set, YEW_JOURNAL_DEL, BYTEOFF(2U), 3U);

    YEW_ASSERT_EQ_U64(set.curs.len, 1U);
    YEW_ASSERT_EQ_U64(set.primary, 0U);
    assert_cursor(&set.curs.data[0], 2U, 2U, 40U);
    yew_cset_free(&set);
}

void test_multicursor_touching_selections_merge_to_union(void)
{
    CursorSet set;

    yew_cset_init(&set, test_cursor(2U, 0U, 20U));
    YEW_ASSERT(!yew_cset_add(&set, test_cursor(5U, 2U, 50U)));

    YEW_ASSERT_EQ_U64(set.curs.len, 1U);
    assert_cursor(&set.curs.data[0], 5U, 0U, 20U);
    yew_cset_free(&set);
}

void test_multicursor_adjacent_selections_remain_distinct(void)
{
    CursorSet set;

    yew_cset_init(&set, test_cursor(1U, 0U, 10U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(3U, 2U, 30U)));

    YEW_ASSERT_EQ_U64(set.curs.len, 2U);
    assert_cursor(&set.curs.data[0], 1U, 0U, 10U);
    assert_cursor(&set.curs.data[1], 3U, 2U, 30U);
    yew_cset_free(&set);
}

void test_multicursor_all_merged_set_remains_valid_at_one(void)
{
    Ed ed;
    Win win;
    StatuslineText status;

    mc_ed_init(&ed, &win, (const u8 *)"abcdefg", 7U);
    win.cs.curs.data[0] = test_cursor(2U, 0U, 20U);
    YEW_ASSERT(!yew_cset_add(&win.cs, test_cursor(4U, 2U, 40U)));
    YEW_ASSERT(!yew_cset_add(&win.cs, test_cursor(6U, 4U, 60U)));

    YEW_ASSERT_EQ_U64(win.cs.curs.len, 1U);
    YEW_ASSERT_EQ_U64(win.cs.primary, 0U);
    yew_cset_check(&win.cs);
    ed.mode = YEW_MODE_L;
    win.buf->meta.eol = YEW_EOL_LF;
    win.vp.rows = 10U;
    yew_statusline_build(&ed, &win, 80U, &status);
    YEW_ASSERT_NULL(strstr(status.body, "\xC3\x97"));
    yew_statusline_text_free(&status);
    mc_ed_free(&ed, &win);
}

static void build_permutation(CursorSet *set, bool reverse)
{
    Cursor a = test_cursor(12U, 4U, 12U);
    Cursor b = test_cursor(10U, 15U, 10U);
    Cursor c = test_cursor(40U, 40U, 40U);

    yew_cset_init(set, test_cursor(30U, 30U, 30U));
    if (reverse) {
        (void)yew_cset_add(set, c);
        (void)yew_cset_add(set, b);
        (void)yew_cset_add(set, a);
    } else {
        (void)yew_cset_add(set, a);
        (void)yew_cset_add(set, b);
        (void)yew_cset_add(set, c);
    }
}

void test_multicursor_permutation_determinism(void)
{
    CursorSet forward;
    CursorSet reverse;
    size_t i;

    build_permutation(&forward, false);
    build_permutation(&reverse, true);
    YEW_ASSERT_EQ_U64(forward.curs.len, reverse.curs.len);
    YEW_ASSERT_EQ_U64(forward.primary, reverse.primary);
    for (i = 0U; i < forward.curs.len; i++) {
        assert_cursor(&forward.curs.data[i], reverse.curs.data[i].pos.v,
                      reverse.curs.data[i].anchor.v,
                      reverse.curs.data[i].goal_col.v);
    }
    yew_cset_free(&reverse);
    yew_cset_free(&forward);
}

void test_multicursor_adjust_bias_and_merge(void)
{
    CursorSet insert;
    CursorSet before;
    CursorSet deletion;
    CursorSet collapse;

    yew_cset_init(&insert, test_cursor(5U, 5U, 5U));
    yew_cset_adjust(&insert, YEW_JOURNAL_INS, BYTEOFF(5U), 3U);
    assert_cursor(&insert.curs.data[0], 8U, 8U, 5U);
    yew_cset_adjust(&insert, YEW_JOURNAL_INS, BYTEOFF(4U), 2U);
    assert_cursor(&insert.curs.data[0], 10U, 10U, 5U);

    yew_cset_init(&before, test_cursor(3U, 2U, 3U));
    yew_cset_adjust(&before, YEW_JOURNAL_INS, BYTEOFF(4U), 9U);
    assert_cursor(&before.curs.data[0], 3U, 2U, 3U);

    yew_cset_init(&deletion, test_cursor(12U, 5U, 12U));
    yew_cset_adjust(&deletion, YEW_JOURNAL_DEL, BYTEOFF(6U), 3U);
    assert_cursor(&deletion.curs.data[0], 9U, 5U, 12U);
    yew_cset_adjust(&deletion, YEW_JOURNAL_DEL, BYTEOFF(4U), 3U);
    assert_cursor(&deletion.curs.data[0], 6U, 4U, 12U);
    yew_cset_adjust(&deletion, YEW_JOURNAL_DEL, BYTEOFF(20U), 2U);
    assert_cursor(&deletion.curs.data[0], 6U, 4U, 12U);

    yew_cset_init(&collapse, test_cursor(9U, 9U, 90U));
    YEW_ASSERT(yew_cset_add(&collapse, test_cursor(7U, 7U, 70U)));
    yew_cset_adjust(&collapse, YEW_JOURNAL_DEL, BYTEOFF(6U), 4U);
    YEW_ASSERT_EQ_U64(collapse.curs.len, 1U);
    YEW_ASSERT_EQ_U64(collapse.primary, 0U);
    assert_cursor(&collapse.curs.data[0], 6U, 6U, 90U);

    yew_cset_free(&collapse);
    yew_cset_free(&deletion);
    yew_cset_free(&before);
    yew_cset_free(&insert);
}

static u64 multicursor_random(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u64 multicursor_oracle_adjust(u64 pos, bool right_bias, u8 op,
                                     u64 at, u64 len)
{
    if (op == YEW_JOURNAL_INS) {
        if (pos > at || (pos == at && right_bias))
            pos += len;
    } else if (pos >= at + len) {
        pos -= len;
    } else if (pos >= at) {
        pos = at;
    }
    return pos;
}

void test_multicursor_adjust_random_oracle(void)
{
    CursorSet set;
    u64 state = UINT64_C(0xd1b54a32d192ed03);
    u64 text_len = 1000U;
    u64 pos = 750U;
    u64 anchor = 250U;
    u64 i;

    yew_cset_init(&set, test_cursor(pos, anchor, 17U));
    for (i = 0U; i < 10000U; i++) {
        u8 op;
        u64 at;
        u64 len;
        bool collapsed = pos == anchor;

        if (text_len == 0U || (multicursor_random(&state) & 1U) == 0U) {
            op = YEW_JOURNAL_INS;
            at = multicursor_random(&state) % (text_len + 1U);
            len = multicursor_random(&state) % 9U;
            text_len += len;
        } else {
            op = YEW_JOURNAL_DEL;
            at = multicursor_random(&state) % text_len;
            len = 1U + multicursor_random(&state) % (text_len - at);
            if (len > 8U)
                len = 8U;
            text_len -= len;
        }
        pos = multicursor_oracle_adjust(pos, true, op, at, len);
        anchor = collapsed ? pos :
            multicursor_oracle_adjust(anchor, false, op, at, len);
        yew_cset_adjust(&set, op, BYTEOFF(at), len);
        YEW_ASSERT_EQ_U64(set.curs.len, 1U);
        YEW_ASSERT_EQ_U64(set.primary, 0U);
        assert_cursor(&set.curs.data[0], pos, anchor, 17U);
    }
    yew_cset_free(&set);
}

static void integrated_move(const TextBuf *tb, CursorSet *set, u64 motion)
{
    size_t i;

    for (i = 0U; i < set->curs.len; i++) {
        Cursor *cursor = &set->curs.data[i];

        switch (motion) {
        case 0U: yew_cursor_left(tb, cursor); break;
        case 1U: yew_cursor_right(tb, cursor); break;
        case 2U: yew_cursor_up(tb, cursor); break;
        case 3U: yew_cursor_down(tb, cursor); break;
        case 4U: yew_cursor_line_home(tb, cursor); break;
        default: yew_cursor_line_end(tb, cursor); break;
        }
    }
    yew_cset_normalize(tb, set);
}

static void integrated_assert(const TextBuf *tb, const CursorSet *set,
                              const MarkSet *marks, const MarkId *ids,
                              size_t mark_count)
{
    size_t i;

    yew_cset_check(set);
    YEW_ASSERT(set->curs.len != 0U);
    YEW_ASSERT((size_t)set->primary < set->curs.len);
    for (i = 0U; i < set->curs.len; i++) {
        YEW_ASSERT(yew_is_grapheme_boundary(tb, set->curs.data[i].pos));
        YEW_ASSERT(yew_is_grapheme_boundary(tb,
                                            set->curs.data[i].anchor));
    }
    for (i = 0U; i < mark_count; i++) {
        ByteOff pos = yew_mark_pos(marks, ids[i]);

        YEW_ASSERT(pos.v <= yew_textbuf_len(tb));
        YEW_ASSERT(yew_is_grapheme_boundary(tb, pos));
    }
}

void test_multicursor_integrated_edit_motion_fuzz(void)
{
    static const u64 seeds[] = {
        1U, UINT64_C(0x243f6a8885a308d3),
        UINT64_C(0x9e3779b97f4a7c15), UINT64_C(0xd1b54a32d192ed03)
    };
    static const u8 initial[] = "alpha\nbeta\ngamma\n";
    enum { MARK_COUNT = 24 };
    size_t seed_i;

    for (seed_i = 0U; seed_i < YEW_ARRAY_LEN(seeds); seed_i++) {
        TextBuf *tb = yew_textbuf_from_bytes(initial, sizeof(initial) - 1U);
        MarkSet *marks = yew_marks_new();
        MarkId ids[MARK_COUNT];
        CursorSet set;
        u64 state = seeds[seed_i];
        size_t i;

        yew_cset_init(&set, test_cursor(0U, 0U, 0U));
        YEW_ASSERT(yew_cset_add(&set, test_cursor(6U, 6U, 0U)));
        YEW_ASSERT(yew_cset_add(&set, test_cursor(11U, 11U, 0U)));
        for (i = 0U; i < MARK_COUNT; i++) {
            u64 pos = (u64)i % (yew_textbuf_len(tb) + 1U);

            ids[i] = yew_mark_add(marks, BYTEOFF(pos),
                                  (i & 1U) == 0U ? YEW_BIAS_LEFT
                                                : YEW_BIAS_RIGHT);
        }

        for (i = 0U; i < 10000U; i++) {
            u64 action = multicursor_random(&state) % 12U;

            if (action < 6U) {
                integrated_move(tb, &set, action);
            } else if (action == 6U || action == 7U) {
                static const u8 payloads[] = {'x', '\n'};
                ByteOff at = set.curs.data[set.primary].pos;
                const u8 *payload = &payloads[action - 6U];

                yew_textbuf_insert(tb, at, payload, 1U);
                yew_marks_adjust(marks, YEW_JOURNAL_INS, at, 1U);
                yew_cset_adjust(&set, YEW_JOURNAL_INS, at, 1U);
                yew_cset_normalize(tb, &set);
            } else if (action == 8U) {
                ByteOff at = set.curs.data[set.primary].pos;

                if (at.v < yew_textbuf_len(tb)) {
                    ByteOff end = yew_grapheme_next_boundary(tb, at);
                    u64 len = end.v - at.v;

                    yew_textbuf_delete(tb, (Span){at.v, end.v});
                    yew_marks_adjust(marks, YEW_JOURNAL_DEL, at, len);
                    yew_cset_adjust(&set, YEW_JOURNAL_DEL, at, len);
                    yew_cset_normalize(tb, &set);
                }
            } else if (action == 9U && set.curs.len < 16U) {
                u64 pos = multicursor_random(&state) %
                          (yew_textbuf_len(tb) + 1U);
                LineNo line = yew_textbuf_line_of(tb, BYTEOFF(pos));
                GCol goal = yew_off_to_gcol(tb,
                    yew_textbuf_line_span(tb, line), BYTEOFF(pos));

                (void)yew_cset_add(&set, test_cursor(pos, pos, goal.v));
            } else if (action == 10U && set.curs.len > 1U) {
                yew_cset_remove_all_but_primary(&set);
            } else {
                integrated_move(tb, &set,
                                multicursor_random(&state) % 6U);
            }
            integrated_assert(tb, &set, marks, ids, MARK_COUNT);
        }
        yew_cset_free(&set);
        yew_marks_free(marks);
        yew_textbuf_free(tb);
    }
}

void test_multicursor_remove_and_normalize_clamp(void)
{
    TextBuf *tb = yew_textbuf_from_bytes((const u8 *)"abcd", 4U);
    CursorSet set;

    yew_cset_init(&set, test_cursor(99U, 99U, 9U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(1U, 1U, 1U)));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(3U, 3U, 3U)));
    yew_cset_normalize(tb, &set);
    YEW_ASSERT_EQ_U64(set.curs.len, 3U);
    YEW_ASSERT_EQ_U64(set.primary, 2U);
    assert_cursor(&set.curs.data[2], 4U, 4U, 9U);

    yew_cset_remove_all_but_primary(&set);
    YEW_ASSERT_EQ_U64(set.curs.len, 1U);
    YEW_ASSERT_EQ_U64(set.primary, 0U);
    assert_cursor(&set.curs.data[0], 4U, 4U, 9U);
    yew_cset_free(&set);
    yew_textbuf_free(tb);
}

void test_multicursor_normalize_preserves_sticky_motion(void)
{
    TextBuf *tb = yew_textbuf_from_bytes((const u8 *)"abcdef\nxy", 9U);
    Cursor cursor = test_cursor(5U, 5U, 5U);
    CursorSet set;

    yew_cursor_down(tb, &cursor);
    assert_cursor(&cursor, 8U, 8U, 5U);

    yew_cset_init(&set, cursor);
    yew_cset_normalize(tb, &set);
    yew_cursor_right(tb, &set.curs.data[0]);
    assert_cursor(&set.curs.data[0], 9U, 9U, 2U);

    yew_cset_free(&set);
    yew_textbuf_free(tb);
}

void test_multicursor_edit_guard_requires_multi_transaction(void)
{
    CursorSet set;
    Bytebuf output;
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    ssize_t count;
    u8 chunk[256];

    yew_cset_init(&set, test_cursor(1U, 1U, 1U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(2U, 2U, 2U)));
    bytebuf_init(&output);
    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("YEW_LOG", "/dev/null", 1);
        yew_cset_require_single_edit(&set);
        _exit(0);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    YEW_ASSERT(strstr((const char *)output.data, "MULTI transaction") !=
               NULL);
    bytebuf_free(&output);
    yew_cset_free(&set);
}

typedef enum DeferredGuard {
    DEFERRED_REGEX_LIFT,
    DEFERRED_COMPLETION,
    DEFERRED_LSP_EDIT
} DeferredGuard;

static void assert_deferred_guard(DeferredGuard guard, const char *sprint)
{
    CursorSet set;
    Bytebuf output;
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    ssize_t count;
    u8 chunk[256];

    yew_cset_init(&set, test_cursor(1U, 1U, 1U));
    YEW_ASSERT(yew_cset_add(&set, test_cursor(2U, 2U, 2U)));
    bytebuf_init(&output);
    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("YEW_LOG", "/dev/null", 1);
        if (guard == DEFERRED_REGEX_LIFT)
            yew_mc_require_literal_lift(true);
        else if (guard == DEFERRED_COMPLETION)
            yew_mc_require_single_completion(&set);
        else
            yew_mc_require_single_lsp_edit(&set);
        _exit(0);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    YEW_ASSERT(strstr((const char *)output.data, sprint) != NULL);
    bytebuf_free(&output);
    yew_cset_free(&set);
}

void test_multicursor_deferred_guards_name_their_sprints(void)
{
    assert_deferred_guard(DEFERRED_REGEX_LIFT, "Sprint 21");
    assert_deferred_guard(DEFERRED_COMPLETION, "Sprint 43");
    assert_deferred_guard(DEFERRED_LSP_EDIT, "Sprint 47");
}
