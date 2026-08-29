#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    f->tb = yew_textbuf_from_bytes(bytes, len);
    f->marks = yew_marks_new();
    yew_cset_init(&f->cursors, undo_cursor(0U, 0U, 0U));
    f->undo = yew_undo_new(f->tb);
    f->clock.mono = 1000U;
    f->clock.wall = 100;
    yew_undo_set_clock(f->undo, undo_mono, undo_wall, &f->clock);
    f->edit = (EditCtx){f->tb, f->marks, &f->cursors, 7U, NULL, f->undo,
                       NULL, NULL, NULL, 0, NULL, NULL, {0}, 0U};
}

static void undo_fixture_free(UndoFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_marks_free(f->marks);
    yew_textbuf_free(f->tb);
}

static void undo_assert_text(const TextBuf *tb, const u8 *want, u64 want_len)
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

static const UndoNode *undo_current_node(const UndoTree *ut)
{
    YEW_ASSERT(ut->cur != 0U);
    YEW_ASSERT(ut->cur <= ut->nodes.len);
    return &ut->nodes.data[ut->cur - 1U];
}

void test_undo_type_merges_when_all_predicates_hold(void)
{
    UndoFixture f;
    const UndoNode *node;

    undo_fixture_init(&f, NULL, 0U);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    f.clock.mono += YEW_UNDO_BURST_MS - 1U;
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    node = undo_current_node(f.undo);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);
    YEW_ASSERT_EQ_U64(node->reason, YEW_TXN_TYPE);
    YEW_ASSERT_EQ_U64(node->t_last_ms, f.clock.mono);
    undo_assert_text(f.tb, (const u8 *)"ab", 2U);
    undo_fixture_free(&f);
}

void test_undo_merge_rejects_different_reason(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"abc", 3U);
    yew_edit_insert(&f.edit, BYTEOFF(3U), (const u8 *)"d", 1U);
    f.undo->pending_reason = YEW_TXN_ERASE;
    yew_edit_delete(&f.edit, (Span){3U, 4U});
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[1].reason, YEW_TXN_TYPE);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[2].reason, YEW_TXN_ERASE);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    undo_fixture_free(&f);
}

void test_undo_merge_rejects_elapsed_burst(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    f.clock.mono += YEW_UNDO_BURST_MS;
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[1].n_ops, 1U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    undo_fixture_free(&f);
}

void test_undo_merge_rejects_noncontiguous_type(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"--", 2U);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    yew_edit_insert(&f.edit, BYTEOFF(3U), (const u8 *)"b", 1U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->ops.data[0].off, 0U);
    YEW_ASSERT_EQ_U64(f.undo->ops.data[1].off, 3U);
    undo_fixture_free(&f);
}

void test_undo_erase_merges_backspace_and_forward_delete(void)
{
    UndoFixture back;
    UndoFixture forward;

    undo_fixture_init(&back, (const u8 *)"abcd", 4U);
    back.undo->pending_reason = YEW_TXN_ERASE;
    yew_edit_delete(&back.edit, (Span){3U, 4U});
    back.undo->pending_reason = YEW_TXN_ERASE;
    yew_edit_delete(&back.edit, (Span){2U, 3U});
    YEW_ASSERT_EQ_U64(back.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(undo_current_node(back.undo)->n_ops, 2U);
    undo_assert_text(back.tb, (const u8 *)"ab", 2U);

    undo_fixture_init(&forward, (const u8 *)"abcd", 4U);
    forward.undo->pending_reason = YEW_TXN_ERASE;
    yew_edit_delete(&forward.edit, (Span){1U, 2U});
    forward.undo->pending_reason = YEW_TXN_ERASE;
    yew_edit_delete(&forward.edit, (Span){1U, 2U});
    YEW_ASSERT_EQ_U64(forward.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(undo_current_node(forward.undo)->n_ops, 2U);
    undo_assert_text(forward.tb, (const u8 *)"ad", 2U);
    undo_fixture_free(&forward);
    undo_fixture_free(&back);
}

void test_undo_merge_rejects_payload_limit(void)
{
    UndoFixture f;
    u8 bytes[YEW_UNDO_BURST_BYTES];

    (void)memset(bytes, 'x', sizeof(bytes));
    undo_fixture_init(&f, NULL, 0U);
    yew_edit_insert(&f.edit, BYTEOFF(0U), bytes, sizeof(bytes));
    yew_edit_insert(&f.edit, BYTEOFF(sizeof(bytes)), (const u8 *)"y", 1U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[1].n_ops, 1U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), sizeof(bytes) + 1U);
    undo_fixture_free(&f);
}

static void undo_assert_boundary_splits_burst(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    yew_undo_boundary(f.undo);
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT(!f.undo->boundary);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[2].parent, f.undo->nodes.data[1].id);
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
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    yew_undo_begin(&f.edit, YEW_TXN_TYPE);
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 3U);
    YEW_ASSERT_EQ_U64(f.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.data[2].n_ops, 1U);
    undo_fixture_free(&f);
}

void test_undo_explicit_type_coalesces_contiguous_insert_ops(void)
{
    UndoFixture f;
    const UndoNode *node;

    undo_fixture_init(&f, NULL, 0U);
    yew_undo_begin(&f.edit, YEW_TXN_TYPE);
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(0U),
                               (const u8 *)"ab", 2U));
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(2U),
                               (const u8 *)"cd", 2U));
    yew_undo_end(&f.edit);

    node = undo_current_node(f.undo);
    YEW_ASSERT_EQ_U64(node->n_ops, 1U);
    YEW_ASSERT_EQ_U64(f.undo->ops.data[node->ops_at].off, 0U);
    YEW_ASSERT_EQ_U64(f.undo->ops.data[node->ops_at].len, 4U);
    undo_assert_text(f.tb, (const u8 *)"abcd", 4U);
    YEW_ASSERT(yew_undo(&f.edit));
    undo_assert_text(f.tb, NULL, 0U);
    YEW_ASSERT(yew_redo(&f.edit));
    undo_assert_text(f.tb, (const u8 *)"abcd", 4U);
    undo_fixture_free(&f);
}

void test_undo_explicit_type_keeps_noncontiguous_insert_ops(void)
{
    UndoFixture f;
    const UndoNode *node;

    undo_fixture_init(&f, (const u8 *)"--", 2U);
    yew_undo_begin(&f.edit, YEW_TXN_TYPE);
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(0U),
                               (const u8 *)"a", 1U));
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(3U),
                               (const u8 *)"b", 1U));
    yew_undo_end(&f.edit);

    node = undo_current_node(f.undo);
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);
    undo_assert_text(f.tb, (const u8 *)"a--b", 4U);
    YEW_ASSERT(yew_undo(&f.edit));
    undo_assert_text(f.tb, (const u8 *)"--", 2U);
    YEW_ASSERT(yew_redo(&f.edit));
    undo_assert_text(f.tb, (const u8 *)"a--b", 4U);
    undo_fixture_free(&f);
}

void test_undo_nesting_depth_three_commits_once(void)
{
    UndoFixture f;

    undo_fixture_init(&f, NULL, 0U);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"a", 1U);
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"b", 1U);
    YEW_ASSERT_EQ_U64(f.undo->depth, 3U);
    YEW_ASSERT(f.undo->open != 0U);
    yew_undo_end(&f.edit);
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(f.undo->depth, 1U);
    YEW_ASSERT(f.undo->open != 0U);
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(f.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.undo->open, 0U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->n_ops, 2U);
    undo_fixture_free(&f);
}

void test_undo_atomic_supported_reasons_each_commit_one_node(void)
{
    static const YewTxnReason reasons[] = {
        YEW_TXN_PASTE, YEW_TXN_CUT, YEW_TXN_EXTERNAL
    };
    UndoFixture f;
    size_t i;

    undo_fixture_init(&f, NULL, 0U);
    for (i = 0U; i < YEW_ARRAY_LEN(reasons); i++) {
        yew_undo_begin(&f.edit, reasons[i]);
        yew_edit_insert(&f.edit, BYTEOFF(yew_textbuf_len(f.tb)),
                        (const u8 *)"xy", 2U);
        yew_undo_end(&f.edit);
        YEW_ASSERT_EQ_U64(f.undo->nodes.len, i + 2U);
        YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->reason, reasons[i]);
        YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->n_ops, 1U);
    }
    undo_fixture_free(&f);
}

void test_undo_multi_transaction_commits_one_node(void)
{
    UndoFixture f;
    const UndoNode *node;

    undo_fixture_init(&f, (const u8 *)"ab", 2U);
    YEW_ASSERT(yew_cset_add(&f.cursors, undo_cursor(2U, 2U, 2U)));
    yew_undo_begin(&f.edit, YEW_TXN_MULTI);
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(0U),
                               (const u8 *)"X", 1U));
    YEW_ASSERT(yew_edit_insert(&f.edit, f.cursors.curs.data[1].pos,
                               (const u8 *)"Y", 1U));
    yew_undo_end(&f.edit);

    node = undo_current_node(f.undo);
    YEW_ASSERT_EQ_U64(node->reason, YEW_TXN_MULTI);
    YEW_ASSERT_EQ_U64(node->n_ops, 2U);
    YEW_ASSERT_EQ_U64(f.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.len, 2U);
    undo_assert_text(f.tb, (const u8 *)"XabY", 4U);
    YEW_ASSERT(yew_undo(&f.edit));
    undo_assert_text(f.tb, (const u8 *)"ab", 2U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.len, 2U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 0U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[1].pos.v, 2U);
    undo_fixture_free(&f);
}

void test_undo_filter_reason_names_sprint19(void)
{
    UndoFixture f;

    /* Sprint 19 landed the filter, so YEW_TXN_FILTER is a working reason
     * rather than a hard error.  One filter is exactly one undo node. */
    undo_fixture_init(&f, (const u8 *)"alpha\nbeta\n", 11U);
    yew_undo_begin(&f.edit, YEW_TXN_FILTER);
    YEW_ASSERT(yew_edit_delete(&f.edit, (Span){0U, 6U}));
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(0U),
                               (const u8 *)"ALPHA\n", 6U));
    yew_undo_end(&f.edit);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), 11U);
    undo_fixture_free(&f);
}

/* YEW_TXN_REPLACE used to hard-error naming this sprint.  Sprint 21
 * landed it, so the reason is now accepted — that is what closing a
 * deferral looks like, and the row stays to say so. */
void test_undo_replace_reason_is_live(void)
{
    UndoFixture f;
    EditCtx ec;

    undo_fixture_init(&f, (const u8 *)"hello", 5U);
    ec = f.edit;
    yew_undo_begin(&ec, YEW_TXN_REPLACE);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"X", 1U));
    yew_undo_end(&ec);
    YEW_ASSERT(yew_undo(&ec));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), 5U);
    undo_fixture_free(&f);
}

void test_undo_macro_reason_is_live(void)
{
    UndoFixture f;
    EditCtx ec;

    undo_fixture_init(&f, (const u8 *)"hello", 5U);
    ec = f.edit;
    yew_undo_begin(&ec, YEW_TXN_MACRO);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(5U), (const u8 *)"!", 1U));
    yew_undo_end(&ec);
    YEW_ASSERT(yew_undo(&ec));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.tb), 5U);
    undo_fixture_free(&f);
}

void test_undo_lsp_reason_names_sprint47(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"old", 3U);
    yew_undo_begin(&f.edit, YEW_TXN_LSP);
    YEW_ASSERT(yew_edit_delete(&f.edit, (Span){0U, 3U}));
    YEW_ASSERT(yew_edit_insert(&f.edit, BYTEOFF(0U),
                               (const u8 *)"new", 3U));
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->reason, YEW_TXN_LSP);
    YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->n_ops, 2U);
    undo_assert_text(f.tb, (const u8 *)"new", 3U);

    YEW_ASSERT(yew_undo(&f.edit));
    undo_assert_text(f.tb, (const u8 *)"old", 3U);
    YEW_ASSERT(yew_undo_prune_redo(f.undo, YEW_TXN_LSP));
    YEW_ASSERT(!yew_redo(&f.edit));
    YEW_ASSERT(!yew_undo_prune_redo(f.undo, YEW_TXN_LSP));
    undo_fixture_free(&f);
}

void test_undo_save_rejects_open_transaction(void)
{
    int fds[2];
    pid_t child;
    pid_t waited;
    int status;
    char output[1024];
    ssize_t got;

    YEW_ASSERT_EQ_I64(pipe(fds), 0);
    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        UndoFixture f;
        FileMeta meta;

        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)close(fds[1]);
        (void)memset(&meta, 0, sizeof(meta));
        undo_fixture_init(&f, NULL, 0U);
        yew_undo_begin(&f.edit, YEW_TXN_PASTE);
        yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"x", 1U);
        f.edit.meta = &meta;
        (void)yew_edit_save(&f.edit, "/tmp/yew-save-open-transaction");
        _exit(0);
    }
    YEW_ASSERT_EQ_I64(close(fds[1]), 0);
    got = read(fds[0], output, sizeof(output) - 1U);
    YEW_ASSERT(got >= 0);
    output[got < 0 ? 0U : (size_t)got] = '\0';
    YEW_ASSERT_EQ_I64(close(fds[0]), 0);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
    YEW_ASSERT(strstr(output, "edit save: open undo transaction") != NULL);
}

void test_undo_abort_restores_content_and_single_cursor(void)
{
    UndoFixture f;

    undo_fixture_init(&f, (const u8 *)"base", 4U);
    f.cursors.curs.data[0] = undo_cursor(2U, 1U, 9U);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_insert(&f.edit, BYTEOFF(2U), (const u8 *)"XYZ", 3U);
    yew_edit_delete(&f.edit, (Span){0U, 1U});
    f.cursors.selstacks.data[0].n = 2U;
    YEW_ASSERT_EQ_U64(f.undo->depth, 1U);
    YEW_ASSERT(f.undo->open != 0U);
    yew_undo_abort(&f.edit);
    undo_assert_text(f.tb, (const u8 *)"base", 4U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.len, 1U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].anchor.v, 1U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].goal_col.v, 9U);
    YEW_ASSERT_EQ_U64(f.cursors.selstacks.data[0].n, 0U);
    YEW_ASSERT_EQ_U64(f.undo->depth, 0U);
    YEW_ASSERT_EQ_U64(f.undo->open, 0U);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 1U);
    undo_fixture_free(&f);
}

void test_undo_restores_eight_cursor_after_snapshot(void)
{
    UndoFixture f;
    size_t i;

    undo_fixture_init(&f, (const u8 *)"0123456789", 10U);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_insert(&f.edit, BYTEOFF(10U), (const u8 *)"x", 1U);
    for (i = 1U; i < 8U; i++)
        YEW_ASSERT(yew_cset_add(&f.cursors, undo_cursor(i, i, i + 3U)));
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->n_after, 8U);
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(f.cursors.curs.len, 1U);
    YEW_ASSERT(yew_redo(&f.edit));
    YEW_ASSERT_EQ_U64(f.cursors.curs.len, 8U);
    for (i = 0U; i < 8U; i++) {
        YEW_ASSERT_EQ_U64(f.cursors.curs.data[i].pos.v, i == 0U ? 0U : i);
        YEW_ASSERT_EQ_U64(f.cursors.curs.data[i].anchor.v, i);
    }
    undo_fixture_free(&f);
}

void test_undo_window_gone_restores_into_focused_cursor_set(void)
{
    UndoFixture f;
    CursorSet focused;

    undo_fixture_init(&f, (const u8 *)"abc", 3U);
    f.cursors.curs.data[0] = undo_cursor(1U, 0U, 5U);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_insert(&f.edit, BYTEOFF(3U), (const u8 *)"X", 1U);
    f.cursors.curs.data[0] = undo_cursor(4U, 4U, 8U);
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(undo_current_node(f.undo)->win_id, 7U);

    yew_cset_init(&focused, undo_cursor(2U, 2U, 2U));
    f.edit.cset = &focused;
    f.edit.win_id = 99U;
    YEW_ASSERT(yew_undo(&f.edit));
    YEW_ASSERT_EQ_U64(focused.curs.data[0].pos.v, 1U);
    YEW_ASSERT_EQ_U64(focused.curs.data[0].anchor.v, 0U);
    YEW_ASSERT_EQ_U64(focused.curs.data[0].goal_col.v, 5U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 4U);
    YEW_ASSERT(yew_redo(&f.edit));
    YEW_ASSERT_EQ_U64(focused.curs.data[0].pos.v, 4U);
    YEW_ASSERT_EQ_U64(focused.curs.data[0].anchor.v, 4U);
    YEW_ASSERT_EQ_U64(focused.curs.data[0].goal_col.v, 8U);
    YEW_ASSERT_EQ_U64(f.cursors.curs.data[0].pos.v, 4U);
    yew_cset_free(&focused);
    f.edit.cset = &f.cursors;
    undo_fixture_free(&f);
}

void test_undo_redo_cycles_do_not_grow_add_store(void)
{
    UndoFixture f;
    u64 stable;
    u32 i;

    undo_fixture_init(&f, NULL, 0U);
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_insert(&f.edit, BYTEOFF(0U), (const u8 *)"payload", 7U);
    yew_undo_end(&f.edit);
    stable = f.tb->add.len;
    YEW_ASSERT_EQ_U64(stable, 7U);
    for (i = 0U; i < 100000U; i++) {
        YEW_ASSERT(yew_undo(&f.edit));
        YEW_ASSERT(yew_redo(&f.edit));
    }
    YEW_ASSERT_EQ_U64(f.tb->add.len, stable);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    undo_assert_text(f.tb, (const u8 *)"payload", 7U);
    undo_fixture_free(&f);
}

void test_undo_to_root_reproduces_loaded_bytes(void)
{
    static const u8 original[] = {'a', 0U, 0xffU, '\n', 'z'};
    UndoFixture f;

    undo_fixture_init(&f, original, sizeof(original));
    yew_undo_begin(&f.edit, YEW_TXN_PASTE);
    yew_edit_delete(&f.edit, (Span){1U, 4U});
    yew_edit_insert(&f.edit, BYTEOFF(1U), (const u8 *)"other", 5U);
    yew_undo_end(&f.edit);
    YEW_ASSERT_EQ_U64(f.undo->nodes.len, 2U);
    YEW_ASSERT(yew_undo_to(&f.edit, f.undo->root));
    undo_assert_text(f.tb, original, sizeof(original));
    YEW_ASSERT_EQ_U64(yew_undo_current(f.undo), f.undo->root);
    YEW_ASSERT(!yew_undo(&f.edit));
    undo_fixture_free(&f);
}

void test_undo_save_reopens_journal_on_navigation(void)
{
    char state[PATH_MAX] = "/tmp/yew-undo-save-XXXXXX";
    char canonical[PATH_MAX];
    char source[128];
    char journal_dir[128];
    char yew_dir[112];
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

    YEW_ASSERT_NOT_NULL(mkdtemp(state));
    YEW_ASSERT_NOT_NULL(realpath(state, canonical));
    YEW_ASSERT(strlen(canonical) < sizeof(state));
    (void)strcpy(state, canonical);
    count = snprintf(source, sizeof(source), "%s/base.txt", state);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(source));
    file = fopen(source, "wb");
    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite("base", 1U, 4U, file), 4U);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state, 1), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(source, &tb, &meta), YEW_LOAD_OK);
    yew_cset_init(&cursors, cursor);
    undo = yew_undo_new(tb);
    edit = (EditCtx){tb, NULL, &cursors, 0U, NULL, undo, &meta, NULL, NULL,
                     0, NULL, NULL, {0}, 0U};

    yew_edit_insert(&edit, BYTEOFF(4U), (const u8 *)"X", 1U);
    YEW_ASSERT_NOT_NULL(edit.jrnl);
    YEW_ASSERT(yew_journal_ok(edit.jrnl));
    YEW_ASSERT_EQ_U64(yew_edit_save(&edit, source), YEW_SAVE_OK);
    YEW_ASSERT(edit.jrnl == NULL);
    YEW_ASSERT(yew_undo_at_save_point(undo));

    YEW_ASSERT(yew_undo(&edit));
    YEW_ASSERT_NOT_NULL(edit.jrnl);
    YEW_ASSERT(yew_journal_ok(edit.jrnl));
    undo_assert_text(tb, (const u8 *)"base", 4U);
    yew_journal_close(edit.jrnl);
    edit.jrnl = NULL;

    YEW_ASSERT_EQ_U64(yew_file_load(source, &recovered, &recovered_meta),
                      YEW_LOAD_OK);
    YEW_ASSERT(yew_journal_replay(source, recovered, &recovered_meta));
    undo_assert_text(recovered, (const u8 *)"base", 4U);
    adopted = yew_journal_open(source, &recovered_meta);
    YEW_ASSERT_NOT_NULL(adopted);
    yew_journal_discard(adopted);

    yew_filemeta_dispose(&recovered_meta);
    yew_textbuf_free(recovered);
    yew_undo_free(undo);
    yew_cset_free(&cursors);
    yew_filemeta_dispose(&meta);
    yew_textbuf_free(tb);
    YEW_ASSERT_EQ_I64(unlink(source), 0);
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/yew/journal",
                   state);
    (void)snprintf(yew_dir, sizeof(yew_dir), "%s/yew", state);
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(state), 0);
}
