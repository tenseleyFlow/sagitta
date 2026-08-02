#define _POSIX_C_SOURCE 200809L

#include "edit/ed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui/draw.h"
#include "ui/viewport.h"
#include "util/log.h"

static void ed_buffer_free(Ed *ed)
{
    Buffer *b = &ed->buffer;

    if (!ed->model_ready)
        return;
    sag_ed_insert_barrier(ed);
    sag_vp_free(&ed->single_win);
    if (b->jrn != NULL) {
        sag_journal_close(b->jrn);
        b->jrn = NULL;
    }
    sag_cset_free(&ed->single_win.cs);
    sag_marks_free(b->marks);
    sag_undo_free(b->undo);
    sag_textbuf_free(b->tb);
    sag_filemeta_dispose(&b->meta);
    (void)memset(b, 0, sizeof(*b));
    (void)memset(&ed->single_win, 0, sizeof(ed->single_win));
    ed->win = NULL;
    ed->ws.bufs = NULL;
    ed->ws.nbufs = 0U;
    ed->model_ready = false;
}

static bool ed_model_finish(Ed *ed, TextBuf *tb, const char *path)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    ed->buffer.tb = tb;
    ed->buffer.path = path == NULL ? NULL : arena_strdup(&ed->arena, path);
    ed->buffer.undo = sag_undo_new(tb);
    sag_undo_mark_saved(ed->buffer.undo);
    ed->buffer.marks = sag_marks_new();
    ed->buffer.jrn = NULL;
    sag_cset_init(&ed->single_win.cs, cursor);
    ed->single_win.buf = &ed->buffer;
    sag_vp_init(&ed->single_win);
    ed->win = &ed->single_win;
    ed->ws.bufs = &ed->buffer;
    ed->ws.nbufs = 1U;
    ed->model_ready = true;
    ed->durability_failed = false;
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    ed->doc_damage_lo = 0U;
    ed->doc_damage_hi = 0U;
    ed->drawn_top_valid = false;
    return true;
}

void sag_ed_init(Ed *ed)
{
    if (ed == NULL)
        SAG_BUG("editor init: NULL editor");
    (void)memset(ed, 0, sizeof(*ed));
    arena_init(&ed->arena);
    interner_init(&ed->interner, &ed->arena);
    bytebuf_init(&ed->frame);
    bytebuf_init(&ed->paste);
    sag_timers_init(&ed->timers);
    sag_dispatch_init(ed);
    ed->dispatch_ready = true;
    ed->exit_code = SAG_EXIT_OK;
}

void sag_ed_free(Ed *ed)
{
    if (ed == NULL)
        return;
    ed_buffer_free(ed);
    sag_msg_clear(ed);
    if (ed->grid_ready)
        sag_grid_free(&ed->grid);
    if (ed->input_ready) {
        if (ed->tty_ready)
            sag_input_disable(ed->tty.wfd);
        sag_input_free(&ed->in);
    }
    sag_term_oob_clear();
    if (ed->dispatch_ready)
        sag_dispatch_free(ed);
    sag_timers_free(&ed->timers);
    bytebuf_free(&ed->paste);
    bytebuf_free(&ed->frame);
    interner_free(&ed->interner);
    arena_free_all(&ed->arena);
    if (ed->tty_ready) {
        sag_tty_altscreen(&ed->tty, false);
        sag_tty_close(&ed->tty);
    }
    (void)memset(ed, 0, sizeof(*ed));
}

SagLoadErr sag_ed_open(Ed *ed, const char *path)
{
    TextBuf *tb = NULL;
    SagLoadErr result;

    if (ed == NULL || path == NULL)
        return SAG_LOAD_IO;
    ed_buffer_free(ed);
    sag_filemeta_init(&ed->buffer.meta);
    result = sag_file_load(path, &tb, &ed->buffer.meta);
    if (result != SAG_LOAD_OK && result != SAG_LOAD_ENOENT) {
        sag_filemeta_dispose(&ed->buffer.meta);
        return result;
    }
    if (tb == NULL)
        tb = sag_textbuf_new();
    (void)ed_model_finish(ed, tb, path);
    if (ed->buffer.meta.realpath != NULL &&
        sag_journal_probe(ed->buffer.meta.realpath, &ed->buffer.meta))
        sag_ed_prompt(ed, SAG_PROMPT_RECOVER);
    return result;
}

bool sag_ed_open_scratch(Ed *ed)
{
    TextBuf *tb;

    if (ed == NULL)
        return false;
    ed_buffer_free(ed);
    sag_filemeta_init(&ed->buffer.meta);
    tb = sag_textbuf_new();
    return ed_model_finish(ed, tb, NULL);
}

bool sag_buf_dirty(const Buffer *b)
{
    return b != NULL && b->undo != NULL && !sag_undo_at_save_point(b->undo);
}

EditCtx sag_ed_edit_ctx(Ed *ed)
{
    EditCtx ec = {0};

    if (ed == NULL || !ed->model_ready || ed->win == NULL)
        return ec;
    ec.tb = ed->buffer.tb;
    ec.marks = ed->buffer.marks;
    ec.cset = &ed->win->cs;
    ec.win_id = 0U;
    ec.jrnl = ed->buffer.jrn;
    ec.undo = ed->buffer.undo;
    ec.meta = ed->buffer.path == NULL ? NULL : &ed->buffer.meta;
    return ec;
}

void sag_ed_finish_edit(Ed *ed, const EditCtx *ec)
{
    if (ed == NULL || ec == NULL)
        return;
    ed->buffer.jrn = ec->jrnl;
    if (ec->jrnl != NULL && !sag_journal_ok(ec->jrnl)) {
        ed->durability_failed = true;
        sag_msg(ed, SAG_MSG_ERROR,
                "crash journal failed; save or q! before continuing");
    }
}

void sag_ed_damage_document(Ed *ed)
{
    if (ed == NULL || ed->win == NULL)
        return;
    ed->doc_damage_lo = 0U;
    ed->doc_damage_hi = ed->win->rect.h;
    ed->footer_dirty = true;
}

void sag_ed_damage_line(Ed *ed, LineNo line, bool line_count_changed)
{
    Win *win;
    LineNo top;
    u16 lo;
    u16 hi;

    if (ed == NULL || ed->win == NULL)
        return;
    win = ed->win;
    sag_vp_invalidate_from(win, line);
    if (line_count_changed)
        ed->layout_dirty = true;
    if (win->vp.wrap) {
        sag_ed_damage_document(ed);
        return;
    }
    top = sag_win_view_top(win);
    if (line.v < top.v) {
        if (line_count_changed)
            sag_ed_damage_document(ed);
        return;
    }
    if (!sag_win_view_row(win, line, &lo))
        return;
    hi = line_count_changed ? win->rect.h : (u16)(lo + 1U);
    if (ed->doc_damage_lo >= ed->doc_damage_hi) {
        ed->doc_damage_lo = lo;
        ed->doc_damage_hi = hi;
    } else {
        if (lo < ed->doc_damage_lo)
            ed->doc_damage_lo = lo;
        if (hi > ed->doc_damage_hi)
            ed->doc_damage_hi = hi;
    }
}

Cursor *sag_ed_cursor(Ed *ed)
{
    if (ed == NULL || ed->win == NULL || ed->win->cs.curs.len == 0U ||
        (size_t)ed->win->cs.primary >= ed->win->cs.curs.len)
        return NULL;
    return &ed->win->cs.curs.data[ed->win->cs.primary];
}

void sag_ed_insert_barrier(Ed *ed)
{
    EditCtx ec;

    if (ed == NULL || !ed->insert_txn)
        return;
    ec = sag_ed_edit_ctx(ed);
    sag_undo_end(&ec);
    sag_ed_finish_edit(ed, &ec);
    ed->insert_txn = false;
}

CmdStatus sag_ed_invoke(Ed *ed, CmdId id, CmdCtx *cx)
{
    const CmdDesc *desc;
    EditCtx ec;
    CmdStatus status;
    bool changes;
    bool durability_command;
    bool newline;
    bool opened = false;
    bool started_in_insert;

    if (ed == NULL || cx == NULL)
        return SAG_CMD_ERR_ARG;
    desc = sag_cmd_desc(id);
    if (desc == NULL)
        return SAG_CMD_ERR_ARG;
    cx->ed = ed;
    if (cx->win == NULL)
        cx->win = ed->win;
    changes = (desc->flags & SAG_CMD_CHANGES_BUFFER) != 0U;
    durability_command = changes ||
                         strcmp(desc->name, "ed.edit.undo") == 0 ||
                         strcmp(desc->name, "ed.edit.redo") == 0;
    newline = strcmp(desc->name, "ed.edit.insert.newline") == 0;
    started_in_insert = ed->mode == SAG_MODE_I;

    if (durability_command && ed->durability_failed) {
        sag_msg(ed, SAG_MSG_ERROR,
                "crash journal failed; save or q! before continuing");
        return SAG_CMD_ERR_IO;
    }

    if (started_in_insert && (!changes || newline))
        sag_ed_insert_barrier(ed);
    if (changes && ed->model_ready) {
        ec = sag_ed_edit_ctx(ed);
        if (started_in_insert && !newline) {
            if (!ed->insert_txn) {
                sag_undo_begin(&ec, SAG_TXN_TYPE);
                ed->insert_txn = true;
                opened = true;
            }
        } else {
            sag_undo_begin(&ec,
                           strstr(desc->name, ".delete.") != NULL ||
                                   strcmp(desc->name,
                                          "ed.edit.line.delete") == 0
                               ? SAG_TXN_ERASE
                               : SAG_TXN_TYPE);
            opened = true;
        }
    }

    status = sag_cmd_invoke(id, cx);
    if (changes && ed->model_ready) {
        ec = sag_ed_edit_ctx(ed);
        if (status != SAG_CMD_OK &&
            (opened || (started_in_insert && ed->insert_txn))) {
            sag_undo_abort(&ec);
            if (started_in_insert)
                ed->insert_txn = false;
        } else if (!started_in_insert && ed->mode == SAG_MODE_I &&
                   !newline && opened) {
            /* `o`/`O` create the line and enter I as one editing run. */
            ed->insert_txn = true;
        } else if ((!started_in_insert || newline) && opened) {
            sag_undo_end(&ec);
        }
        sag_ed_finish_edit(ed, &ec);
    }
    if (durability_command && status == SAG_CMD_ERR_IO) {
        ed->durability_failed = true;
        sag_msg(ed, SAG_MSG_ERROR,
                "crash journal failed; save or q! before continuing");
    }
    ed->footer_dirty = true;
    return status;
}

void sag_ed_prompt(Ed *ed, PromptKind prompt)
{
    const char *path;

    if (ed == NULL)
        return;
    ed->prompt = prompt;
    path = ed->buffer.path == NULL ? "[no name]" : ed->buffer.path;
    switch (prompt) {
    case SAG_PROMPT_NONE:
        sag_msg_clear(ed);
        break;
    case SAG_PROMPT_RECOVER:
        sag_msg(ed, SAG_MSG_ERROR,
                "recover unsaved changes to %s? [r]ecover [d]iscard [esc] open as-is",
                path);
        break;
    case SAG_PROMPT_QUIT_DIRTY:
        sag_msg(ed, SAG_MSG_ERROR,
                "save changes to %s? [w]rite [d]iscard [esc] cancel", path);
        break;
    case SAG_PROMPT_OVERWRITE:
        sag_msg(ed, SAG_MSG_ERROR,
                "file changed on disk - [o]verwrite [esc] cancel");
        break;
    }
    if (prompt != SAG_PROMPT_NONE)
        ed->msg.prompt = true;
}

CmdStatus sag_ed_file_save(Ed *ed, bool force)
{
    EditCtx ec;
    SagSaveErr result;
    u64 lines;

    if (ed == NULL || !ed->model_ready)
        return SAG_CMD_ERR_STATE;
    sag_ed_insert_barrier(ed);
    if (ed->buffer.path == NULL) {
        sag_msg(ed, SAG_MSG_ERROR,
                "no file name (save-as lands in Sprint 18)");
        ed->quit_after_save = false;
        return SAG_CMD_ERR_STATE;
    }
    ec = sag_ed_edit_ctx(ed);
    if (force) {
        result = sag_file_save_force(ec.tb, ec.meta, ed->buffer.path);
        if (result == SAG_SAVE_OK) {
            sag_undo_boundary(ec.undo);
            sag_undo_mark_saved(ec.undo);
            if (ec.jrnl != NULL) {
                sag_journal_discard(ec.jrnl);
                ec.jrnl = NULL;
            }
        }
    } else {
        result = sag_edit_save(&ec, ed->buffer.path);
    }
    sag_ed_finish_edit(ed, &ec);
    if (result == SAG_SAVE_CHANGED_ON_DISK) {
        sag_ed_prompt(ed, SAG_PROMPT_OVERWRITE);
        return SAG_CMD_ERR_IO;
    }
    if (result != SAG_SAVE_OK) {
        sag_msg(ed, SAG_MSG_ERROR, "could not write %s", ed->buffer.path);
        ed->quit_after_save = false;
        return SAG_CMD_ERR_IO;
    }
    ed->durability_failed = false;
    ed->prompt = SAG_PROMPT_NONE;
    lines = sag_textbuf_line_count(ed->buffer.tb);
    sag_msg(ed, SAG_MSG_INFO, "wrote %s, %llu lines", ed->buffer.path,
            (unsigned long long)lines);
    if (ed->quit_after_save) {
        ed->quit_after_save = false;
        ed->quit = true;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_ed_request_quit(Ed *ed, bool force)
{
    if (ed == NULL || !ed->model_ready)
        return SAG_CMD_ERR_STATE;
    sag_ed_insert_barrier(ed);
    if (!force && ed->durability_failed) {
        sag_msg(ed, SAG_MSG_ERROR,
                "crash journal failed; save or q! to discard changes");
        return SAG_CMD_ERR_IO;
    }
    if (!force && sag_buf_dirty(&ed->buffer)) {
        sag_ed_prompt(ed, SAG_PROMPT_QUIT_DIRTY);
        return SAG_CMD_OK;
    }
    ed->quit = true;
    ed->exit_code = SAG_EXIT_OK;
    return SAG_CMD_OK;
}

static bool prompt_key(Ed *ed, Key key)
{
    u32 code = key.code;

    if (key.ev == SAG_KEY_RELEASE)
        return true;
    if (code == SAG_KEY_ESCAPE) {
        ed->quit_after_save = false;
        sag_ed_prompt(ed, SAG_PROMPT_NONE);
        return true;
    }
    if (key.mods != 0U || code > 0x7fU)
        return true;
    if (ed->prompt == SAG_PROMPT_RECOVER && code == (u32)'r') {
        EditCtx ec = sag_ed_edit_ctx(ed);
        bool recovered = sag_journal_replay_edit(ed->buffer.meta.realpath,
                                                 &ec, &ed->buffer.meta);

        if (recovered)
            ec.jrnl = sag_journal_open(ed->buffer.meta.realpath,
                                       &ed->buffer.meta);
        sag_ed_finish_edit(ed, &ec);
        sag_ed_prompt(ed, SAG_PROMPT_NONE);
        if (recovered && ec.jrnl == NULL) {
            ed->durability_failed = true;
            sag_msg(ed, SAG_MSG_ERROR,
                    "recovered changes, but crash journal could not reopen; save or q!");
        } else {
            sag_msg(ed, recovered ? SAG_MSG_WARN : SAG_MSG_ERROR,
                    recovered ? "recovered unsaved changes" :
                                "could not recover unsaved changes");
        }
        return true;
    }
    if (ed->prompt == SAG_PROMPT_RECOVER && code == (u32)'d') {
        bool discarded = sag_journal_discard_path(
            ed->buffer.meta.realpath, &ed->buffer.meta);

        sag_ed_prompt(ed, SAG_PROMPT_NONE);
        if (!discarded)
            sag_msg(ed, SAG_MSG_ERROR, "could not discard recovery journal");
        return true;
    }
    if (ed->prompt == SAG_PROMPT_QUIT_DIRTY && code == (u32)'w') {
        ed->quit_after_save = true;
        (void)sag_ed_file_save(ed, false);
        return true;
    }
    if (ed->prompt == SAG_PROMPT_QUIT_DIRTY && code == (u32)'d') {
        ed->prompt = SAG_PROMPT_NONE;
        ed->quit = true;
        return true;
    }
    if (ed->prompt == SAG_PROMPT_OVERWRITE && code == (u32)'o') {
        (void)sag_ed_file_save(ed, true);
        return true;
    }
    return true;
}

void sag_ed_handle_key(Ed *ed, Key key, i64 now_ms)
{
    const u16 command_mods = SAG_MOD_ALT | SAG_MOD_CTRL | SAG_MOD_SUPER |
                             SAG_MOD_HYPER | SAG_MOD_META;

    if (ed == NULL || key.kind != SAG_EV_KEY)
        return;
    if (key.ev != SAG_KEY_RELEASE && sag_msg_dismiss_overlay(ed)) {
        return;
    }
    if (key.code == SAG_KEY_ESCAPE &&
        (ed->chord.n != 0U || ed->chord.count_given)) {
        sag_dispatch_key(ed, key, now_ms);
        return;
    }
    if (ed->prompt != SAG_PROMPT_NONE) {
        (void)prompt_key(ed, key);
        return;
    }
    if (ed->msg.active && ed->msg.sev == SAG_MSG_ERROR)
        sag_msg_clear(ed);
    if (ed->mode == SAG_MODE_I && key.ev != SAG_KEY_RELEASE &&
        key.ntext != 0U && (key.mods & command_mods) == 0U) {
        CmdCtx cx = {0};
        CmdId id = sag_cmd_lookup("ed.edit.insert.text", 19U);

        cx.ed = ed;
        cx.win = ed->win;
        cx.count = 1U;
        cx.sarg = (const char *)key.text;
        cx.sarg_len = key.ntext;
        cx.source = SAG_SRC_KEY;
        ed->last_cmd = id;
        ed->last_status = sag_ed_invoke(ed, id, &cx);
        ed->dispatch_count++;
        return;
    }
    sag_dispatch_key(ed, key, now_ms);
}

void sag_ed_handle_paste(Ed *ed, const u8 *bytes, size_t len, bool end)
{
    if (ed == NULL)
        return;
    if (!end && bytes == NULL && len == 0U) {
        ed->paste.len = 0U;
        ed->paste_active = true;
        return;
    }
    if (!ed->paste_active)
        return;
    if (!end) {
        if (bytes != NULL && len != 0U)
            bytebuf_append(&ed->paste, bytes, len);
        return;
    }
    if (ed->mode == SAG_MODE_I && ed->prompt == SAG_PROMPT_NONE &&
        ed->paste.len != 0U) {
        CmdCtx cx = {0};
        CmdId id = sag_cmd_lookup("ed.edit.insert.text", 19U);

        cx.ed = ed;
        cx.win = ed->win;
        cx.count = 1U;
        cx.sarg = (const char *)ed->paste.data;
        cx.sarg_len = ed->paste.len > UINT32_MAX ? UINT32_MAX :
                                                    (u32)ed->paste.len;
        cx.source = SAG_SRC_KEY;
        ed->last_cmd = id;
        ed->last_status = sag_ed_invoke(ed, id, &cx);
        ed->dispatch_count++;
    }
    ed->paste.len = 0U;
    ed->paste_active = false;
}

void sag_ed_resize(Ed *ed, bool resumed)
{
    bool resized = false;

    if (ed == NULL || !ed->tty_ready)
        return;
    if (resumed) {
        if (!ed->tty.raw) {
            ed->quit = true;
            ed->exit_code = SAG_EXIT_IO;
            return;
        }
        sag_tty_altscreen(&ed->tty, true);
        sag_input_enable(ed->tty.wfd, &ed->tty.caps);
    }
    if (sag_tty_winsize(&ed->tty) && ed->grid_ready &&
        (ed->grid.rows != (u16)ed->tty.rows ||
         ed->grid.cols != (u16)ed->tty.cols)) {
        if (!sag_grid_resize(&ed->grid, (u16)ed->tty.rows,
                             (u16)ed->tty.cols)) {
            ed->quit = true;
            ed->exit_code = SAG_EXIT_IO;
            return;
        }
        if (ed->win != NULL)
            sag_vp_invalidate(ed->win);
        resized = true;
    }
    if (!resumed && !resized)
        return;
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
}

static bool write_all(int fd, const u8 *bytes, size_t len)
{
    while (len != 0U) {
        ssize_t n = write(fd, bytes, len);

        if (n > 0) {
            bytes += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void sag_ed_render(Ed *ed)
{
    Win *win;

    if (ed == NULL || !ed->grid_ready || !ed->render_ready ||
        !ed->model_ready)
        return;
    win = ed->win;
    if (!ed->drawn_top_valid ||
        ed->drawn_top.v != sag_win_view_top(win).v ||
        ed->drawn_top_sub != win->vp.top_sub ||
        ed->drawn_left.v != win->vp.left.v ||
        ed->drawn_wrap != win->vp.wrap)
        sag_ed_damage_document(ed);
    if (ed->full_damage) {
        sag_draw_document_rows(ed, win, 0U, win->rect.h);
        sag_grid_mark_all(&ed->grid);
    } else if (ed->doc_damage_lo < ed->doc_damage_hi) {
        sag_draw_document_rows(ed, win, ed->doc_damage_lo,
                               ed->doc_damage_hi);
    }
    if (ed->full_damage || ed->footer_dirty)
        sag_draw_footer(ed, win);
    sag_draw_cursor(ed, win);
    ed->frame.len = 0U;
    (void)sag_render_frame(&ed->render, &ed->grid, &ed->frame);
    if (!write_all(ed->tty.wfd, ed->frame.data, ed->frame.len)) {
        ed->quit = true;
        ed->exit_code = SAG_EXIT_IO;
        return;
    }
    sag_grid_flip(&ed->grid);
    ed->full_damage = false;
    ed->footer_dirty = false;
    ed->doc_damage_lo = ed->grid.rows;
    ed->doc_damage_hi = 0U;
    ed->drawn_top = sag_win_view_top(win);
    ed->drawn_top_sub = win->vp.top_sub;
    ed->drawn_left = win->vp.left;
    ed->drawn_wrap = win->vp.wrap;
    ed->drawn_top_valid = true;
}

static const char *load_error_text(SagLoadErr error)
{
    switch (error) {
    case SAG_LOAD_EACCES:
        return "permission denied";
    case SAG_LOAD_EISDIR:
        return "directory arguments land in Sprint 25";
    case SAG_LOAD_TOO_LARGE:
        return "file is too large";
    case SAG_LOAD_IO:
        return "input/output error";
    case SAG_LOAD_OK:
    case SAG_LOAD_ENOENT:
        break;
    }
    return "unknown load error";
}

static const char *ed_getenv(const char *name)
{
    return getenv(name);
}

static int ed_driver_inner(const char *path)
{
    Ed ed;
    SagLoadErr load = SAG_LOAD_OK;
    int result;
    u16 rows;
    u16 cols;

    sag_ed_init(&ed);
    errno = 0;
    if (!sag_tty_open(&ed.tty)) {
        result = errno == ENOTTY ? SAG_EXIT_ERR : SAG_EXIT_IO;
        sag_tty_close(&ed.tty);
        sag_ed_free(&ed);
        return result;
    }
    ed.tty_ready = true;
    if (!sag_tty_raw(&ed.tty)) {
        sag_ed_free(&ed);
        return SAG_EXIT_IO;
    }
    sag_tty_altscreen(&ed.tty, true);
    if (!ed.tty.alt) {
        sag_ed_free(&ed);
        return SAG_EXIT_IO;
    }
    sag_tty_probe_start(&ed.tty, sag_now_ms());
    sag_input_enable(ed.tty.wfd, &ed.tty.caps);
    sag_input_init(&ed.in, &ed.tty.caps);
    ed.input_ready = true;

    if (path == NULL) {
        (void)sag_ed_open_scratch(&ed);
    } else {
        load = sag_ed_open(&ed, path);
        if (load != SAG_LOAD_OK && load != SAG_LOAD_ENOENT) {
            const char *message = load_error_text(load);

            sag_ed_free(&ed);
            (void)fprintf(stderr, "sagitta: error: cannot open %s: %s\n",
                          path, message);
            return SAG_EXIT_IO;
        }
    }
    rows = ed.tty.rows > 0 && ed.tty.rows <= UINT16_MAX ?
               (u16)ed.tty.rows : 24U;
    cols = ed.tty.cols > 0 && ed.tty.cols <= UINT16_MAX ?
               (u16)ed.tty.cols : 80U;
    if (!sag_grid_init(&ed.grid, &ed.interner, rows, cols)) {
        sag_ed_free(&ed);
        return SAG_EXIT_IO;
    }
    ed.grid_ready = true;
    sag_render_init(&ed.render, &ed.tty.caps, ed_getenv);
    ed.render_ready = true;
    sag_ed_layout(&ed);
    sag_ed_render(&ed);
    result = ed.quit ? ed.exit_code : sag_loop_run(&ed);
    sag_ed_free(&ed);
    return result;
}

int sag_ed_driver(const char *path)
{
    TtyGuard guard;
    int result;

    if (!sag_tty_guard_start(&guard))
        return SAG_EXIT_IO;
    result = ed_driver_inner(path);
    if (!sag_tty_guard_finish(&guard) && result == SAG_EXIT_OK)
        result = SAG_EXIT_IO;
    return result;
}
