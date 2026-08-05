#include "ui/layout.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/gutter.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "util/log.h"


/* ---------------------------------------------------------------- */
/* Pane tree (Sprint 22 §2)                                         */
/* ---------------------------------------------------------------- */

Pane *sag_pane_new_leaf(Win *win)
{
    Pane *p = sag_xcalloc(1U, sizeof(*p));

    p->is_leaf = true;
    p->win = win;
    p->ratio = 0.5f;
    return p;
}

void sag_pane_free(Pane *p)
{
    if (p == NULL)
        return;
    if (!p->is_leaf) {
        sag_pane_free(p->a);
        sag_pane_free(p->b);
    }
    free(p);
}

u32 sag_pane_leaf_count(const Pane *root)
{
    if (root == NULL)
        return 0U;
    if (root->is_leaf)
        return 1U;
    return sag_pane_leaf_count(root->a) + sag_pane_leaf_count(root->b);
}

void sag_pane_collect_leaves(Pane *root, Pane **out, u32 cap, u32 *n)
{
    if (root == NULL || out == NULL || n == NULL || *n >= cap)
        return;
    if (root->is_leaf) {
        out[(*n)++] = root;
        return;
    }
    sag_pane_collect_leaves(root->a, out, cap, n);
    sag_pane_collect_leaves(root->b, out, cap, n);
}

Pane *sag_pane_first_leaf(Pane *root)
{
    Pane *at = root;

    while (at != NULL && !at->is_leaf)
        at = at->a;
    return at;
}

void sag_pane_tree_walk(Pane *root, SagPaneVisit fn, void *ctx)
{
    if (root == NULL || fn == NULL)
        return;
    fn(root, ctx);
    if (!root->is_leaf) {
        sag_pane_tree_walk(root->a, fn, ctx);
        sag_pane_tree_walk(root->b, fn, ctx);
    }
}

static u16 clamp_u16(i32 v, i32 lo, i32 hi)
{
    if (hi < lo)
        return (u16)(lo < 0 ? 0 : lo);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return (u16)(v < 0 ? 0 : v);
}

/*
 * The split point in cells, from the ratio.  THE single rounding site.
 *
 * Two call sites rounding differently produce a one-cell disagreement
 * between what was drawn and what gets hit-tested, which is precisely
 * what the layout/draw split exists to forbid.  So every consumer —
 * layout, drag resize, the tests — asks this function.
 *
 * `span` is the total cells including the one border cell the split
 * node owns, so the two children share `span - 1`.
 */
static u16 split_cells(u16 span, float ratio, u16 min_a, u16 min_b)
{
    i32 usable = (i32)span - 1;
    i32 cells;

    if (usable <= 0)
        return 0U;
    cells = (i32)((float)usable * ratio + 0.5f);
    return clamp_u16(cells, (i32)min_a, usable - (i32)min_b);
}

static void layout_rec(Pane *p, Rect r)
{
    p->rect = r;
    if (p->is_leaf)
        return;
    /*
     * When the area cannot hold both minima the DEEPER subtree collapses
     * to zero size and draw skips it — it is never freed.  Destroying
     * panes on shrink loses user state to a transient window drag, and
     * growing the terminal back has to restore what was there.
     */
    if (p->dir == SAG_SPLIT_H) {
        u16 aw;

        if (r.w < (u16)(SAG_PANE_MIN_W * 2 + 1)) {
            layout_rec(p->a, r);
            layout_rec(p->b, (Rect){r.x, r.y, 0U, 0U});
            return;
        }
        aw = split_cells(r.w, p->ratio, SAG_PANE_MIN_W, SAG_PANE_MIN_W);
        layout_rec(p->a, (Rect){r.x, r.y, aw, r.h});
        /* The border column at r.x + aw belongs to the split node. */
        layout_rec(p->b, (Rect){(u16)(r.x + aw + 1U), r.y,
                                (u16)(r.w - aw - 1U), r.h});
    } else {
        u16 ah;

        if (r.h < (u16)(SAG_PANE_MIN_H * 2 + 1)) {
            layout_rec(p->a, r);
            layout_rec(p->b, (Rect){r.x, r.y, 0U, 0U});
            return;
        }
        ah = split_cells(r.h, p->ratio, SAG_PANE_MIN_H, SAG_PANE_MIN_H);
        layout_rec(p->a, (Rect){r.x, r.y, r.w, ah});
        layout_rec(p->b, (Rect){r.x, (u16)(r.y + ah + 1U), r.w,
                                (u16)(r.h - ah - 1U)});
    }
}

void sag_layout_compute(Pane *root, Rect area)
{
    if (root == NULL)
        return;
    layout_rec(root, area);
}

static bool rect_contains(Rect r, u16 x, u16 y)
{
    return r.w != 0U && r.h != 0U && x >= r.x && x < (u32)r.x + r.w &&
           y >= r.y && y < (u32)r.y + r.h;
}

Pane *sag_pane_leaf_at(Pane *root, u16 x, u16 y)
{
    if (root == NULL || !rect_contains(root->rect, x, y))
        return NULL;
    if (root->is_leaf)
        return root;
    {
        Pane *hit = sag_pane_leaf_at(root->a, x, y);

        if (hit != NULL)
            return hit;
    }
    /* Falls through to NULL on the border cell, which belongs to the
     * split node and is not a leaf. */
    return sag_pane_leaf_at(root->b, x, y);
}

/*
 * Would splitting `leaf` leave both halves at or above the minimum, at
 * the size it currently has?
 */
static bool split_fits(const Pane *leaf, SplitDir dir)
{
    if (dir == SAG_SPLIT_H)
        return leaf->rect.w >= (u16)(SAG_PANE_MIN_W * 2 + 1);
    return leaf->rect.h >= (u16)(SAG_PANE_MIN_H * 2 + 1);
}

Pane *sag_pane_split(Ed *ed, Pane *leaf, SplitDir dir)
{
    Pane *a;
    Pane *b;
    Win *win;

    if (ed == NULL || leaf == NULL || !leaf->is_leaf)
        return NULL;
    if (sag_pane_leaf_count(sag_ed_pane_root(ed)) >=
        (u32)SAG_PANE_MAX_LEAVES)
        return NULL;
    if (!split_fits(leaf, dir))
        return NULL;
    win = sag_ed_win_clone(ed, leaf->win);
    if (win == NULL)
        return NULL;
    a = sag_xcalloc(1U, sizeof(*a));
    b = sag_xcalloc(1U, sizeof(*b));
    /* The existing Win stays in child a; the clone takes b and focus. */
    a->is_leaf = true;
    a->win = leaf->win;
    a->parent = leaf;
    a->ratio = 0.5f;
    b->is_leaf = true;
    b->win = win;
    b->parent = leaf;
    b->ratio = 0.5f;
    /* The leaf becomes the split node in place, so every pointer at it
     * — including Ed's focus — stays valid without a fixup pass. */
    leaf->is_leaf = false;
    leaf->win = NULL;
    leaf->dir = dir;
    leaf->ratio = 0.5f;
    leaf->a = a;
    leaf->b = b;
    return b;
}

bool sag_pane_close(Ed *ed, Pane *leaf)
{
    Pane *parent;
    Pane *sibling;

    if (ed == NULL || leaf == NULL || !leaf->is_leaf)
        return false;
    parent = leaf->parent;
    if (parent == NULL)
        return false; /* the root leaf refuses; Sprint 23 owns this */
    sibling = parent->a == leaf ? parent->b : parent->a;
    sag_ed_win_release(ed, leaf->win);
    /*
     * The sibling's CONTENT is moved into the parent node rather than
     * the parent being replaced by the sibling pointer: that keeps the
     * parent's address stable, so anything holding it (focus, a drag in
     * progress) is not left dangling.
     */
    parent->is_leaf = sibling->is_leaf;
    parent->dir = sibling->dir;
    parent->ratio = sibling->ratio;
    parent->win = sibling->win;
    parent->a = sibling->a;
    parent->b = sibling->b;
    if (!parent->is_leaf) {
        parent->a->parent = parent;
        parent->b->parent = parent;
    }
    /*
     * The sibling NODE is freed after its content moves into the
     * parent, so anything pointing at it now dangles.  Focus is the one
     * that matters and the one nobody remembers: closing pane A when
     * focus sits on its sibling B leaves focus on freed memory, and the
     * next keystroke uses it.  fuzz_panes found this in eight
     * operations.
     */
    if (ed->focus == sibling || ed->focus == leaf)
        ed->focus = sag_pane_first_leaf(parent);
    free(sibling);
    free(leaf);
    return true;
}

/* ---------------------------------------------------------------- */
/* Focus (Sprint 22 §4)                                             */
/* ---------------------------------------------------------------- */

typedef struct LeafList {
    Pane *v[SAG_PANE_MAX_LEAVES * 2];
    u32 n;
} LeafList;

static void collect_leaves(Pane *p, LeafList *out)
{
    if (p == NULL || out->n >= SAG_ARRAY_LEN(out->v))
        return;
    if (p->is_leaf) {
        if (p->rect.w != 0U && p->rect.h != 0U)
            out->v[out->n++] = p;
        return;
    }
    collect_leaves(p->a, out);
    collect_leaves(p->b, out);
}

/*
 * Spatial directional focus.
 *
 * Take the focused leaf's centre; among leaves strictly beyond it in
 * `dir`, pick the one whose facing edge is nearest, breaking ties by
 * how close its perpendicular span is to the centre line.  No candidate
 * is a no-op: it never wraps, because wrapping makes a direction key
 * mean two different things depending on where you happen to be.
 */
Pane *sag_pane_dir(Pane *root, const Pane *from, SagDir dir)
{
    LeafList leaves;
    Pane *best = NULL;
    i32 best_gap = 0;
    i32 best_off = 0;
    i32 cx;
    i32 cy;
    u32 i;

    if (root == NULL || from == NULL || !from->is_leaf)
        return NULL;
    leaves.n = 0U;
    collect_leaves(root, &leaves);
    cx = (i32)from->rect.x + (i32)from->rect.w / 2;
    cy = (i32)from->rect.y + (i32)from->rect.h / 2;
    for (i = 0U; i < leaves.n; i++) {
        Pane *c = leaves.v[i];
        i32 gap;
        i32 off;

        if (c == from)
            continue;
        switch (dir) {
        case SAG_DIR_LEFT:
            if ((i32)c->rect.x + (i32)c->rect.w > cx)
                continue;
            gap = cx - ((i32)c->rect.x + (i32)c->rect.w);
            off = (i32)c->rect.y + (i32)c->rect.h / 2 - cy;
            break;
        case SAG_DIR_RIGHT:
            if ((i32)c->rect.x < cx)
                continue;
            gap = (i32)c->rect.x - cx;
            off = (i32)c->rect.y + (i32)c->rect.h / 2 - cy;
            break;
        case SAG_DIR_UP:
            if ((i32)c->rect.y + (i32)c->rect.h > cy)
                continue;
            gap = cy - ((i32)c->rect.y + (i32)c->rect.h);
            off = (i32)c->rect.x + (i32)c->rect.w / 2 - cx;
            break;
        default:
            if ((i32)c->rect.y < cy)
                continue;
            gap = (i32)c->rect.y - cy;
            off = (i32)c->rect.x + (i32)c->rect.w / 2 - cx;
            break;
        }
        if (off < 0)
            off = -off;
        if (best == NULL || gap < best_gap ||
            (gap == best_gap && off < best_off)) {
            best = c;
            best_gap = gap;
            best_off = off;
        }
    }
    return best;
}

Pane *sag_pane_next(Pane *root, const Pane *from)
{
    LeafList leaves;
    u32 i;

    leaves.n = 0U;
    collect_leaves(root, &leaves);
    if (leaves.n == 0U)
        return NULL;
    for (i = 0U; i < leaves.n; i++) {
        if (leaves.v[i] == from)
            return leaves.v[(i + 1U) % leaves.n];
    }
    return leaves.v[0];
}

/* ---------------------------------------------------------------- */
/* Resize (Sprint 22 §5)                                            */
/* ---------------------------------------------------------------- */

Pane *sag_pane_ancestor_split(const Pane *leaf, SplitDir axis)
{
    const Pane *at = leaf;

    while (at != NULL && at->parent != NULL) {
        if (at->parent->dir == axis && !at->parent->is_leaf)
            return at->parent;
        at = at->parent;
    }
    return NULL;
}

bool sag_pane_resize(Pane *split, i32 cells)
{
    u16 span;
    u16 min;
    i32 usable;
    i32 now;
    i32 want;

    if (split == NULL || split->is_leaf)
        return false;
    span = split->dir == SAG_SPLIT_H ? split->rect.w : split->rect.h;
    min = split->dir == SAG_SPLIT_H ? (u16)SAG_PANE_MIN_W
                                    : (u16)SAG_PANE_MIN_H;
    usable = (i32)span - 1;
    if (usable <= 0)
        return false;
    now = (i32)split_cells(span, split->ratio, min, min);
    want = now + cells;
    if (want < (i32)min || want > usable - (i32)min)
        return false;
    /*
     * Cells back to a ratio, once.  Per-event round-trips accumulate
     * float error and make a dragged border stutter against the
     * pointer, so callers accumulate CELLS and convert here at the end.
     */
    split->ratio = (float)want / (float)usable;
    return true;
}

/*
 * Gives one leaf's Win the geometry its Pane was assigned.  The gutter
 * is per pane, since two panes on the same buffer can be scrolled to
 * line 9 and line 1000 and need different widths.
 */
static void layout_leaf_win(Ed *ed, Pane *leaf)
{
    Win *w = leaf->win;
    u16 gutter;
    u16 old_cols;
    u16 old_gutter;

    if (w == NULL)
        return;
    if (leaf->rect.w == 0U || leaf->rect.h == 0U) {
        /* Collapsed: draw skips it, and it keeps its state for when the
         * terminal grows back. */
        w->rect = leaf->rect;
        w->vp.rows = 0U;
        w->vp.cols = 0U;
        return;
    }
    old_cols = w->vp.cols;
    old_gutter = w->gutter_width;
    gutter = leaf->rect.w < 20U ? 0U : sag_gutter_width(w);
    if (gutter >= leaf->rect.w)
        gutter = 0U;
    w->gutter_width = gutter;
    w->rect = (Rect){(u16)(leaf->rect.x + gutter), leaf->rect.y,
                     (u16)(leaf->rect.w - gutter), leaf->rect.h};
    w->vp.rows = leaf->rect.h;
    w->vp.cols = w->rect.w;
    if (old_cols != w->vp.cols || old_gutter != gutter) {
        sag_vp_invalidate(w);
        ed->full_damage = true;
        ed->footer_dirty = true;
    }
    sag_vp_clamp(w);
    sag_vp_follow(w);
}

static void layout_leaf_visit(Pane *p, void *ctx)
{
    if (p->is_leaf)
        layout_leaf_win(ctx, p);
}

void sag_layout(Ed *ed)
{
    u16 content_rows;

    if (ed == NULL || ed->win == NULL)
        SAG_BUG("editor layout: missing window");

    ed->footer_rect = (Rect){0U, 0U, 0U, 0U};
    if (ed->grid.rows >= 2U) {
        content_rows = (u16)(ed->grid.rows - 1U);
        ed->footer_rect = (Rect){0U, content_rows, ed->grid.cols, 1U};
    } else {
        content_rows = ed->grid.rows;
    }
    /*
     * The tree owns the document area; the footer row is reserved
     * before it and the tab strip's rows will be reserved the same way
     * in Sprint 23 — a parameter here, not a renderer.
     *
     * A NULL root is legal: several fixtures build an Ed directly
     * rather than through sag_ed_init.  Laying the single window out
     * against the same area keeps them working, and is exactly what the
     * one-leaf tree would compute anyway.
     */
    if (ed->pane_root != NULL) {
        sag_layout_compute(ed->pane_root,
                           (Rect){0U, 0U, ed->grid.cols, content_rows});
        sag_pane_tree_walk(ed->pane_root, layout_leaf_visit, ed);
    } else {
        Pane solo;

        (void)memset(&solo, 0, sizeof(solo));
        solo.is_leaf = true;
        solo.win = ed->win;
        solo.rect = (Rect){0U, 0U, ed->grid.cols, content_rows};
        layout_leaf_win(ed, &solo);
    }
    ed->layout_dirty = false;
}

void sag_ed_layout(Ed *ed)
{
    sag_layout(ed);
}
