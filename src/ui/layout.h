#ifndef SAG_UI_LAYOUT_H
#define SAG_UI_LAYOUT_H

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
    SAG_SPLIT_H, /* side by side, border COLUMN between */
    SAG_SPLIT_V  /* stacked, border ROW between         */
} SplitDir;

typedef enum {
    SAG_DIR_LEFT,
    SAG_DIR_RIGHT,
    SAG_DIR_UP,
    SAG_DIR_DOWN
} SagDir;

enum {
    /* Leaf CONTENT cells, excluding the border the parent split owns. */
    SAG_PANE_MIN_W = 12,
    SAG_PANE_MIN_H = 3,
    /* Hard cap per tab; split refuses past it. */
    SAG_PANE_MAX_LEAVES = 16
};

typedef struct Pane {
    bool is_leaf;
    Rect rect; /* filled by sag_layout_compute */
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

Pane *sag_pane_new_leaf(Win *win);
void sag_pane_free(Pane *p);

/*
 * NULL when the split is refused: no room at the current size, or the
 * leaf cap is reached.  Never split-then-clamp — a split that would
 * immediately render a two-column sliver is a refusal, not a layout
 * problem to fix afterwards.
 *
 * "Current size" means the leaf's rect as of the last
 * sag_layout_compute, so a tree that has never been laid out refuses
 * every split.  That is the honest answer — the room available is not
 * knowable before layout — and in the editor layout always precedes
 * input.
 */
Pane *sag_pane_split(Ed *ed, Pane *leaf, SplitDir dir);
/* Replaces the parent split with the sibling subtree.  The root leaf
 * refuses to close; Sprint 23 owns the last-pane-of-last-tab case. */
bool sag_pane_close(Ed *ed, Pane *leaf);

void sag_layout_compute(Pane *root, Rect area);
/* NULL when (x, y) is off the tree or lands on a border. */
Pane *sag_pane_leaf_at(Pane *root, u16 x, u16 y);
u32 sag_pane_leaf_count(const Pane *root);
/*
 * The first leaf in tree order.  Anything that has to land focus
 * somewhere after a mutation uses this — a caller that walks down to a
 * leaf itself gets it wrong on the first tree whose surviving child is
 * a split, which is exactly what fuzz_panes caught.
 */
Pane *sag_pane_first_leaf(Pane *root);

/* Spatial, directional, and it never wraps — wrapping makes
 * muscle-memory direction keys ambiguous in three-pane layouts. */
Pane *sag_pane_dir(Pane *root, const Pane *from, SagDir dir);
/* Tree-order cycle, for terminals that eat arrow keys. */
Pane *sag_pane_next(Pane *root, const Pane *from);

/*
 * Sprint 25 serializes the tree through this rather than reaching into
 * node internals.
 */
typedef void (*SagPaneVisit)(Pane *p, void *ctx);
void sag_pane_tree_walk(Pane *root, SagPaneVisit fn, void *ctx);

/* The nearest ancestor split on `axis`, or NULL. */
Pane *sag_pane_ancestor_split(const Pane *leaf, SplitDir axis);
/*
 * Adjusts a split's ratio by `cells` worth, clamped so both sides stay
 * at or above the minimum.  False when the clamp refused it.
 */
bool sag_pane_resize(Pane *split, i32 cells);

void sag_layout(Ed *ed);

#endif
