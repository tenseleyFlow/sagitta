#include "edit/file_cmds.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "fl/flruntime.h"
#include "fl/fltxn.h"
#include "text/journal.h"
#include "ui/layout.h"
#include "ui/viewport.h"

CmdStatus sag_file_cmd_save(Ed *ed, bool force)
{
    if (ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_file_save(ed, force);
}

CmdStatus sag_file_cmd_save_current(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_file_save_win(cx->ed, cx->win, false);
}

CmdStatus sag_file_cmd_write(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->sarg != NULL &&
        memchr(cx->sarg, '\0', cx->sarg_len) != NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->sarg != NULL && cx->sarg_len != 0U)
        return sag_ed_file_write_to_win(cx->ed, cx->win, cx->sarg,
                                        cx->bang);
    return sag_ed_file_save_win(cx->ed, cx->win, cx->bang);
}

CmdStatus sag_file_cmd_write_quit(CmdCtx *cx)
{
    CmdStatus status = sag_file_cmd_write(cx);

    if (status != SAG_CMD_OK)
        return status;
    return sag_ed_request_quit(cx->ed, false);
}

CmdStatus sag_file_cmd_new(CmdCtx *cx)
{
    SagLoadErr load;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (sag_buf_dirty(&cx->ed->buffer) && !cx->bang) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "buffer has unsaved changes (use :new!)");
        return SAG_CMD_ERR_STATE;
    }
    if (cx->sarg == NULL || cx->sarg[0] == '\0')
        return sag_ed_open_scratch(cx->ed) ? SAG_CMD_OK : SAG_CMD_ERR_IO;
    load = sag_ed_open(cx->ed, cx->sarg);
    return load == SAG_LOAD_OK || load == SAG_LOAD_ENOENT ? SAG_CMD_OK :
                                                           SAG_CMD_ERR_IO;
}

CmdStatus sag_file_cmd_buf_open(CmdCtx *cx)
{
    Buffer *b;
    char *path;
    u32 old_nbufs;
    bool was_loaded;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->sarg_len == 0U || !cx->ed->model_ready ||
        cx->ed->fl_model_teardown)
        return SAG_CMD_ERR_ARG;
    if (memchr(cx->sarg, '\0', cx->sarg_len) != NULL)
        return SAG_CMD_ERR_ARG;
    path = sag_xmalloc((size_t)cx->sarg_len + 1U);
    (void)memcpy(path, cx->sarg, cx->sarg_len);
    path[cx->sarg_len] = '\0';
    old_nbufs = cx->ed->ws.nbufs;
    b = sag_ws_file_buf(cx->ed, path);
    free(path);
    if (b == NULL)
        return SAG_CMD_ERR_IO;
    was_loaded = b->tb != NULL;
    if (sag_buf_hydrate(cx->ed, b) != 0) {
        /* Do not leave a failed first open as an unreachable deferred
         * buffer.  An existing deferred buffer stays registered so a later
         * retry can hydrate the same stable object. */
        if (cx->ed->ws.nbufs != old_nbufs)
            sag_ws_scratch_drop(cx->ed, b);
        return SAG_CMD_ERR_IO;
    }
    if (!sag_ed_show_buffer(cx->ed, b))
        return SAG_CMD_ERR_STATE;
    if (!was_loaded)
        sag_fl_hook_buffer(cx->ed, FL_EV_BUF_OPEN, b);
    return SAG_CMD_OK;
}

static bool job_uses_buffer(const Ed *ed, const Buffer *b)
{
    u32 i;

    for (i = 0U; i < ed->jobs.len; i++) {
        const SagJob *job = &ed->jobs.v[i];

        if (job->buf == b || (b->tb != NULL && job->in_buf == b->tb))
            return true;
    }
    return false;
}

static void retarget_tree(Ed *ed, Pane *root, Buffer *from, Buffer *to)
{
    Pane *leaves[SAG_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return;
    sag_pane_collect_leaves(root, leaves, SAG_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++) {
        Win *w = leaves[i]->win;

        if (w != NULL && w->buf == from)
            sag_ed_win_set_buffer(ed, w, to);
    }
}

static void retarget_buffer(Ed *ed, Buffer *from, Buffer *to)
{
    size_t i;

    /* sag_ed_show_buffer also repairs the register context for the focused
     * window.  The remaining views are reset through the general window
     * seam below. */
    if (ed->win != NULL && ed->win->buf == from)
        (void)sag_ed_show_buffer(ed, to);
    for (i = 0U; i < ed->tabs.v.len; i++) {
        Tab *tab = &ed->tabs.v.data[i];

        retarget_tree(ed, tab->root, from, to);
        if (tab->buffer_id == from->id) {
            tab->buffer_id = to->id;
            free(tab->path);
            tab->path = NULL;
            if (to->path != NULL) {
                size_t n = strlen(to->path) + 1U;

                tab->path = sag_xmalloc(n);
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
    sag_vp_free(w);
    sag_cset_free(&w->cs);
    sag_cset_init(&w->cs, origin);
    sag_vp_init(w);
}

static void reset_primary_tree(Pane *root, Buffer *primary)
{
    Pane *leaves[SAG_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return;
    sag_pane_collect_leaves(root, leaves, SAG_ARRAY_LEN(leaves), &n);
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
    if (b->jrn != NULL)
        sag_journal_close(b->jrn);
    sag_marks_free(b->marks);
    sag_undo_free(b->undo);
    sag_textbuf_free(b->tb);
    sag_filemeta_dispose(&b->meta);
    (void)memset(b, 0, sizeof(*b));
    sag_filemeta_init(&b->meta);
    b->id = ed->ws.next_buf_id++;
    b->tb = sag_textbuf_new();
    b->undo = sag_undo_new(b->tb);
    sag_undo_mark_saved(b->undo);
    b->marks = sag_marks_new();
    b->tabwidth = SAG_VP_TABWIDTH;

    for (i = 0U; i < ed->tabs.v.len; i++) {
        Tab *tab = &ed->tabs.v.data[i];

        reset_primary_tree(tab->root, b);
        if (tab->buffer_id == old_id) {
            tab->buffer_id = b->id;
            free(tab->path);
            tab->path = NULL;
        }
    }
    reset_primary_tree(ed->pane_root, b);
    if (ed->win != NULL && ed->win->buf == b)
        sag_reg_bind_context(&ed->regs, b->undo, &b->meta);
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    ed->drawn_cursor_line_valid = false;
    ed->drawn_top_valid = false;
    sag_state_mark_dirty(ed);
}

CmdStatus sag_file_cmd_buf_close(CmdCtx *cx)
{
    Buffer *b;

    if (cx == NULL || cx->ed == NULL || !cx->ed->model_ready ||
        cx->ed->fl_model_teardown)
        return SAG_CMD_ERR_ARG;
    b = cx->win != NULL ? cx->win->buf : sag_ed_doc(cx->ed);
    if (b == NULL)
        return SAG_CMD_ERR_STATE;
    if (sag_buf_dirty(b) && !cx->bang)
        return SAG_CMD_ERR_IO;
    /* A subprocess owns these pointers until its slot is released.  Force
     * means discard unsaved bytes; it never means manufacture a dangling
     * asynchronous sink or input source. */
    if (job_uses_buffer(cx->ed, b))
        return SAG_CMD_ERR_STATE;
    if (fl_txn_enlisted(sag_fl_vm(cx->ed), b->undo))
        return SAG_CMD_ERR_STATE;
    cx->ed->fl_model_teardown = true;
    sag_fl_hook_buffer(cx->ed, FL_EV_BUF_CLOSE, b);
    if (sag_buf_dirty(b) && !cx->bang) {
        cx->ed->fl_model_teardown = false;
        return SAG_CMD_ERR_IO;
    }
    if (fl_txn_enlisted(sag_fl_vm(cx->ed), b->undo)) {
        cx->ed->fl_model_teardown = false;
        return SAG_CMD_ERR_STATE;
    }
    if (b == &cx->ed->buffer) {
        close_primary(cx->ed);
        cx->ed->fl_model_teardown = false;
        return SAG_CMD_OK;
    }
    retarget_buffer(cx->ed, b, &cx->ed->buffer);
    fl_h_drop_buffer(cx->ed, b->id);
    sag_ws_scratch_drop(cx->ed, b);
    sag_state_mark_dirty(cx->ed);
    cx->ed->fl_model_teardown = false;
    return SAG_CMD_OK;
}

CmdStatus sag_file_cmd_reload(CmdCtx *cx)
{
    SagLoadErr load;
    const char *path;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    path = sag_ed_doc(cx->ed)->path;
    if (path == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "buffer has no file name");
        return SAG_CMD_ERR_STATE;
    }
    if (sag_buf_dirty(&cx->ed->buffer) && !cx->bang) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "buffer has unsaved changes (use :reload!)");
        return SAG_CMD_ERR_STATE;
    }
    load = sag_ed_open(cx->ed, path);
    return load == SAG_LOAD_OK ? SAG_CMD_OK : SAG_CMD_ERR_IO;
}

CmdStatus sag_file_cmd_quit(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_request_quit(cx->ed, cx->bang);
}

CmdStatus sag_file_cmd_quit_force(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_request_quit(cx->ed, true);
}

CmdStatus sag_file_cmd_suspend(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    sag_tty_suspend(&cx->ed->tty);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_file_cmd_redraw(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->ed->win != NULL)
        sag_vp_invalidate(cx->ed->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}
