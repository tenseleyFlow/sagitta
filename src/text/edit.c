#include "text/edit.h"

#include <stdlib.h>
#include <string.h>

#include "util/log.h"

static void edit_require(const EditCtx *ec)
{
    if (ec == NULL || ec->tb == NULL)
        YEW_BUG("edit: NULL context or buffer");
}

bool yew_edit_ensure_journal(EditCtx *ec)
{
    const char *path;

    if (ec->meta == NULL)
        return true;
    if (ec->jrnl != NULL)
        return yew_journal_ok(ec->jrnl);
    path = ec->meta->realpath;
    if (path == NULL)
        YEW_BUG("edit: file-backed buffer has no journal path");
    ec->jrnl = yew_journal_open(path, ec->meta);
    return ec->jrnl != NULL;
}

static u8 *copy_range(const TextBuf *tb, Span range)
{
    TextIter it;
    u8 *copy;
    u64 done = 0U;
    u64 len = range.hi - range.lo;

    copy = yew_xmalloc(len == 0U ? 1U : (size_t)len);
    if (len == 0U)
        return copy;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        YEW_BUG("edit delete: cannot iterate valid range");
    while (done < len) {
        const u8 *bytes;
        u64 avail;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &avail))
            YEW_BUG("edit delete: truncated iterator");
        take = avail < len - done ? avail : len - done;
        (void)memcpy(copy + (size_t)done, bytes, (size_t)take);
        done += take;
        if (done < len && !yew_textiter_advance(&it, tb))
            YEW_BUG("edit delete: truncated iterator advance");
    }
    return copy;
}

static void require_edit_wrapped(const EditCtx *ec)
{
    if (ec->cset == NULL || ec->cset->curs.len <= 1U)
        return;
    if (ec->undo == NULL || ec->undo->depth == 0U ||
        ec->undo->pending_reason != YEW_TXN_MULTI)
        yew_cset_require_single_edit(ec->cset);
    if (!ec->cset->batching)
        yew_cset_check_text(ec->tb, ec->cset);
}

bool yew_edit_insert(EditCtx *ec, ByteOff at, const u8 *bytes, u64 len)
{
    LineNo line;
    u64 old_lines;
    u64 new_lines;
    u64 payload;

    edit_require(ec);
    if (at.v > yew_textbuf_len(ec->tb))
        YEW_BUG("edit insert: offset out of bounds");
    if (bytes == NULL && len != 0U)
        YEW_BUG("edit insert: NULL payload");
    if (len == 0U)
        return true;
    line = yew_textbuf_line_of(ec->tb, at);
    old_lines = yew_textbuf_line_count(ec->tb);
    require_edit_wrapped(ec);
    if (!yew_edit_ensure_journal(ec))
        return false;
    payload = ec->tb->add.len;
    if (ec->undo != NULL)
        yew_undo_prepare_insert(ec, at, len);
    yew_textbuf_insert(ec->tb, at, bytes, len);
    new_lines = yew_textbuf_line_count(ec->tb);
    if (ec->marks != NULL)
        yew_marks_adjust(ec->marks, YEW_JOURNAL_INS, at, len);
    if (ec->cset != NULL)
        yew_cset_adjust(ec->cset, YEW_JOURNAL_INS, at, len);
    if (ec->jrnl != NULL)
        (void)yew_journal_record(ec->jrnl, YEW_JOURNAL_INS, at.v, bytes,
                                 len);
    if (ec->undo != NULL)
        yew_undo_record_insert(ec, at, len, payload);
    if (ec->on_change != NULL)
        ec->on_change(ec->on_change_ctx, at, line, 0U,
                      new_lines - old_lines, ec->now_ms, true);
    return ec->jrnl == NULL || yew_journal_ok(ec->jrnl);
}

bool yew_edit_delete(EditCtx *ec, Span range)
{
    LineNo line;
    u64 old_lines;
    u64 new_lines;
    u8 *removed;
    u64 len;

    edit_require(ec);
    if (range.lo > range.hi || range.hi > yew_textbuf_len(ec->tb))
        YEW_BUG("edit delete: range out of bounds");
    len = range.hi - range.lo;
    if (len == 0U)
        return true;
    line = yew_textbuf_line_of(ec->tb, BYTEOFF(range.lo));
    old_lines = yew_textbuf_line_count(ec->tb);
    require_edit_wrapped(ec);
    removed = copy_range(ec->tb, range);
    if (!yew_edit_ensure_journal(ec)) {
        free(removed);
        return false;
    }
    if (ec->undo != NULL)
        yew_undo_prepare_delete(ec, range);
    yew_textbuf_delete(ec->tb, range);
    new_lines = yew_textbuf_line_count(ec->tb);
    if (ec->marks != NULL)
        yew_marks_adjust(ec->marks, YEW_JOURNAL_DEL, BYTEOFF(range.lo), len);
    if (ec->cset != NULL)
        yew_cset_adjust(ec->cset, YEW_JOURNAL_DEL, BYTEOFF(range.lo), len);
    if (ec->jrnl != NULL)
        (void)yew_journal_record(ec->jrnl, YEW_JOURNAL_DEL, range.lo,
                                 removed, len);
    if (ec->undo != NULL)
        yew_undo_record_delete(ec, range);
    if (ec->on_change != NULL)
        ec->on_change(ec->on_change_ctx, BYTEOFF(range.lo), line,
                      old_lines - new_lines, 0U, ec->now_ms, true);
    free(removed);
    return ec->jrnl == NULL || yew_journal_ok(ec->jrnl);
}

YewSaveErr yew_edit_save(EditCtx *ec, const char *path)
{
    YewSaveErr result;

    edit_require(ec);
    if (ec->meta == NULL)
        YEW_BUG("edit save: scratch buffer has no file metadata");
    if (ec->undo != NULL &&
        (ec->undo->depth != 0U || ec->undo->open != 0U))
        YEW_BUG("edit save: open undo transaction");
    result = yew_file_save(ec->tb, ec->meta, path);
    if (result != YEW_SAVE_OK)
        return result;
    if (ec->undo != NULL) {
        yew_undo_boundary(ec->undo);
        yew_undo_mark_saved(ec->undo);
    }
    if (ec->jrnl != NULL) {
        yew_journal_discard(ec->jrnl);
        ec->jrnl = NULL;
    }
    return YEW_SAVE_OK;
}
