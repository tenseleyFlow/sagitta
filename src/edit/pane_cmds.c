/*
 * Sprint 22 §4/§5: the pane commands.
 *
 * Every one of these dispatches through the Sprint 13 registry; the
 * keymap binds NAMES, and no key calls a function here directly (DoD 8).
 */
#include "edit/pane_cmds.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "mod/git/fussmode.h"
#include "ui/message.h"
#include "ui/win.h"
#include "util/log.h"

/*
 * Focus is a LEAF pointer, so anything that reshapes the tree has to
 * leave it pointing at a leaf that still exists.  Rather than trusting
 * each mutation to fix it up, every one of them ends here.
 */
static void pane_refocus(Ed *ed, Pane *want)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    Pane *target;
    Win *before;
    bool before_live = false;
    u32 n = 0U;
    u32 i;

    if (ed == NULL || ed->pane_root == NULL)
        return;
    before = ed->win;
    target = want != NULL && want->is_leaf ? want : ed->focus;
    if (target == NULL || !target->is_leaf)
        target = yew_pane_first_leaf(ed->pane_root);
    yew_pane_collect_leaves(ed->pane_root, leaves,
                            YEW_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++)
        if (leaves[i]->win == before)
            before_live = true;
    if (before_live && target != NULL && target->win != before)
        yew_shadow_dismiss(ed, before);
    ed->focus = target;
    if (ed->focus != NULL && ed->focus->win != NULL)
        ed->win = ed->focus->win;
    /*
     * The TAB's copy of focus moves too.
     *
     * Nothing else kept it in sync, and yew_pane_split mutates the
     * focused leaf IN PLACE into the split node — so after one split
     * the tab's `focus` pointed at a split node, whose `win` is NULL.
     * yew_tab_switch assigns ed->focus = t->focus, so switching away
     * and back landed focus on a non-leaf with a stale ed->win, and
     * Sprint 25 serialized that as "pane 0 is focused" no matter which
     * one actually was.  The resume gate is what surfaced it.
     */
    {
        Tab *t = yew_tab_at(ed, ed->tabs.active);

        if (t != NULL && ed->focus != NULL && ed->focus->is_leaf)
            t->focus = ed->focus;
    }
    ed->layout_dirty = true;
    ed->full_damage = true;
    /* §5: split, close and plain focus movement all funnel through
     * here, so this one call covers the whole "active window changed"
     * trigger without three separate ones to keep in sync. */
    yew_state_mark_dirty(ed);
    if (ed->win != before)
        yew_fl_hook_window(ed, FL_EV_WIN_FOCUS, ed->win);
}

void yew_pane_refocus(Ed *ed, Pane *want)
{
    pane_refocus(ed, want);
}

static CmdStatus pane_split(CmdCtx *cx, SplitDir dir)
{
    Pane *nu;

    if (cx == NULL || cx->ed == NULL || cx->ed->focus == NULL)
        return YEW_CMD_ERR_STATE;
    nu = yew_pane_split(cx->ed, cx->ed->focus, dir);
    if (nu == NULL) {
        /*
         * A refusal, not a failure to clamp.  The message says which of
         * the two reasons it was, because "no room" and "too many
         * panes" call for different responses from the user.
         */
        if (yew_pane_leaf_count(cx->ed->pane_root) >=
            (u32)YEW_PANE_MAX_LEAVES)
            yew_msg(cx->ed, YEW_MSG_ERROR, "too many panes (max %d)",
                    YEW_PANE_MAX_LEAVES);
        else
            yew_msg(cx->ed, YEW_MSG_ERROR, "no room to split");
        return YEW_CMD_ERR_STATE;
    }
    /* The new pane takes focus, which is what makes a split feel like
     * "open a view here" rather than "rearrange the screen". */
    pane_refocus(cx->ed, nu);
    return YEW_CMD_OK;
}

CmdStatus yew_pane_cmd_split_h(CmdCtx *cx)
{
    return pane_split(cx, YEW_SPLIT_H);
}

CmdStatus yew_pane_cmd_split_v(CmdCtx *cx)
{
    return pane_split(cx, YEW_SPLIT_V);
}

CmdStatus yew_pane_cmd_close(CmdCtx *cx)
{
    Ed *ed;
    Tab *tab = NULL;
    Pane *leaf = NULL;
    Pane *parent;
    Pane *sibling;
    size_t i;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    if (cx->win == NULL)
        cx->win = ed->win;
    for (i = 0U; i < ed->tabs.v.len && leaf == NULL; i++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 k;

        yew_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (k = 0U; k < n; k++) {
            if (leaves[k]->win == cx->win) {
                leaf = leaves[k];
                tab = &ed->tabs.v.data[i];
                break;
            }
        }
    }
    if (leaf == NULL && ed->focus != NULL && ed->focus->win == cx->win)
        leaf = ed->focus;
    if (leaf == NULL)
        return YEW_CMD_ERR_STATE;
    parent = leaf->parent;
    if (parent == NULL) {
        yew_msg(ed, YEW_MSG_ERROR,
                "cannot close the only pane; close its tab instead");
        return YEW_CMD_ERR_STATE;
    }
    sibling = parent->a == leaf ? parent->b : parent->a;
    /*
     * Close collapses the sibling INTO the parent node, so `parent` is
     * the surviving node and `sibling` is freed.  Focus therefore moves
     * to the parent's first leaf in tree order, not to the sibling
     * pointer, which is about to be gone.
     */
    (void)sibling;
    if (!yew_pane_close(ed, leaf))
        return YEW_CMD_ERR_STATE;
    if (tab != NULL && (tab->focus == leaf || tab->focus == sibling))
        tab->focus = yew_pane_first_leaf(parent);
    if (tab == NULL || tab == yew_tab_at(ed, ed->tabs.active)) {
        ed->focus = tab == NULL ? NULL : tab->focus;
        pane_refocus(ed, ed->focus != NULL ? ed->focus :
                                               yew_pane_first_leaf(parent));
    } else {
        ed->layout_dirty = true;
        ed->full_damage = true;
        yew_state_mark_dirty(ed);
    }
    /* Window release can defer FUSS commit cancellation until no tree or
     * focus pointer refers to the released editor.  Pane close reaches that
     * safe point here, after collapse and refocus, just as tab close does
     * after compaction and switch. */
    yew_fuss_windows_changed(ed);
    return YEW_CMD_OK;
}

static CmdStatus pane_focus(CmdCtx *cx, YewDir dir)
{
    Pane *to;

    if (cx == NULL || cx->ed == NULL || cx->ed->focus == NULL)
        return YEW_CMD_ERR_STATE;
    to = yew_pane_dir(cx->ed->pane_root, cx->ed->focus, dir);
    if (to == NULL)
        return YEW_CMD_OK; /* no-op at the edge; it never wraps */
    pane_refocus(cx->ed, to);
    return YEW_CMD_OK;
}

CmdStatus yew_pane_cmd_focus_left(CmdCtx *cx)
{
    return pane_focus(cx, YEW_DIR_LEFT);
}

CmdStatus yew_pane_cmd_focus_right(CmdCtx *cx)
{
    return pane_focus(cx, YEW_DIR_RIGHT);
}

CmdStatus yew_pane_cmd_focus_up(CmdCtx *cx)
{
    return pane_focus(cx, YEW_DIR_UP);
}

CmdStatus yew_pane_cmd_focus_down(CmdCtx *cx)
{
    return pane_focus(cx, YEW_DIR_DOWN);
}

CmdStatus yew_pane_cmd_focus_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    pane_refocus(cx->ed, yew_pane_next(cx->ed->pane_root, cx->ed->focus));
    return YEW_CMD_OK;
}

CmdStatus yew_pane_cmd_focus_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    pane_refocus(cx->ed, yew_pane_prev(cx->ed->pane_root, cx->ed->focus));
    return YEW_CMD_OK;
}

/*
 * Grow and shrink move the nearest ancestor split on whichever axis is
 * closest, so the keys mean "make this pane bigger" without the user
 * having to know which way the tree happens to branch here.
 */
static CmdStatus pane_resize(CmdCtx *cx, i32 sign)
{
    Ed *ed;
    Pane *split;
    i32 cells;
    bool first_child;

    if (cx == NULL || cx->ed == NULL || cx->ed->focus == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    split = yew_pane_ancestor_split(ed->focus, YEW_SPLIT_H);
    if (split == NULL)
        split = yew_pane_ancestor_split(ed->focus, YEW_SPLIT_V);
    if (split == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "only one pane");
        return YEW_CMD_ERR_STATE;
    }
    /* Two cells' worth, per §5, times any count the user gave. */
    cells = 2 * sign;
    if (cx->count_given && cx->count != 0U)
        cells = (i32)cx->count * sign;
    /*
     * Which side the focused leaf is on decides the sign: growing the
     * SECOND child means moving the boundary toward the first.
     */
    {
        const Pane *at = ed->focus;

        while (at->parent != NULL && at->parent != split)
            at = at->parent;
        first_child = at->parent == split && split->a == at;
    }
    if (!first_child)
        cells = -cells;
    if (!yew_pane_resize(split, cells)) {
        yew_msg(ed, YEW_MSG_ERROR, "no room to resize");
        return YEW_CMD_ERR_STATE;
    }
    ed->layout_dirty = true;
    ed->full_damage = true;
    yew_state_mark_dirty(ed);
    return YEW_CMD_OK;
}

CmdStatus yew_pane_cmd_grow(CmdCtx *cx)
{
    return pane_resize(cx, 1);
}

CmdStatus yew_pane_cmd_shrink(CmdCtx *cx)
{
    return pane_resize(cx, -1);
}

/* ---------------------------------------------------------------- */
/* Border drag (Sprint 22 §5)                                       */
/* ---------------------------------------------------------------- */

void yew_pane_drag_begin(Ed *ed, Pane *split, u16 x, u16 y)
{
    if (ed == NULL || split == NULL || split->is_leaf)
        return;
    ed->drag.split = split;
    ed->drag.entry_ratio = split->ratio;
    ed->drag.origin = split->dir == YEW_SPLIT_H ? x : y;
    ed->drag.active = true;
}

/*
 * Motion accumulates CELLS.  The conversion back to a ratio happens
 * once, at release — a ratio round-trip per motion event accumulates
 * float error and makes the border stutter against the pointer.
 */
void yew_pane_drag_motion(Ed *ed, u16 x, u16 y)
{
    i32 now;
    i32 delta;

    if (ed == NULL || !ed->drag.active || ed->drag.split == NULL)
        return;
    now = ed->drag.split->dir == YEW_SPLIT_H ? (i32)x : (i32)y;
    delta = now - (i32)ed->drag.origin;
    if (delta == 0)
        return;
    if (yew_pane_resize(ed->drag.split, delta))
        ed->drag.origin = (u16)now;
    ed->layout_dirty = true;
    ed->full_damage = true;
}

void yew_pane_drag_end(Ed *ed)
{
    if (ed == NULL)
        return;
    /* Marked at RELEASE, not per motion event: the intermediate ratios
     * are not a state the user ever chose. */
    if (ed->drag.active)
        yew_state_mark_dirty(ed);
    ed->drag.active = false;
    ed->drag.split = NULL;
}

void yew_pane_drag_cancel(Ed *ed)
{
    if (ed == NULL || !ed->drag.active)
        return;
    /* Esc restores the ratio the drag started from, which is why the
     * entry value is kept rather than recomputed. */
    if (ed->drag.split != NULL)
        ed->drag.split->ratio = ed->drag.entry_ratio;
    ed->drag.active = false;
    ed->drag.split = NULL;
    ed->layout_dirty = true;
    ed->full_damage = true;
}

/*
 * Click to focus.  The ONLY mouse behaviour this sprint, and it goes
 * through the region registry rather than re-deriving which pane owns a
 * cell — that re-derivation is exactly what the registry exists to
 * prevent.
 */
bool yew_pane_click(Ed *ed, u16 x, u16 y)
{
    Region hit;

    if (ed == NULL)
        return false;
    hit = yew_region_hit(x, y);
    if (hit.kind == YEW_REGION_PANE_BORDER) {
        Pane *split = yew_pane_split_by_index(ed, hit.payload);

        if (split != NULL) {
            yew_pane_drag_begin(ed, split, x, y);
            return true;
        }
        return false;
    }
    if (hit.kind != YEW_REGION_PANE)
        return false;
    {
        Pane *leaf = yew_pane_leaf_by_index(ed, hit.payload);

        if (leaf == NULL)
            return false;
        pane_refocus(ed, leaf);
        /*
         * Land the cursor on the clicked GRAPHEME.  The cell -> column
         * conversion lives in src/unicode/, so a double-width character
         * earlier on the line cannot desync the click from the glyph.
         */
        if (leaf->win != NULL && leaf->win->buf != NULL) {
            yew_win_click_to_cursor(leaf->win, x, y);
        }
        return true;
    }
}

/* ---------------------------------------------------------------- */
/* Per-frame index tables                                           */
/* ---------------------------------------------------------------- */

void yew_pane_tables_reset(Ed *ed)
{
    if (ed == NULL)
        return;
    ed->nleaf_tab = 0U;
    ed->nsplit_tab = 0U;
}

i32 yew_pane_table_add_leaf(Ed *ed, Pane *leaf)
{
    if (ed == NULL || leaf == NULL ||
        ed->nleaf_tab >= YEW_PANE_MAX_LEAVES)
        return -1;
    ed->leaf_tab[ed->nleaf_tab] = leaf;
    return (i32)ed->nleaf_tab++;
}

i32 yew_pane_table_add_split(Ed *ed, Pane *split)
{
    if (ed == NULL || split == NULL ||
        ed->nsplit_tab >= YEW_PANE_MAX_LEAVES)
        return -1;
    ed->split_tab[ed->nsplit_tab] = split;
    return (i32)ed->nsplit_tab++;
}

Pane *yew_pane_leaf_by_index(Ed *ed, i32 index)
{
    if (ed == NULL || index < 0 || (u32)index >= ed->nleaf_tab)
        return NULL;
    return ed->leaf_tab[index];
}

Pane *yew_pane_split_by_index(Ed *ed, i32 index)
{
    if (ed == NULL || index < 0 || (u32)index >= ed->nsplit_tab)
        return NULL;
    return ed->split_tab[index];
}
