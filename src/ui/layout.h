#ifndef YEW_UI_LAYOUT_H
#define YEW_UI_LAYOUT_H

/*
 * Sprint 22 §1/§2: the pane tree and the layout/draw split.
 *
 * THE LAW: layout computes Rects in CELLS; drawing consumes them;
 * hit-testing consumes the same values.  No renderer recomputes a
 * position from text content, and no width math happens outside
 * src/unicode/.
 *
 * The reason is the one behind the region registry too: placement
 * derived twice drifts the moment text stops being one cell per byte.
 * Here it is computed once, in cells, and shared.
 *
 * A Rect is in CELLS — never bytes, never graphemes.  A function taking
 * both a Rect and text must clip through the Sprint 2 width tables, and
 * the clipped text must be styling-free: an embedded SGR sequence
 * counted as visible cells silently shortens the run.
 */

#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef struct Rect {
    u16 x, y, w, h;
} Rect;

typedef enum {
    YEW_SPLIT_H, /* side by side, border COLUMN between */
    YEW_SPLIT_V  /* stacked, border ROW between         */
} SplitDir;

typedef enum {
    YEW_DIR_LEFT,
    YEW_DIR_RIGHT,
    YEW_DIR_UP,
    YEW_DIR_DOWN
} YewDir;

enum {
    /* Leaf CONTENT cells, excluding the border the parent split owns. */
    YEW_PANE_MIN_W = 12,
    YEW_PANE_MIN_H = 3,
    /* Hard cap per tab; split refuses past it. */
    YEW_PANE_MAX_LEAVES = 16
};

typedef struct Pane {
    bool is_leaf;
    Rect rect; /* filled by yew_layout_compute */
    struct Pane *parent;

    /* split node */
    SplitDir dir;
    /*
     * The FIRST child's share.  The ratio is stored and the cell split
     * is recomputed from it on every layout — cells are never stored.
     * Storing cells makes a SIGWINCH resize drift the proportions;
     * storing the ratio makes resize idempotent, which invariant 5
     * requires (same state -> same grid).
     */
    float ratio;
    struct Pane *a, *b;

    /* leaf node */
    Win *win;
} Pane;

Pane *yew_pane_new_leaf(Win *win);
/* Takes the editor so the per-frame pane tables die with the nodes they
 * index — see layout.c for the use-after-free that shaped this. */
void yew_pane_free(Ed *ed, Pane *p);

/*
 * NULL when the split is refused: no room at the current size, or the
 * leaf cap is reached.  Never split-then-clamp — a split that would
 * immediately render a two-column sliver is a refusal, not a layout
 * problem to fix afterwards.
 *
 * "Current size" means the leaf's rect as of the last
 * yew_layout_compute, so a tree that has never been laid out refuses
 * every split.  That is the honest answer — the room available is not
 * knowable before layout — and in the editor layout always precedes
 * input.
 */
Pane *yew_pane_split(Ed *ed, Pane *leaf, SplitDir dir);
/* Replaces the parent split with the sibling subtree.  The root leaf
 * refuses to close; Sprint 23 owns the last-pane-of-last-tab case. */
bool yew_pane_close(Ed *ed, Pane *leaf);

void yew_layout_compute(Pane *root, Rect area);
/* NULL when (x, y) is off the tree or lands on a border. */
Pane *yew_pane_leaf_at(Pane *root, u16 x, u16 y);
u32 yew_pane_leaf_count(const Pane *root);
/*
 * The first leaf in tree order.  Anything that has to land focus
 * somewhere after a mutation uses this — a caller that walks down to a
 * leaf itself gets it wrong on the first tree whose surviving child is
 * a split, which is exactly what fuzz_panes caught.
 */
Pane *yew_pane_first_leaf(Pane *root);
/* Fills `out` with every leaf in tree order, capped at `cap`. */
void yew_pane_collect_leaves(Pane *root, Pane **out, u32 cap, u32 *n);

/* Spatial, directional, and it never wraps — wrapping makes
 * muscle-memory direction keys ambiguous in three-pane layouts. */
Pane *yew_pane_dir(Pane *root, const Pane *from, YewDir dir);
/* Tree-order cycle, for terminals that eat arrow keys. */
Pane *yew_pane_next(Pane *root, const Pane *from);
Pane *yew_pane_prev(Pane *root, const Pane *from);

/*
 * Sprint 25 serializes the tree through this rather than reaching into
 * node internals.
 */
typedef void (*YewPaneVisit)(Pane *p, void *ctx);
void yew_pane_tree_walk(Pane *root, YewPaneVisit fn, void *ctx);

/* The nearest ancestor split on `axis`, or NULL. */
Pane *yew_pane_ancestor_split(const Pane *leaf, SplitDir axis);
/*
 * Adjusts a split's ratio by `cells` worth, clamped so both sides stay
 * at or above the minimum.  False when the clamp refused it.
 */
bool yew_pane_resize(Pane *split, i32 cells);

void yew_layout(Ed *ed);

#endif
