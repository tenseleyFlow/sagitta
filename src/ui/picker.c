/*
 * Sprint 26 §5.  See picker.h for the three laws this exists to obey.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "ui/cmdline.h"
#include "ui/filter.h"
#include "ui/message.h"
#include "ui/region.h"
#include "unicode/width.h"
#include "util/buf.h"
#include "util/log.h"

/*
 * ONE picker at a time, module-local.
 *
 * Not a field on Ed: nothing outside this file has any business
 * knowing a list is half-filtered, and the group picker (s24) keeps its
 * state the same way for the same reason.
 */
typedef struct PickerState {
    PickerSpec spec;
    bool active;

    /*
     * §7: the incremental filter owns the candidate set; `ranked` is
     * only the VISIBLE window it hands back, recomputed per draw.
     * Ranking 100 000 candidates into a sorted array per keystroke is
     * the thing §7 exists not to do.
     */
    FilterState filter;
    FzRanked ranked[SAG_FILTER_TOPK];
    u32 n_ranked;
    u32 total;
    /* A sliced rescan is still running; the footer says ` scanning…`
     * and the idle timer keeps calling sag_picker_tick. */
    bool scanning;

    /*
     * THE SELECTION, held as an item PAYLOAD.
     *
     * `sel_row` is derived from it on every draw, never stored as the
     * truth — see picker.h law 1.  `has_sel` distinguishes "payload 0
     * is selected" from "nothing is".
     */
    i32 sel_payload;
    bool has_sel;
    u32 scroll;
    /*
     * The picker OWNS the command line.
     *
     * Until it does, the line still belongs to the `:buffers` prompt
     * that invoked us — and reading that as the filter pattern made a
     * freshly opened picker filter its own candidates against the word
     * "buffers" and show none of them.  The footer said `0/3` and the
     * list was empty; only typing a character (which opens our line and
     * replaces the text) made it recover.
     */
    bool filter_open;

    Rect box;
    /* The filter line's text, re-read on every keystroke. */
    Bytebuf text;
    FilterState filter_state;
} PickerState;

static PickerState pk;

bool sag_picker_active(const Ed *ed)
{
    (void)ed;
    return pk.active;
}

u32 sag_picker_shown(const Ed *ed)
{
    (void)ed;
    /* MATCHES, not drawn rows: the footer says "3/1043" about the
     * filtered set, and only 20 of those are ever on screen. */
    return pk.active ? sag_filter_matched(&pk.filter_state) : 0U;
}

u32 sag_picker_total(const Ed *ed)
{
    (void)ed;
    return pk.active ? pk.total : 0U;
}

/* ---------------------------------------------------------------- */
/* Selection, by payload                                            */
/* ---------------------------------------------------------------- */

/* Where the held payload currently sits; 0 when it is gone. */
static u32 sel_row(void)
{
    u32 i;

    if (!pk.has_sel)
        return 0U;
    for (i = 0U; i < pk.n_ranked; i++) {
        const PickItem *items;
        u32 n = 0U;

        items = pk.spec.items(pk.spec.ctx, &n);
        if (pk.ranked[i].idx < n &&
            items[pk.ranked[i].idx].payload == pk.sel_payload)
            return i;
    }
    /*
     * The held item left the filtered set.  Falling to row 0 is
     * deliberate and VISIBLE: the cursor jumps to the top where the
     * user can see it, rather than staying on a row that now means
     * something else.
     */
    return 0U;
}

static void sel_set_row(u32 row)
{
    const PickItem *items;
    u32 n = 0U;

    if (pk.n_ranked == 0U) {
        pk.has_sel = false;
        return;
    }
    if (row >= pk.n_ranked)
        row = pk.n_ranked - 1U;
    items = pk.spec.items(pk.spec.ctx, &n);
    if (pk.ranked[row].idx >= n)
        return;
    pk.sel_payload = items[pk.ranked[row].idx].payload;
    pk.has_sel = true;
}

i32 sag_picker_selected(const Ed *ed)
{
    const PickItem *items;
    u32 n = 0U;
    u32 row;

    (void)ed;
    if (!pk.active || pk.n_ranked == 0U || !pk.has_sel)
        return 0;
    /*
     * What is UNDER THE CURSOR, not the payload we are holding.
     *
     * The held payload is an intent: it says "follow this item".  When
     * the item is filtered out, sel_row() falls to 0 and the cursor is
     * visibly at the top — but reporting the held payload anyway would
     * mean Enter opened a file that is not in the list, which is a
     * worse version of the bug the payload discipline exists to
     * prevent.
     */
    row = sel_row();
    items = pk.spec.items(pk.spec.ctx, &n);
    if (row >= pk.n_ranked || pk.ranked[row].idx >= n)
        return 0;
    return items[pk.ranked[row].idx].payload;
}

/* ---------------------------------------------------------------- */
/* Filtering                                                        */
/* ---------------------------------------------------------------- */

/*
 * Opens the filter line if it is not already up.  See sag_picker_open
 * for why this cannot happen at open time.
 */
static void ensure_filter(Ed *ed)
{
    if (!pk.active || ed == NULL || ed->cmdline.active)
        return;
    sag_cmdline_open(ed, SAG_PROMPT_INPUT, "");
    pk.filter_open = true;
}

/* Re-reads the visible window from the filter's candidate set. */
static void refresh_window(void)
{
    u32 n = 0U;
    const PickItem *items = pk.spec.items(pk.spec.ctx, &n);

    pk.n_ranked = sag_filter_top(&pk.filter_state, items,
                                 pk.spec.path_mode, pk.ranked,
                                 (u32)SAG_FILTER_TOPK);
}

void sag_picker_refilter(Ed *ed)
{
    const PickItem *items;
    u32 n = 0U;
    bool complete;

    if (!pk.active || ed == NULL)
        return;
    items = pk.spec.items(pk.spec.ctx, &n);
    pk.total = n;
    if (n == 0U) {
        pk.n_ranked = 0U;
        pk.has_sel = false;
        pk.scanning = false;
        return;
    }
    pk.text.len = 0U;
    /* Only OUR line is the pattern.  See PickerState.filter_open. */
    if (pk.filter_open)
        sag_cmdline_text(ed, &pk.text);
    /*
     * An empty Bytebuf has a NULL data pointer, and the scorer reads a
     * NULL pattern as NO MATCH rather than as the empty pattern — so
     * handing it through directly made a freshly opened picker show
     * zero of its candidates.  "" is the empty pattern; NULL is the
     * absence of one, and they are not the same question.
     */
    complete = sag_filter_apply(&pk.filter_state, items, n,
                                pk.spec.path_mode,
                                pk.text.data == NULL
                                    ? ""
                                    : (const char *)pk.text.data,
                                (u32)pk.text.len,
                                SAG_PICKER_SLICE_US);
    pk.scanning = !complete;
    refresh_window();

    /*
     * The selection is NOT recomputed here.  It is a payload, and it
     * stays whatever it was; sel_row() finds it again wherever the new
     * ordering put it.  Recomputing from a row index here is precisely
     * law 1's bug.
     */
    if (!pk.has_sel)
        sel_set_row(0U);
    pk.scroll = 0U;
}

/*
 * §7.2: continues a sliced rescan.  Called from the idle timer, so a
 * backspace over 100 000 candidates costs 2 ms this frame and the rest
 * later — a partial list for one frame is invisible, a stalled
 * keystroke is not (s21's overlay doctrine).
 */
bool sag_picker_tick(Ed *ed)
{
    const PickItem *items;
    u32 n = 0U;

    if (!pk.active || !pk.scanning || ed == NULL)
        return false;
    items = pk.spec.items(pk.spec.ctx, &n);
    pk.scanning = sag_filter_step(&pk.filter_state, items,
                                  pk.spec.path_mode, SAG_PICKER_SLICE_US);
    refresh_window();
    if (!pk.has_sel)
        sel_set_row(0U);
    ed->full_damage = true;
    return pk.scanning;
}

bool sag_picker_scanning(const Ed *ed)
{
    (void)ed;
    return pk.active && pk.scanning;
}

/* ---------------------------------------------------------------- */
/* Opening and closing                                              */
/* ---------------------------------------------------------------- */

void sag_picker_open(Ed *ed, const PickerSpec *s)
{
    if (ed == NULL || s == NULL || s->items == NULL)
        return;
    if (pk.active)
        sag_picker_close(ed, false);
    /*
     * REFUSES rather than drawing a box too small to show a list.  A
     * six-cell dialog is not a degraded experience, it is a broken one.
     */
    if (ed->grid.cols < (u16)SAG_PICKER_MIN_COLS ||
        ed->grid.rows < (u16)SAG_PICKER_MIN_ROWS) {
        sag_msg(ed, SAG_MSG_ERROR, "terminal too small for %s",
                s->title == NULL ? "the picker" : s->title);
        return;
    }
    (void)memset(&pk, 0, sizeof(pk));
    pk.spec = *s;
    pk.active = true;
    bytebuf_init(&pk.text);
    sag_filter_init(&pk.filter_state);
    /*
     * Law 2: the filter line IS the s18 widget — but it is opened
     * LAZILY, not here.
     *
     * A picker is normally launched from the command line (`:find`),
     * and that prompt is still open while the command runs.  Opening
     * ours inside the command meant the invoking prompt's close, which
     * happens the moment the command returns, closed the picker's
     * filter line too — leaving a picker with a blank input row and no
     * way to type into it.  ensure_filter opens it on the first key or
     * draw, by which time the caller's prompt is gone.
     */
    sag_picker_refilter(ed);
    ed->full_damage = true;
    ed->layout_dirty = true;
}

void sag_picker_close(Ed *ed, bool accepted)
{
    if (!pk.active)
        return;
    pk.active = false;
    if (ed != NULL) {
        if (ed->cmdline.active)
            sag_cmdline_close(ed, accepted);
        ed->full_damage = true;
        ed->layout_dirty = true;
    }
    pk.n_ranked = 0U;
    pk.scanning = false;
    pk.filter_open = false;
    sag_filter_free(&pk.filter_state);
    bytebuf_free(&pk.text);
}

/* ---------------------------------------------------------------- */
/* Keys                                                             */
/* ---------------------------------------------------------------- */

static u16 list_rows(void)
{
    /* Box minus: top border, filter line, separator, footer. */
    return pk.box.h > 4U ? (u16)(pk.box.h - 4U) : 0U;
}

static void move_by(i32 delta)
{
    u32 row = sel_row();
    i64 want = (i64)row + delta;

    if (pk.n_ranked == 0U)
        return;
    if (want < 0)
        want = 0;
    if (want >= (i64)pk.n_ranked)
        want = (i64)pk.n_ranked - 1;
    sel_set_row((u32)want);
}

static bool accept_selected(Ed *ed, u8 how)
{
    i32 payload = sag_picker_selected(ed);

    /*
     * Returns TRUE either way: an empty list has nothing to accept, but
     * the key is still ours and must not fall through to the document
     * (law 3).
     */
    if (pk.n_ranked == 0U || !pk.has_sel)
        return true;
    if (pk.spec.accept == NULL) {
        sag_picker_close(ed, true);
        return true;
    }
    /*
     * The split happens BEFORE accept, so the callback opens into the
     * window the user asked for and does not have to know how it got
     * there.
     */
    if (how == (u8)SAG_PICK_ACCEPT_VSPLIT ||
        how == (u8)SAG_PICK_ACCEPT_HSPLIT) {
        Pane *nu = sag_pane_split(ed, ed->focus,
                                  how == (u8)SAG_PICK_ACCEPT_VSPLIT
                                      ? SAG_SPLIT_V
                                      : SAG_SPLIT_H);

        if (nu == NULL) {
            sag_msg(ed, SAG_MSG_ERROR, "no room to split");
            return true;
        }
        ed->focus = nu;
        if (nu->win != NULL)
            ed->win = nu->win;
    }
    {
        PickerSpec spec = pk.spec;

        /* Closed FIRST: accept may open a file, split, or push another
         * picker, and none of that should happen with this one still
         * holding the keymap layer. */
        sag_picker_close(ed, true);
        (void)spec.accept(ed, spec.ctx, payload, how);
    }
    return true;
}

void sag_picker_select_payload(Ed *ed, i32 payload)
{
    if (!pk.active || ed == NULL)
        return;
    /*
     * Stored as the held payload rather than as a row, so a rescan
     * landing between this press and its release cannot slide the
     * selection onto a neighbour.  sel_row() derives the row again on
     * every draw.
     */
    pk.sel_payload = payload;
    pk.has_sel = true;
    ed->full_damage = true;
}

bool sag_picker_accept(Ed *ed)
{
    if (!pk.active || ed == NULL)
        return false;
    return accept_selected(ed, (u8)SAG_PICK_ACCEPT_HERE);
}

void sag_picker_scroll(Ed *ed, i32 rows)
{
    if (!pk.active || ed == NULL || rows == 0)
        return;
    move_by(rows);
    ed->full_damage = true;
}

bool sag_picker_key(Ed *ed, const Key *k)
{
    u16 page;

    if (!pk.active || ed == NULL || k == NULL)
        return false;
    ensure_filter(ed);
    page = list_rows();
    if (page == 0U)
        page = 1U;
    switch (k->code) {
    case SAG_KEY_ESCAPE:
        sag_picker_close(ed, false);
        return true;
    case SAG_KEY_ENTER:
        return accept_selected(ed, (u8)SAG_PICK_ACCEPT_HERE);
    case SAG_KEY_UP:
        move_by(-1);
        ed->full_damage = true;
        return true;
    case SAG_KEY_DOWN:
        move_by(1);
        ed->full_damage = true;
        return true;
    case SAG_KEY_PAGE_UP:
        move_by(-(i32)page);
        ed->full_damage = true;
        return true;
    case SAG_KEY_PAGE_DOWN:
        move_by((i32)page);
        ed->full_damage = true;
        return true;
    case SAG_KEY_HOME:
        sel_set_row(0U);
        ed->full_damage = true;
        return true;
    case SAG_KEY_END:
        sel_set_row(pk.n_ranked == 0U ? 0U : pk.n_ranked - 1U);
        ed->full_damage = true;
        return true;
    default:
        break;
    }
    /* Ctrl chords, for terminals that eat arrows and for hands that
     * would rather not leave the home row (invariant 9). */
    if ((k->mods & SAG_MOD_CTRL) != 0U && k->ntext == 0U) {
        switch (k->code) {
        case (u32)'n':
        case (u32)'N':
            move_by(1);
            ed->full_damage = true;
            return true;
        case (u32)'p':
        case (u32)'P':
            move_by(-1);
            ed->full_damage = true;
            return true;
        case (u32)'v':
        case (u32)'V':
            return accept_selected(ed, (u8)SAG_PICK_ACCEPT_VSPLIT);
        case (u32)'s':
        case (u32)'S':
            return accept_selected(ed, (u8)SAG_PICK_ACCEPT_HSPLIT);
        default:
            break;
        }
    }
    /*
     * Everything else goes to the filter line — and whatever it does
     * not use is SWALLOWED here rather than falling through to the
     * document (law 3).
     */
    if (ed->cmdline.active) {
        Bytebuf before;
        bool changed;

        /*
         * Compared against a COPY of the old text, not against a
         * length.  `ab` -> `ba` is the same length and a completely
         * different filter, and re-ranking only on a length change
         * would leave the list showing the previous query's results.
         */
        bytebuf_init(&before);
        bytebuf_append(&before, pk.text.data, (size_t)pk.text.len);
        (void)sag_cmdline_key(ed, k);
        pk.text.len = 0U;
        sag_cmdline_text(ed, &pk.text);
        changed = before.len != pk.text.len ||
                  (pk.text.len != 0U &&
                   memcmp(before.data, pk.text.data,
                          (size_t)pk.text.len) != 0);
        bytebuf_free(&before);
        if (changed) {
            sag_picker_refilter(ed);
            ed->full_damage = true;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Drawing                                                          */
/* ---------------------------------------------------------------- */

static void draw_row(Ed *ed, u16 y, u16 x0, u16 w, const PickItem *it,
                     const FzMatch *m, bool selected)
{
    SagColor fg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor bg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor accent = {SAG_COLOR_RGB, 120U, 180U, 255U};
    SagColor dim = {SAG_COLOR_RGB, 120U, 120U, 120U};
    u16 label_w = w > 2U ? (u16)(w - 2U) : 0U;
    size_t fit;
    int cells = 0;
    u16 at;
    u32 j;

    if (label_w == 0U)
        return;
    /*
     * The detail column is dropped under 40 cells: two columns in a
     * narrow box means neither is readable.
     */
    if (w >= (u16)SAG_PICKER_DETAIL_MIN_W && it->detail != NULL) {
        size_t dlen = strlen(it->detail);
        int dcells = 0;
        size_t dfit = sag_str_clip((const u8 *)it->detail, dlen,
                                   (int)(w / 3U), &dcells);

        if (dcells > 0 && (u16)dcells + 4U < w) {
            u16 dx = (u16)(x0 + w - 1U - (u16)dcells);

            (void)sag_grid_puts(&ed->grid, y, dx, (const u8 *)it->detail,
                                dfit, dim, bg, SAG_ATTR_DIM);
            label_w = (u16)(dx - x0 - 2U);
        }
    }
    /* Clipped through the width tables, never by byte count — a label
     * ending in a multibyte name must not be cut mid-sequence. */
    fit = sag_str_clip((const u8 *)it->label, strlen(it->label),
                       (int)label_w, &cells);
    at = (u16)(x0 + 1U);
    (void)sag_grid_puts(&ed->grid, y, at, (const u8 *)it->label, fit, fg,
                        bg, selected ? SAG_ATTR_REVERSE : 0U);
    /*
     * Matched bytes in the accent style, drawn OVER the label.  The
     * positions are byte offsets from the scorer, so each is re-clipped
     * to a cell column rather than assumed to be one cell wide.
     */
    if (!selected && m != NULL) {
        for (j = 0U; j < m->n_pos; j++) {
            u16 pos = m->pos[j];
            int pre_cells = 0;

            if ((size_t)pos >= fit)
                break;
            (void)sag_str_clip((const u8 *)it->label, (size_t)pos, 1000,
                               &pre_cells);
            if ((u16)pre_cells < label_w) {
                (void)sag_grid_puts(&ed->grid, y,
                                    (u16)(at + (u16)pre_cells),
                                    (const u8 *)it->label + pos, 1U,
                                    accent, bg, SAG_ATTR_BOLD);
            }
        }
    }
    if ((it->flags & (u8)SAG_PICK_MODIFIED) != 0U && w > 2U) {
        (void)sag_grid_puts(&ed->grid, y, (u16)(x0 + w - 1U),
                            (const u8 *)"*", 1U, accent, bg, 0U);
    }
}

void sag_picker_draw(Ed *ed, Rect area)
{
    SagColor fg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor bg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor dim = {SAG_COLOR_RGB, 120U, 120U, 120U};
    const PickItem *items;
    u32 n = 0U;
    u16 w;
    u16 h;
    u16 x0;
    u16 y0;
    u16 rows;
    u16 i;
    u32 cur;
    u16 row_width;
    char line[SAG_PICKER_MAX_W + 64];

    if (!pk.active || ed == NULL || area.w == 0U || area.h == 0U)
        return;
    ensure_filter(ed);
    w = area.w < (u16)SAG_PICKER_MAX_W ? area.w : (u16)SAG_PICKER_MAX_W;
    if (w > area.w - 2U)
        w = (u16)(area.w > 2U ? area.w - 2U : area.w);
    h = area.h < (u16)SAG_PICKER_MAX_H ? area.h : (u16)SAG_PICKER_MAX_H;
    if (h > area.h - 2U)
        h = (u16)(area.h > 2U ? area.h - 2U : area.h);
    x0 = (u16)(area.x + (area.w - w) / 2U);
    y0 = (u16)(area.y + (area.h - h) / 2U);
    pk.box = (Rect){x0, y0, w, h};
    rows = list_rows();

    {
        Cell blank;

        (void)memset(&blank, 0, sizeof(blank));
        for (i = 0U; i < h; i++)
            sag_grid_fill(&ed->grid, (u16)(y0 + i), x0, (u16)(x0 + w),
                          blank);
    }
    /* The dialog owns its rectangle, so a click on it never falls
     * through to the pane underneath. */
    sag_region_add(SAG_REGION_BLOCK, pk.box, 0);

    (void)snprintf(line, sizeof(line), " %s ",
                   pk.spec.title == NULL ? "Pick" : pk.spec.title);
    (void)sag_grid_puts(&ed->grid, y0, x0, (const u8 *)line, strlen(line),
                        fg, bg, SAG_ATTR_BOLD);

    /* The filter line draws itself — it is the s18 widget. */
    if (ed->cmdline.active)
        sag_cmdline_draw(ed, (Rect){x0, (u16)(y0 + 1U), w, 1U});

    items = pk.spec.items(pk.spec.ctx, &n);
    cur = sel_row();
    /* Scroll to keep the selection visible, computed here rather than
     * stored: the list reorders under it on every keystroke. */
    if (rows > 0U) {
        if (cur < pk.scroll)
            pk.scroll = cur;
        else if (cur >= pk.scroll + rows)
            pk.scroll = (u32)(cur - rows + 1U);
    }
    /* Rows stop at the preview's edge when there is one. */
    {
        u16 list_w = w;

        if (pk.spec.preview != NULL && w >= (u16)SAG_PICKER_PREVIEW_MIN_W)
            list_w = (u16)(w / 2U);
        row_width = list_w;
    }
    for (i = 0U; i < rows; i++) {
        u32 idx = pk.scroll + i;
        u16 y = (u16)(y0 + 3U + i);

        if (idx >= pk.n_ranked)
            break;
        if (pk.ranked[idx].idx >= n)
            continue;
        draw_row(ed, y, x0, row_width, &items[pk.ranked[idx].idx],
                 &pk.ranked[idx].m, idx == cur);
        /*
         * The region payload is the item's PAYLOAD, not the row — the
         * same law the selection follows, so Sprint 27's click lands on
         * the file the user pointed at even if the list reorders
         * between the paint and the press.
         */
        sag_region_add(SAG_REGION_PICK_ROW,
                       (Rect){x0, y, row_width, 1U},
                       items[pk.ranked[idx].idx].payload);
    }

    /*
     * `shown/total`, plus ` scanning…` while a sliced rescan is in
     * flight (§7.2) — the count is meaningful mid-scan, so the footer
     * stays honest rather than showing a number that will change.
     */
    (void)snprintf(line, sizeof(line),
                   " %u/%u%s   up/down move . enter open . ^v split . esc",
                   (unsigned)sag_filter_matched(&pk.filter_state),
                   (unsigned)pk.total,
                   pk.scanning ? " scanning..." : "");
    (void)sag_grid_puts(&ed->grid, (u16)(y0 + h - 1U), x0,
                        (const u8 *)line, strlen(line), dim, bg,
                        SAG_ATTR_DIM);

    /*
     * The preview is a SLOT BESIDE the list, never over it.
     *
     * Handing it the list's own rectangle painted the selected file's
     * contents on top of the rows — the list was still there
     * underneath, drawn first and immediately covered, which looked
     * exactly like the finder listing the wrong things.
     *
     * It only appears when the box is wide enough to give both halves
     * something readable; below that there is no honest way to show a
     * preview and a list at once, so the list wins.
     */
    if (pk.spec.preview != NULL && pk.has_sel && pk.n_ranked > 0U &&
        w >= (u16)SAG_PICKER_PREVIEW_MIN_W) {
        u16 split = (u16)(w / 2U);

        pk.spec.preview(ed, pk.spec.ctx, sag_picker_selected(ed),
                        (Rect){(u16)(x0 + split + 1U), (u16)(y0 + 3U),
                               (u16)(w - split - 2U), rows});
    }
}
