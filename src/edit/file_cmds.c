#include "edit/file_cmds.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "fl/fltxn.h"
#include "mod/git/fussmode.h"
#include "mod/lsp/lsp.h"
#include "text/journal.h"
#include "ui/layout.h"
#include "ui/macrobrowse.h"
#include "ui/viewport.h"
#include "ws/symidx.h"
#include "ws/trust_prompt.h"

CmdStatus yew_file_cmd_save(Ed *ed, bool force)
{
    bool handled = false;
    CmdStatus status;

    if (ed == NULL)
        return YEW_CMD_ERR_ARG;
    status = yew_fuss_commit_save(ed, yew_ed_doc(ed), &handled);
    if (handled)
        return status;
    if (ed->win != NULL && ed->win->buf != NULL &&
        ed->win->buf->macro_reg != 0U)
        return yew_macro_store(ed, ed->win->buf);
    return yew_ed_file_save(ed, force);
}

CmdStatus yew_file_cmd_save_current(CmdCtx *cx)
{
    bool handled = false;
    CmdStatus status;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    status = yew_fuss_commit_save(cx->ed,
                                  cx->win == NULL ? NULL : cx->win->buf,
                                  &handled);
    if (handled)
        return status;
    if (cx->win != NULL && cx->win->buf != NULL &&
        cx->win->buf->macro_reg != 0U)
        return yew_macro_store(cx->ed, cx->win->buf);
    return yew_ed_file_save_win(cx->ed, cx->win, false);
}

CmdStatus yew_file_cmd_write(CmdCtx *cx)
{
    bool handled = false;
    CmdStatus status;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    if (cx->sarg != NULL &&
        memchr(cx->sarg, '\0', cx->sarg_len) != NULL)
        return YEW_CMD_ERR_ARG;
    if (cx->sarg != NULL && cx->sarg_len != 0U)
        return yew_ed_file_write_to_win(cx->ed, cx->win, cx->sarg,
                                        cx->bang);
    status = yew_fuss_commit_save(cx->ed,
                                  cx->win == NULL ? NULL : cx->win->buf,
                                  &handled);
    if (handled)
        return status;
    if (cx->win != NULL && cx->win->buf != NULL &&
        cx->win->buf->macro_reg != 0U)
        return yew_macro_store(cx->ed, cx->win->buf);
    return yew_ed_file_save_win(cx->ed, cx->win, cx->bang);
}

CmdStatus yew_file_cmd_write_quit(CmdCtx *cx)
{
    CmdStatus status = yew_file_cmd_write(cx);

    if (status != YEW_CMD_OK)
        return status;
    return yew_ed_request_quit(cx->ed, false);
}

CmdStatus yew_file_cmd_new(CmdCtx *cx)
{
    YewLoadErr load;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    if (yew_buf_dirty(&cx->ed->buffer) && !cx->bang) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "buffer has unsaved changes (use :new!)");
        return YEW_CMD_ERR_STATE;
    }
    if (cx->sarg == NULL || cx->sarg[0] == '\0')
        return yew_ed_open_scratch(cx->ed) ? YEW_CMD_OK : YEW_CMD_ERR_IO;
    load = yew_ed_open(cx->ed, cx->sarg);
    return load == YEW_LOAD_OK || load == YEW_LOAD_ENOENT ? YEW_CMD_OK :
                                                           YEW_CMD_ERR_IO;
}

CmdStatus yew_file_cmd_buf_open(CmdCtx *cx)
{
    Buffer *b;
    char *path;
    u32 old_nbufs;
    bool was_loaded;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->sarg_len == 0U || !cx->ed->model_ready ||
        cx->ed->fl_model_teardown)
        return YEW_CMD_ERR_ARG;
    if (memchr(cx->sarg, '\0', cx->sarg_len) != NULL)
        return YEW_CMD_ERR_ARG;
    path = yew_xmalloc((size_t)cx->sarg_len + 1U);
    (void)memcpy(path, cx->sarg, cx->sarg_len);
    path[cx->sarg_len] = '\0';
    old_nbufs = cx->ed->ws.nbufs;
    b = yew_ws_file_buf(cx->ed, path);
    yew_xfree(path);
    if (b == NULL)
        return YEW_CMD_ERR_IO;
    was_loaded = b->tb != NULL;
    if (yew_buf_hydrate(cx->ed, b) != 0) {
        /* Do not leave a failed first open as an unreachable deferred
         * buffer.  An existing deferred buffer stays registered so a later
         * retry can hydrate the same stable object. */
        if (cx->ed->ws.nbufs != old_nbufs)
            yew_ws_scratch_drop(cx->ed, b);
        return YEW_CMD_ERR_IO;
    }
    if (!yew_ed_show_buffer(cx->ed, b))
        return YEW_CMD_ERR_STATE;
    if (!was_loaded)
        yew_fl_hook_buffer(cx->ed, FL_EV_BUF_OPEN, b);
    return YEW_CMD_OK;
}

static bool job_uses_buffer(const Ed *ed, const Buffer *b)
{
    u32 i;

    for (i = 0U; i < ed->jobs.len; i++) {
        const YewJob *job = &ed->jobs.v[i];

        if (job->buf == b || (b->tb != NULL && job->in_buf == b->tb))
            return true;
    }
    return false;
}

static void retarget_tree(Ed *ed, Pane *root, Buffer *from, Buffer *to)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return;
    yew_pane_collect_leaves(root, leaves, YEW_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++) {
        Win *w = leaves[i]->win;

        if (w != NULL && w->buf == from)
            yew_ed_win_set_buffer(ed, w, to);
    }
}

static void retarget_buffer(Ed *ed, Buffer *from, Buffer *to)
{
    size_t i;

    /* yew_ed_show_buffer also repairs the register context for the focused
     * window.  The remaining views are reset through the general window
     * seam below. */
    if (ed->win != NULL && ed->win->buf == from)
        (void)yew_ed_show_buffer(ed, to);
    for (i = 0U; i < ed->tabs.v.len; i++) {
        Tab *tab = &ed->tabs.v.data[i];

        retarget_tree(ed, tab->root, from, to);
        if (tab->buffer_id == from->id) {
            tab->buffer_id = to->id;
            yew_xfree(tab->path);
            tab->path = NULL;
            if (to->path != NULL) {
                size_t n = strlen(to->path) + 1U;

                tab->path = yew_xmalloc(n);
                (void)memcpy(tab->path, to->path, n);
            }
        }
    }
    retarget_tree(ed, ed->pane_root, from, to);
}

static void reset_window_view(Win *w)
{
    Cursor origin = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    if (w == NULL)
        return;
    yew_vp_free(w);
    yew_cset_free(&w->cs);
    yew_cset_init(&w->cs, origin);
    yew_vp_init(w);
}

static void reset_primary_tree(Pane *root, Buffer *primary)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return;
    yew_pane_collect_leaves(root, leaves, YEW_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++) {
        Win *w = leaves[i]->win;

        if (w != NULL && w->buf == primary)
            reset_window_view(w);
    }
}

static void close_primary(Ed *ed)
{
    Buffer *b = &ed->buffer;
    u32 old_id = b->id;
    size_t i;

    fl_h_drop_buffer(ed, old_id);
    yew_symidx_drop_buffer(&ed->ws, old_id);
    yew_opt_scope_free(&b->opt_overrides);
    if (b->jrn != NULL)
        yew_journal_close(b->jrn);
    yew_marks_free(b->marks);
    yew_undo_free(b->undo);
    yew_syn_detach(&b->syn);
    yew_textbuf_free(b->tb);
    yew_filemeta_dispose(&b->meta);
    (void)memset(b, 0, sizeof(*b));
    yew_filemeta_init(&b->meta);
    b->id = ed->ws.next_buf_id++;
    b->tb = yew_textbuf_new();
    yew_syn_buf_init(&b->syn);
    yew_syn_attach(&b->syn, YEW_LANG_NONE, b->tb);
    b->undo = yew_undo_new(b->tb);
    yew_undo_mark_saved(b->undo);
    b->marks = yew_marks_new();
    b->tabwidth = YEW_VP_TABWIDTH;

    for (i = 0U; i < ed->tabs.v.len; i++) {
        Tab *tab = &ed->tabs.v.data[i];

        reset_primary_tree(tab->root, b);
        if (tab->buffer_id == old_id) {
            tab->buffer_id = b->id;
            yew_xfree(tab->path);
            tab->path = NULL;
        }
    }
    reset_primary_tree(ed->pane_root, b);
    if (ed->win != NULL && ed->win->buf == b)
        yew_reg_bind_context(&ed->regs, b->undo, &b->meta);
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    ed->drawn_cursor_line_valid = false;
    ed->drawn_top_valid = false;
    yew_state_mark_dirty(ed);
}

CmdStatus yew_file_cmd_buf_close(CmdCtx *cx)
{
    Buffer *b;
    bool handled = false;
    CmdStatus status;

    if (cx == NULL || cx->ed == NULL || !cx->ed->model_ready ||
        cx->ed->fl_model_teardown)
        return YEW_CMD_ERR_ARG;
    b = cx->win != NULL ? cx->win->buf : yew_ed_doc(cx->ed);
    if (b == NULL)
        return YEW_CMD_ERR_STATE;
    status = yew_fuss_commit_close(cx->ed, b, &handled);
    if (handled)
        return status;
    if (yew_buf_dirty(b) && !cx->bang)
        return YEW_CMD_ERR_IO;
    /* A subprocess owns these pointers until its slot is released.  Force
     * means discard unsaved bytes; it never means manufacture a dangling
     * asynchronous sink or input source. */
    if (job_uses_buffer(cx->ed, b))
        return YEW_CMD_ERR_STATE;
    if (fl_txn_enlisted(yew_fl_vm(cx->ed), b->undo))
        return YEW_CMD_ERR_STATE;
    cx->ed->fl_model_teardown = true;
    yew_fl_hook_buffer(cx->ed, FL_EV_BUF_CLOSE, b);
    if (yew_buf_dirty(b) && !cx->bang) {
        cx->ed->fl_model_teardown = false;
        return YEW_CMD_ERR_IO;
    }
    if (fl_txn_enlisted(yew_fl_vm(cx->ed), b->undo)) {
        cx->ed->fl_model_teardown = false;
        return YEW_CMD_ERR_STATE;
    }
    yew_lsp_buffer_close(cx->ed, b);
    if (b == &cx->ed->buffer) {
        close_primary(cx->ed);
        cx->ed->fl_model_teardown = false;
        return YEW_CMD_OK;
    }
    retarget_buffer(cx->ed, b, &cx->ed->buffer);
    fl_h_drop_buffer(cx->ed, b->id);
    {
        u32 closed_id = b->id;

        yew_ws_scratch_drop(cx->ed, b);
        yew_trust_prompt_buffer_closed(cx->ed, closed_id);
    }
    yew_state_mark_dirty(cx->ed);
    cx->ed->fl_model_teardown = false;
    return YEW_CMD_OK;
}

CmdStatus yew_file_cmd_reload(CmdCtx *cx)
{
    YewLoadErr load;
    const char *path;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    path = yew_ed_doc(cx->ed)->path;
    if (path == NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "buffer has no file name");
        return YEW_CMD_ERR_STATE;
    }
    if (yew_buf_dirty(&cx->ed->buffer) && !cx->bang) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "buffer has unsaved changes (use :reload!)");
        return YEW_CMD_ERR_STATE;
    }
    load = yew_ed_open(cx->ed, path);
    return load == YEW_LOAD_OK ? YEW_CMD_OK : YEW_CMD_ERR_IO;
}

CmdStatus yew_file_cmd_quit(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    return yew_ed_request_quit(cx->ed, cx->bang);
}

CmdStatus yew_file_cmd_quit_force(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    return yew_ed_request_quit(cx->ed, true);
}

CmdStatus yew_file_cmd_suspend(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    yew_tty_suspend(&cx->ed->tty);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_file_cmd_redraw(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    if (cx->ed->win != NULL)
        yew_vp_invalidate(cx->ed->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}
