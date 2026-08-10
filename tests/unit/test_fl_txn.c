#include "harness.h"
#include "flfix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/fltxn.h"
#include "fl/gc.h"
#include "text/edit.h"

typedef struct TxnBuf {
    TextBuf *tb;
    MarkSet *marks;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx ec;
} TxnBuf;

typedef struct TxnFix {
    FlVm vm;
    Ed ed;
} TxnFix;

static Cursor txn_cursor(void)
{
    Cursor c = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    return c;
}

static void txn_buf_open(TxnBuf *b, const char *text)
{
    size_t n = strlen(text);

    b->tb = sag_textbuf_from_bytes((const u8 *)text, (u64)n);
    b->marks = sag_marks_new();
    sag_cset_init(&b->cursors, txn_cursor());
    b->undo = sag_undo_new(b->tb);
    b->ec = (EditCtx){b->tb, b->marks, &b->cursors, 1U, NULL, b->undo,
                      NULL, NULL, NULL, 0};
}

static void txn_buf_close(TxnBuf *b)
{
    sag_undo_free(b->undo);
    sag_cset_free(&b->cursors);
    sag_marks_free(b->marks);
    sag_textbuf_free(b->tb);
}

static void txn_fix_open(TxnFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    f->vm.host = &fl_host_editor;
    f->vm.ed = &f->ed;
}

static void txn_fix_close(TxnFix *f)
{
    free(f->vm.txn.enlisted);
    fl_gc_free_all(&f->vm);
}

static TxnBuf *script_buf;

static u32 txn_dump_node_count(const UndoTree *undo)
{
    FILE *file = tmpfile();
    char line[256];
    u32 count = 0U;

    SAG_ASSERT_NOT_NULL(file);
    sag_undo_dump(undo, file);
    SAG_ASSERT_EQ_I64(fflush(file), 0);
    SAG_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    while (fgets(line, sizeof(line), file) != NULL)
        if (strncmp(line, "node ", 5U) == 0)
            count++;
    SAG_ASSERT(feof(file));
    SAG_ASSERT_EQ_I64(fclose(file), 0);
    return count;
}

static void txn_assert_text(const TextBuf *tb, const char *want)
{
    TextIter it;
    u64 done = 0U;
    u64 want_len = (u64)strlen(want);

    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), want_len);
    if (want_len == 0U)
        return;
    SAG_ASSERT(sag_textiter_begin(&it, tb, BYTEOFF(0U)));
    do {
        const u8 *bytes;
        u64 len;

        SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
        SAG_ASSERT(done + len <= want_len);
        SAG_ASSERT_EQ_MEM(bytes, want + done, len);
        done += len;
    } while (sag_textiter_advance(&it, tb));
    SAG_ASSERT_EQ_U64(done, want_len);
}

static bool txn_script_mutate(FlVm *vm, FlValue *args, u32 nargs,
                              FlValue *out)
{
    u64 at;

    (void)args;
    (void)nargs;
    if (script_buf == NULL || !fl_txn_enlist(vm, &script_buf->ec))
        return false;
    at = sag_textbuf_len(script_buf->tb);
    if (!sag_edit_insert(&script_buf->ec, BYTEOFF(at), (const u8 *)"x", 1U))
        return fl_raise(vm, "io", "test mutation failed");
    *out = FL_NIL_V;
    return true;
}

static void txn_script_open(FlFix *f, Ed *ed, TxnBuf *b)
{
    FlNative *native;
    u32 name;

    flfix_open(f);
    (void)memset(ed, 0, sizeof(*ed));
    txn_buf_open(b, "");
    script_buf = b;
    f->vm.ed = ed;
    f->vm.host = &fl_host_editor;
    name = sag_intern_cstr(&f->in, "mutate");
    native = fl_gc_alloc(&f->vm, sizeof(*native), FL_NATIVE);
    native->fn = txn_script_mutate;
    native->name_id = name;
    native->min_ar = 0U;
    native->max_ar = 0U;
    native->caps = 0U;
    fl_gc_protect(&f->vm, FL_OBJ_V(FL_NATIVE, native));
    (void)fl_map_set(&f->vm, f->vm.globals, FL_INT_V((i64)name),
                     FL_OBJ_V(FL_NATIVE, native));
    fl_gc_release(&f->vm, 1U);
}

static void txn_script_close(FlFix *f, TxnBuf *b)
{
    script_buf = NULL;
    txn_buf_close(b);
    flfix_close(f);
}

void test_fl_txn_implicit_entry_enlists_once(void)
{
    TxnFix f;
    TxnBuf b;

    txn_fix_open(&f);
    txn_buf_open(&b, "a");
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT_EQ_U64(f.vm.txn.n, 1U);
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(1U), (const u8 *)"bc", 2U));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, true));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 3U);
    SAG_ASSERT(sag_undo(&b.ec));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 1U);
    txn_buf_close(&b);
    txn_fix_close(&f);
}

void test_fl_txn_nested_edit_flattens_to_one_undo(void)
{
    TxnFix f;
    TxnBuf b;

    txn_fix_open(&f);
    txn_buf_open(&b, "");
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(f.vm.host->edit_begin(&f.vm));
    SAG_ASSERT(f.vm.host->edit_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(0U), (const u8 *)"a", 1U));
    SAG_ASSERT(f.vm.host->edit_end(&f.vm, true));
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(1U), (const u8 *)"b", 1U));
    SAG_ASSERT(f.vm.host->edit_end(&f.vm, true));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, true));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 2U);
    /* Root plus exactly one script edit node. */
    SAG_ASSERT_EQ_U64(txn_dump_node_count(b.undo), 2U);
    SAG_ASSERT(sag_undo(&b.ec));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 0U);
    SAG_ASSERT(!sag_undo(&b.ec));
    txn_buf_close(&b);
    txn_fix_close(&f);
}

void test_fl_txn_error_crossing_outer_edit_rolls_back(void)
{
    TxnFix f;
    TxnBuf b;

    txn_fix_open(&f);
    txn_buf_open(&b, "old");
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(f.vm.host->edit_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(3U), (const u8 *)"new", 3U));
    SAG_ASSERT(f.vm.host->edit_end(&f.vm, false));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 3U);
    SAG_ASSERT(f.vm.host->run_end(&f.vm, false));
    SAG_ASSERT_EQ_U64(f.vm.txn.n, 0U);
    SAG_ASSERT_EQ_U64(f.vm.txn.depth, 0U);
    SAG_ASSERT(!f.vm.txn.entry_active);
    txn_buf_close(&b);
    txn_fix_close(&f);
}

void test_fl_txn_multi_buffer_commit_and_abort(void)
{
    TxnFix f;
    TxnBuf a;
    TxnBuf b;
    UndoTree *second_undo;
    EditCtx second_ec;

    txn_fix_open(&f);
    txn_buf_open(&a, "a");
    txn_buf_open(&b, "b");
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &a.ec));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT(sag_edit_insert(&a.ec, BYTEOFF(1U), (const u8 *)"1", 1U));
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(1U), (const u8 *)"2", 1U));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, true));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(a.tb), 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 2U);

    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &a.ec));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT(sag_edit_insert(&a.ec, BYTEOFF(2U), (const u8 *)"x", 1U));
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(2U), (const u8 *)"y", 1U));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, false));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(a.tb), 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 2U);
    SAG_ASSERT(sag_undo(&a.ec));
    SAG_ASSERT(sag_undo(&b.ec));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(a.tb), 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 1U);
    txn_buf_close(&b);
    txn_buf_close(&a);

    /* Two undo trees editing one model make rollback order observable.
     * Forward-order abort would leave "Yld"; reverse order restores it. */
    txn_buf_open(&a, "old");
    second_undo = sag_undo_new(a.tb);
    second_ec = a.ec;
    second_ec.undo = second_undo;
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &a.ec));
    SAG_ASSERT(sag_edit_insert(&a.ec, BYTEOFF(0U), (const u8 *)"X", 1U));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &second_ec));
    SAG_ASSERT(sag_edit_insert(&second_ec, BYTEOFF(1U),
                               (const u8 *)"Y", 1U));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, false));
    txn_assert_text(a.tb, "old");
    sag_undo_free(second_undo);
    txn_buf_close(&a);
    txn_fix_close(&f);
}

void test_fl_txn_headless_enlist_is_noop(void)
{
    FlVm vm = {0};
    TxnBuf b;

    vm.host = &fl_host_null;
    txn_buf_open(&b, "x");
    SAG_ASSERT(fl_txn_enlist(&vm, &b.ec));
    SAG_ASSERT_EQ_U64(vm.txn.n, 0U);
    SAG_ASSERT(vm.host->edit_begin(&vm));
    SAG_ASSERT(vm.host->edit_end(&vm, false));
    txn_buf_close(&b);
}

void test_fl_txn_reenlist_refreshes_abort_context(void)
{
    TxnFix f;
    TxnBuf b;
    CursorSet current;
    EditCtx ec;
    Cursor c = {BYTEOFF(1U), {0U}, BYTEOFF(1U)};

    txn_fix_open(&f);
    txn_buf_open(&b, "a");
    sag_cset_init(&current, c);
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    ec = b.ec;
    ec.cset = &current;
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    SAG_ASSERT_EQ_U64(current.curs.data[0].pos.v, 2U);
    SAG_ASSERT(fl_txn_enlist(&f.vm, &ec));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, false));
    SAG_ASSERT_EQ_U64(current.curs.data[0].pos.v, 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 1U);
    sag_cset_free(&current);
    txn_buf_close(&b);
    txn_fix_close(&f);
}

void test_fl_txn_save_flushes_implicit_and_refuses_explicit(void)
{
    TxnFix f;
    TxnBuf b;

    txn_fix_open(&f);
    txn_buf_open(&b, "a");
    SAG_ASSERT(f.vm.host->run_begin(&f.vm));
    SAG_ASSERT(fl_txn_enlist(&f.vm, &b.ec));
    SAG_ASSERT(sag_edit_insert(&b.ec, BYTEOFF(1U), (const u8 *)"b", 1U));
    SAG_ASSERT(fl_txn_enlisted(&f.vm, b.undo));
    SAG_ASSERT(fl_txn_prepare_save(&f.vm, b.undo));
    SAG_ASSERT(!fl_txn_enlisted(&f.vm, b.undo));
    SAG_ASSERT_EQ_U64(b.undo->depth, 0U);
    SAG_ASSERT(f.vm.host->edit_begin(&f.vm));
    SAG_ASSERT(!fl_txn_prepare_save(&f.vm, b.undo));
    SAG_ASSERT(f.vm.host->edit_end(&f.vm, false));
    SAG_ASSERT(f.vm.host->run_end(&f.vm, false));
    txn_buf_close(&b);
    txn_fix_close(&f);
}

void test_fl_txn_try_boundary_controls_rollback(void)
{
    FlFix f;
    Ed ed;
    TxnBuf b;

    txn_script_open(&f, &ed, &b);
    FL_EQ(&f,
          "edit { mutate(); try { error(\"caught\") } catch e { mutate() } }",
          "nil");
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 2U);
    SAG_ASSERT_EQ_U64(txn_dump_node_count(b.undo), 2U);
    SAG_ASSERT(sag_undo(&b.ec));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 0U);

    FL_EQ(&f,
          "try { edit { mutate(); error(\"escape\") } } catch e { nil }",
          "nil");
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 0U);
    txn_script_close(&f, &b);

    txn_script_open(&f, &ed, &b);
    FL_EQ(&f,
          "fn helper() { mutate() }\nedit { helper(); helper() }",
          "nil");
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 2U);
    /* Moving the same edits behind a helper cannot split the boundary. */
    SAG_ASSERT_EQ_U64(txn_dump_node_count(b.undo), 2U);
    txn_script_close(&f, &b);
}

void test_fl_txn_vm_is_reusable_after_rollback(void)
{
    FlFix f;
    Ed ed;
    TxnBuf b;

    txn_script_open(&f, &ed, &b);
    FL_EQ(&f, "edit { mutate(); error(\"escape\") }", "!user: escape");
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 0U);
    SAG_ASSERT_EQ_U64(f.vm.nframes, 0U);
    SAG_ASSERT_EQ_U64(f.vm.nhandlers, 0U);
    SAG_ASSERT_EQ_U64(f.vm.edit_depth, 0U);
    SAG_ASSERT_EQ_U64(f.vm.txn.depth, 0U);
    SAG_ASSERT(!f.vm.txn.entry_active);
    FL_EQ(&f, "edit { mutate() }", "nil");
    SAG_ASSERT_EQ_U64(sag_textbuf_len(b.tb), 1U);
    txn_script_close(&f, &b);
}
