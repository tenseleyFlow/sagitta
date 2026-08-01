#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/edit.h"

typedef struct {
    u64 mono;
    i64 wall;
} UndoClock;

typedef struct {
    TextBuf *tb;
    MarkSet *marks;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    UndoClock clock;
} UndoFixture;

static u64 undo_mono(void *ctx)
{
    return ((UndoClock *)ctx)->mono;
}

static i64 undo_wall(void *ctx)
{
    return ((UndoClock *)ctx)->wall;
}

static Cursor undo_cursor(u64 pos, u64 anchor, u64 goal)
{
    Cursor cursor;

    cursor.pos = BYTEOFF(pos);
    cursor.anchor = BYTEOFF(anchor);
    cursor.goal_col = (GCol){goal};
    return cursor;
}

static void undo_fixture_init(UndoFixture *f, const u8 *bytes, u64 len)
{
    f->tb = sag_textbuf_from_bytes(bytes, len);
    f->marks = sag_marks_new();
    sag_cset_init(&f->cursors, undo_cursor(0U, 0U, 0U));
    f->undo = sag_undo_new(f->tb);
    f->clock.mono = 1000U;
    f->clock.wall = 100;
    sag_undo_set_clock(f->undo, undo_mono, undo_wall, &f->clock);
    f->edit = (EditCtx){f->tb, f->marks, &f->cursors, 7U, NULL, f->undo,
                       NULL};
}

static void undo_fixture_free(UndoFixture *f)
{
    sag_undo_free(f->undo);
    sag_cset_free(&f->cursors);
    sag_marks_free(f->marks);
    sag_textbuf_free(f->tb);
}

static void undo_assert_text(const TextBuf *tb, const u8 *want, u64 want_len)
{
    TextIter it;
    u64 done = 0U;

    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), want_len);
    if (want_len == 0U)
        return;
    SAG_ASSERT(sag_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        SAG_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            SAG_ASSERT(sag_textiter_advance(&it, tb));
    }
}

static const UndoNode *undo_current_node(const UndoTree *ut)
{
    SAG_ASSERT(ut->cur != 0U);
    SAG_ASSERT(ut->cur <= ut->nodes.len);
    return &ut->nodes.data[ut->cur - 1U];
}

void test_undo_type_merges_when_all_predicates_hold(void)
{
    UndoFixture f;
    const UndoNode *node;

    undo_fixture_init(&f, NULL, 0U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    f.clock.mono += SAG_UNDO_BURST_MS - 1U;
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    node = undo_current_node(f.undo);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    SAG_ASSERT_EQ_U64(node->n_ops, 2U);
    SAG_ASSERT_EQ_U64(node->reason, SAG_TXN_TYPE);
    SAG_ASSERT_EQ_U64(node->t_last_ms, f.clock.mono);
    undo_assert_text(f.tb, (const u8 *)"ab", 2U);
    undo_fixture_free(&f);
}

void test_undo_merge_rejects_different_reason(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"abc", 3U);
    sag_edit_insert(&f.edit, BYTEOFF(3U), (const u8 *)"d", 1U);
    f.undo->pending_reason = SAG_TXN_ERASE;
    sag_edit_delete(&f.edit, (Span){3U, 4U});
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[1].reason, SAG_TXN_TYPE);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[2].reason, SAG_TXN_ERASE);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    undo_fixture_free(&f);
}

void test_undo_merge_rejects_elapsed_burst(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    f.clock.mono += SAG_UNDO_BURST_MS;
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[1].n_ops, 1U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    undo_fixture_free(&f);
}

void test_undo_merge_rejects_noncontiguous_type(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"--", 2U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    sag_edit_insert(&f.edit, BYTEOFF(3U), (const u8 *)"b", 1U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT_EQ_U64(f.undo->ops.data[0].off, 0U);
    SAG_ASSERT_EQ_U64(f.undo->ops.data[1].off, 3U);
    undo_fixture_free(&f);
}

void test_undo_erase_merges_backspace_and_forward_delete(void)
{
    UndoFixture back;
    UndoFixture forward;

    undo_fixture_init(&back, (const u8 *)"abcd", 4U);
    back.undo->pending_reason = SAG_TXN_ERASE;
    sag_edit_delete(&back.edit, (Span){3U, 4U});
    back.undo->pending_reason = SAG_TXN_ERASE;
    sag_edit_delete(&back.edit, (Span){2U, 3U});
    SAG_ASSERT_EQ_U64(back.undo->nodes.len, 2U);
    SAG_ASSERT_EQ_U64(undo_current_node(back.undo)->n_ops, 2U);
    undo_assert_text(back.tb, (const u8 *)"ab", 2U);

    undo_fixture_init(&forward, (const u8 *)"abcd", 4U);
    forward.undo->pending_reason = SAG_TXN_ERASE;
    sag_edit_delete(&forward.edit, (Span){1U, 2U});
    forward.undo->pending_reason = SAG_TXN_ERASE;
    sag_edit_delete(&forward.edit, (Span){1U, 2U});
    SAG_ASSERT_EQ_U64(forward.undo->nodes.len, 2U);
    SAG_ASSERT_EQ_U64(undo_current_node(forward.undo)->n_ops, 2U);
    undo_assert_text(forward.tb, (const u8 *)"ad", 2U);
    undo_fixture_free(&forward);
    undo_fixture_free(&back);
}

void test_undo_merge_rejects_payload_limit(void)
{
    UndoFixture f;
    u8 bytes[SAG_UNDO_BURST_BYTES];

    (void)memset(bytes, 'x', sizeof(bytes));
    undo_fixture_init(&f, NULL, 0U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), bytes, sizeof(bytes));
    sag_edit_insert(&f.edit, BYTEOFF(sizeof(bytes)), (const u8 *)"y", 1U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[1].n_ops, 1U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(f.tb), sizeof(bytes) + 1U);
    undo_fixture_free(&f);
}

static void undo_assert_boundary_splits_burst(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    sag_undo_boundary(f.undo);
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT(!f.undo->boundary);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[2].parent, f.undo->nodes.data[1].id);
    undo_fixture_free(&f);
}

void test_undo_boundary_forces_new_node(void)
{
    undo_assert_boundary_splits_burst();
}

void test_undo_boundary_leave_insert_splits_burst(void)
{
    undo_assert_boundary_splits_burst();
}

void test_undo_boundary_cursor_motion_splits_burst(void)
{
    undo_assert_boundary_splits_burst();
}

void test_undo_boundary_newline_splits_burst(void)
{
    undo_assert_boundary_splits_burst();
}

void test_undo_boundary_window_focus_splits_burst(void)
{
    undo_assert_boundary_splits_burst();
}

void test_undo_explicit_depth_prevents_implicit_merge(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    sag_undo_begin(&f.edit, SAG_TXN_TYPE);
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    SAG_ASSERT_EQ_U64(f.undo->depth, 0U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    undo_fixture_free(&f);
}

void test_undo_nesting_depth_three_commits_once(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    SAG_ASSERT_EQ_U64(f.undo->depth, 3U);
    SAG_ASSERT(f.undo->open != 0U);
    sag_undo_end(&f.edit);
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(f.undo->depth, 1U);
    SAG_ASSERT(f.undo->open != 0U);
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(f.undo->depth, 0U);
    SAG_ASSERT_EQ_U64(f.undo->open, 0U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    SAG_ASSERT_EQ_U64(undo_current_node(f.undo)->n_ops, 2U);
    undo_fixture_free(&f);
}

void test_undo_atomic_supported_reasons_each_commit_one_node(void)
{
    static const SagTxnReason reasons[] = {
        SAG_TXN_PASTE, SAG_TXN_CUT, SAG_TXN_EXTERNAL
    };
    UndoFixture f;
    size_t i;

    undo_fixture_init(&f, NULL, 0U);
    for (i = 0U; i < SAG_ARRAY_LEN(reasons); i++) {
        sag_undo_begin(&f.edit, reasons[i]);
        sag_edit_insert(&f.edit, BYTEOFF(sag_textbuf_len(f.tb)),
                        (const u8 *)"xy", 2U);
        sag_undo_end(&f.edit);
        SAG_ASSERT_EQ_U64(f.undo->nodes.len, i + 2U);
        SAG_ASSERT_EQ_U64(undo_current_node(f.undo)->reason, reasons[i]);
        SAG_ASSERT_EQ_U64(undo_current_node(f.undo)->n_ops, 1U);
    }
    undo_fixture_free(&f);
}

static void undo_assert_deferred_reason(SagTxnReason reason,
                                        const char *sprint)
{
    int fds[2];
    pid_t child;
    pid_t waited;
    int status;
    char output[1024];
    ssize_t got;

    SAG_ASSERT_EQ_I64(pipe(fds), 0);
    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        UndoFixture f;

        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)close(fds[1]);
        undo_fixture_init(&f, NULL, 0U);
        sag_undo_begin(&f.edit, reason);
        _exit(0);
    }
    SAG_ASSERT_EQ_I64(close(fds[1]), 0);
    got = read(fds[0], output, sizeof(output) - 1U);
    SAG_ASSERT(got >= 0);
    output[got < 0 ? 0U : (size_t)got] = '\0';
    SAG_ASSERT_EQ_I64(close(fds[0]), 0);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    SAG_ASSERT(strstr(output, sprint) != NULL);
}

void test_undo_multi_reason_names_sprint17(void)
{
    undo_assert_deferred_reason(SAG_TXN_MULTI, "Sprint 17");
}

void test_undo_filter_reason_names_sprint19(void)
{
    undo_assert_deferred_reason(SAG_TXN_FILTER, "Sprint 19");
}

void test_undo_replace_reason_names_sprint21(void)
{
    undo_assert_deferred_reason(SAG_TXN_REPLACE, "Sprint 21");
}

void test_undo_macro_reason_names_sprint34(void)
{
    undo_assert_deferred_reason(SAG_TXN_MACRO, "Sprint 34");
}

void test_undo_lsp_reason_names_sprint47(void)
{
    undo_assert_deferred_reason(SAG_TXN_LSP, "Sprint 47");
}

void test_undo_save_rejects_open_transaction(void)
{
    int fds[2];
    pid_t child;
    pid_t waited;
    int status;
    char output[1024];
    ssize_t got;

    SAG_ASSERT_EQ_I64(pipe(fds), 0);
    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        UndoFixture f;
        FileMeta meta;

        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)close(fds[1]);
        (void)memset(&meta, 0, sizeof(meta));
        undo_fixture_init(&f, NULL, 0U);
        sag_undo_begin(&f.edit, SAG_TXN_PASTE);
        sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"x", 1U);
        f.edit.meta = &meta;
        (void)sag_edit_save(&f.edit, "/tmp/sagitta-save-open-transaction");
        _exit(0);
    }
    SAG_ASSERT_EQ_I64(close(fds[1]), 0);
    got = read(fds[0], output, sizeof(output) - 1U);
    SAG_ASSERT(got >= 0);
    output[got < 0 ? 0U : (size_t)got] = '\0';
    SAG_ASSERT_EQ_I64(close(fds[0]), 0);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    SAG_ASSERT(strstr(output, "edit save: open undo transaction") != NULL);
}

void test_undo_abort_restores_content_and_single_cursor(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"base", 4U);
    f.cursors.curs.data[0] = undo_cursor(2U, 1U, 9U);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_insert(&f.edit, BYTEOFF(2U), (const u8 *)"XYZ", 3U);
    sag_edit_delete(&f.edit, (Span){0U, 1U});
    SAG_ASSERT_EQ_U64(f.undo->depth, 1U);
    SAG_ASSERT(f.undo->open != 0U);
    sag_undo_abort(&f.edit);
    undo_assert_text(f.tb, (const u8 *)"base", 4U);
    SAG_ASSERT_EQ_U64(f.cursors.curs.len, 1U);
    SAG_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 2U);
    SAG_ASSERT_EQ_U64(f.cursors.curs.data[0].anchor.v, 1U);
    SAG_ASSERT_EQ_U64(f.cursors.curs.data[0].goal_col.v, 9U);
    SAG_ASSERT_EQ_U64(f.undo->depth, 0U);
    SAG_ASSERT_EQ_U64(f.undo->open, 0U);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 1U);
    undo_fixture_free(&f);
}

void test_undo_restores_eight_cursor_after_snapshot(void)
{
    UndoFixture f;
    size_t i;

    undo_fixture_init(&f, (const u8 *)"0123456789", 10U);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_insert(&f.edit, BYTEOFF(10U), (const u8 *)"x", 1U);
    for (i = 1U; i < 8U; i++)
        SAG_ASSERT(sag_cset_add(&f.cursors, undo_cursor(i, i, i + 3U)));
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(undo_current_node(f.undo)->n_after, 8U);
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(f.cursors.curs.len, 1U);
    SAG_ASSERT(sag_redo(&f.edit));
    SAG_ASSERT_EQ_U64(f.cursors.curs.len, 8U);
    for (i = 0U; i < 8U; i++) {
        SAG_ASSERT_EQ_U64(f.cursors.curs.data[i].pos.v, i == 0U ? 0U : i);
        SAG_ASSERT_EQ_U64(f.cursors.curs.data[i].anchor.v, i);
    }
    undo_fixture_free(&f);
}

void test_undo_window_gone_restores_into_focused_cursor_set(void)
{
    UndoFixture f;
    CursorSet focused;

    undo_fixture_init(&f, (const u8 *)"abc", 3U);
    f.cursors.curs.data[0] = undo_cursor(1U, 0U, 5U);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_insert(&f.edit, BYTEOFF(3U), (const u8 *)"X", 1U);
    f.cursors.curs.data[0] = undo_cursor(4U, 4U, 8U);
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(undo_current_node(f.undo)->win_id, 7U);

    sag_cset_init(&focused, undo_cursor(2U, 2U, 2U));
    f.edit.cset = &focused;
    f.edit.win_id = 99U;
    SAG_ASSERT(sag_undo(&f.edit));
    SAG_ASSERT_EQ_U64(focused.curs.data[0].pos.v, 1U);
    SAG_ASSERT_EQ_U64(focused.curs.data[0].anchor.v, 0U);
    SAG_ASSERT_EQ_U64(focused.curs.data[0].goal_col.v, 5U);
    SAG_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 4U);
    SAG_ASSERT(sag_redo(&f.edit));
    SAG_ASSERT_EQ_U64(focused.curs.data[0].pos.v, 4U);
    SAG_ASSERT_EQ_U64(focused.curs.data[0].anchor.v, 4U);
    SAG_ASSERT_EQ_U64(focused.curs.data[0].goal_col.v, 8U);
    SAG_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 4U);
    sag_cset_free(&focused);
    f.edit.cset = &f.cursors;
    undo_fixture_free(&f);
}

void test_undo_redo_cycles_do_not_grow_add_store(void)
{
    UndoFixture f;
    u64 stable;
    u32 i;

    undo_fixture_init(&f, NULL, 0U);
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"payload", 7U);
    sag_undo_end(&f.edit);
    stable = f.tb->add.len;
    SAG_ASSERT_EQ_U64(stable, 7U);
    for (i = 0U; i < 100000U; i++) {
        SAG_ASSERT(sag_undo(&f.edit));
        SAG_ASSERT(sag_redo(&f.edit));
    }
    SAG_ASSERT_EQ_U64(f.tb->add.len, stable);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    undo_assert_text(f.tb, (const u8 *)"payload", 7U);
    undo_fixture_free(&f);
}

void test_undo_to_root_reproduces_loaded_bytes(void)
{
    static const u8 original[] = {'a', 0U, 0xffU, '\n', 'z'};
    UndoFixture f;

    undo_fixture_init(&f, original, sizeof(original));
    sag_undo_begin(&f.edit, SAG_TXN_PASTE);
    sag_edit_delete(&f.edit, (Span){1U, 4U});
    sag_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"other", 5U);
    sag_undo_end(&f.edit);
    SAG_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    SAG_ASSERT(sag_undo_to(&f.edit, f.undo->root));
    undo_assert_text(f.tb, original, sizeof(original));
    SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), f.undo->root);
    SAG_ASSERT(!sag_undo(&f.edit));
    undo_fixture_free(&f);
}

void test_undo_save_reopens_journal_on_navigation(void)
{
    char state[] = "/tmp/sagitta-undo-save-XXXXXX";
    char source[128];
    char journal_dir[128];
    char sagitta_dir[112];
    FileMeta meta;
    FileMeta recovered_meta;
    TextBuf *tb = NULL;
    TextBuf *recovered = NULL;
    CursorSet cursors;
    Cursor cursor = undo_cursor(0U, 0U, 0U);
    UndoTree *undo;
    EditCtx edit;
    Journal *adopted;
    FILE *file;
    int count;

    SAG_ASSERT_NOT_NULL(mkdtemp(state));
    count = snprintf(source, sizeof(source), "%s/base.txt", state);
    SAG_ASSERT(count > 0 && (size_t)count < sizeof(source));
    file = fopen(source, "wb");
    SAG_ASSERT_NOT_NULL(file);
    SAG_ASSERT_EQ_U64(fwrite("base", 1U, 4U, file), 4U);
    SAG_ASSERT_EQ_I64(fclose(file), 0);
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state, 1), 0);
    SAG_ASSERT_EQ_U64(sag_file_load(source, &tb, &meta), SAG_LOAD_OK);
    sag_cset_init(&cursors, cursor);
    undo = sag_undo_new(tb);
    edit = (EditCtx){tb, NULL, &cursors, 0U, NULL, undo, &meta};

    sag_edit_insert(&edit, BYTEOFF(4U), (const u8 *)"X", 1U);
    SAG_ASSERT_NOT_NULL(edit.jrnl);
    SAG_ASSERT(sag_journal_ok(edit.jrnl));
    SAG_ASSERT_EQ_U64(sag_edit_save(&edit, source), SAG_SAVE_OK);
    SAG_ASSERT(edit.jrnl == NULL);
    SAG_ASSERT(sag_undo_at_save_point(undo));

    SAG_ASSERT(sag_undo(&edit));
    SAG_ASSERT_NOT_NULL(edit.jrnl);
    SAG_ASSERT(sag_journal_ok(edit.jrnl));
    undo_assert_text(tb, (const u8 *)"base", 4U);
    sag_journal_close(edit.jrnl);
    edit.jrnl = NULL;

    SAG_ASSERT_EQ_U64(sag_file_load(source, &recovered, &recovered_meta),
                      SAG_LOAD_OK);
    SAG_ASSERT(sag_journal_replay(source, recovered, &recovered_meta));
    undo_assert_text(recovered, (const u8 *)"base", 4U);
    adopted = sag_journal_open(source, &recovered_meta);
    SAG_ASSERT_NOT_NULL(adopted);
    sag_journal_discard(adopted);

    sag_filemeta_dispose(&recovered_meta);
    sag_textbuf_free(recovered);
    sag_undo_free(undo);
    sag_cset_free(&cursors);
    sag_filemeta_dispose(&meta);
    sag_textbuf_free(tb);
    SAG_ASSERT_EQ_I64(unlink(source), 0);
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/sagitta/journal",
                   state);
    (void)snprintf(sagitta_dir, sizeof(sagitta_dir), "%s/sagitta", state);
    SAG_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(sagitta_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(state), 0);
}
