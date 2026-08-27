#define _XOPEN_SOURCE 700

#include "edit/ed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ui/ctxmenu.h"
#include "ui/draw.h"
#include "ui/grouppicker.h"
#include "ui/panel.h"
#include "ui/picker.h"
#include "ui/pickers.h"
#include "ui/shadowdraw.h"
#include "ui/viewport.h"
#include "fl/flruntime.h"
#include "fl/flconf.h"
#include "edit/option.h"
#include "edit/theme_cmds.h"
#include "edit/bind.h"
#include "edit/block.h"
#include "unicode/width.h"
#include "fl/fltxn.h"
#include "fl/record.h"
#include "fl/macrolib.h"
#include "mod/lsp/lsp.h"
#include "mod/ai/ai.h"
#include "mod/git/git.h"
#include "mod/git/editor.h"
#include "mod/git/fussmode.h"
#if YEW_WITH_PLUGINS
#include "mod/plug/plug.h"
#endif
#include "syn/defs.h"
#include "util/log.h"
#include "util/rss.h"

static void ed_syn_init(Buffer *b)
{
    yew_syn_buf_init(&b->syn);
}

static bool opt_string_is(const OptVal *value, const char *want)
{
    size_t len = strlen(want);

    return value != NULL &&
           (value->type == (u8)YEW_OPT_STR ||
            value->type == (u8)YEW_OPT_ENUM) &&
           value->as.str.len == (u32)len &&
           memcmp(value->as.str.s, want, len) == 0;
}

static void ed_save_opts(Ed *ed, Buffer *buffer, Win *win,
                         YewSaveOpts *opts)
{
    OptVal value;

    yew_file_save_opts_default(opts);
    if (yew_opt_get(ed, buffer, win, "save.strategy", 13U, &value)) {
        if (opt_string_is(&value, "atomic"))
            opts->strategy = YEW_SAVE_STRATEGY_ATOMIC;
        else if (opt_string_is(&value, "inplace"))
            opts->strategy = YEW_SAVE_STRATEGY_INPLACE;
    }
    if (yew_opt_get(ed, buffer, win, "save.check_disk", 15U, &value)) {
        if (opt_string_is(&value, "off"))
            opts->check_disk = YEW_SAVE_CHECK_OFF;
        else if (opt_string_is(&value, "content"))
            opts->check_disk = YEW_SAVE_CHECK_CONTENT;
    }
    if (yew_opt_get(ed, buffer, win, "save.check_disk_max", 19U, &value))
        opts->check_disk_max = (u64)value.as.i;
    if (yew_opt_get(ed, buffer, win, "save.backup_keep", 16U, &value))
        opts->backup_keep = (u32)value.as.i;
    if (yew_opt_get(ed, buffer, win, "save.backup_dir", 15U, &value))
        opts->backup_dir = value.as.str.s;
}

static u8 *ed_first_line(const TextBuf *tb, u32 *len_out)
{
    Span span = yew_textbuf_line_span(tb, LINENO(0U));
    u64 want = span.hi - span.lo;
    TextIter it;
    u8 *line;
    u64 copied = 0U;

    if (want > YEW_SYN_LINE_BYTE_CAP)
        want = YEW_SYN_LINE_BYTE_CAP;
    line = yew_xmalloc((size_t)(want == 0U ? 1U : want));
    if (want != 0U && yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
        while (copied < want) {
            const u8 *chunk;
            u64 n;

            if (!yew_textiter_chunk(&it, tb, &chunk, &n))
                break;
            if (n > want - copied)
                n = want - copied;
            (void)memcpy(line + copied, chunk, (size_t)n);
            copied += n;
            if (copied != want && !yew_textiter_advance(&it, tb))
                break;
        }
    }
    while (copied != 0U &&
           (line[copied - 1U] == '\n' || line[copied - 1U] == '\r'))
        copied--;
    *len_out = (u32)copied;
    return line;
}

static void ed_fortran_score(const TextBuf *tb, SynFortranScore *score)
{
    u64 line_count = yew_textbuf_line_count(tb);
    u64 line_no;

    yew_syn_fortran_score_init(score);
    for (line_no = 0U; line_no < line_count && score->nonblank < 100U;
         line_no++) {
        Span span = yew_textbuf_line_span(tb, LINENO(line_no));
        u64 want = span.hi - span.lo;
        TextIter it;
        u8 *line;
        u64 copied = 0U;

        if (want > YEW_SYN_LINE_BYTE_CAP)
            want = YEW_SYN_LINE_BYTE_CAP;
        line = yew_xmalloc((size_t)(want == 0U ? 1U : want));
        if (want != 0U && yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
            while (copied < want) {
                const u8 *chunk;
                u64 n;

                if (!yew_textiter_chunk(&it, tb, &chunk, &n))
                    break;
                if (n > want - copied)
                    n = want - copied;
                (void)memcpy(line + copied, chunk, (size_t)n);
                copied += n;
                if (copied != want && !yew_textiter_advance(&it, tb))
                    break;
            }
        }
        yew_syn_fortran_score_line(score, line, (size_t)copied);
        free(line);
    }
}

void yew_ed_syn_bind(Buffer *b)
{
    const SynLangDesc *desc;
    SynEngine *engine;
    SynFortranScore score;
    const SynFortranScore *score_ptr = NULL;
    SynFortranForm form = YEW_FORTRAN_AUTO;
    OptVal option;
    u8 *line;
    u32 len;
    u32 lang;

    if (b == NULL || b->tb == NULL)
        return;
    line = ed_first_line(b->tb, &len);
    if (b->owner != NULL &&
        yew_opt_get(b->owner, b, NULL, "fortran_form", 12U, &option) &&
        option.type == (u8)YEW_OPT_ENUM) {
        if (option.as.str.len == 4U &&
            memcmp(option.as.str.s, "free", 4U) == 0)
            form = YEW_FORTRAN_FREE;
        else if (option.as.str.len == 5U &&
                 memcmp(option.as.str.s, "fixed", 5U) == 0)
            form = YEW_FORTRAN_FIXED;
    }
    if (form == YEW_FORTRAN_AUTO &&
        yew_syn_fortran_ambiguous_path(b->path)) {
        ed_fortran_score(b->tb, &score);
        score_ptr = &score;
    }
    lang = yew_syn_lang_for_scored(b->path, line, len, score_ptr, form,
                                   false);
    free(line);
    yew_syn_attach(&b->syn, lang, b->tb);
    b->lang = NULL;
    if (lang == YEW_LANG_NONE)
        return;
    engine = yew_syn_engine_for(lang);
    if (engine == NULL)
        return;
    yew_syn_buf_bind(&b->syn, engine);
    desc = yew_syn_lang_desc(lang);
    b->lang = desc == NULL ? NULL :
              strcmp(desc->name, "fortran-fixed") == 0 ?
              "fortran(fixed)" : desc->name;
}

/* Tears down everything a buffer owns without touching the list slot. */
static void ed_buffer_dispose(Buffer *b)
{
    yew_syn_detach(&b->syn);
    yew_opt_scope_free(&b->opt_overrides);
    if (b->jrn != NULL) {
        yew_journal_close(b->jrn);
        b->jrn = NULL;
    }
    yew_marks_free(b->marks);
    yew_undo_free(b->undo);
    yew_textbuf_free(b->tb);
    yew_filemeta_dispose(&b->meta);
    (void)memset(b, 0, sizeof(*b));
}

static void ed_ws_free(Ed *ed)
{
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++)
        yew_symidx_drop_buffer(&ed->ws, ed->ws.bufs[i]->id);
    /* Slot 0 aliases &ed->buffer and is disposed by the caller; every
     * other slot is heap-owned here. */
    for (i = 1U; i < ed->ws.nbufs; i++) {
        ed_buffer_dispose(ed->ws.bufs[i]);
        free(ed->ws.bufs[i]);
    }
    free(ed->ws.bufs);
    ed->ws.bufs = NULL;
    ed->ws.nbufs = 0U;
    ed->ws.cap = 0U;
}

static void ed_buffer_free(Ed *ed)
{
    Buffer *b = &ed->buffer;
    u32 i;

    if (!ed->model_ready)
        return;
    ed->fl_model_teardown = true;
    for (i = 0U; i < ed->ws.nbufs; i++) {
        yew_lsp_buffer_close(ed, ed->ws.bufs[i]);
        yew_fl_hook_buffer(ed, FL_EV_BUF_CLOSE, ed->ws.bufs[i]);
    }
    yew_ed_insert_barrier(ed);
    yew_reg_bind_context(&ed->regs, NULL, NULL);
    yew_search_state_free(&ed->search);
    yew_search_confirm_cancel(ed);
    /*
     * Every tree belongs to a tab, including the first one, and each
     * tab releases its own leaves' views — so this is the ONE owner.
     * Freeing ed->panes here as well double-freed every split view.
     */
    yew_tabs_free(ed);
    /* After the tabs: dissolving a group walks the tabs array, so the
     * groups outlive the members they are asked about. */
    yew_groups_free(ed);
    ed->pane_root = NULL;
    ed->focus = NULL;
    yew_overlay_free(&ed->single_win.overlay);
    yew_overlay_free(&ed->single_win.lsp_highlight.read);
    yew_overlay_free(&ed->single_win.lsp_highlight.write);
    YewGitDiffRowStyleVec_free(&ed->single_win.git_diff_rows);
    SpanVec_free(&ed->single_win.git_diff_intra);
    yew_compl_free(&ed->single_win.compl);
    yew_panel_close(ed, &ed->single_win.panel);
    yew_shadow_dismiss(ed, &ed->single_win);
    yew_shadow_free(&ed->single_win.shadow);
    yew_gutter_signs_free(&ed->single_win);
    free(ed->single_win.syn_spans);
    ed->single_win.syn_spans = NULL;
    ed->single_win.syn_spans_cap = 0U;
    yew_vp_free(&ed->single_win);
    yew_cset_free(&ed->single_win.cs);
    yew_opt_scope_free(&ed->single_win.opt_overrides);
    ed_ws_free(ed);
    ed_buffer_dispose(b);
    (void)memset(&ed->single_win, 0, sizeof(ed->single_win));
    ed->win = NULL;
    ed->model_ready = false;
    ed->fl_model_teardown = false;
}

static void ed_ws_push(Ed *ed, Buffer *b)
{
    if (ed->ws.nbufs == ed->ws.cap) {
        u32 cap = ed->ws.cap == 0U ? 4U : ed->ws.cap * 2U;

        if (cap < ed->ws.cap)
            YEW_BUG("workspace buffer list overflow");
        ed->ws.bufs = yew_xreallocarray(ed->ws.bufs, cap,
                                        sizeof(*ed->ws.bufs));
        ed->ws.cap = cap;
    }
    ed->ws.bufs[ed->ws.nbufs++] = b;
}

const char *yew_buf_label(const Buffer *b)
{
    if (b == NULL)
        return "";
    if (b->name != NULL)
        return b->name;
    return b->path == NULL ? "[No Name]" : b->path;
}

Buffer *yew_ws_scratch_new(Ed *ed, const char *name, u32 flags)
{
    Buffer *b;

    if (ed == NULL || !ed->model_ready || name == NULL)
        return NULL;
    b = yew_xcalloc(1U, sizeof(*b));
    b->owner = ed;
    ed_syn_init(b);
    b->id = ed->ws.next_buf_id++;
    yew_filemeta_init(&b->meta);
    b->tb = yew_textbuf_new();
    yew_syn_attach(&b->syn, YEW_LANG_NONE, b->tb);
    b->name = arena_strdup(&ed->arena, name);
    b->flags = flags | YEW_BUF_SCRATCH;
    b->tabwidth = YEW_VP_TABWIDTH;
    /* A scratch buffer still carries an undo tree so the edit chokepoint
     * stays uniform; YEW_BUF_NOUNDO governs whether edits record into it. */
    b->undo = yew_undo_new(b->tb);
    yew_undo_mark_saved(b->undo);
    b->marks = yew_marks_new();
    b->jrn = NULL;
    ed_ws_push(ed, b);
    return b;
}

/*
 * Sprint 24 §3: a file buffer that has NOT been read.
 *
 * It carries a path and nothing else — no TextBuf, no undo tree, no
 * marks.  That is the whole trick: residency is a question about the
 * ALLOCATION, so it cannot drift the way a `deferred` flag can.  Every
 * save path already ends up asking "is there text here", and the
 * honest answer is a null pointer rather than a boolean someone has to
 * remember to clear.
 */
Buffer *yew_ws_file_buf(Ed *ed, const char *path)
{
    Buffer *b;
    u32 i;

    if (ed == NULL || !ed->model_ready)
        return NULL;
    if (path != NULL) {
        /* One buffer per file: two buffers on one path are two claims
         * on one save destination. */
        for (i = 0U; i < ed->ws.nbufs; i++) {
            Buffer *e = ed->ws.bufs[i];

            if (e->name == NULL && e->path != NULL &&
                strcmp(e->path, path) == 0)
                return e;
        }
    }
    b = yew_xcalloc(1U, sizeof(*b));
    b->owner = ed;
    ed_syn_init(b);
    b->id = ed->ws.next_buf_id++;
    yew_filemeta_init(&b->meta);
    b->tabwidth = YEW_VP_TABWIDTH;
    b->path = path == NULL ? NULL : arena_strdup(&ed->arena, path);
    if (path == NULL) {
        /*
         * An untitled tab has nowhere to be read FROM, so deferring it
         * would mean a buffer that can never become resident.  It is
         * born loaded and empty.
         */
        b->tb = yew_textbuf_new();
        yew_syn_attach(&b->syn, YEW_LANG_NONE, b->tb);
        b->undo = yew_undo_new(b->tb);
        yew_undo_mark_saved(b->undo);
        b->marks = yew_marks_new();
    }
    ed_ws_push(ed, b);
    return b;
}

bool yew_buf_resident(const Buffer *b)
{
    /* Asked of the allocation.  DoD 2 greps src/ui for a residency
     * FLAG and must find none. */
    return b != NULL && b->tb != NULL;
}

int yew_buf_hydrate(Ed *ed, Buffer *b)
{
    TextBuf *tb = NULL;
    YewLoadErr load;

    if (ed == NULL || b == NULL)
        return -1;
    if (b->tb != NULL)
        return 0; /* already resident: no read, no work */
    if (b->path == NULL)
        return -1;
    /*
     * yew_file_load overwrites meta->realpath, which realpath(3)
     * allocated on the PREVIOUS load -- so re-entering a deferred tab
     * leaked PATH_MAX bytes every time.  Disposing here rather than in
     * yew_buf_defer keeps the pairing next to the call that overwrites
     * it; dispose re-inits, so a never-loaded buffer is fine.
     */
    yew_filemeta_dispose(&b->meta);
    load = yew_file_load(b->path, &tb, &b->meta);
    if (load != YEW_LOAD_OK && load != YEW_LOAD_ENOENT) {
        yew_textbuf_free(tb);
        return -1;
    }
    /* A path that does not exist yet opens empty, exactly as it does
     * on the command line. */
    if (tb == NULL)
        tb = yew_textbuf_new();
    b->tb = tb;
    yew_ed_syn_bind(b);
    b->undo = yew_undo_new(tb);
    yew_undo_mark_saved(b->undo);
    if (b->marks == NULL)
        b->marks = yew_marks_new();
    /*
     * Sprint 25 §6: restored marks become real here, at the first
     * moment there is text to anchor them to.  Clamped to the buffer,
     * because the file may well have changed on disk since the state
     * was written and a mark past the end is not a cosmetic problem.
     */
    {
        u32 i;
        u64 size = yew_textbuf_len(b->tb);

        for (i = 0U; i < 26U; i++) {
            if (!b->pending_mark_set[i])
                continue;
            b->pending_mark_set[i] = false;
            (void)yew_ed_mark_set(ed, b, (u8)('a' + i),
                                  BYTEOFF(b->pending_marks[i] > size
                                              ? size
                                              : b->pending_marks[i]));
        }
    }
    yew_lsp_buffer_open(ed, b);
    return 0;
}

void yew_buf_defer(Ed *ed, Buffer *b)
{
    if (b == NULL || b->tb == NULL || b->path == NULL)
        return;
    yew_lsp_buffer_close(ed, b);
    if (b->jrn != NULL) {
        yew_journal_close(b->jrn);
        b->jrn = NULL;
    }
    /*
     * Releasing the undo tree is what clears modified: dirtiness is
     * derived from it, so a buffer read from nowhere reports clean
     * without anyone assigning a flag.
     */
    yew_undo_free(b->undo);
    b->undo = NULL;
    yew_marks_free(b->marks);
    b->marks = NULL;
    yew_syn_detach(&b->syn);
    yew_filemeta_content_forget(&b->meta);
    yew_textbuf_free(b->tb);
    b->tb = NULL;
}

Buffer *yew_ws_buf_by_id(Ed *ed, u32 id)
{
    u32 i;

    if (ed == NULL || !ed->model_ready)
        return NULL;
    for (i = 0U; i < ed->ws.nbufs; i++) {
        if (ed->ws.bufs[i]->id == id)
            return ed->ws.bufs[i];
    }
    return NULL;
}

static Win *win_by_id_in(Pane *p, u32 id)
{
    Win *w;

    if (p == NULL)
        return NULL;
    if (p->is_leaf)
        return p->win != NULL && p->win->id == id ? p->win : NULL;
    w = win_by_id_in(p->a, id);
    return w != NULL ? w : win_by_id_in(p->b, id);
}

Win *yew_ed_win_by_id(Ed *ed, u32 id)
{
    size_t i;
    Win *w;

    if (ed == NULL || id == 0U)
        return NULL;
    /*
     * Every tab's tree, not just the active one.  A handle taken in
     * one tab must survive a switch to another -- a script that walks
     * buffers across tabs would otherwise watch its own windows
     * disappear as the user navigated.  ed->pane_root is the active
     * tab's root and is already one of these.
     */
    for (i = 0U; i < ed->tabs.v.len; i++) {
        w = win_by_id_in(ed->tabs.v.data[i].root, id);
        if (w != NULL)
            return w;
    }
    w = win_by_id_in(ed->pane_root, id);
    if (w != NULL)
        return w;
    return ed->pane_root == NULL && ed->single_win.id == id ?
               &ed->single_win : NULL;
}

Buffer *yew_ws_scratch_find(Ed *ed, const char *name)
{
    u32 i;

    if (ed == NULL || name == NULL)
        return NULL;
    for (i = 1U; i < ed->ws.nbufs; i++) {
        Buffer *b = ed->ws.bufs[i];

        if (b->name != NULL && strcmp(b->name, name) == 0)
            return b;
    }
    return NULL;
}

void yew_ws_scratch_drop(Ed *ed, Buffer *b)
{
    u32 i;

    if (ed == NULL || b == NULL || b == &ed->buffer)
        return;
    for (i = 1U; i < ed->ws.nbufs; i++) {
        if (ed->ws.bufs[i] != b)
            continue;
        /* Never leave a window pointing at freed memory: a window showing
         * this buffer falls back to the document buffer first. */
        if (ed->win != NULL && ed->win->buf == b)
            (void)yew_ed_show_buffer(ed, &ed->buffer);
        yew_symidx_drop_buffer(&ed->ws, b->id);
        ed_buffer_dispose(b);
        free(b);
        (void)memmove(&ed->ws.bufs[i], &ed->ws.bufs[i + 1U],
                      (size_t)(ed->ws.nbufs - i - 1U) *
                          sizeof(*ed->ws.bufs));
        ed->ws.nbufs--;
        return;
    }
}

bool yew_ed_show_buffer(Ed *ed, Buffer *b)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    u32 i;
    bool found = false;

    if (ed == NULL || b == NULL || ed->win == NULL || !ed->model_ready)
        return false;
    for (i = 0U; i < ed->ws.nbufs; i++) {
        if (ed->ws.bufs[i] == b) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    if (ed->win->buf == b)
        return true;
    yew_ed_insert_barrier(ed);
    yew_lsp_highlight_clear(ed, ed->win);
    yew_panel_close(ed, &ed->win->panel);
    yew_shadow_dismiss(ed, ed->win);
    yew_vp_free(ed->win);
    yew_cset_free(&ed->win->cs);
    yew_cset_init(&ed->win->cs, cursor);
    ed->win->buf = b;
    ed->win->git_sign_gen = 0U;
    ed->win->git_sign_buf = 0U;
    YewGitDiffRowStyleVec_free(&ed->win->git_diff_rows);
    SpanVec_free(&ed->win->git_diff_intra);
    yew_vp_init(ed->win);
    yew_reg_bind_context(&ed->regs, b->undo, &b->meta);
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    ed->drawn_cursor_line_valid = false;
    ed->drawn_top_valid = false;
    return true;
}

static bool ed_model_finish(Ed *ed, TextBuf *tb, const char *path)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    ed->buffer.tb = tb;
    ed->buffer.owner = ed;
    ed_syn_init(&ed->buffer);
    ed->buffer.id = ed->ws.next_buf_id++;
    ed->buffer.path = path == NULL ? NULL : arena_strdup(&ed->arena, path);
    yew_ed_syn_bind(&ed->buffer);
    ed->buffer.tabwidth = YEW_VP_TABWIDTH;
    ed->buffer.undo = yew_undo_new(tb);
    yew_undo_mark_saved(ed->buffer.undo);
    ed->buffer.marks = yew_marks_new();
    ed->buffer.jrn = NULL;
    yew_search_opts_init(&ed->search_opts);
    yew_search_state_init(&ed->search);
    yew_overlay_init(&ed->single_win.overlay);
    yew_overlay_init(&ed->single_win.lsp_highlight.read);
    yew_overlay_init(&ed->single_win.lsp_highlight.write);
    yew_shadow_init(&ed->single_win.shadow);
    /* One leaf holding the document window: the pane tree always
     * exists, so no code path has to ask whether panes are "on". */
    ed->pane_root = yew_pane_new_leaf(&ed->single_win);
    ed->focus = ed->pane_root;
    /*
     * Tab 0 owns the tree that already exists rather than cloning a
     * second view of the same buffer: the editor always has exactly one
     * tab, so no code path has to ask whether tabs are "on".
     */
    yew_tabs_init(&ed->tabs);
    yew_groups_init(&ed->groups);
    {
        Tab first;

        (void)memset(&first, 0, sizeof(first));
        first.tab_id = ed->tabs.next_tab_id++;
        first.root = ed->pane_root;
        first.focus = ed->focus;
        first.buffer_id = ed->buffer.id;
        /*
         * malloc'd, like every other tab's path.  Arena-owning this one
         * made Tab.path mean two different things depending on which
         * tab you had, and tab_destroy's free() then corrupted the heap
         * — and a reorder moves this tab away from index 0, so "the
         * first one is special" is not even checkable.
         */
        first.path = NULL;
        if (ed->buffer.path != NULL) {
            size_t n = strlen(ed->buffer.path) + 1U;

            first.path = yew_xmalloc(n);
            (void)memcpy(first.path, ed->buffer.path, n);
        }
        TabVec_push(&ed->tabs.v, first);
        ed->tabs.active = 0;
    }
    yew_reg_bind_context(&ed->regs, ed->buffer.undo, &ed->buffer.meta);
    ed->single_win.id = ed->next_win_id++;
    ed->single_win.syn_spans = yew_xcalloc(YEW_SYN_MAX_SPANS,
                                            sizeof(SynSpan));
    ed->single_win.syn_spans_cap = YEW_SYN_MAX_SPANS;
    yew_cset_init(&ed->single_win.cs, cursor);
    ed->single_win.buf = &ed->buffer;
    yew_vp_init(&ed->single_win);
    ed->win = &ed->single_win;
    ed->model_ready = true;
    /* Slot 0 is the document buffer and is never heap-owned. */
    ed_ws_push(ed, &ed->buffer);
    ed->durability_failed = false;
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    ed->cursor_follow_pending = false;
    ed->doc_damage_lo = 0U;
    ed->doc_damage_hi = 0U;
    ed->drawn_cursor_line_valid = false;
    ed->drawn_top_valid = false;
    return true;
}

void yew_ed_init(Ed *ed)
{
    const char *prof;
    const char *frame_tags;
    char *root;

    if (ed == NULL)
        YEW_BUG("editor init: NULL editor");
    (void)memset(ed, 0, sizeof(*ed));
    arena_init(&ed->arena);
    prof = getenv("YEW_PROF");
    frame_tags = getenv("YEW_PERF_FRAME_TAGS");
    ed->perf_frame_tags = frame_tags != NULL &&
                          strcmp(frame_tags, "1") == 0;
    yew_prof_init(&ed->prof, &ed->arena,
                  prof != NULL && strcmp(prof, "1") == 0);
    arena_init(&ed->cmdline.comp_arena);
    interner_init(&ed->interner, &ed->arena);
    ed->ws.owner = ed;
    yew_symidx_init(&ed->ws.sym_ws, &ed->interner);
    ed->ws.sym_ws.owner = &ed->ws;
    bytebuf_init(&ed->frame);
    bytebuf_init(&ed->paste);
    yew_reg_init(&ed->regs);
    yew_timers_init(&ed->timers);
    yew_jobs_init(&ed->jobs);
    yew_ai_state_init(ed);
    yew_git_state_init(ed);
    yew_git_editor_state_init(ed);
    yew_fuss_state_init(ed);
    yew_mouse_init(&ed->mouse);
    yew_shadow_test_install();
    yew_block_provider_syntax_install(true);
    root = realpath(".", NULL);
    if (root == NULL)
        root = getcwd(NULL, 0U);
    ed->ws.dir = arena_strdup(&ed->arena, root == NULL ? "." : root);
    free(root);
    fl_origin_reg_init(&ed->origins);
    fl_h_table_init(&ed->handles);
    yew_record_init(&ed->rec);
    yew_theme_init(&ed->theme);
    ed->theme_last_dark = yew_xmalloc(sizeof("quiver-dark"));
    (void)memcpy(ed->theme_last_dark, "quiver-dark", sizeof("quiver-dark"));
    ed->theme_last_light = yew_xmalloc(sizeof("quiver-light"));
    (void)memcpy(ed->theme_last_light, "quiver-light",
                 sizeof("quiver-light"));
    yew_cmd_set_record_tap(yew_record_tap);
    /* Window ids start at 1; 0 is never handed out, so a zeroed Win is
     * distinguishable from window one. */
    ed->next_win_id = 1U;
    /* Symbol records reserve buffer id 0 for workspace-only files. */
    ed->ws.next_buf_id = 1U;
    yew_dispatch_init(ed);
    ed->dispatch_ready = true;
    if (!yew_fl_runtime_init(ed))
        YEW_BUG("editor init: Fletch runtime initialization failed");
    yew_bind_init(ed);
    ed->macrolib = yew_macrolib_new(ed);
    if (ed->macrolib == NULL)
        YEW_BUG("editor init: macro library initialization failed");
    yew_opt_init(ed);
    yew_opt_provider_set(ed, NULL);
    ed->exit_code = YEW_EXIT_OK;
}

const char *yew_ws_root(const Ed *ed)
{
    return ed == NULL || ed->ws.dir == NULL ? "." : ed->ws.dir;
}

bool yew_ed_set_workspace_root(Ed *ed, const char *dir)
{
    char *root;
    struct stat st;

    if (ed == NULL || dir == NULL || stat(dir, &st) != 0 ||
        !S_ISDIR(st.st_mode))
        return false;
    root = realpath(dir, NULL);
    if (root == NULL)
        return false;
    ed->ws.dir = arena_strdup(&ed->arena, root);
    free(root);
    return true;
}

void yew_ed_free(Ed *ed)
{
    if (ed == NULL)
        return;
    yew_trust_prompt_cancel(ed);
    /* Belt and braces: a path that never reached yew_state_close still
     * must not leave its lock behind.  Dispose saves nothing. */
    yew_state_dispose(ed);
    yew_picker_close(ed, false);
    yew_pickers_dispose();
    yew_cmdline_dispose(ed);
    /* The symbol walk borrows a job slot and must release its filesystem
     * traversal before the generic job table is dismantled. */
    yew_symwalk_dispose(ed);
    /* LSP framed owners and diagnostics borrow jobs, buffers and marks. */
    yew_lsp_free(ed);
    /* AI owns transport jobs and pooled sockets, so it must release them
     * before the generic job table is dismantled. */
    yew_ai_state_free(ed);
    /* F mode borrows snapshots and scratch buffers; release it first. */
    yew_fuss_state_free(ed);
    /* Jobs die with the process (never persisted, s25); kill and reap
     * before the buffers they append into go away. */
    yew_jobs_free(ed);
    yew_git_editor_state_free(ed);
    /* Git refreshes and verbs borrow generic job slots too. */
    yew_git_state_free(ed);
    /* Close hooks are the last script-visible point for every buffer. */
    ed_buffer_free(ed);
#if YEW_WITH_PLUGINS
    yew_plug_free(ed);
#endif
    yew_macrolib_free(ed, ed->macrolib);
    ed->macrolib = NULL;
    yew_bind_free(ed);
    yew_config_free(ed);
    yew_fl_runtime_free(ed);
    yew_opt_free(ed);
    yew_theme_free(&ed->theme);
    free(ed->theme_last_dark);
    free(ed->theme_last_light);
    ed->theme_last_dark = NULL;
    ed->theme_last_light = NULL;
    yew_record_free(&ed->rec);
    yew_reg_free(&ed->regs);
    yew_msg_clear(ed);
    yew_msg_hint_clear(ed);
    if (ed->grid_ready)
        yew_grid_free(&ed->grid);
    if (ed->input_ready) {
        if (ed->tty_ready)
            yew_input_disable(ed->tty.wfd);
        yew_input_free(&ed->in);
    }
    yew_term_oob_clear();
    if (ed->dispatch_ready)
        yew_dispatch_free(ed);
    fl_h_table_free(&ed->handles);
    fl_origin_reg_free(&ed->origins);
    yew_timers_free(&ed->timers);
    bytebuf_free(&ed->paste);
    bytebuf_free(&ed->frame);
    yew_symidx_workspace_free(&ed->ws);
    interner_free(&ed->interner);
    arena_free_all(&ed->cmdline.comp_arena);
    arena_free_all(&ed->arena);
    if (ed->tty_ready) {
        yew_tty_altscreen(&ed->tty, false);
        yew_tty_close(&ed->tty);
    }
    yew_rss_checkpoint("closed");
    (void)memset(ed, 0, sizeof(*ed));
}

YewLoadErr yew_ed_open(Ed *ed, const char *path)
{
    TextBuf *tb = NULL;
    YewLoadErr result;

    if (ed == NULL || path == NULL || ed->fl_model_teardown)
        return YEW_LOAD_IO;
    ed_buffer_free(ed);
    yew_filemeta_init(&ed->buffer.meta);
    result = yew_file_load(path, &tb, &ed->buffer.meta);
    if (result != YEW_LOAD_OK && result != YEW_LOAD_ENOENT) {
        yew_filemeta_dispose(&ed->buffer.meta);
        return result;
    }
    if (!ed->rss_loaded_logged) {
        yew_rss_checkpoint("loaded");
        ed->rss_loaded_logged = true;
    }
    if (tb == NULL)
        tb = yew_textbuf_new();
    (void)ed_model_finish(ed, tb, path);
    if (!ed->headless && ed->buffer.meta.realpath != NULL &&
        yew_journal_probe(ed->buffer.meta.realpath, &ed->buffer.meta))
        yew_ed_prompt(ed, YEW_PROMPT_RECOVER);
    yew_fl_hook_buffer(ed, FL_EV_BUF_OPEN, &ed->buffer);
    return result;
}

bool yew_ed_open_scratch(Ed *ed)
{
    TextBuf *tb;

    if (ed == NULL || ed->fl_model_teardown)
        return false;
    ed_buffer_free(ed);
    yew_filemeta_init(&ed->buffer.meta);
    tb = yew_textbuf_new();
    return ed_model_finish(ed, tb, NULL);
}

bool yew_ed_open_memory(Ed *ed, const u8 *bytes, size_t len,
                        const char *name)
{
    TextBuf *tb;

    if (ed == NULL || name == NULL || (bytes == NULL && len != 0U) ||
        ed->fl_model_teardown)
        return false;
    ed_buffer_free(ed);
    yew_filemeta_init(&ed->buffer.meta);
    tb = yew_textbuf_from_bytes(bytes, (u64)len);
    if (!ed_model_finish(ed, tb, NULL)) {
        yew_textbuf_free(tb);
        return false;
    }
    ed->buffer.name = arena_strdup(&ed->arena, name);
    ed->buffer.flags |= YEW_BUF_SCRATCH;
    return true;
}

bool yew_buf_dirty(const Buffer *b)
{
    return b != NULL && b->undo != NULL && !yew_undo_at_save_point(b->undo);
}

bool yew_buf_readonly(const Buffer *b)
{
    mode_t write_bits = S_IWUSR | S_IWGRP | S_IWOTH;

    return b != NULL &&
           (((b->flags & YEW_BUF_READONLY) != 0U) ||
            (b->meta.exists && (b->meta.mode & write_bits) == 0));
}

u64 yew_buf_len(const Buffer *b)
{
    return b == NULL || b->tb == NULL ? 0U : yew_textbuf_len(b->tb);
}

u64 yew_buf_line_count(const Buffer *b)
{
    return b == NULL || b->tb == NULL ? 0U :
           yew_textbuf_line_count(b->tb);
}

Span yew_buf_line_span(const Buffer *b, LineNo line)
{
    return b == NULL || b->tb == NULL ? (Span){0U, 0U} :
           yew_textbuf_line_span(b->tb, line);
}

LineNo yew_buf_line_of(const Buffer *b, ByteOff off)
{
    return b == NULL || b->tb == NULL ? LINENO(0U) :
           yew_textbuf_line_of(b->tb, off);
}

/*
 * THE document — the one the focused window is showing.
 *
 * Sprint 23 could say `ed->buffer` and mean it, because every tab
 * cloned a view of that single buffer.  Sprint 24 gives each tab its
 * own, so "the buffer" and "the buffer the user is looking at" stop
 * being the same object — and a save path that keeps asking for the
 * former writes tab 1's bytes to tab 3's path, which is invariant 2
 * (no byte confusion) failing silently.
 *
 * The fallback is not decoration: fixtures build an Ed without a window
 * and still expect the primary buffer to answer.
 */
Buffer *yew_ed_doc(Ed *ed)
{
    if (ed == NULL)
        return NULL;
    if (ed->win != NULL && ed->win->buf != NULL)
        return ed->win->buf;
    return &ed->buffer;
}

/* Shim so the text layer can feed the changelist without knowing it
 * exists.  Job output goes to YEW_BUF_NOUNDO scratch buffers, which are
 * not places the user "changed" and so are excluded here. */
static void ed_on_change(void *ctx, ByteOff at, LineNo line,
                         u64 removed_lines, u64 inserted_lines,
                         i64 now_ms, bool new_change)
{
    Buffer *b = ctx;

    if (b == NULL)
        return;
    if (new_change && (b->flags & YEW_BUF_NOUNDO) == 0U) {
        yew_change_record(b, at, now_ms);
        yew_fl_hook_note_change(b->owner, b, at.v);
    }
    (void)line;
    (void)removed_lines;
    (void)inserted_lines;
    if (b->owner != NULL)
        b->owner->footer_dirty = true;
}

bool yew_ed_mark_set(Ed *ed, Buffer *b, u8 name, ByteOff at)
{
    u32 slot;

    (void)ed;
    if (b == NULL || b->marks == NULL || name < 'a' || name > 'z')
        return false;
    slot = (u32)(name - 'a');
    /* Re-setting a name drops the old mark: leaking one per keystroke
     * of `ma` would grow the mark set without bound. */
    if (b->named_set[slot] && yew_mark_alive(b->marks, b->named[slot]))
        yew_mark_del(b->marks, b->named[slot]);
    b->named[slot] = yew_mark_add(b->marks, at, YEW_BIAS_LEFT);
    b->named_set[slot] = true;
    return true;
}

bool yew_ed_mark_get(Ed *ed, const Buffer *b, u8 name, ByteOff *out)
{
    u32 slot;

    (void)ed;
    if (b == NULL || b->marks == NULL || name < 'a' || name > 'z')
        return false;
    slot = (u32)(name - 'a');
    if (!b->named_set[slot] || !yew_mark_alive(b->marks, b->named[slot]))
        return false;
    if (out != NULL)
        *out = yew_mark_pos(b->marks, b->named[slot]);
    return true;
}

struct Pane *yew_ed_pane_root(Ed *ed)
{
    return ed == NULL ? NULL : ed->pane_root;
}

/*
 * A new view of the SAME buffer.  The cursor set and viewport are
 * copied so the split opens showing what the user was already looking
 * at; the jumplist is not, because it is this view's history and a view
 * that has been nowhere has no history (Sprint 21 §5).
 */
Win *yew_ed_win_clone(Ed *ed, const Win *src)
{
    Win *w;

    /*
     * No cap here.  YEW_PANE_MAX_LEAVES bounds the leaves of ONE tab's
     * tree, and yew_pane_split enforces it by counting that tree.
     * Sprint 23 also kept a write-only registry of every cloned Win and
     * capped it with the same constant, which quietly made 16 the limit
     * on tabs-plus-splits for the whole editor — a 40-file group (D3)
     * could not be opened at all, and the refusal surfaced as
     * yew_tab_open returning -1 for no stated reason.  Nothing ever
     * read the registry: tab trees own their leaves and release them.
     */
    if (ed == NULL || src == NULL)
        return NULL;
    w = yew_xcalloc(1U, sizeof(*w));
    w->id = ed->next_win_id++;
    w->buf = src->buf;
    w->rect = src->rect;
    w->number_style = src->number_style;
    w->gutter_width = src->gutter_width;
    w->syn_spans = yew_xcalloc(YEW_SYN_MAX_SPANS, sizeof(SynSpan));
    w->syn_spans_cap = YEW_SYN_MAX_SPANS;
    {
        Cursor seed = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

        if (src->cs.curs.len > 0U && src->cs.primary < src->cs.curs.len)
            seed = src->cs.curs.data[src->cs.primary];
        yew_cset_init(&w->cs, seed);
    }
    yew_vp_init(w);
    yew_opt_scope_clone(&w->opt_overrides, &src->opt_overrides);
    w->vp.top = src->vp.top;
    w->vp.top_sub = src->vp.top_sub;
    yew_overlay_init(&w->overlay);
    yew_overlay_init(&w->lsp_highlight.read);
    yew_overlay_init(&w->lsp_highlight.write);
    yew_shadow_init(&w->shadow);
    yew_jumplist_init(&w->jumps);
    return w;
}

/*
 * Points a window at a buffer with a fresh view.
 *
 * The cursor is NOT carried over: a clone seeds its cursor from the
 * window it copied, and that offset means nothing in a different file
 * — in a shorter one it is past the end.
 */
void yew_ed_win_set_buffer(Ed *ed, Win *w, Buffer *b)
{
    Cursor origin = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    if (w == NULL || b == NULL || w->buf == b)
        return;
    if (w->compl.open)
        yew_compl_close_result(ed, w, false);
    yew_lsp_highlight_clear(ed, w);
    yew_panel_close(ed, &w->panel);
    yew_shadow_dismiss(ed, w);
    yew_vp_free(w);
    yew_cset_free(&w->cs);
    yew_cset_init(&w->cs, origin);
    w->buf = b;
    w->git_sign_gen = 0U;
    w->git_sign_buf = 0U;
    YewGitDiffRowStyleVec_free(&w->git_diff_rows);
    SpanVec_free(&w->git_diff_intra);
    yew_vp_init(w);
}

void yew_ed_win_release(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL)
        return;
    fl_h_drop_window(ed, w->id);
    yew_lsp_highlight_clear(ed, w);
    if (w == &ed->single_win)
        return;
    yew_overlay_free(&w->overlay);
    yew_overlay_free(&w->lsp_highlight.read);
    yew_overlay_free(&w->lsp_highlight.write);
    YewGitDiffRowStyleVec_free(&w->git_diff_rows);
    SpanVec_free(&w->git_diff_intra);
    if (w->compl.open)
        yew_compl_close_result(ed, w, false);
    yew_compl_free(&w->compl);
    yew_panel_close(ed, &w->panel);
    yew_shadow_dismiss(ed, w);
    yew_shadow_free(&w->shadow);
    yew_gutter_signs_free(w);
    free(w->syn_spans);
    yew_vp_free(w);
    yew_cset_free(&w->cs);
    yew_opt_scope_free(&w->opt_overrides);
    free(w);
}

EditCtx yew_ed_edit_ctx_buffer(Ed *ed, Buffer *buffer)
{
    EditCtx ec = {0};

    if (ed == NULL || buffer == NULL || buffer->tb == NULL)
        return ec;
    ec.tb = buffer->tb;
    ec.marks = buffer->marks;
    ec.jrnl = buffer->jrn;
    ec.undo = buffer->undo;
    ec.meta = buffer->path == NULL ? NULL : &buffer->meta;
    ec.on_change = ed_on_change;
    ec.on_change_ctx = buffer;
    ec.now_ms = ed->now_ms;
    ec.ed = ed;
    ec.buffer = buffer;
    return ec;
}

EditCtx yew_ed_edit_ctx_for(Ed *ed, Win *win)
{
    EditCtx ec;

    if (ed == NULL || win == NULL)
        return (EditCtx){0};
    ec = yew_ed_edit_ctx_buffer(ed, win->buf);
    if (ec.tb == NULL)
        return ec;
    ec.cset = &win->cs;
    ec.win_id = win->id;
    return ec;
}

static void ed_damage_win_line(Ed *ed, Win *win, LineNo line,
                               bool line_count_changed);

static u32 ed_syn_visible_leaves(const Ed *ed, Pane **leaves)
{
    u32 n = 0U;
    u32 i;

    if (ed == NULL || ed->pane_root == NULL)
        return 0U;
    yew_pane_collect_leaves(ed->pane_root, leaves, YEW_PANE_MAX_LEAVES, &n);
    for (i = 0U; i < n; i++) {
        if (leaves[i] == ed->focus) {
            Pane *focused = leaves[i];

            while (i > 0U) {
                leaves[i] = leaves[i - 1U];
                i--;
            }
            leaves[0] = focused;
            break;
        }
    }
    return n;
}

bool yew_ed_syn_pending(const Ed *ed)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    u32 n;
    u32 i;

    n = ed_syn_visible_leaves(ed, leaves);
    for (i = 0U; i < n; i++) {
        const Win *win = leaves[i]->win;

        if (win != NULL && win->buf != NULL && win->buf->tb != NULL &&
            (win->buf->syn.settling ||
             win->buf->syn.embed_pending != YEW_LANG_NONE))
            return true;
    }
    return false;
}

void yew_ed_syn_tick(Ed *ed, i64 budget_us, bool prioritize_focus)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    Buffer *candidates[YEW_PANE_MAX_LEAVES];
    Win *candidate_views[YEW_PANE_MAX_LEAVES];
    u32 n;
    u32 i;
    u32 ncandidates = 0U;
    u32 selected = 0U;
    u32 shown = 0U;
    Buffer *b;
    Win *view;
    LineNo view_lo;
    LineNo view_hi;
    SynSettleReport report;
    bool status_before;
    bool status_after;
    bool spec_before;
    bool provisional_changed;

    n = ed_syn_visible_leaves(ed, leaves);
    for (i = 0U; i < n; i++) {
        Win *candidate = leaves[i]->win;
        u32 j;

        if (candidate == NULL || candidate->buf == NULL ||
            candidate->buf->tb == NULL ||
            (!candidate->buf->syn.settling &&
             candidate->buf->syn.embed_pending == YEW_LANG_NONE))
            continue;
        for (j = 0U; j < ncandidates; j++) {
            if (candidates[j] == candidate->buf)
                break;
        }
        if (j != ncandidates)
            continue;
        candidates[ncandidates] = candidate->buf;
        candidate_views[ncandidates] = candidate;
        ncandidates++;
    }
    if (ncandidates == 0U)
        return;
    if (!prioritize_focus && ed->syn_rr_last_valid) {
        for (i = 0U; i < ncandidates; i++) {
            if (candidates[i]->id == ed->syn_rr_last_buf_id) {
                selected = (i + 1U) % ncandidates;
                break;
            }
        }
    }
    b = candidates[selected];
    view = candidate_views[selected];
    ed->syn_rr_last_buf_id = b->id;
    ed->syn_rr_last_valid = true;

    view_lo = LINENO(UINT64_MAX);
    view_hi = LINENO(0U);
    for (i = 0U; i < n; i++) {
        Win *shown_win = leaves[i]->win;
        LineNo lo;
        LineNo hi;

        if (shown_win == NULL || shown_win->buf != b)
            continue;
        shown++;
        lo = yew_win_view_top(shown_win);
        hi = yew_vp_last_visible_line(shown_win);
        if (hi.v != UINT64_MAX)
            hi.v++;
        if (lo.v < view_lo.v)
            view_lo = lo;
        if (hi.v > view_hi.v)
            view_hi = hi;
    }
    status_before = ed->win != NULL && b == ed->win->buf &&
                    yew_syn_status_visible(&b->syn);
    spec_before = b->syn.spec_valid;
    yew_syn_settle(&b->syn, b->tb, view_lo, view_hi, budget_us, &report);
    provisional_changed = spec_before != b->syn.spec_valid;
    status_after = ed->win != NULL && b == ed->win->buf &&
                   yew_syn_status_visible(&b->syn);
    if ((provisional_changed || report.hit_view) &&
        (shown > 1U || view != ed->win)) {
        ed->full_damage = true;
    } else if (provisional_changed) {
        yew_ed_damage_document(ed);
    } else if (report.hit_view) {
        u64 lo = report.damage_lo.v > view_lo.v ? report.damage_lo.v :
                                                      view_lo.v;
        u64 hi = report.damage_hi.v < view_hi.v ? report.damage_hi.v :
                                                      view_hi.v;
        u64 line;

        for (line = lo; line < hi; line++)
            ed_damage_win_line(ed, view, LINENO(line), false);
    }
    if (ed->win != NULL && b == ed->win->buf &&
        (status_before || status_after || report.fixpoint || b->syn.degraded))
        ed->footer_dirty = true;
}

EditCtx yew_ed_edit_ctx(Ed *ed)
{
    return ed == NULL ? (EditCtx){0} : yew_ed_edit_ctx_for(ed, ed->win);
}

void yew_ed_finish_edit(Ed *ed, const EditCtx *ec)
{
    if (ed == NULL || ec == NULL)
        return;
    /*
     * Drop stale highlight spans the moment the text moves under them.
     *
     * The overlay carries buf_gen and would notice at its next refresh,
     * but "next refresh" is not soon enough: between the edit and the
     * refresh the spans describe bytes that no longer exist, and a
     * draw in that window paints from offsets past the end of the
     * buffer.  fuzz_search caught exactly that — a 7..8 span in a
     * 7-byte buffer — by checking after every operation rather than
     * after every repaint.
     */
    if (ed->win != NULL && ed->win->buf != NULL &&
        ed->win->buf->tb == ec->tb &&
        ed->win->overlay.buf_gen != ec->tb->gen)
        yew_overlay_invalidate(&ed->win->overlay);
    /*
     * The journal belongs to whichever buffer the edit ran against.
     * Matching on slot 0 alone stopped being right once tabs stopped
     * sharing one buffer, and the miss is silent: the edit journals,
     * the handle is dropped on the floor, and the next crash recovers
     * nothing.
     */
    if (ec->buffer != NULL && ec->buffer->tb == ec->tb) {
        ec->buffer->jrn = ec->jrnl;
    } else {
        Buffer *doc = yew_ed_doc(ed);

        if (doc != NULL && doc->tb == ec->tb)
            doc->jrn = ec->jrnl;
        else if (ed->buffer.tb == ec->tb)
            ed->buffer.jrn = ec->jrnl;
    }
    if (ec->jrnl != NULL && !yew_journal_ok(ec->jrnl)) {
        ed->durability_failed = true;
        yew_msg(ed, YEW_MSG_ERROR,
                "crash journal failed; save or q! before continuing");
    }
}

void yew_ed_damage_document(Ed *ed)
{
    if (ed == NULL || ed->win == NULL)
        return;
    if (ed->damage_batching) {
        ed->damage_batch_pending = true;
        return;
    }
    ed->doc_damage_lo = 0U;
    ed->doc_damage_hi = ed->win->rect.h;
    ed->footer_dirty = true;
}

void yew_ed_damage_rows(Ed *ed, u16 lo, u16 hi)
{
    if (ed == NULL || ed->win == NULL || lo >= hi)
        return;
    if (ed->damage_batching) {
        ed->damage_batch_pending = true;
        return;
    }
    if (lo > ed->win->rect.h)
        lo = ed->win->rect.h;
    if (hi > ed->win->rect.h)
        hi = ed->win->rect.h;
    if (lo >= hi)
        return;
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

static void ed_damage_win_line(Ed *ed, Win *win, LineNo line,
                               bool line_count_changed)
{
    LineNo top;
    u16 lo;
    u16 hi;

    if (ed == NULL || win == NULL)
        return;
    if (ed->damage_batching) {
        ed->damage_batch_pending = true;
        return;
    }
    yew_vp_invalidate_from(win, line);
    if (line_count_changed)
        ed->layout_dirty = true;
    if (win->vp.wrap) {
        yew_ed_damage_document(ed);
        return;
    }
    top = yew_win_view_top(win);
    if (line.v < top.v) {
        if (line_count_changed)
            yew_ed_damage_document(ed);
        return;
    }
    if (!yew_win_view_row(win, line, &lo))
        return;
    hi = line_count_changed ? win->rect.h : (u16)(lo + 1U);
    yew_ed_damage_rows(ed, lo, hi);
}

void yew_ed_damage_line(Ed *ed, LineNo line, bool line_count_changed)
{
    if (ed == NULL)
        return;
    ed_damage_win_line(ed, ed->win, line, line_count_changed);
}

static void ed_syn_batch_flush(Ed *ed)
{
    if (!ed->damage_batch_syn_pending)
        return;
    yew_syn_edit(&ed->damage_batch_syn_buffer->syn,
                 ed->damage_batch_syn_lo, 0U, 0U);
    ed->damage_batch_syn_pending = false;
    ed->damage_batch_syn_buffer = NULL;
}

void yew_ed_syn_note_edit(Ed *ed, Buffer *buffer, LineNo lo,
                          u64 removed, u64 inserted)
{
    bool owns_batch;

    if (ed == NULL || buffer == NULL)
        YEW_BUG("syntax edit notification: missing editor or buffer");
    owns_batch = ed->damage_batching && ed->damage_batch_win != NULL &&
                 ed->damage_batch_win->buf == buffer;
    if (!owns_batch || removed != 0U || inserted != 0U) {
        ed_syn_batch_flush(ed);
        yew_syn_edit(&buffer->syn, lo, removed, inserted);
        return;
    }
    if (ed->damage_batch_syn_pending &&
        ed->damage_batch_syn_buffer != buffer)
        ed_syn_batch_flush(ed);
    if (!ed->damage_batch_syn_pending || lo.v < ed->damage_batch_syn_lo.v)
        ed->damage_batch_syn_lo = lo;
    ed->damage_batch_syn_pending = true;
    ed->damage_batch_syn_buffer = buffer;
}

void yew_ed_damage_batch_begin(Ed *ed, Win *win)
{
    if (ed == NULL)
        return;
    if (ed->damage_batching)
        YEW_BUG("damage batch: nested begin");
    ed->damage_batching = true;
    ed->damage_batch_pending = true;
    ed->damage_batch_syn_pending = false;
    ed->damage_batch_syn_buffer = NULL;
    ed->damage_batch_win = win;
    ed->damage_batch_lines = win != NULL && win->buf != NULL &&
                                     win->buf->tb != NULL
                                 ? yew_textbuf_line_count(win->buf->tb) : 0U;
}

void yew_ed_damage_batch_end(Ed *ed)
{
    bool pending;
    Win *win;

    if (ed == NULL)
        return;
    if (!ed->damage_batching)
        YEW_BUG("damage batch: end without begin");
    pending = ed->damage_batch_pending;
    win = ed->damage_batch_win;
    ed_syn_batch_flush(ed);
    ed->damage_batching = false;
    ed->damage_batch_pending = false;
    ed->damage_batch_win = NULL;
    if (win != NULL) {
        yew_vp_invalidate(win);
        if (win->buf != NULL && win->buf->tb != NULL &&
            yew_textbuf_line_count(win->buf->tb) != ed->damage_batch_lines)
            ed->layout_dirty = true;
    }
    ed->damage_batch_lines = 0U;
    if (pending) {
        yew_ed_damage_document(ed);
        ed->damage_batch_finalizations++;
    }
}

bool yew_ed_damage_batch_active(const Ed *ed)
{
    return ed != NULL && ed->damage_batching;
}

Cursor *yew_ed_cursor(Ed *ed)
{
    if (ed == NULL || ed->win == NULL || ed->win->cs.curs.len == 0U ||
        (size_t)ed->win->cs.primary >= ed->win->cs.curs.len)
        return NULL;
    return &ed->win->cs.curs.data[ed->win->cs.primary];
}

void yew_ed_insert_barrier(Ed *ed)
{
    EditCtx ec;

    if (ed == NULL || !ed->insert_txn)
        return;
    ec = yew_ed_edit_ctx(ed);
    yew_undo_end(&ec);
    yew_ed_finish_edit(ed, &ec);
    ed->insert_txn = false;
}

CmdStatus yew_ed_dispatch_resolved(Ed *ed, CmdId id, CmdCtx *cx)
{
    const CmdDesc *desc;
    bool multi;

    if (ed == NULL || cx == NULL)
        return YEW_CMD_ERR_ARG;
    desc = yew_cmd_desc(id);
    if (desc == NULL)
        return YEW_CMD_ERR_ARG;
    cx->ed = ed;
    if (cx->win == NULL)
        cx->win = ed->win;
    multi = (desc->flags & YEW_CMD_CHANGES_BUFFER) != 0U &&
            ed->model_ready && cx->win != NULL &&
            cx->win->cs.curs.len > 1U && !cx->cursor_given &&
            cx->range.kind == YEW_RANGE_NONE &&
            (desc->flags & YEW_CMD_MULTI_AGGREGATE) == 0U;
    return multi ? yew_mc_run(cx->win, id, cx) : yew_cmd_invoke(id, cx);
}

CmdStatus yew_ed_invoke(Ed *ed, CmdId id, CmdCtx *cx)
{
    const CmdDesc *desc;
    EditCtx ec;
    CmdStatus status;
    bool changes;
    bool edits_text;
    bool multiple;
    bool document_target;
    bool durability_command;
    bool newline;
    bool shadow_accept;
    bool shadow_motion;
    bool shadow_quiet;
    bool shadow_holdoff_before;
    bool opened = false;
    bool started_in_insert;

    if (ed == NULL || cx == NULL)
        return YEW_CMD_ERR_ARG;
    desc = yew_cmd_desc(id);
    if (desc == NULL)
        return YEW_CMD_ERR_ARG;
    cx->ed = ed;
    if (cx->win == NULL)
        cx->win = ed->win;
    changes = (desc->flags & YEW_CMD_CHANGES_BUFFER) != 0U;
    edits_text = changes || strcmp(desc->name, "ed.edit.undo") == 0 ||
                 strcmp(desc->name, "ed.edit.redo") == 0;
    document_target = cx->win == ed->win;
    multiple = changes && ed->model_ready && cx->win != NULL &&
               cx->win->cs.curs.len > 1U;
    durability_command = document_target && edits_text;
    newline = ed->undo_break_on_newline &&
              strcmp(desc->name, "ed.edit.insert.newline") == 0;
    shadow_accept = strncmp(desc->name, "ed.shadow.accept_", 17U) == 0 ||
                    strcmp(desc->name, "ed.compl.accept") == 0;
    shadow_motion = document_target &&
                    strncmp(desc->name, "ed.move.", 8U) == 0;
    shadow_quiet = document_target &&
                   (cx->source == YEW_SRC_REPLAY ||
                    strcmp(desc->name, "ed.edit.undo") == 0 ||
                    strcmp(desc->name, "ed.edit.redo") == 0 ||
                    strncmp(desc->name, "ed.macro.replay", 15U) == 0 ||
                    strncmp(desc->name, "ed.file.save", 12U) == 0 ||
                    strncmp(desc->name, "ed.file.write", 13U) == 0);
    started_in_insert = ed->mode == YEW_MODE_I;

    if ((shadow_motion || shadow_quiet) && cx->win != NULL)
        yew_shadow_dismiss(ed, cx->win);

    if (durability_command && ed->durability_failed) {
        yew_msg(ed, YEW_MSG_ERROR,
                "crash journal failed; save or q! before continuing");
        return YEW_CMD_ERR_IO;
    }

    if (started_in_insert && (!changes || newline || shadow_accept))
        yew_ed_insert_barrier(ed);
    if (changes && ed->model_ready) {
        ec = yew_ed_edit_ctx_for(ed, cx->win);
        if (started_in_insert && !newline && !shadow_accept) {
            if (!ed->insert_txn) {
                yew_undo_begin(&ec,
                               multiple ? YEW_TXN_MULTI : YEW_TXN_TYPE);
                ed->insert_txn = true;
                opened = true;
            }
        } else {
            /*
             * The reason has to describe what the command IS, because
             * a command that opens its own inner transaction must
             * agree with the one wrapped around it — `:s` opens
             * YEW_TXN_REPLACE inside, and a TYPE wrapper aborts on the
             * mismatch.  Reached through the real dispatcher only, so
             * unit tests calling the replace API directly never saw it;
             * the pty golden did.
             */
            yew_undo_begin(
                &ec,
                multiple ? YEW_TXN_MULTI
                      : (shadow_accept ? YEW_TXN_PASTE
                      : (strcmp(desc->name, "ed.search.replace") == 0
                             ? YEW_TXN_REPLACE
                      : (strstr(desc->name, ".delete.") != NULL ||
                                 strcmp(desc->name,
                                        "ed.edit.line.delete") == 0
                             ? YEW_TXN_ERASE
                             : YEW_TXN_TYPE))));
            opened = true;
        }
    }

    shadow_holdoff_before = ed->shadow_holdoff;
    if (shadow_quiet)
        ed->shadow_holdoff = true;
    status = yew_ed_dispatch_resolved(ed, id, cx);
    ed->shadow_holdoff = shadow_holdoff_before;
    if (status == YEW_CMD_OK && shadow_motion && cx->win != NULL)
        yew_shadow_arm(ed, cx->win);
    if (status == YEW_CMD_OK && document_target && ed->win != NULL &&
        (changes || durability_command ||
         strncmp(desc->name, "ed.move.", 8U) == 0 ||
         strncmp(desc->name, "ed.view.", 8U) == 0))
        yew_selstack_clear(ed->win);
    if (changes && ed->model_ready) {
        ec = yew_ed_edit_ctx_for(ed, cx->win);
        if (status != YEW_CMD_OK &&
            (opened || (started_in_insert && ed->insert_txn))) {
            yew_undo_abort(&ec);
            if (started_in_insert)
                ed->insert_txn = false;
        } else if (!started_in_insert && ed->mode == YEW_MODE_I &&
                   !newline && opened) {
            /* `o`/`O` create the line and enter I as one editing run. */
            if (ed->win != NULL && ed->win->cs.curs.len > 1U)
                yew_undo_promote_multi(&ec);
            ed->insert_txn = true;
        } else if ((!started_in_insert || newline || shadow_accept) &&
                   opened) {
            yew_undo_end(&ec);
        }
        yew_ed_finish_edit(ed, &ec);
    }
    if (durability_command && status == YEW_CMD_ERR_IO) {
        ed->durability_failed = true;
        yew_msg(ed, YEW_MSG_ERROR,
                "crash journal failed; save or q! before continuing");
    }
    if (ed->cmdline.active && cx->win == yew_cmdline_target(ed)) {
        if (edits_text)
            yew_cmdline_edited(ed);
        else
            yew_cmdline_sync(ed);
    }
    ed->footer_dirty = true;
    return status;
}

CmdStatus yew_ed_invoke_parsed(Ed *ed, CmdId id,
                               const YewCmdInvoke *invoke)
{
    const CmdDesc *desc;
    CmdCtx cx = {0};
    const char *arg = NULL;

    if (ed == NULL || invoke == NULL)
        return YEW_CMD_ERR_ARG;
    desc = yew_cmd_desc(id);
    if (desc == NULL || invoke->argv.n == 0U)
        return YEW_CMD_ERR_ARG;
    if (invoke->argv.n > 1U)
        arg = invoke->argv.v[1];
    cx.win = invoke->win;
    cx.range = invoke->range;
    cx.argv = invoke->argv;
    cx.count = invoke->count > 0 && invoke->count <= UINT32_MAX ?
               (u32)invoke->count : 1U;
    cx.count_given = invoke->count > 0;
    cx.bang = invoke->bang;
    cx.source = YEW_SRC_CMDLINE;
    switch ((CmdArity)desc->arity) {
    case YEW_ARITY_NONE:
        break;
    case YEW_ARITY_STR:
    case YEW_ARITY_OPT_STR:
        cx.sarg = arg;
        cx.sarg_len = arg == NULL ? 0U :
                      strlen(arg) > UINT32_MAX ? UINT32_MAX :
                                                (u32)strlen(arg);
        break;
    case YEW_ARITY_INT:
    case YEW_ARITY_OPT_INT:
        if (arg != NULL) {
            char *end = NULL;
            long long value;

            errno = 0;
            value = strtoll(arg, &end, 10);
            if (errno != 0 || end == arg || *end != '\0')
                return YEW_CMD_ERR_ARG;
            cx.iarg = (i64)value;
        }
        break;
    }
    return yew_ed_invoke(ed, id, &cx);
}

void yew_ed_prompt(Ed *ed, PromptKind prompt)
{
    const char *path;

    if (ed == NULL)
        return;
    ed->prompt = prompt;
    {
        const Buffer *doc = yew_ed_doc(ed);

        path = doc == NULL || doc->path == NULL ? "[no name]" : doc->path;
    }
    switch (prompt) {
    case YEW_PROMPT_NONE:
        yew_msg_clear(ed);
        break;
    case YEW_PROMPT_RECOVER:
        yew_msg(ed, YEW_MSG_ERROR,
                "recover unsaved changes to %s? [r]ecover [d]iscard [esc] open as-is",
                path);
        break;
    case YEW_PROMPT_QUIT_DIRTY:
        yew_msg(ed, YEW_MSG_ERROR,
                "save changes to %s? [w]rite [d]iscard [esc] cancel", path);
        break;
    case YEW_PROMPT_OVERWRITE:
        yew_msg(ed, YEW_MSG_ERROR,
                "file changed on disk - [o]verwrite [esc] cancel");
        break;
    case YEW_PROMPT_WS_FORGET:
        /* ed.ws.forget writes its own question, because it names the
         * directory it is about to remove and this function does not
         * have it. */
        break;
    case YEW_PROMPT_WORKSPACE_TRUST:
        /* trust_prompt.c owns the path, fingerprint reason, and choices. */
        break;
    case YEW_PROMPT_AI_BLOCK:
        /* The AI module owns the matched rule, line, and action text. */
        break;
    case YEW_PROMPT_PLUGIN_CAP:
        /* The plugin module owns the plugin/capability names and choices. */
        break;
    }
    if (prompt != YEW_PROMPT_NONE)
        ed->msg.prompt = true;
}

CmdStatus yew_ed_file_save_win(Ed *ed, Win *win, bool force)
{
    EditCtx ec;
    YewSaveErr result;
    YewSaveOpts opts;
    Buffer *doc;
    u32 doc_id;
    u32 win_id;
    u64 lines;

    if (ed == NULL || !ed->model_ready || win == NULL || win->buf == NULL)
        return YEW_CMD_ERR_STATE;
    yew_shadow_dismiss(ed, win);
    yew_ed_insert_barrier(ed);
    doc = win->buf;
    if (doc == NULL || doc->path == NULL) {
        yew_msg(ed, YEW_MSG_ERROR,
                "no file name; use :w path");
        ed->quit_after_save = false;
        return YEW_CMD_ERR_STATE;
    }
    /*
     * Sprint 24 §3: a tab that was never read holds no text, so there
     * is nothing to write.  Saying so distinctly matters — an I/O error
     * would send the user looking at permissions on a file that is
     * perfectly fine, and writing the empty buffer would destroy it.
     */
    if (doc->tb == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "%s was never loaded; nothing written",
                doc->path);
        ed->quit_after_save = false;
        return YEW_CMD_ERR_STATE;
    }
    if (!fl_txn_prepare_save(yew_fl_vm(ed), doc->undo))
        return YEW_CMD_ERR_STATE;
    doc_id = doc->id;
    win_id = win->id;
    yew_fl_hook_buffer(ed, FL_EV_BUF_SAVE, doc);
    win = yew_ed_win_by_id(ed, win_id);
    doc = win == NULL ? NULL : win->buf;
    if (doc == NULL || doc->id != doc_id || doc->path == NULL ||
        doc->tb == NULL)
        return YEW_CMD_ERR_STATE;
    if (!fl_txn_prepare_save(yew_fl_vm(ed), doc->undo))
        return YEW_CMD_ERR_STATE;
    ec = yew_ed_edit_ctx_for(ed, win);
    ed_save_opts(ed, doc, win, &opts);
    if (force) {
        result = yew_file_save_force_opts(ec.tb, ec.meta, doc->path, &opts);
        if (yew_save_committed(result)) {
            yew_undo_boundary(ec.undo);
            yew_undo_mark_saved(ec.undo);
            if (ec.jrnl != NULL) {
                yew_journal_discard(ec.jrnl);
                ec.jrnl = NULL;
            }
        }
    } else {
        result = yew_edit_save_opts(&ec, doc->path, &opts);
    }
    yew_ed_finish_edit(ed, &ec);
    if (result == YEW_SAVE_CHANGED_ON_DISK) {
        if (!ed->headless)
            yew_ed_prompt(ed, YEW_PROMPT_OVERWRITE);
        else
            yew_msg(ed, YEW_MSG_ERROR,
                    "%s changed on disk; pass {force: true} to save",
                    doc->path);
        return YEW_CMD_ERR_IO;
    }
    if (!yew_save_committed(result)) {
        yew_msg(ed, YEW_MSG_ERROR, "could not write %s", doc->path);
        ed->quit_after_save = false;
        return YEW_CMD_ERR_IO;
    }
    ed->durability_failed = false;
    ed->prompt = YEW_PROMPT_NONE;
    lines = yew_textbuf_line_count(doc->tb);
    if (result == YEW_SAVE_BACKUP_FAILED)
        yew_msg(ed, YEW_MSG_WARN,
                "wrote %s, but backup retention failed", doc->path);
    else
        yew_msg(ed, YEW_MSG_INFO, "wrote %s, %llu lines", doc->path,
                (unsigned long long)lines);
    yew_lsp_buffer_save(ed, doc);
    yew_git_invalidate(ed);
    yew_symidx_workspace_replace(&ed->ws, doc);
    yew_fl_hook_buffer(ed, FL_EV_BUF_SAVED, doc);
    if (ed->quit_after_save) {
        ed->quit_after_save = false;
        ed->quit = true;
    }
    return YEW_CMD_OK;
}

CmdStatus yew_ed_file_save(Ed *ed, bool force)
{
    return ed == NULL ? YEW_CMD_ERR_STATE :
                        yew_ed_file_save_win(ed, ed->win, force);
}

CmdStatus yew_ed_file_write_to_win(Ed *ed, Win *win, const char *path,
                                   bool force)
{
    FileMeta next;
    TextBuf *existing = NULL;
    EditCtx ec;
    YewLoadErr load;
    YewSaveErr result;
    YewSaveOpts opts;
    Buffer *doc;
    u32 doc_id;
    u32 win_id;
    int tab_index = -1;
    size_t i;

    if (ed == NULL || !ed->model_ready || win == NULL || win->buf == NULL ||
        path == NULL || path[0] == '\0')
        return YEW_CMD_ERR_ARG;
    yew_ed_insert_barrier(ed);
    doc = win->buf;
    /* Nothing to write out under a new name either (§3). */
    if (doc == NULL || doc->tb == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "nothing loaded; nothing written");
        return YEW_CMD_ERR_STATE;
    }
    if (!fl_txn_prepare_save(yew_fl_vm(ed), doc->undo))
        return YEW_CMD_ERR_STATE;
    doc_id = doc->id;
    win_id = win->id;
    yew_fl_hook_buffer(ed, FL_EV_BUF_SAVE, doc);
    win = yew_ed_win_by_id(ed, win_id);
    doc = win == NULL ? NULL : win->buf;
    if (doc == NULL || doc->id != doc_id || doc->tb == NULL)
        return YEW_CMD_ERR_STATE;
    if (!fl_txn_prepare_save(yew_fl_vm(ed), doc->undo))
        return YEW_CMD_ERR_STATE;
    yew_filemeta_init(&next);
    load = yew_file_load(path, &existing, &next);
    yew_textbuf_free(existing);
    if (load != YEW_LOAD_OK && load != YEW_LOAD_ENOENT) {
        yew_filemeta_dispose(&next);
        yew_msg(ed, YEW_MSG_ERROR, "could not inspect %s", path);
        return YEW_CMD_ERR_IO;
    }
    ec = yew_ed_edit_ctx_for(ed, win);
    ec.meta = &next;
    ed_save_opts(ed, doc, win, &opts);
    result = force ? yew_file_save_force_opts(ec.tb, ec.meta, path, &opts) :
                     yew_edit_save_opts(&ec, path, &opts);
    if (force && yew_save_committed(result)) {
        yew_undo_boundary(ec.undo);
        yew_undo_mark_saved(ec.undo);
        if (ec.jrnl != NULL) {
            yew_journal_discard(ec.jrnl);
            ec.jrnl = NULL;
        }
    }
    yew_ed_finish_edit(ed, &ec);
    if (!yew_save_committed(result)) {
        yew_filemeta_dispose(&next);
        yew_msg(ed, YEW_MSG_ERROR, "could not write %s", path);
        return YEW_CMD_ERR_IO;
    }
    yew_lsp_buffer_close(ed, doc);
    yew_filemeta_dispose(&doc->meta);
    doc->meta = next;
    doc->path = arena_strdup(&ed->arena, path);
    yew_ed_syn_bind(doc);
    yew_lsp_buffer_open(ed, doc);
    yew_lsp_buffer_save(ed, doc);
    if (ed->win == win) {
        yew_search_opts_init(&ed->search_opts);
        yew_search_state_init(&ed->search);
        yew_overlay_init(&ed->single_win.overlay);
    }
    /*
     * A save-as RENAMES what the active tab shows.  It does not build a
     * new world.
     *
     * Sprints 22 and 23 grew a copy of ed_model_finish's tail here, so
     * `:w other` rebuilt the pane tree and reset the tab array to a
     * single entry — silently discarding every split and every other
     * tab, and leaking all of them, because yew_tabs_init only zeroes
     * the struct.  Nothing about writing bytes to a path justifies
     * touching either tree.
     */
    for (i = 0U; i < ed->tabs.v.len; i++) {
        if (win_by_id_in(ed->tabs.v.data[i].root, win_id) == win) {
            tab_index = (int)i;
            break;
        }
    }
    if (tab_index >= 0)
        yew_tab_set_path(ed, tab_index, doc->path);
    if (ed->win == win)
        yew_reg_bind_context(&ed->regs, doc->undo, &doc->meta);
    ed->durability_failed = false;
    if (result == YEW_SAVE_BACKUP_FAILED)
        yew_msg(ed, YEW_MSG_WARN,
                "wrote %s, but backup retention failed", path);
    else
        yew_msg(ed, YEW_MSG_INFO, "wrote %s, %llu lines", path,
                (unsigned long long)yew_textbuf_line_count(doc->tb));
    yew_symidx_workspace_replace(&ed->ws, doc);
    yew_fl_hook_buffer(ed, FL_EV_BUF_SAVED, doc);
    return YEW_CMD_OK;
}

CmdStatus yew_ed_file_write_to(Ed *ed, const char *path, bool force)
{
    return ed == NULL ? YEW_CMD_ERR_ARG :
                        yew_ed_file_write_to_win(ed, ed->win, path, force);
}

CmdStatus yew_ed_request_quit(Ed *ed, bool force)
{
    if (ed == NULL || !ed->model_ready)
        return YEW_CMD_ERR_STATE;
    yew_ed_insert_barrier(ed);
    if (!force && ed->durability_failed) {
        yew_msg(ed, YEW_MSG_ERROR,
                "crash journal failed; save or q! to discard changes");
        return YEW_CMD_ERR_IO;
    }
    if (!force && yew_buf_dirty(&ed->buffer)) {
        yew_ed_prompt(ed, YEW_PROMPT_QUIT_DIRTY);
        return YEW_CMD_OK;
    }
    ed->quit = true;
    ed->exit_code = YEW_EXIT_OK;
    return YEW_CMD_OK;
}

static bool prompt_key(Ed *ed, Key key)
{
    u32 code = key.code;

    if (key.ev == YEW_KEY_RELEASE)
        return true;
    if (ed->prompt == YEW_PROMPT_WORKSPACE_TRUST) {
        u8 answer = code == YEW_KEY_ESCAPE ? 0x1BU :
                    (key.mods == 0U && code <= 0x7fU ? (u8)code : 0U);

        return yew_trust_prompt_key(ed, answer);
    }
    if (ed->prompt == YEW_PROMPT_AI_BLOCK) {
        u8 answer = code == YEW_KEY_ESCAPE ? 0x1BU :
                    (key.mods == 0U && code <= 0x7fU ? (u8)code : 0U);

        return yew_ai_block_prompt_key(ed, answer);
    }
#if YEW_WITH_PLUGINS
    if (ed->prompt == YEW_PROMPT_PLUGIN_CAP) {
        u32 answer = code == YEW_KEY_ESCAPE ? YEW_KEY_ESCAPE :
                     (key.mods == 0U && code <= 0x7fU ? code : 0U);

        return yew_plug_prompt_key(ed, answer);
    }
#endif
    if (code == YEW_KEY_ESCAPE) {
        ed->quit_after_save = false;
        yew_ed_prompt(ed, YEW_PROMPT_NONE);
        return true;
    }
    if (key.mods != 0U || code > 0x7fU)
        return true;
    if (ed->prompt == YEW_PROMPT_RECOVER && code == (u32)'r') {
        EditCtx ec = yew_ed_edit_ctx(ed);
        Buffer *doc = yew_ed_doc(ed);
        bool recovered = yew_journal_replay_edit(doc->meta.realpath, &ec,
                                                 &doc->meta);

        if (recovered)
            ec.jrnl = yew_journal_open(doc->meta.realpath, &doc->meta);
        yew_ed_finish_edit(ed, &ec);
        yew_ed_prompt(ed, YEW_PROMPT_NONE);
        if (recovered && ec.jrnl == NULL) {
            ed->durability_failed = true;
            yew_msg(ed, YEW_MSG_ERROR,
                    "recovered changes, but crash journal could not reopen; save or q!");
        } else {
            yew_msg(ed, recovered ? YEW_MSG_WARN : YEW_MSG_ERROR,
                    recovered ? "recovered unsaved changes" :
                                "could not recover unsaved changes");
        }
        return true;
    }
    if (ed->prompt == YEW_PROMPT_RECOVER && code == (u32)'d') {
        Buffer *doc = yew_ed_doc(ed);
        bool discarded = yew_journal_discard_path(doc->meta.realpath,
                                                  &doc->meta);

        yew_ed_prompt(ed, YEW_PROMPT_NONE);
        if (!discarded)
            yew_msg(ed, YEW_MSG_ERROR, "could not discard recovery journal");
        return true;
    }
    if (ed->prompt == YEW_PROMPT_QUIT_DIRTY && code == (u32)'w') {
        ed->quit_after_save = true;
        (void)yew_ed_file_save(ed, false);
        return true;
    }
    if (ed->prompt == YEW_PROMPT_QUIT_DIRTY && code == (u32)'d') {
        ed->prompt = YEW_PROMPT_NONE;
        ed->quit = true;
        return true;
    }
    if (ed->prompt == YEW_PROMPT_OVERWRITE && code == (u32)'o') {
        (void)yew_ed_file_save(ed, true);
        return true;
    }
    return true;
}

void yew_ed_handle_key(Ed *ed, Key key, i64 now_ms)
{
    const u16 command_mods = YEW_MOD_ALT | YEW_MOD_CTRL | YEW_MOD_SUPER |
                             YEW_MOD_HYPER | YEW_MOD_META;
    bool compl_fallthrough = false;

    if (ed == NULL || key.kind != YEW_EV_KEY)
        return;
    if (key.ev != YEW_KEY_RELEASE &&
        !(ed->search.active && key.code == YEW_KEY_ENTER))
        yew_search_preview_cancel(ed);
    yew_record_key(ed, key);
    ed->now_ms = now_ms;
    /*
     * Esc mid-gesture cancels it and restores the state the gesture
     * started from, before the key reaches any mode that would also act
     * on Escape.  Sprint 27 widened this from the border drag alone to
     * every gesture the router owns.
     */
    if (key.code == YEW_KEY_ESCAPE && yew_mouse_gesture_active(ed)) {
        yew_mouse_cancel(ed);
        return;
    }
    /*
     * Sprint 27 §5: the context menu is a keymap layer, and it is the
     * TOPMOST one — a menu that let a key through would act on the
     * document behind an open pop-up, the same law s24's and s26's
     * dialogs obey.
     */
    if (yew_mouse_menu_key(ed, &key))
        return;
    if (key.code == YEW_KEY_ESCAPE &&
        (ed->chord.n != 0U || ed->chord.count_given)) {
        yew_dispatch_key(ed, key, now_ms);
        return;
    }
    if (ed->prompt != YEW_PROMPT_NONE) {
        (void)prompt_key(ed, key);
        return;
    }
    /*
     * Sprint 24 §4: the picker is MODAL, so it takes the key before the
     * command line and before any mode.  It swallows everything it does
     * not use — a dialog that let unhandled keys through would edit the
     * document behind it.
     */
    if (yew_gp_key(ed, key)) {
        yew_gp_apply(ed);
        return;
    }
    /*
     * Sprint 26 §5: the list picker is modal for the same reason the
     * group picker is — it swallows what it does not use, so no key
     * reaches the document behind it.
     */
    if (yew_picker_active(ed) && yew_picker_key(ed, &key))
        return;
    if (yew_lsp_rename_key(ed, &key))
        return;
    /*
     * Sprint 47: panels are transient, not modal.  Scroll keys belong to
     * the panel; every other key closes it and falls through this same
     * dispatch pass, so no input is swallowed behind an informational
     * overlay.
     */
    if (ed->win != NULL && ed->win->panel.open &&
        yew_panel_key(ed, &ed->win->panel, &key))
        return;
    if (key.ev != YEW_KEY_RELEASE && yew_msg_dismiss_overlay(ed))
        return;
    if (ed->cmdline.active && yew_cmdline_key(ed, &key))
        return;
    if (ed->win != NULL && ed->win->compl.open) {
        if (yew_compl_key(ed, ed->win, &key))
            return;
        compl_fallthrough = ed->win->compl.open;
    }
    /*
     * Sprint 24 §7.  BEFORE the insert-mode text path: a digit arriving
     * inside the jump window is the second half of a chord, and letting
     * it fall through would type it into the document while the user
     * was navigating.
     */
    if (yew_tab_jump_key(ed, key))
        return;
    if (yew_fuss_active(ed) && yew_fuss_key(ed, &key, now_ms))
        return;
    if (ed->msg.active && ed->msg.sev == YEW_MSG_ERROR)
        yew_msg_clear(ed);
    if (ed->mode == YEW_MODE_I && key.ev != YEW_KEY_RELEASE &&
        key.ntext != 0U && (key.mods & command_mods) == 0U) {
        CmdCtx cx = {0};
        CmdId id = yew_cmd_lookup("ed.edit.insert.text", 19U);

        cx.ed = ed;
        cx.win = ed->win;
        cx.count = 1U;
        cx.sarg = (const char *)key.text;
        cx.sarg_len = key.ntext;
        cx.source = YEW_SRC_KEY;
        ed->last_cmd = id;
        ed->last_status = yew_ed_invoke(ed, id, &cx);
        ed->dispatch_count++;
        if (compl_fallthrough)
            yew_compl_after_key(ed, ed->win);
        else if (ed->last_status == YEW_CMD_OK) {
            (void)yew_compl_maybe_auto_trigger(ed, ed->win);
            yew_lsp_signature_maybe_auto_trigger(ed, ed->win, key.text,
                                                  key.ntext);
        }
        return;
    }
    yew_dispatch_key(ed, key, now_ms);
    if (compl_fallthrough)
        yew_compl_after_key(ed, ed->win);
}

/*
 * The mouse routing spine used to live here.  Sprint 18.5 joined the
 * decoder to the region registry for the completion menu alone; Sprint
 * 27 moved the whole join to ui/mouse.c, so that file is the only place
 * a pointer event becomes an action (its DoD 2).  Nothing in this file
 * routes one, and the event loop's only mouse line hands it straight to
 * yew_mouse_event.
 */
void yew_ed_handle_paste(Ed *ed, const u8 *bytes, size_t len, bool end)
{
    if (ed == NULL)
        return;
    if (!end && bytes == NULL && len == 0U) {
        if (ed->win != NULL)
            yew_shadow_dismiss(ed, ed->win);
        ed->paste.len = 0U;
        ed->paste_active = true;
        ed->shadow_holdoff = true;
        return;
    }
    if (!ed->paste_active)
        return;
    if (!end) {
        if (bytes != NULL && len != 0U)
            bytebuf_append(&ed->paste, bytes, len);
        return;
    }
    if (ed->cmdline.active && ed->paste.len != 0U) {
        yew_cmdline_paste(ed, ed->paste.data, ed->paste.len);
    } else if (ed->mode == YEW_MODE_I && ed->prompt == YEW_PROMPT_NONE &&
        ed->paste.len != 0U) {
        CmdCtx cx = {0};
        CmdId id = yew_cmd_lookup("ed.edit.insert.text", 19U);

        cx.ed = ed;
        cx.win = ed->win;
        cx.count = 1U;
        cx.sarg = (const char *)ed->paste.data;
        cx.sarg_len = ed->paste.len > UINT32_MAX ? UINT32_MAX :
                                                    (u32)ed->paste.len;
        cx.source = YEW_SRC_KEY;
        ed->last_cmd = id;
        ed->last_status = yew_ed_invoke(ed, id, &cx);
        ed->dispatch_count++;
    }
    ed->paste.len = 0U;
    ed->paste_active = false;
    ed->shadow_holdoff = false;
}

void yew_ed_resize(Ed *ed, bool resumed)
{
    bool resized = false;

    if (ed == NULL || !ed->tty_ready)
        return;
    if (resumed) {
        if (!ed->tty.raw) {
            ed->quit = true;
            ed->exit_code = YEW_EXIT_IO;
            return;
        }
        yew_tty_altscreen(&ed->tty, true);
        yew_input_enable(ed->tty.wfd, &ed->tty.caps);
    }
    if (yew_tty_winsize(&ed->tty) && ed->grid_ready &&
        (ed->grid.rows != (u16)ed->tty.rows ||
         ed->grid.cols != (u16)ed->tty.cols)) {
        if (!yew_grid_resize(&ed->grid, (u16)ed->tty.rows,
                             (u16)ed->tty.cols)) {
            ed->quit = true;
            ed->exit_code = YEW_EXIT_IO;
            return;
        }
        if (ed->win != NULL)
            yew_vp_invalidate(ed->win);
        if (ed->win != NULL && ed->win->compl.open)
            yew_compl_resize(ed, ed->win);
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

static void perf_frame_tag(Ed *ed, size_t visible_bytes)
{
    char tag[64];
    int n;

    ed->perf_frame_visible_bytes = visible_bytes > UINT32_MAX
                                       ? UINT32_MAX : (u32)visible_bytes;
    if (ed->perf_frame_tags && ed->perf_frame_keys != 0U) {
        n = snprintf(tag, sizeof(tag), "\033]777;yew-key;%u;%u\a",
                     (unsigned)ed->perf_frame_keys,
                     (unsigned)ed->perf_frame_visible_bytes);
        if (n <= 0 || (size_t)n >= sizeof(tag))
            YEW_BUG("performance frame tag overflow");
        bytebuf_reserve(&ed->frame, ed->frame.len + (size_t)n);
        if (ed->frame.len != 0U)
            (void)memmove(ed->frame.data + (size_t)n, ed->frame.data,
                         ed->frame.len);
        (void)memcpy(ed->frame.data, tag, (size_t)n);
        ed->frame.len += (size_t)n;
    }
    ed->perf_frame_output_bytes = ed->frame.len > UINT32_MAX ? UINT32_MAX :
                                  (u32)ed->frame.len;
}

void yew_ed_render(Ed *ed)
{
    Win *win;
    LineNo cursor_line;
    bool cursor_line_changed;
    bool view_changed;
    bool fuss;

    if (ed == NULL || !ed->grid_ready || !ed->render_ready ||
        !ed->model_ready)
        return;
    win = ed->win;
    fuss = yew_fuss_active(ed);
    cursor_line = LINENO(0U);
    if (fuss) {
        if (ed->full_damage)
            yew_fuss_draw(ed);
        if (ed->full_damage || ed->footer_dirty)
            yew_draw_footer(ed, win);
        yew_grid_cursor(&ed->grid, 0U, 0U, false);
        goto draw_overlays;
    }
    if (ed->cursor_follow_pending) {
        yew_win_follow_cursor(win);
        ed->cursor_follow_pending = false;
    }
    cursor_line = yew_textbuf_line_of(win->buf->tb,
                                      yew_ed_cursor(ed)->pos);
    cursor_line_changed = ed->drawn_cursor_line_valid &&
                          ed->drawn_cursor_line.v != cursor_line.v;
    view_changed = !ed->drawn_top_valid ||
                   ed->drawn_top.v != yew_win_view_top(win).v ||
                   ed->drawn_top_sub != win->vp.top_sub ||
                   ed->drawn_left.v != win->vp.left.v ||
                   ed->drawn_wrap != win->vp.wrap ||
                   !ed->drawn_cursor_line_valid;
    if (view_changed) {
        yew_ed_damage_document(ed);
    } else if ((win->number_style == YEW_NUM_REL ||
                win->number_style == YEW_NUM_HYBRID) &&
               cursor_line_changed) {
        /* Relative values change on every visible line, but only inside the
         * gutter.  Re-running document drawing here used to revisit text,
         * syntax spans, selections, diagnostics, and overlays on every
         * vertical arrow even though the cell diff ultimately emitted only
         * the numbers.  Keep the distance-number UX without turning cursor
         * motion into a document repaint. */
        yew_gutter_draw(ed, win, 0U, win->rect.h);
        ed->footer_dirty = true;
    } else if (win->number_style == YEW_NUM_ABS && cursor_line_changed) {
        u16 row;

        /* Absolute numbering changes only the old and new cursor-line
         * accents.  Cursor motion otherwise moves the terminal cursor
         * without repainting document cells, so explicitly damage both
         * gutter rows instead of leaving the old number bold until the
         * next full redraw. */
        if (yew_win_view_row(win, ed->drawn_cursor_line, &row))
            yew_ed_damage_rows(ed, row, (u16)(row + 1U));
        if (yew_win_view_row(win, cursor_line, &row))
            yew_ed_damage_rows(ed, row, (u16)(row + 1U));
    }
    if (win->shadow.live)
        ed->full_damage = true;
    if (ed->full_damage) {
        /*
         * Every leaf plus the borders their splits own.  With a single
         * pane this draws exactly the rows the one-window path drew,
         * which is what keeps the Sprint 6-21 goldens byte-identical.
         */
        yew_draw_panes(ed);
        yew_grid_mark_all(&ed->grid);
    } else if (ed->doc_damage_lo < ed->doc_damage_hi) {
        if (yew_pane_leaf_count(ed->pane_root) > 1U) {
            /* Partial damage is expressed in the focused pane's rows,
             * and a border or a neighbour may share them; redrawing the
             * tree is correct and still bounded by the screen. */
            yew_draw_panes(ed);
        } else {
            yew_draw_document_rows(ed, win, ed->doc_damage_lo,
                                   ed->doc_damage_hi);
        }
    }
    if (ed->full_damage || ed->footer_dirty)
        yew_draw_footer(ed, win);
    if (!ed->cmdline.active)
        yew_draw_cursor(ed, win);
    yew_shadow_draw_panes(ed);
draw_overlays:
    /*
     * The picker draws LAST, after the footer, cursor preparation, and
     * passive shadow text.
     *
     * It is modal and owns its rectangle, and its filter line IS the
     * command line — so drawing it inside yew_draw_panes meant the
     * footer then painted the same widget at the bottom of the screen
     * and left the picker's copy blank.  One widget, one place, and the
     * modal thing on top.
     */
    if (yew_picker_active(ed))
        yew_picker_draw(ed, (Rect){0U, 0U, ed->grid.cols, ed->grid.rows});
    /*
     * Sprint 27 §5: the context menu is drawn after everything, for the
     * same reason and with the same consequence — its BLOCK and CTX_ROW
     * regions are added last, and last-added-wins is what makes it
     * shadow whatever is beneath with no z-order machinery.
     */
    if (yew_ctx_active())
        yew_mouse_menu_draw(ed);
    /* Sprint 44: the non-modal completion popup is the final overlay and
     * therefore owns the last-added hit regions for its exact boxes. */
    if (win->compl.open)
        yew_compl_draw(ed, win, &ed->grid);
    /* Sprint 47: the floating panel is the final overlay.  Its draw pass
     * registers the stored rect as the last-added BLOCK region. */
    if (win->panel.open)
        yew_panel_draw(ed, &win->panel, &ed->grid);
    ed->frame.len = 0U;
    (void)yew_render_frame(&ed->render, &ed->grid, &ed->frame);
    perf_frame_tag(ed, ed->frame.len);
    yew_prof_phase(&ed->prof, YEW_PH_WRITE);
    if (!write_all(ed->tty.wfd, ed->frame.data, ed->frame.len)) {
        ed->quit = true;
        ed->exit_code = YEW_EXIT_IO;
        return;
    }
    if (!ed->rss_paint_logged) {
        yew_rss_checkpoint("paint");
        ed->rss_paint_logged = true;
    }
    yew_grid_flip(&ed->grid);
    ed->full_damage = false;
    ed->footer_dirty = false;
    ed->doc_damage_lo = ed->grid.rows;
    ed->doc_damage_hi = 0U;
    if (!fuss) {
        ed->drawn_top = yew_win_view_top(win);
        ed->drawn_top_sub = win->vp.top_sub;
        ed->drawn_left = win->vp.left;
        ed->drawn_wrap = win->vp.wrap;
        ed->drawn_cursor_line = cursor_line;
        ed->drawn_cursor_line_valid = true;
        ed->drawn_top_valid = true;
    } else {
        ed->drawn_cursor_line_valid = false;
        ed->drawn_top_valid = false;
    }
}

static const char *load_error_text(YewLoadErr error)
{
    switch (error) {
    case YEW_LOAD_EACCES:
        return "permission denied";
    case YEW_LOAD_EISDIR:
        return "is a directory";
    case YEW_LOAD_TOO_LARGE:
        return "file is too large";
    case YEW_LOAD_IO:
        return "input/output error";
    case YEW_LOAD_OK:
    case YEW_LOAD_ENOENT:
        break;
    }
    return "unknown load error";
}

static const char *ed_getenv(const char *name)
{
    return getenv(name);
}

static int ed_driver_inner(const char *const *paths, size_t npaths,
                           const YewEdStartup *startup)
{
    Ed ed;
    YewLoadErr load = YEW_LOAD_OK;
    int result;
    i64 wall_now;
    u16 rows;
    u16 cols;

    yew_ed_init(&ed);
    if (startup != NULL && startup->workspace_dir != NULL &&
        !yew_ed_set_workspace_root(&ed, startup->workspace_dir)) {
        (void)fprintf(stderr,
                      "yew: error: workspace is not a directory: %s\n",
                      startup->workspace_dir);
        yew_ed_free(&ed);
        return YEW_EXIT_ERR;
    }
    wall_now = (i64)time(NULL);
    if (wall_now >= 0)
        yew_git_editor_clock_anchor(&ed, yew_now_ms(), wall_now);
    yew_config_init(&ed, startup);
    errno = 0;
    if (!yew_tty_open(&ed.tty)) {
        result = errno == ENOTTY ? YEW_EXIT_ERR : YEW_EXIT_IO;
        yew_tty_close(&ed.tty);
        yew_ed_free(&ed);
        return result;
    }
    ed.tty_ready = true;
    if (!yew_tty_raw(&ed.tty)) {
        yew_ed_free(&ed);
        return YEW_EXIT_IO;
    }
    yew_tty_altscreen(&ed.tty, true);
    if (!ed.tty.alt) {
        yew_ed_free(&ed);
        return YEW_EXIT_IO;
    }
    yew_tty_probe_start(&ed.tty, yew_now_ms());
    yew_input_enable(ed.tty.wfd, &ed.tty.caps);
    yew_input_init(&ed.in, &ed.tty.caps);
    ed.input_ready = true;

    if (npaths == 0U) {
        (void)yew_ed_open_scratch(&ed);
    } else {
        size_t i;

        load = yew_ed_open(&ed, paths[0]);
        if (load != YEW_LOAD_OK && load != YEW_LOAD_ENOENT) {
            const char *message = load_error_text(load);

            yew_ed_free(&ed);
            (void)fprintf(stderr, "yew: error: cannot open %s: %s\n",
                          paths[0], message);
            return YEW_EXIT_IO;
        }
        for (i = 1U; i < npaths; i++) {
            if (yew_tab_open(&ed, paths[i]) < 0) {
                yew_ed_free(&ed);
                (void)fprintf(stderr, "yew: error: cannot open tab: %s\n",
                              paths[i]);
                return YEW_EXIT_IO;
            }
        }
    }
    rows = ed.tty.rows > 0 && ed.tty.rows <= UINT16_MAX ?
               (u16)ed.tty.rows : 24U;
    cols = ed.tty.cols > 0 && ed.tty.cols <= UINT16_MAX ?
               (u16)ed.tty.cols : 80U;
    if (!yew_grid_init(&ed.grid, &ed.interner, rows, cols)) {
        yew_ed_free(&ed);
        return YEW_EXIT_IO;
    }
    ed.grid_ready = true;
    yew_render_init(&ed.render, &ed.tty.caps, ed_getenv);
    ed.render_ready = true;
    yew_theme_sync_surfaces(&ed);
    /*
     * State comes up AFTER the buffers so a restore has somewhere to
     * land, and the lock is claimed before the first change can mark
     * anything dirty.
     */
    (void)yew_config_load_all(&ed, NULL);
    yew_rss_checkpoint("config");
    (void)yew_theme_auto_startup(&ed);
    {
        const char *override = startup != NULL && startup->theme != NULL
                                   ? startup->theme : getenv("YEW_THEME");

        if (override != NULL) {
            char error[192];

            if (!yew_theme_set(&ed, override, error, sizeof(error)))
                yew_msg(&ed, YEW_MSG_ERROR, "%s", error);
        }
    }
    if (!ed.clean)
        yew_state_open(&ed);
    /*
     * Restored only when no file was named.  `yew yew.c` is a
     * request to edit that file, and burying it under forty restored
     * tabs answers a question nobody asked; the arrangement comes back
     * when you start the way you left — in the directory, with no
     * argument.
     */
    if (npaths == 0U && !ed.clean)
        (void)yew_ws_restore(&ed);
#if YEW_WITH_PLUGINS
    /* Plugins see the restored workspace, but load before ws.open so their
     * declared workspace hooks observe the first session boundary. */
    (void)yew_plug_boot(&ed);
#endif
    yew_symwalk_start(&ed);
    /* The workspace hook sees restored tabs and buffers, and runs before
     * the first paint.  Startup used to paint once before restore, which
     * made this ordering impossible and also caused a redundant frame. */
#if YEW_WITH_PLUGINS
    if (!yew_plug_startup_pending(&ed))
#endif
        yew_fl_hook_workspace(&ed, FL_EV_WS_OPEN);
    yew_ed_layout(&ed);
    yew_ed_render(&ed);
    result = ed.quit ? ed.exit_code : yew_loop_run(&ed);
    yew_fl_hook_workspace(&ed, FL_EV_WS_CLOSE);
    /* Clean quit: the unconditional save, before anything is freed. */
    if (!ed.clean)
        yew_state_close(&ed);
    yew_ed_free(&ed);
    return result;
}

int yew_ed_driver(const char *path)
{
    return yew_ed_driver_opts(path, NULL);
}

int yew_ed_driver_opts(const char *path, const YewEdStartup *startup)
{
    const char *paths[1];

    if (path == NULL)
        return yew_ed_driver_files_opts(NULL, 0U, startup);
    paths[0] = path;
    return yew_ed_driver_files_opts(paths, 1U, startup);
}

int yew_ed_driver_files_opts(const char *const *paths, size_t npaths,
                             const YewEdStartup *startup)
{
    YewEdStartup effective = {0};
    TtyGuard guard;
    int result;
    size_t i;

    if (startup != NULL)
        effective = *startup;
    for (i = 0U; i < npaths; i++) {
        struct stat st;

        if (stat(paths[i], &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (npaths != 1U || effective.workspace_dir != NULL) {
            (void)fprintf(stderr,
                          "yew: error: directory argument cannot be combined with files or --workspace: %s\n",
                          paths[i]);
            return YEW_EXIT_ERR;
        }
        effective.workspace_dir = paths[i];
        paths = NULL;
        npaths = 0U;
        break;
    }

    if (!yew_tty_guard_start(&guard))
        return YEW_EXIT_IO;
    result = ed_driver_inner(paths, npaths, &effective);
    if (!yew_tty_guard_finish(&guard) && result == YEW_EXIT_OK)
        result = YEW_EXIT_IO;
    return result;
}
