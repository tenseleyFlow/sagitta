#include "harness.h"

#include <string.h>

#include "text/edit.h"
#include "util/buf.h"

static Bytebuf regress_materialize(const TextBuf *tb)
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

static Bytebuf regress_snap_materialize(const TextBuf *tb,
                                        const TextSnap *snap)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (sag_textsnap_iter(&it, snap, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    return out;
}

static void regress_assert_text(const TextBuf *tb, const char *want)
{
    Bytebuf got = regress_materialize(tb);
    size_t len = strlen(want);

    SAG_ASSERT_EQ_U64(got.len, len);
    SAG_ASSERT_EQ_MEM(got.data, want, len);
    bytebuf_free(&got);
}

void test_piece_regress_exact_seam_roundtrip(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);

    /* Former seam failure: insert on both sides of the new middle piece,
     * delete across both seams, then restore the removed original byte. */
    sag_textbuf_insert(tb, BYTEOFF(3U), (const u8 *)"X", 1U);
    sag_textbuf_insert(tb, BYTEOFF(3U), (const u8 *)"L", 1U);
    sag_textbuf_insert(tb, BYTEOFF(5U), (const u8 *)"R", 1U);
    regress_assert_text(tb, "abcLXRdef");
    sag_textbuf_delete(tb, (Span){2U, 6U});
    regress_assert_text(tb, "abdef");
    sag_textbuf_insert(tb, BYTEOFF(2U), (const u8 *)"c", 1U);
    regress_assert_text(tb, "abcdef");
    sag_textbuf_check(tb);
    sag_textbuf_free(tb);
}

void test_piece_regress_delete_across_tree_branches(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"0123456789", 10U);

    /* Keep this trace literal and readable: it exercises a deletion whose
     * endpoints are inside different branches after scattered inserts. */
    sag_textbuf_insert(tb, BYTEOFF(2U), (const u8 *)"aa", 2U);
    sag_textbuf_insert(tb, BYTEOFF(8U), (const u8 *)"bb", 2U);
    sag_textbuf_insert(tb, BYTEOFF(5U), (const u8 *)"cc", 2U);
    regress_assert_text(tb, "01aa2cc345bb6789");
    sag_textbuf_delete(tb, (Span){3U, 13U});
    regress_assert_text(tb, "01a789");
    sag_textbuf_check(tb);
    sag_textbuf_free(tb);
}

void test_piece_regress_undo_branch_survives(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"root", 4U);
    MarkSet *marks = sag_marks_new();
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    u32 branch_a;

    sag_cset_init(&cursors, cursor);
    undo = sag_undo_new(tb);
    edit = (EditCtx){tb, marks, &cursors, 3U, NULL, undo, NULL};

    sag_undo_boundary(undo);
    sag_edit_insert(&edit, BYTEOFF(4U), (const u8 *)"A", 1U);
    branch_a = sag_undo_current(undo);
    regress_assert_text(tb, "rootA");
    SAG_ASSERT(sag_undo(&edit));
    regress_assert_text(tb, "root");
    sag_undo_boundary(undo);
    sag_edit_insert(&edit, BYTEOFF(4U), (const u8 *)"B", 1U);
    regress_assert_text(tb, "rootB");
    SAG_ASSERT(sag_undo_to(&edit, branch_a));
    regress_assert_text(tb, "rootA");

    sag_undo_free(undo);
    sag_cset_free(&cursors);
    sag_marks_free(marks);
    sag_textbuf_free(tb);
}

void test_piece_regress_snapshot_seam_release_orders(void)
{
    TextBuf *live = sag_textbuf_from_bytes((const u8 *)"left|right", 10U);
    TextSnap snap = sag_textbuf_snap(live);
    Bytebuf got;

    sag_textbuf_insert(live, BYTEOFF(5U), (const u8 *)"A", 1U);
    sag_textbuf_insert(live, BYTEOFF(6U), (const u8 *)"B", 1U);
    sag_textbuf_delete(live, (Span){4U, 7U});
    regress_assert_text(live, "leftright");
    got = regress_snap_materialize(live, &snap);
    SAG_ASSERT_EQ_U64(got.len, 10U);
    SAG_ASSERT_EQ_MEM(got.data, "left|right", 10U);
    bytebuf_free(&got);

    /* Release the live owner first; the snapshot must keep both the old
     * root and its backing stores alive until its own release. */
    sag_textbuf_free(live);
    got = regress_snap_materialize(NULL, &snap);
    SAG_ASSERT_EQ_MEM(got.data, "left|right", 10U);
    bytebuf_free(&got);
    sag_textsnap_release(NULL, &snap);
    SAG_ASSERT_NULL(snap.root);
}

void test_piece_regress_undo_compact_zero_repair_run(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdefgh", 8U);
    MarkSet *marks = sag_marks_new();
    MarkId inside = sag_mark_add(marks, BYTEOFF(3U), SAG_BIAS_RIGHT);
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    CursorSet cursors;
    UndoTree *undo = sag_undo_new(tb);
    EditCtx edit;
    const UndoNode *insert_node;
    const UndoRepairRun *insert_run;

    sag_cset_init(&cursors, cursor);
    edit = (EditCtx){tb, marks, &cursors, 1U, NULL, undo, NULL};

    /* A deletion first records the collapsed mark. The following insert has
     * an empty repair run, but its offset still belongs at the current end
     * of the repair arena. History compaction used to reject offset zero. */
    sag_undo_begin(&edit, SAG_TXN_CUT);
    sag_edit_delete(&edit, (Span){1U, 5U});
    sag_undo_end(&edit);
    SAG_ASSERT_EQ_U64(sag_mark_pos(marks, inside).v, 1U);
    SAG_ASSERT_EQ_U64(undo->repairs.len, 1U);

    sag_undo_begin(&edit, SAG_TXN_PASTE);
    sag_edit_insert(&edit, BYTEOFF(sag_textbuf_len(tb)),
                    (const u8 *)"X", 1U);
    sag_undo_end(&edit);
    insert_node = &undo->nodes.data[undo->cur - 1U];
    insert_run = &undo->repair_runs.data[insert_node->ops_at];
    SAG_ASSERT_EQ_U64(insert_node->rep_at, 1U);
    SAG_ASSERT_EQ_U64(insert_run->at, insert_node->rep_at);
    SAG_ASSERT_EQ_U64(insert_run->len, 0U);
    SAG_ASSERT(sag_undo(&edit));
    SAG_ASSERT(sag_redo(&edit));
    regress_assert_text(tb, "afghX");
    sag_textbuf_check(tb);

    sag_undo_free(undo);
    sag_cset_free(&cursors);
    sag_marks_free(marks);
    sag_textbuf_free(tb);
}
