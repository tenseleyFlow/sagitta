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
    /* Sparse workspace-record identity; 0 means no unknown fields. */
    u32 state_token;
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
/*
 * Sprint 57.22 §3: the same split with a SIDE.
 *
 * `new_first` puts the new leaf in child `a` — the LEFT of a
 * side-by-side split, the TOP of a stacked one — and `false` is
 * `yew_pane_split` exactly, which is how it is implemented.  Every
 * refusal above applies unchanged, and it is asked BEFORE anything is
 * cloned, so a refused split leaves the tree as it was.
 *
 * The old window's `state_token` follows THAT WINDOW into whichever
 * child it lands in.  The retained workspace record belongs to the
 * window that was already there; giving it to the clone restores the
 * new pane with the old one's scroll position, silently.
 */
Pane *yew_pane_split_side(Ed *ed, Pane *leaf, SplitDir dir,
                          bool new_first);
/* Replaces the parent split with the sibling subtree.  The root leaf
 * refuses to close; Sprint 23 owns the last-pane-of-last-tab case. */
bool yew_pane_close(Ed *ed, Pane *leaf);

/*
 * Sprint 57.22 §2: THE EDGE ZONES, and the one function that owns them.
 *
 * A tab dragged onto a leaf's left, right or bottom edge spawns a pane
 * there.  Which cells are that edge, whether the edge is offered at
 * all, and where the new pane would land are ONE answer computed here —
 * the release's hit test and the drag's affordance both ask this, so a
 * zone the user can see is always a zone that works.
 *
 *     depth = clamp(dimension / 5, 3, 12)    cells, integer division
 *
 * `dimension` is the leaf's content WIDTH for the left/right zones and
 * its content HEIGHT for the bottom one.  (The enum spelling reads
 * backwards from the gesture: a left/right zone is a YEW_SPLIT_H,
 * side-by-side split, and it is the WIDTH that has to hold two panes.)
 *
 * A zone EXISTS only when yew_pane_split_side would succeed for it:
 * the leaf cap and the same fit test the split itself asks, asked here
 * BEFORE the zone is offered.  Left and right additionally require
 * `2 * depth < width`, because two bands that touch leave no interior
 * and the drop becomes unaimable.
 *
 * There is no top zone: the strip is up there, and a tab released
 * upward is already the row-1 reorder gesture.
 */
typedef enum {
    YEW_PANE_ZONE_NONE = 0,
    YEW_PANE_ZONE_LEFT,
    YEW_PANE_ZONE_RIGHT,
    YEW_PANE_ZONE_BOTTOM
} PaneZone;

typedef struct PaneZoneHit {
    PaneZone zone;
    /* yew_pane_split_side's two arguments for this side. */
    SplitDir dir;
    bool new_first;
    /* The cells that select the zone. */
    Rect band;
    /*
     * Where the new leaf would land, computed through the SAME rounding
     * yew_layout_compute uses — the affordance promises a rectangle and
     * the layout has to keep the promise.
     */
    Rect preview;
} PaneZoneHit;

/* Exposed so the rule can be tested at its boundaries directly. */
u16 yew_pane_zone_depth(u16 dimension);
/*
 * The live zone at (x, y) on `leaf`, or false when the pointer is in
 * the leaf's interior, off it, or on an edge that is not offered.
 *
 * A corner belongs to the LEFT or RIGHT band: those are the two that
 * can be refused for width, so letting them win keeps the corner's
 * meaning the same whenever they exist at all.
 */
bool yew_pane_zone_at(Ed *ed, const Pane *leaf, u16 x, u16 y,
                      PaneZoneHit *out);

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
