#include "edit/notify.h"

#include "edit/buf.h"
#include "edit/shadow.h"
#include "util/log.h"

/* Sprint 44 replaces these two fixed no-op consumers in place. */
static void yew_symidx_note_pre(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    (void)ec;
    (void)kind;
    (void)at;
    (void)len;
}

static void yew_symidx_note_post(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    (void)ec;
    (void)kind;
    (void)at;
    (void)len;
}

/* Sprint 46 replaces this fixed core shim with the module consumer. */
static void yew_lsp_note_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    (void)ec;
    (void)kind;
    (void)at;
    (void)len;
}

void yew_edit_notify_pre(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    if (ec == NULL || ec->tb == NULL ||
        (kind != YEW_JOURNAL_INS && kind != YEW_JOURNAL_DEL))
        YEW_BUG("edit notify pre: invalid edit");
    ec->notify_line = yew_textbuf_line_of(ec->tb, at);
    ec->notify_old_lines = yew_textbuf_line_count(ec->tb);
    yew_symidx_note_pre(ec, kind, at, len);
    yew_lsp_note_edit(ec, kind, at, len);
}

void yew_edit_notify_post(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    u64 new_lines;
    u64 removed;
    u64 inserted;

    if (ec == NULL || ec->tb == NULL ||
        (kind != YEW_JOURNAL_INS && kind != YEW_JOURNAL_DEL))
        YEW_BUG("edit notify post: invalid edit");
    new_lines = yew_textbuf_line_count(ec->tb);
    removed = ec->notify_old_lines > new_lines ?
                  ec->notify_old_lines - new_lines : 0U;
    inserted = new_lines > ec->notify_old_lines ?
                   new_lines - ec->notify_old_lines : 0U;
    if (ec->buffer != NULL) {
        if (ec->notify_old_lines == ec->buffer->syn.entry.len)
            yew_syn_edit(&ec->buffer->syn, ec->notify_line,
                         removed, inserted);
        else
            yew_syn_attach(&ec->buffer->syn, ec->buffer->syn.lang, ec->tb);
    }
    yew_shadow_on_edit(ec, kind, at, len);
    yew_symidx_note_post(ec, kind, at, len);
}
