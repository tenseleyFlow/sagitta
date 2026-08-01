#include "text/edit.h"

#include <stdlib.h>
#include <string.h>

#include "util/log.h"

static void edit_require(const EditCtx *ec)
{
    if (ec == NULL || ec->tb == NULL)
        SAG_BUG("edit: NULL context or buffer");
}

void sag_edit_ensure_journal(EditCtx *ec)
{
    const char *path;

    if (ec->meta == NULL || ec->jrnl != NULL)
        return;
    path = ec->meta->realpath;
    if (path == NULL)
        SAG_BUG("edit: file-backed buffer has no journal path");
    ec->jrnl = sag_journal_open(path, ec->meta);
    if (ec->jrnl == NULL)
        SAG_BUG("edit: cannot open crash journal before mutation");
}

static u8 *copy_range(const TextBuf *tb, Span range)
{
    TextIter it;
    u8 *copy;
    u64 done = 0U;
    u64 len = range.hi - range.lo;

    copy = sag_xmalloc(len == 0U ? 1U : (size_t)len);
    if (len == 0U)
        return copy;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        SAG_BUG("edit delete: cannot iterate valid range");
    while (done < len) {
        const u8 *bytes;
        u64 avail;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &bytes, &avail))
            SAG_BUG("edit delete: truncated iterator");
        take = avail < len - done ? avail : len - done;
        (void)memcpy(copy + (size_t)done, bytes, (size_t)take);
        done += take;
        if (done < len && !sag_textiter_advance(&it, tb))
            SAG_BUG("edit delete: truncated iterator advance");
    }
    return copy;
}

void sag_edit_insert(EditCtx *ec, ByteOff at, const u8 *bytes, u64 len)
{
    u64 payload;

    edit_require(ec);
    if (at.v > sag_textbuf_len(ec->tb))
        SAG_BUG("edit insert: offset out of bounds");
    if (bytes == NULL && len != 0U)
        SAG_BUG("edit insert: NULL payload");
    if (len == 0U)
        return;
    sag_edit_ensure_journal(ec);
    if (ec->cset != NULL)
        sag_cset_require_single_edit(ec->cset);
    payload = ec->tb->add.len;
    if (ec->undo != NULL)
        sag_undo_prepare_insert(ec, at, len);
    sag_textbuf_insert(ec->tb, at, bytes, len);
    if (ec->marks != NULL)
        sag_marks_adjust(ec->marks, SAG_JOURNAL_INS, at, len);
    if (ec->cset != NULL)
        sag_cset_adjust(ec->cset, SAG_JOURNAL_INS, at, len);
    if (ec->jrnl != NULL)
        sag_journal_record(ec->jrnl, SAG_JOURNAL_INS, at.v, bytes, len);
    if (ec->undo != NULL)
        sag_undo_record_insert(ec, at, len, payload);
}

void sag_edit_delete(EditCtx *ec, Span range)
{
    u8 *removed;
    u64 len;

    edit_require(ec);
    if (range.lo > range.hi || range.hi > sag_textbuf_len(ec->tb))
        SAG_BUG("edit delete: range out of bounds");
    len = range.hi - range.lo;
    if (len == 0U)
        return;
    sag_edit_ensure_journal(ec);
    if (ec->cset != NULL)
        sag_cset_require_single_edit(ec->cset);
    removed = copy_range(ec->tb, range);
    if (ec->undo != NULL)
        sag_undo_prepare_delete(ec, range);
    sag_textbuf_delete(ec->tb, range);
    if (ec->marks != NULL)
        sag_marks_adjust(ec->marks, SAG_JOURNAL_DEL, BYTEOFF(range.lo), len);
    if (ec->cset != NULL)
        sag_cset_adjust(ec->cset, SAG_JOURNAL_DEL, BYTEOFF(range.lo), len);
    if (ec->jrnl != NULL)
        sag_journal_record(ec->jrnl, SAG_JOURNAL_DEL, range.lo, removed, len);
    if (ec->undo != NULL)
        sag_undo_record_delete(ec, range);
    free(removed);
}

SagSaveErr sag_edit_save(EditCtx *ec, const char *path)
{
    SagSaveErr result;

    edit_require(ec);
    if (ec->meta == NULL)
        SAG_BUG("edit save: scratch buffer has no file metadata");
    result = sag_file_save(ec->tb, ec->meta, path);
    if (result != SAG_SAVE_OK)
        return result;
    if (ec->undo != NULL) {
        sag_undo_boundary(ec->undo);
        sag_undo_mark_saved(ec->undo);
    }
    if (ec->jrnl != NULL) {
        sag_journal_discard(ec->jrnl);
        ec->jrnl = NULL;
    }
    return SAG_SAVE_OK;
}
