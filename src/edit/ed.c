#define _XOPEN_SOURCE 700

#include "edit/ed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui/ctxmenu.h"
#include "ui/draw.h"
#include "ui/grouppicker.h"
#include "ui/picker.h"
#include "ui/pickers.h"
#include "ui/viewport.h"
#include "util/log.h"

/* Tears down everything a buffer owns without touching the list slot. */
static void ed_buffer_dispose(Buffer *b)
{
    if (b->jrn != NULL) {
        sag_journal_close(b->jrn);
        b->jrn = NULL;
    }
    sag_marks_free(b->marks);
    sag_undo_free(b->undo);
    sag_textbuf_free(b->tb);
    sag_filemeta_dispose(&b->meta);
    (void)memset(b, 0, sizeof(*b));
}

static void ed_ws_free(Ed *ed)
{
    u32 i;

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

    if (!ed->model_ready)
        return;
    sag_ed_insert_barrier(ed);
    sag_reg_bind_context(&ed->regs, NULL, NULL);
    sag_search_state_free(&ed->search);
    sag_search_confirm_cancel(ed);
    /*
     * Every tree belongs to a tab, including the first one, and each
     * tab releases its own leaves' views — so this is the ONE owner.
     * Freeing ed->panes here as well double-freed every split view.
     */
    sag_tabs_free(ed);
    /* After the tabs: dissolving a group walks the tabs array, so the
     * groups outlive the members they are asked about. */
    sag_groups_free(ed);
    ed->pane_root = NULL;
    ed->focus = NULL;
    sag_overlay_free(&ed->single_win.overlay);
    sag_vp_free(&ed->single_win);
    sag_cset_free(&ed->single_win.cs);
    ed_ws_free(ed);
    ed_buffer_dispose(b);
    (void)memset(&ed->single_win, 0, sizeof(ed->single_win));
    ed->win = NULL;
    ed->model_ready = false;
}

static void ed_ws_push(Ed *ed, Buffer *b)
{
    if (ed->ws.nbufs == ed->ws.cap) {
        u32 cap = ed->ws.cap == 0U ? 4U : ed->ws.cap * 2U;

        if (cap < ed->ws.cap)
            SAG_BUG("workspace buffer list overflow");
        ed->ws.bufs = sag_xreallocarray(ed->ws.bufs, cap,
                                        sizeof(*ed->ws.bufs));
        ed->ws.cap = cap;
    }
    ed->ws.bufs[ed->ws.nbufs++] = b;
}

const char *sag_buf_label(const Buffer *b)
{
    if (b == NULL)
        return "";
    if (b->name != NULL)
        return b->name;
    return b->path == NULL ? "[No Name]" : b->path;
}

Buffer *sag_ws_scratch_new(Ed *ed, const char *name, u32 flags)
{
    Buffer *b;

    if (ed == NULL || !ed->model_ready || name == NULL)
        return NULL;
    b = sag_xcalloc(1U, sizeof(*b));
    b->id = ed->ws.next_buf_id++;
    sag_filemeta_init(&b->meta);
    b->tb = sag_textbuf_new();
    b->name = arena_strdup(&ed->arena, name);
    b->flags = flags | SAG_BUF_SCRATCH;
    b->tabwidth = SAG_VP_TABWIDTH;
    /* A scratch buffer still carries an undo tree so the edit chokepoint
     * stays uniform; SAG_BUF_NOUNDO governs whether edits record into it. */
    b->undo = sag_undo_new(b->tb);
    sag_undo_mark_saved(b->undo);
    b->marks = sag_marks_new();
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
Buffer *sag_ws_file_buf(Ed *ed, const char *path)
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
    b = sag_xcalloc(1U, sizeof(*b));
    b->id = ed->ws.next_buf_id++;
    sag_filemeta_init(&b->meta);
    b->tabwidth = SAG_VP_TABWIDTH;
    b->path = path == NULL ? NULL : arena_strdup(&ed->arena, path);
    if (path == NULL) {
        /*
         * An untitled tab has nowhere to be read FROM, so deferring it
         * would mean a buffer that can never become resident.  It is
         * born loaded and empty.
         */
        b->tb = sag_textbuf_new();
        b->undo = sag_undo_new(b->tb);
        sag_undo_mark_saved(b->undo);
        b->marks = sag_marks_new();
    }
    ed_ws_push(ed, b);
    return b;
}

bool sag_buf_resident(const Buffer *b)
{
    /* Asked of the allocation.  DoD 2 greps src/ui for a residency
     * FLAG and must find none. */
    return b != NULL && b->tb != NULL;
}

int sag_buf_hydrate(Ed *ed, Buffer *b)
{
    TextBuf *tb = NULL;
    SagLoadErr load;

    if (ed == NULL || b == NULL)
        return -1;
    if (b->tb != NULL)
        return 0; /* already resident: no read, no work */
    if (b->path == NULL)
        return -1;
    /*
     * sag_file_load overwrites meta->realpath, which realpath(3)
     * allocated on the PREVIOUS load -- so re-entering a deferred tab
     * leaked PATH_MAX bytes every time.  Disposing here rather than in
     * sag_buf_defer keeps the pairing next to the call that overwrites
     * it; dispose re-inits, so a never-loaded buffer is fine.
     */
    sag_filemeta_dispose(&b->meta);
    load = sag_file_load(b->path, &tb, &b->meta);
    if (load != SAG_LOAD_OK && load != SAG_LOAD_ENOENT) {
        sag_textbuf_free(tb);
        return -1;
    }
    /* A path that does not exist yet opens empty, exactly as it does
     * on the command line. */
    if (tb == NULL)
        tb = sag_textbuf_new();
    b->tb = tb;
    b->undo = sag_undo_new(tb);
    sag_undo_mark_saved(b->undo);
    if (b->marks == NULL)
        b->marks = sag_marks_new();
    /*
     * Sprint 25 §6: restored marks become real here, at the first
     * moment there is text to anchor them to.  Clamped to the buffer,
     * because the file may well have changed on disk since the state
     * was written and a mark past the end is not a cosmetic problem.
     */
    {
        u32 i;
        u64 size = sag_textbuf_len(b->tb);

        for (i = 0U; i < 26U; i++) {
            if (!b->pending_mark_set[i])
                continue;
            b->pending_mark_set[i] = false;
            (void)sag_ed_mark_set(ed, b, (u8)('a' + i),
                                  BYTEOFF(b->pending_marks[i] > size
                                              ? size
                                              : b->pending_marks[i]));
        }
    }
    return 0;
}

void sag_buf_defer(Ed *ed, Buffer *b)
{
    (void)ed;
    if (b == NULL || b->tb == NULL || b->path == NULL)
        return;
    if (b->jrn != NULL) {
        sag_journal_close(b->jrn);
        b->jrn = NULL;
    }
    /*
     * Releasing the undo tree is what clears modified: dirtiness is
     * derived from it, so a buffer read from nowhere reports clean
     * without anyone assigning a flag.
     */
    sag_undo_free(b->undo);
    b->undo = NULL;
    sag_marks_free(b->marks);
    b->marks = NULL;
    sag_textbuf_free(b->tb);
    b->tb = NULL;
}

Buffer *sag_ws_buf_by_id(Ed *ed, u32 id)
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

Win *sag_ed_win_by_id(Ed *ed, u32 id)
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
    return ed->single_win.id == id ? &ed->single_win : NULL;
}

Buffer *sag_ws_scratch_find(Ed *ed, const char *name)
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

void sag_ws_scratch_drop(Ed *ed, Buffer *b)
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
            (void)sag_ed_show_buffer(ed, &ed->buffer);
        ed_buffer_dispose(b);
        free(b);
        (void)memmove(&ed->ws.bufs[i], &ed->ws.bufs[i + 1U],
                      (size_t)(ed->ws.nbufs - i - 1U) *
                          sizeof(*ed->ws.bufs));
        ed->ws.nbufs--;
        return;
    }
}

bool sag_ed_show_buffer(Ed *ed, Buffer *b)
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
    sag_ed_insert_barrier(ed);
    sag_vp_free(ed->win);
    sag_cset_free(&ed->win->cs);
    sag_cset_init(&ed->win->cs, cursor);
    ed->win->buf = b;
    sag_vp_init(ed->win);
    sag_reg_bind_context(&ed->regs, b->undo, &b->meta);
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
    ed->buffer.id = ed->ws.next_buf_id++;
    ed->buffer.path = path == NULL ? NULL : arena_strdup(&ed->arena, path);
    ed->buffer.tabwidth = SAG_VP_TABWIDTH;
    ed->buffer.undo = sag_undo_new(tb);
    sag_undo_mark_saved(ed->buffer.undo);
    ed->buffer.marks = sag_marks_new();
    ed->buffer.jrn = NULL;
    sag_search_opts_init(&ed->search_opts);
    sag_search_state_init(&ed->search);
    sag_overlay_init(&ed->single_win.overlay);
    /* One leaf holding the document window: the pane tree always
     * exists, so no code path has to ask whether panes are "on". */
    ed->pane_root = sag_pane_new_leaf(&ed->single_win);
    ed->focus = ed->pane_root;
    /*
     * Tab 0 owns the tree that already exists rather than cloning a
     * second view of the same buffer: the editor always has exactly one
     * tab, so no code path has to ask whether tabs are "on".
     */
    sag_tabs_init(&ed->tabs);
    sag_groups_init(&ed->groups);
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

            first.path = sag_xmalloc(n);
            (void)memcpy(first.path, ed->buffer.path, n);
        }
        TabVec_push(&ed->tabs.v, first);
        ed->tabs.active = 0;
    }
    sag_reg_bind_context(&ed->regs, ed->buffer.undo, &ed->buffer.meta);
    ed->single_win.id = ed->next_win_id++;
    sag_cset_init(&ed->single_win.cs, cursor);
    ed->single_win.buf = &ed->buffer;
    sag_vp_init(&ed->single_win);
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

void sag_ed_init(Ed *ed)
{
    char *root;

    if (ed == NULL)
        SAG_BUG("editor init: NULL editor");
    (void)memset(ed, 0, sizeof(*ed));
    arena_init(&ed->arena);
    arena_init(&ed->cmdline.comp_arena);
    interner_init(&ed->interner, &ed->arena);
    bytebuf_init(&ed->frame);
    bytebuf_init(&ed->paste);
    sag_reg_init(&ed->regs);
    sag_timers_init(&ed->timers);
    sag_jobs_init(&ed->jobs);
    sag_mouse_init(&ed->mouse);
    root = realpath(".", NULL);
    if (root == NULL)
        root = getcwd(NULL, 0U);
    ed->ws.dir = arena_strdup(&ed->arena, root == NULL ? "." : root);
    free(root);
    fl_origin_reg_init(&ed->origins);
    fl_h_table_init(&ed->handles);
    /* Window ids start at 1; 0 is never handed out, so a zeroed Win is
     * distinguishable from window one. */
    ed->next_win_id = 1U;
    sag_dispatch_init(ed);
    ed->dispatch_ready = true;
    ed->exit_code = SAG_EXIT_OK;
}

const char *sag_ws_root(const Ed *ed)
{
    return ed == NULL || ed->ws.dir == NULL ? "." : ed->ws.dir;
}

void sag_ed_free(Ed *ed)
{
    if (ed == NULL)
        return;
    /* Belt and braces: a path that never reached sag_state_close still
     * must not leave its lock behind.  Dispose saves nothing. */
    sag_state_dispose(ed);
    sag_picker_close(ed, false);
    sag_pickers_dispose();
    sag_cmdline_dispose(ed);
    /* Jobs die with the process (never persisted, s25); kill and reap
     * before the buffers they append into go away. */
    sag_jobs_free(ed);
    ed_buffer_free(ed);
    sag_reg_free(&ed->regs);
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
    fl_h_table_free(&ed->handles);
    fl_origin_reg_free(&ed->origins);
    sag_timers_free(&ed->timers);
    bytebuf_free(&ed->paste);
    bytebuf_free(&ed->frame);
    interner_free(&ed->interner);
    arena_free_all(&ed->cmdline.comp_arena);
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
Buffer *sag_ed_doc(Ed *ed)
{
    if (ed == NULL)
        return NULL;
    if (ed->win != NULL && ed->win->buf != NULL)
        return ed->win->buf;
    return &ed->buffer;
}

/* Shim so the text layer can feed the changelist without knowing it
 * exists.  Job output goes to SAG_BUF_NOUNDO scratch buffers, which are
 * not places the user "changed" and so are excluded here. */
static void ed_on_change(void *ctx, ByteOff at, i64 now_ms)
{
    Buffer *b = ctx;

    if (b == NULL || (b->flags & SAG_BUF_NOUNDO) != 0U)
        return;
    sag_change_record(b, at, now_ms);
}

bool sag_ed_mark_set(Ed *ed, Buffer *b, u8 name, ByteOff at)
{
    u32 slot;

    (void)ed;
    if (b == NULL || b->marks == NULL || name < 'a' || name > 'z')
        return false;
    slot = (u32)(name - 'a');
    /* Re-setting a name drops the old mark: leaking one per keystroke
     * of `ma` would grow the mark set without bound. */
    if (b->named_set[slot] && sag_mark_alive(b->marks, b->named[slot]))
        sag_mark_del(b->marks, b->named[slot]);
    b->named[slot] = sag_mark_add(b->marks, at, SAG_BIAS_LEFT);
    b->named_set[slot] = true;
    return true;
}

bool sag_ed_mark_get(Ed *ed, const Buffer *b, u8 name, ByteOff *out)
{
    u32 slot;

    (void)ed;
    if (b == NULL || b->marks == NULL || name < 'a' || name > 'z')
        return false;
    slot = (u32)(name - 'a');
    if (!b->named_set[slot] || !sag_mark_alive(b->marks, b->named[slot]))
        return false;
    if (out != NULL)
        *out = sag_mark_pos(b->marks, b->named[slot]);
    return true;
}

struct Pane *sag_ed_pane_root(Ed *ed)
{
    return ed == NULL ? NULL : ed->pane_root;
}

/*
 * A new view of the SAME buffer.  The cursor set and viewport are
 * copied so the split opens showing what the user was already looking
 * at; the jumplist is not, because it is this view's history and a view
 * that has been nowhere has no history (Sprint 21 §5).
 */
Win *sag_ed_win_clone(Ed *ed, const Win *src)
{
    Win *w;

    /*
     * No cap here.  SAG_PANE_MAX_LEAVES bounds the leaves of ONE tab's
     * tree, and sag_pane_split enforces it by counting that tree.
     * Sprint 23 also kept a write-only registry of every cloned Win and
     * capped it with the same constant, which quietly made 16 the limit
     * on tabs-plus-splits for the whole editor — a 40-file group (D3)
     * could not be opened at all, and the refusal surfaced as
     * sag_tab_open returning -1 for no stated reason.  Nothing ever
     * read the registry: tab trees own their leaves and release them.
     */
    if (ed == NULL || src == NULL)
        return NULL;
    w = sag_xcalloc(1U, sizeof(*w));
    w->id = ed->next_win_id++;
    w->buf = src->buf;
    w->rect = src->rect;
    w->number_style = src->number_style;
    w->gutter_width = src->gutter_width;
    {
        Cursor seed = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

        if (src->cs.curs.len > 0U && src->cs.primary < src->cs.curs.len)
            seed = src->cs.curs.data[src->cs.primary];
        sag_cset_init(&w->cs, seed);
    }
    sag_vp_init(w);
    w->vp.top = src->vp.top;
    w->vp.top_sub = src->vp.top_sub;
    sag_overlay_init(&w->overlay);
    sag_jumplist_init(&w->jumps);
    return w;
}

/*
 * Points a window at a buffer with a fresh view.
 *
 * The cursor is NOT carried over: a clone seeds its cursor from the
 * window it copied, and that offset means nothing in a different file
 * — in a shorter one it is past the end.
 */
void sag_ed_win_set_buffer(Ed *ed, Win *w, Buffer *b)
{
    Cursor origin = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    (void)ed;
    if (w == NULL || b == NULL || w->buf == b)
        return;
    sag_vp_free(w);
    sag_cset_free(&w->cs);
    sag_cset_init(&w->cs, origin);
    w->buf = b;
    sag_vp_init(w);
}

void sag_ed_win_release(Ed *ed, Win *w)
{
    if (ed == NULL || w == NULL || w == &ed->single_win)
        return;
    sag_overlay_free(&w->overlay);
    sag_vp_free(w);
    sag_cset_free(&w->cs);
    free(w);
}

EditCtx sag_ed_edit_ctx_for(Ed *ed, Win *win)
{
    EditCtx ec = {0};
    Buffer *buffer;

    if (ed == NULL || win == NULL || win->buf == NULL ||
        win->buf->tb == NULL)
        return ec;
    buffer = win->buf;
    ec.tb = buffer->tb;
    ec.marks = buffer->marks;
    ec.cset = &win->cs;
    ec.win_id = 0U;
    ec.jrnl = buffer->jrn;
    ec.undo = buffer->undo;
    ec.meta = buffer->path == NULL ? NULL : &buffer->meta;
    ec.on_change = ed_on_change;
    ec.on_change_ctx = buffer;
    ec.now_ms = ed->now_ms;
    return ec;
}

EditCtx sag_ed_edit_ctx(Ed *ed)
{
    return ed == NULL ? (EditCtx){0} : sag_ed_edit_ctx_for(ed, ed->win);
}

void sag_ed_finish_edit(Ed *ed, const EditCtx *ec)
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
        sag_overlay_invalidate(&ed->win->overlay);
    /*
     * The journal belongs to whichever buffer the edit ran against.
     * Matching on slot 0 alone stopped being right once tabs stopped
     * sharing one buffer, and the miss is silent: the edit journals,
     * the handle is dropped on the floor, and the next crash recovers
     * nothing.
     */
    {
        Buffer *doc = sag_ed_doc(ed);

        if (doc != NULL && doc->tb == ec->tb)
            doc->jrn = ec->jrnl;
        else if (ed->buffer.tb == ec->tb)
            ed->buffer.jrn = ec->jrnl;
    }
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

void sag_ed_damage_rows(Ed *ed, u16 lo, u16 hi)
{
    if (ed == NULL || ed->win == NULL || lo >= hi)
        return;
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
    sag_ed_damage_rows(ed, lo, hi);
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
    bool edits_text;
    bool multiple;
    bool multi;
    bool document_target;
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
    edits_text = changes || strcmp(desc->name, "ed.edit.undo") == 0 ||
                 strcmp(desc->name, "ed.edit.redo") == 0;
    document_target = cx->win == ed->win;
    multiple = changes && ed->model_ready && cx->win != NULL &&
               cx->win->cs.curs.len > 1U;
    multi = multiple &&
            cx->range.kind == SAG_RANGE_NONE &&
            (desc->flags & SAG_CMD_MULTI_AGGREGATE) == 0U;
    durability_command = document_target && edits_text;
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
        ec = sag_ed_edit_ctx_for(ed, cx->win);
        if (started_in_insert && !newline) {
            if (!ed->insert_txn) {
                sag_undo_begin(&ec,
                               multiple ? SAG_TXN_MULTI : SAG_TXN_TYPE);
                ed->insert_txn = true;
                opened = true;
            }
        } else {
            /*
             * The reason has to describe what the command IS, because
             * a command that opens its own inner transaction must
             * agree with the one wrapped around it — `:s` opens
             * SAG_TXN_REPLACE inside, and a TYPE wrapper aborts on the
             * mismatch.  Reached through the real dispatcher only, so
             * unit tests calling the replace API directly never saw it;
             * the pty golden did.
             */
            sag_undo_begin(
                &ec,
                multiple ? SAG_TXN_MULTI
                      : (strcmp(desc->name, "ed.search.replace") == 0
                             ? SAG_TXN_REPLACE
                      : (strstr(desc->name, ".delete.") != NULL ||
                                 strcmp(desc->name,
                                        "ed.edit.line.delete") == 0
                             ? SAG_TXN_ERASE
                             : SAG_TXN_TYPE)));
            opened = true;
        }
    }

    status = multi ? sag_mc_run(cx->win, id, cx) : sag_cmd_invoke(id, cx);
    if (status == SAG_CMD_OK && document_target && ed->win != NULL &&
        (changes || durability_command ||
         strncmp(desc->name, "ed.move.", 8U) == 0 ||
         strncmp(desc->name, "ed.view.", 8U) == 0))
        sag_selstack_clear(ed->win);
    if (changes && ed->model_ready) {
        ec = sag_ed_edit_ctx_for(ed, cx->win);
        if (status != SAG_CMD_OK &&
            (opened || (started_in_insert && ed->insert_txn))) {
            sag_undo_abort(&ec);
            if (started_in_insert)
                ed->insert_txn = false;
        } else if (!started_in_insert && ed->mode == SAG_MODE_I &&
                   !newline && opened) {
            /* `o`/`O` create the line and enter I as one editing run. */
            if (ed->win != NULL && ed->win->cs.curs.len > 1U)
                sag_undo_promote_multi(&ec);
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
    if (ed->cmdline.active && cx->win == sag_cmdline_target(ed)) {
        if (edits_text)
            sag_cmdline_edited(ed);
        else
            sag_cmdline_sync(ed);
    }
    ed->footer_dirty = true;
    return status;
}

CmdStatus sag_ed_invoke_parsed(Ed *ed, CmdId id,
                               const SagCmdInvoke *invoke)
{
    const CmdDesc *desc;
    CmdCtx cx = {0};
    const char *arg = NULL;

    if (ed == NULL || invoke == NULL)
        return SAG_CMD_ERR_ARG;
    desc = sag_cmd_desc(id);
    if (desc == NULL || invoke->argv.n == 0U)
        return SAG_CMD_ERR_ARG;
    if (invoke->argv.n > 1U)
        arg = invoke->argv.v[1];
    cx.win = invoke->win;
    cx.range = invoke->range;
    cx.argv = invoke->argv;
    cx.count = invoke->count > 0 && invoke->count <= UINT32_MAX ?
               (u32)invoke->count : 1U;
    cx.count_given = invoke->count > 0;
    cx.bang = invoke->bang;
    cx.source = SAG_SRC_CMDLINE;
    switch ((CmdArity)desc->arity) {
    case SAG_ARITY_NONE:
        break;
    case SAG_ARITY_STR:
    case SAG_ARITY_OPT_STR:
        cx.sarg = arg;
        cx.sarg_len = arg == NULL ? 0U :
                      strlen(arg) > UINT32_MAX ? UINT32_MAX :
                                                (u32)strlen(arg);
        break;
    case SAG_ARITY_INT:
    case SAG_ARITY_OPT_INT:
        if (arg != NULL) {
            char *end = NULL;
            long long value;

            errno = 0;
            value = strtoll(arg, &end, 10);
            if (errno != 0 || end == arg || *end != '\0')
                return SAG_CMD_ERR_ARG;
            cx.iarg = (i64)value;
        }
        break;
    }
    return sag_ed_invoke(ed, id, &cx);
}

void sag_ed_prompt(Ed *ed, PromptKind prompt)
{
    const char *path;

    if (ed == NULL)
        return;
    ed->prompt = prompt;
    {
        const Buffer *doc = sag_ed_doc(ed);

        path = doc == NULL || doc->path == NULL ? "[no name]" : doc->path;
    }
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
    case SAG_PROMPT_WS_FORGET:
        /* ed.ws.forget writes its own question, because it names the
         * directory it is about to remove and this function does not
         * have it. */
        break;
    }
    if (prompt != SAG_PROMPT_NONE)
        ed->msg.prompt = true;
}

CmdStatus sag_ed_file_save(Ed *ed, bool force)
{
    EditCtx ec;
    SagSaveErr result;
    Buffer *doc;
    u64 lines;

    if (ed == NULL || !ed->model_ready)
        return SAG_CMD_ERR_STATE;
    sag_ed_insert_barrier(ed);
    doc = sag_ed_doc(ed);
    if (doc == NULL || doc->path == NULL) {
        sag_msg(ed, SAG_MSG_ERROR,
                "no file name; use :w path");
        ed->quit_after_save = false;
        return SAG_CMD_ERR_STATE;
    }
    /*
     * Sprint 24 §3: a tab that was never read holds no text, so there
     * is nothing to write.  Saying so distinctly matters — an I/O error
     * would send the user looking at permissions on a file that is
     * perfectly fine, and writing the empty buffer would destroy it.
     */
    if (doc->tb == NULL) {
        sag_msg(ed, SAG_MSG_ERROR, "%s was never loaded; nothing written",
                doc->path);
        ed->quit_after_save = false;
        return SAG_CMD_ERR_STATE;
    }
    ec = sag_ed_edit_ctx(ed);
    if (force) {
        result = sag_file_save_force(ec.tb, ec.meta, doc->path);
        if (result == SAG_SAVE_OK) {
            sag_undo_boundary(ec.undo);
            sag_undo_mark_saved(ec.undo);
            if (ec.jrnl != NULL) {
                sag_journal_discard(ec.jrnl);
                ec.jrnl = NULL;
            }
        }
    } else {
        result = sag_edit_save(&ec, doc->path);
    }
    sag_ed_finish_edit(ed, &ec);
    if (result == SAG_SAVE_CHANGED_ON_DISK) {
        sag_ed_prompt(ed, SAG_PROMPT_OVERWRITE);
        return SAG_CMD_ERR_IO;
    }
    if (result != SAG_SAVE_OK) {
        sag_msg(ed, SAG_MSG_ERROR, "could not write %s", doc->path);
        ed->quit_after_save = false;
        return SAG_CMD_ERR_IO;
    }
    ed->durability_failed = false;
    ed->prompt = SAG_PROMPT_NONE;
    lines = sag_textbuf_line_count(doc->tb);
    sag_msg(ed, SAG_MSG_INFO, "wrote %s, %llu lines", doc->path,
            (unsigned long long)lines);
    if (ed->quit_after_save) {
        ed->quit_after_save = false;
        ed->quit = true;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_ed_file_write_to(Ed *ed, const char *path, bool force)
{
    FileMeta next;
    TextBuf *existing = NULL;
    EditCtx ec;
    SagLoadErr load;
    SagSaveErr result;
    Buffer *doc;

    if (ed == NULL || !ed->model_ready || path == NULL || path[0] == '\0')
        return SAG_CMD_ERR_ARG;
    sag_ed_insert_barrier(ed);
    doc = sag_ed_doc(ed);
    /* Nothing to write out under a new name either (§3). */
    if (doc == NULL || doc->tb == NULL) {
        sag_msg(ed, SAG_MSG_ERROR, "nothing loaded; nothing written");
        return SAG_CMD_ERR_STATE;
    }
    sag_filemeta_init(&next);
    load = sag_file_load(path, &existing, &next);
    sag_textbuf_free(existing);
    if (load != SAG_LOAD_OK && load != SAG_LOAD_ENOENT) {
        sag_filemeta_dispose(&next);
        sag_msg(ed, SAG_MSG_ERROR, "could not inspect %s", path);
        return SAG_CMD_ERR_IO;
    }
    ec = sag_ed_edit_ctx(ed);
    ec.meta = &next;
    result = force ? sag_file_save_force(ec.tb, ec.meta, path) :
                     sag_edit_save(&ec, path);
    if (force && result == SAG_SAVE_OK) {
        sag_undo_boundary(ec.undo);
        sag_undo_mark_saved(ec.undo);
        if (ec.jrnl != NULL) {
            sag_journal_discard(ec.jrnl);
            ec.jrnl = NULL;
        }
    }
    sag_ed_finish_edit(ed, &ec);
    if (result != SAG_SAVE_OK) {
        sag_filemeta_dispose(&next);
        sag_msg(ed, SAG_MSG_ERROR, "could not write %s", path);
        return SAG_CMD_ERR_IO;
    }
    sag_filemeta_dispose(&doc->meta);
    doc->meta = next;
    doc->path = arena_strdup(&ed->arena, path);
    sag_search_opts_init(&ed->search_opts);
    sag_search_state_init(&ed->search);
    sag_overlay_init(&ed->single_win.overlay);
    /*
     * A save-as RENAMES what the active tab shows.  It does not build a
     * new world.
     *
     * Sprints 22 and 23 grew a copy of ed_model_finish's tail here, so
     * `:w other` rebuilt the pane tree and reset the tab array to a
     * single entry — silently discarding every split and every other
     * tab, and leaking all of them, because sag_tabs_init only zeroes
     * the struct.  Nothing about writing bytes to a path justifies
     * touching either tree.
     */
    sag_tab_set_path(ed, ed->tabs.active, doc->path);
    sag_reg_bind_context(&ed->regs, doc->undo, &doc->meta);
    ed->durability_failed = false;
    sag_msg(ed, SAG_MSG_INFO, "wrote %s, %llu lines", path,
            (unsigned long long)sag_textbuf_line_count(doc->tb));
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
        Buffer *doc = sag_ed_doc(ed);
        bool recovered = sag_journal_replay_edit(doc->meta.realpath, &ec,
                                                 &doc->meta);

        if (recovered)
            ec.jrnl = sag_journal_open(doc->meta.realpath, &doc->meta);
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
        Buffer *doc = sag_ed_doc(ed);
        bool discarded = sag_journal_discard_path(doc->meta.realpath,
                                                  &doc->meta);

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
    ed->now_ms = now_ms;
    /*
     * Esc mid-gesture cancels it and restores the state the gesture
     * started from, before the key reaches any mode that would also act
     * on Escape.  Sprint 27 widened this from the border drag alone to
     * every gesture the router owns.
     */
    if (key.code == SAG_KEY_ESCAPE && sag_mouse_gesture_active(ed)) {
        sag_mouse_cancel(ed);
        return;
    }
    /*
     * Sprint 27 §5: the context menu is a keymap layer, and it is the
     * TOPMOST one — a menu that let a key through would act on the
     * document behind an open pop-up, the same law s24's and s26's
     * dialogs obey.
     */
    if (sag_mouse_menu_key(ed, &key))
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
    /*
     * Sprint 24 §4: the picker is MODAL, so it takes the key before the
     * command line and before any mode.  It swallows everything it does
     * not use — a dialog that let unhandled keys through would edit the
     * document behind it.
     */
    if (sag_gp_key(ed, key)) {
        sag_gp_apply(ed);
        return;
    }
    /*
     * Sprint 26 §5: the list picker is modal for the same reason the
     * group picker is — it swallows what it does not use, so no key
     * reaches the document behind it.
     */
    if (sag_picker_active(ed) && sag_picker_key(ed, &key))
        return;
    if (ed->cmdline.active && sag_cmdline_key(ed, &key))
        return;
    /*
     * Sprint 24 §7.  BEFORE the insert-mode text path: a digit arriving
     * inside the jump window is the second half of a chord, and letting
     * it fall through would type it into the document while the user
     * was navigating.
     */
    if (sag_tab_jump_key(ed, key))
        return;
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

/*
 * The mouse routing spine used to live here.  Sprint 18.5 joined the
 * decoder to the region registry for the completion menu alone; Sprint
 * 27 moved the whole join to ui/mouse.c, so that file is the only place
 * a pointer event becomes an action (its DoD 2).  Nothing in this file
 * routes one, and the event loop's only mouse line hands it straight to
 * sag_mouse_event.
 */
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
    if (ed->cmdline.active && ed->paste.len != 0U) {
        sag_cmdline_paste(ed, ed->paste.data, ed->paste.len);
    } else if (ed->mode == SAG_MODE_I && ed->prompt == SAG_PROMPT_NONE &&
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
    LineNo cursor_line;

    if (ed == NULL || !ed->grid_ready || !ed->render_ready ||
        !ed->model_ready)
        return;
    win = ed->win;
    if (ed->cursor_follow_pending) {
        sag_win_follow_cursor(win);
        ed->cursor_follow_pending = false;
    }
    cursor_line = sag_textbuf_line_of(win->buf->tb,
                                      sag_ed_cursor(ed)->pos);
    if (!ed->drawn_top_valid ||
        ed->drawn_top.v != sag_win_view_top(win).v ||
        ed->drawn_top_sub != win->vp.top_sub ||
        ed->drawn_left.v != win->vp.left.v ||
        ed->drawn_wrap != win->vp.wrap ||
        !ed->drawn_cursor_line_valid ||
        ((win->number_style == SAG_NUM_REL ||
          win->number_style == SAG_NUM_HYBRID) &&
         ed->drawn_cursor_line.v != cursor_line.v))
        sag_ed_damage_document(ed);
    if (ed->full_damage) {
        /*
         * Every leaf plus the borders their splits own.  With a single
         * pane this draws exactly the rows the one-window path drew,
         * which is what keeps the Sprint 6-21 goldens byte-identical.
         */
        sag_draw_panes(ed);
        sag_grid_mark_all(&ed->grid);
    } else if (ed->doc_damage_lo < ed->doc_damage_hi) {
        if (sag_pane_leaf_count(ed->pane_root) > 1U) {
            /* Partial damage is expressed in the focused pane's rows,
             * and a border or a neighbour may share them; redrawing the
             * tree is correct and still bounded by the screen. */
            sag_draw_panes(ed);
        } else {
            sag_draw_document_rows(ed, win, ed->doc_damage_lo,
                                   ed->doc_damage_hi);
        }
    }
    if (ed->full_damage || ed->footer_dirty)
        sag_draw_footer(ed, win);
    /*
     * The picker draws LAST, after the footer.
     *
     * It is modal and owns its rectangle, and its filter line IS the
     * command line — so drawing it inside sag_draw_panes meant the
     * footer then painted the same widget at the bottom of the screen
     * and left the picker's copy blank.  One widget, one place, and the
     * modal thing on top.
     */
    if (sag_picker_active(ed))
        sag_picker_draw(ed, (Rect){0U, 0U, ed->grid.cols, ed->grid.rows});
    /*
     * Sprint 27 §5: the context menu is drawn after everything, for the
     * same reason and with the same consequence — its BLOCK and CTX_ROW
     * regions are added last, and last-added-wins is what makes it
     * shadow whatever is beneath with no z-order machinery.
     */
    if (sag_ctx_active())
        sag_mouse_menu_draw(ed);
    if (!ed->cmdline.active)
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
    ed->drawn_cursor_line = cursor_line;
    ed->drawn_cursor_line_valid = true;
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
    /*
     * State comes up AFTER the buffers so a restore has somewhere to
     * land, and the lock is claimed before the first change can mark
     * anything dirty.
     */
    sag_state_open(&ed);
    /*
     * Restored only when no file was named.  `sag sagitta.c` is a
     * request to edit that file, and burying it under forty restored
     * tabs answers a question nobody asked; the arrangement comes back
     * when you start the way you left — in the directory, with no
     * argument.
     */
    if (path == NULL)
        (void)sag_ws_restore(&ed);
    result = ed.quit ? ed.exit_code : sag_loop_run(&ed);
    /* Clean quit: the unconditional save, before anything is freed. */
    sag_state_close(&ed);
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
