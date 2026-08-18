#include "edit/notify.h"

#include "edit/ed.h"
#include "edit/shadow.h"
#include "mod/lsp/lsp.h"
#include "util/log.h"
#include "ws/symidx.h"

static void adjust_other_window_cursors(EditCtx *ec, u8 kind, ByteOff at,
                                        u64 len)
{
    Ed *ed;
    size_t tab;

    if (ec->ed == NULL || ec->buffer == NULL)
        return;
    ed = ec->ed;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;

        yew_pane_collect_leaves(ed->tabs.v.data[tab].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++) {
            Win *win = leaves[i]->win;

            if (win != NULL && win->buf == ec->buffer &&
                &win->cs != ec->cset)
                yew_cset_adjust(&win->cs, kind, at, len);
        }
    }
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
    adjust_other_window_cursors(ec, kind, at, len);
    yew_lsp_note_edit_post(ec, kind, at, len);
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
