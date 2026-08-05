/*
 * Sprint 22 §1-§5: the pane tree, layout arithmetic, focus and resize.
 *
 * The arithmetic is the risky part, and it is risky in a specific way:
 * every bug in it is a ONE-CELL disagreement between what was drawn and
 * what gets hit-tested.  So the tests here assert exact rects rather
 * than properties, check that laying out twice is byte-identical, and
 * walk every cell of an 80x24 layout confirming that leaf_at agrees
 * with the rect it came from.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/layout.h"

static void ly_fixture(Ed *ed)
{
    /*
     * The command registry is process-wide and other suites replace it
     * with a reduced set; rebuild the full builtin table so a lookup
     * here does not depend on test order.
     */
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    SAG_ASSERT_NOT_NULL(ed->pane_root);
    SAG_ASSERT(ed->pane_root->is_leaf);
    SAG_ASSERT(ed->focus == ed->pane_root);
}

static Rect rect_of(const Pane *p)
{
    return p->rect;
}

static bool rect_eq(Rect a, Rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void test_layout_root_leaf_takes_the_whole_area(void)
{
    Ed ed;
    Rect area = {0U, 0U, 80U, 24U};

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, area);
    SAG_ASSERT(rect_eq(rect_of(ed.pane_root), area));
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 1U);
    sag_ed_free(&ed);
}

/*
 * The border cell belongs to the SPLIT node, so the two children get
 * w-1 between them.  At 80 columns and ratio 0.5 that is 79 usable,
 * rounding to 40 and 39 — asserted exactly, because "about half" is
 * where one-cell drift hides.
 */
void test_layout_h_split_gives_the_border_column_to_the_split(void)
{
    Ed ed;
    Pane *b;

    ly_fixture(&ed);
    /* Layout first: how much room a split has is not knowable before
     * the tree has a size, and in the editor layout always precedes
     * input. */
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    SAG_ASSERT_NOT_NULL(b);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    SAG_ASSERT(rect_eq(rect_of(ed.pane_root->a),
                       (Rect){0U, 0U, 40U, 24U}));
    /* Column 40 is the border; b starts at 41. */
    SAG_ASSERT(rect_eq(rect_of(ed.pane_root->b),
                       (Rect){41U, 0U, 39U, 24U}));
    SAG_ASSERT_EQ_U64(ed.pane_root->a->rect.w + ed.pane_root->b->rect.w +
                          1U,
                      80U);
    sag_ed_free(&ed);
}

void test_layout_v_split_gives_the_border_row_to_the_split(void)
{
    Ed ed;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT(rect_eq(rect_of(ed.pane_root->a),
                       (Rect){0U, 0U, 80U, 12U}));
    SAG_ASSERT(rect_eq(rect_of(ed.pane_root->b),
                       (Rect){0U, 13U, 80U, 11U}));
    sag_ed_free(&ed);
}

/*
 * The odd-width regression the sprint calls for.  81 and 121 columns
 * both leave an odd number of usable cells, which is where two rounding
 * sites would disagree.
 */
void test_layout_odd_widths_round_at_one_site(void)
{
    static const u16 widths[] = {80U, 81U, 120U, 121U};
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(widths); i++) {
        Ed ed;
        u16 w = widths[i];
        Rect a;
        Rect b;

        ly_fixture(&ed);
        sag_layout_compute(ed.pane_root, (Rect){0U, 0U, w, 24U});
        SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root,
                                           SAG_SPLIT_H));
        sag_layout_compute(ed.pane_root, (Rect){0U, 0U, w, 24U});
        a = ed.pane_root->a->rect;
        b = ed.pane_root->b->rect;
        /* Every column is accounted for exactly once: the two leaves
         * plus the single border column. */
        SAG_ASSERT_EQ_U64((u64)a.w + b.w + 1U, w);
        /* And b begins immediately after the border. */
        SAG_ASSERT_EQ_U64(b.x, (u64)a.x + a.w + 1U);
        sag_ed_free(&ed);
    }
}

/* Invariant 5: same state, same grid.  Laying out twice must produce
 * byte-identical rects. */
void test_layout_is_deterministic(void)
{
    Ed ed;
    Pane *b;
    Rect snap[3];

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, b, SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    snap[0] = ed.pane_root->rect;
    snap[1] = ed.pane_root->a->rect;
    snap[2] = ed.pane_root->b->rect;

    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    SAG_ASSERT(memcmp(&snap[0], &ed.pane_root->rect, sizeof(Rect)) == 0);
    SAG_ASSERT(memcmp(&snap[1], &ed.pane_root->a->rect,
                      sizeof(Rect)) == 0);
    SAG_ASSERT(memcmp(&snap[2], &ed.pane_root->b->rect,
                      sizeof(Rect)) == 0);
    sag_ed_free(&ed);
}

/*
 * DoD 4.  A shrink and a grow back must restore byte-identical rects —
 * that is what storing the ratio instead of cells buys, and storing
 * cells would fail here by drifting.
 */
void test_layout_shrink_then_grow_restores_rects(void)
{
    Ed ed;
    Pane *b;
    Rect before[3];
    Rect after[3];

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, b, SAG_SPLIT_V));

    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    before[0] = ed.pane_root->a->rect;
    before[1] = ed.pane_root->b->a->rect;
    before[2] = ed.pane_root->b->b->rect;

    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 40U, 12U});
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    after[0] = ed.pane_root->a->rect;
    after[1] = ed.pane_root->b->a->rect;
    after[2] = ed.pane_root->b->b->rect;
    SAG_ASSERT(memcmp(before, after, sizeof(before)) == 0);
    sag_ed_free(&ed);
}

/* A subtree that cannot fit collapses to zero size and is SKIPPED, not
 * freed: growing the terminal back has to bring it back. */
void test_layout_collapses_rather_than_destroying_on_shrink(void)
{
    Ed ed;
    Pane *b;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    SAG_ASSERT_NOT_NULL(b);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 20U, 24U});
    /* 20 columns cannot hold two 12-column minima plus a border. */
    SAG_ASSERT_EQ_U64(ed.pane_root->b->rect.w, 0U);
    /* The node is still there, with its window. */
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 2U);
    SAG_ASSERT_NOT_NULL(ed.pane_root->b->win);

    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT(ed.pane_root->b->rect.w >= SAG_PANE_MIN_W);
    sag_ed_free(&ed);
}

/* DoD 3: refusal at exactly min*2+1, acceptance at min*2+2. */
void test_layout_split_refuses_below_minimum(void)
{
    Ed ed;

    ly_fixture(&ed);
    /* One column short of holding two minima plus a border. */
    sag_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, (u16)(SAG_PANE_MIN_W * 2), 24U});
    SAG_ASSERT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H));

    /* Exactly enough. */
    sag_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, (u16)(SAG_PANE_MIN_W * 2 + 1), 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H));
    sag_ed_free(&ed);
}

void test_layout_split_refuses_vertically_below_minimum(void)
{
    Ed ed;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, 80U, (u16)(SAG_PANE_MIN_H * 2)});
    SAG_ASSERT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, 80U, (u16)(SAG_PANE_MIN_H * 2 + 1)});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_V));
    sag_ed_free(&ed);
}

void test_layout_split_refuses_past_the_leaf_cap(void)
{
    Ed ed;
    Rect area = {0U, 0U, 400U, 100U};
    Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
    u32 n;
    u32 round;

    ly_fixture(&ed);
    /*
     * Split BREADTH-first, alternating axes.  Always splitting the
     * newest leaf halves it every time and runs out of room around the
     * eighth split, so it would hit the minimum-size refusal and never
     * reach the cap this test is about.
     */
    sag_layout_compute(ed.pane_root, area);
    leaves[0] = ed.pane_root;
    n = 1U;
    for (round = 0U; round < 4U; round++) {
        u32 have = n;
        u32 i;

        for (i = 0U; i < have; i++) {
            Pane *nu = sag_pane_split(&ed, leaves[i],
                                      (round & 1U) != 0U ? SAG_SPLIT_V
                                                         : SAG_SPLIT_H);

            SAG_ASSERT_NOT_NULL(nu);
            /* The old leaf became the split node; its `a` child is the
             * original view. */
            leaves[i] = leaves[i]->a;
            leaves[n++] = nu;
            sag_layout_compute(ed.pane_root, area);
        }
    }
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root),
                      SAG_PANE_MAX_LEAVES);
    /* At the cap, a split with ample room is still refused. */
    SAG_ASSERT(leaves[0]->rect.w >= (u16)(SAG_PANE_MIN_W * 2 + 1));
    SAG_ASSERT_NULL(sag_pane_split(&ed, leaves[0], SAG_SPLIT_H));
    sag_ed_free(&ed);
}

/* Parent pointers stay right through split and close sequences. */
void test_layout_tree_invariants_survive_split_and_close(void)
{
    Ed ed;
    Pane *b;
    Pane *c;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    SAG_ASSERT_NOT_NULL(b);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    c = sag_pane_split(&ed, b, SAG_SPLIT_V);
    SAG_ASSERT_NOT_NULL(c);

    SAG_ASSERT(ed.pane_root->a->parent == ed.pane_root);
    SAG_ASSERT(ed.pane_root->b->parent == ed.pane_root);
    SAG_ASSERT(ed.pane_root->b->a->parent == ed.pane_root->b);
    SAG_ASSERT(ed.pane_root->b->b->parent == ed.pane_root->b);
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 3U);

    /* Closing collapses the parent into the sibling. */
    SAG_ASSERT(sag_pane_close(&ed, c));
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 2U);
    SAG_ASSERT(ed.pane_root->b->is_leaf);
    SAG_ASSERT(ed.pane_root->b->parent == ed.pane_root);
    sag_ed_free(&ed);
}

/* The root leaf refuses to close; Sprint 23 owns that case. */
void test_layout_root_leaf_refuses_to_close(void)
{
    Ed ed;

    ly_fixture(&ed);
    SAG_ASSERT(!sag_pane_close(&ed, ed.pane_root));
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 1U);
    sag_ed_free(&ed);
}

/*
 * The layout/draw split, stated as a test: for EVERY cell of an 80x24
 * layout, leaf_at either names a leaf whose rect contains that cell, or
 * says nothing because the cell is a border.  A one-cell disagreement
 * anywhere shows up here.
 */
void test_layout_leaf_at_agrees_with_every_rect(void)
{
    Ed ed;
    Pane *b;
    u16 x;
    u16 y;
    u32 covered = 0U;
    u32 borders = 0U;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, b, SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    for (y = 0U; y < 24U; y++) {
        for (x = 0U; x < 80U; x++) {
            Pane *leaf = sag_pane_leaf_at(ed.pane_root, x, y);

            if (leaf == NULL) {
                borders++;
                continue;
            }
            SAG_ASSERT(leaf->is_leaf);
            SAG_ASSERT(x >= leaf->rect.x);
            SAG_ASSERT(x < (u32)leaf->rect.x + leaf->rect.w);
            SAG_ASSERT(y >= leaf->rect.y);
            SAG_ASSERT(y < (u32)leaf->rect.y + leaf->rect.h);
            covered++;
        }
    }
    /* One border column over the full height, plus one border row
     * across the right-hand pane only. */
    SAG_ASSERT_EQ_U64(borders, 24U + ed.pane_root->b->rect.w);
    SAG_ASSERT_EQ_U64(covered + borders, 80U * 24U);
    /* Off the tree is NULL, not a wild pointer. */
    SAG_ASSERT_NULL(sag_pane_leaf_at(ed.pane_root, 80U, 0U));
    SAG_ASSERT_NULL(sag_pane_leaf_at(ed.pane_root, 0U, 24U));
    sag_ed_free(&ed);
}

/* Spatial focus on a four-pane grid, against hand-computed answers. */
void test_layout_focus_on_a_four_pane_grid(void)
{
    Ed ed;
    Pane *right;
    Pane *tl;
    Pane *bl;
    Pane *tr;
    Pane *br;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    right = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root->a,
                                       SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, right, SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    tl = ed.pane_root->a->a;
    bl = ed.pane_root->a->b;
    tr = ed.pane_root->b->a;
    br = ed.pane_root->b->b;

    SAG_ASSERT(sag_pane_dir(ed.pane_root, tl, SAG_DIR_RIGHT) == tr);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, tr, SAG_DIR_LEFT) == tl);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, tl, SAG_DIR_DOWN) == bl);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, bl, SAG_DIR_UP) == tl);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, br, SAG_DIR_LEFT) == bl);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, br, SAG_DIR_UP) == tr);

    /* It never wraps: off the edge is a no-op. */
    SAG_ASSERT_NULL(sag_pane_dir(ed.pane_root, tl, SAG_DIR_LEFT));
    SAG_ASSERT_NULL(sag_pane_dir(ed.pane_root, tl, SAG_DIR_UP));
    SAG_ASSERT_NULL(sag_pane_dir(ed.pane_root, br, SAG_DIR_RIGHT));
    SAG_ASSERT_NULL(sag_pane_dir(ed.pane_root, br, SAG_DIR_DOWN));
    sag_ed_free(&ed);
}

/* A T layout: one tall pane on the left, two stacked on the right. */
void test_layout_focus_on_a_three_pane_t(void)
{
    Ed ed;
    Pane *right;
    Pane *left;
    Pane *tr;
    Pane *br;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    right = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, right, SAG_SPLIT_V));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    left = ed.pane_root->a;
    tr = ed.pane_root->b->a;
    br = ed.pane_root->b->b;

    /* From the tall left pane, RIGHT picks the nearer facing edge; both
     * are equidistant in x, so the tie breaks on the centre line, which
     * the top pane wins on a 24-row split (12 vs 11). */
    SAG_ASSERT(sag_pane_dir(ed.pane_root, left, SAG_DIR_RIGHT) == tr);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, tr, SAG_DIR_LEFT) == left);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, br, SAG_DIR_LEFT) == left);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, tr, SAG_DIR_DOWN) == br);
    SAG_ASSERT(sag_pane_dir(ed.pane_root, br, SAG_DIR_UP) == tr);
    sag_ed_free(&ed);
}

void test_layout_focus_next_cycles_in_tree_order(void)
{
    Ed ed;
    Pane *b;
    Pane *first;
    Pane *second;
    Pane *third;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 24U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, b, SAG_SPLIT_H));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 24U});

    first = ed.pane_root->a;
    second = ed.pane_root->b->a;
    third = ed.pane_root->b->b;
    SAG_ASSERT(sag_pane_next(ed.pane_root, first) == second);
    SAG_ASSERT(sag_pane_next(ed.pane_root, second) == third);
    /* And round, which is what a cycle means. */
    SAG_ASSERT(sag_pane_next(ed.pane_root, third) == first);
    sag_ed_free(&ed);
}

/* Resize moves the boundary by the requested cells, and refuses rather
 * than producing an under-minimum pane. */
void test_layout_resize_moves_by_cells_and_clamps(void)
{
    Ed ed;
    Pane *split;
    u16 before;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    split = ed.pane_root;
    before = split->a->rect.w;

    SAG_ASSERT(sag_pane_resize(split, 2));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(split->a->rect.w, (u64)before + 2U);

    SAG_ASSERT(sag_pane_resize(split, -5));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(split->a->rect.w, (u64)before - 3U);

    /* Past the minimum it refuses, and the ratio is untouched. */
    {
        float kept = split->ratio;

        SAG_ASSERT(!sag_pane_resize(split, -1000));
        SAG_ASSERT(split->ratio == kept);
    }
    sag_ed_free(&ed);
}

void test_layout_ancestor_split_walks_to_the_matching_axis(void)
{
    Ed ed;
    Pane *b;
    Pane *deep;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    deep = sag_pane_split(&ed, b, SAG_SPLIT_V);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});

    /* The nearest V ancestor is the inner split; the nearest H
     * ancestor is the root, which is further up. */
    SAG_ASSERT(sag_pane_ancestor_split(deep, SAG_SPLIT_V) == b);
    SAG_ASSERT(sag_pane_ancestor_split(deep, SAG_SPLIT_H) == ed.pane_root);
    /* The root leaf of a one-pane tree has neither. */
    SAG_ASSERT_NULL(sag_pane_ancestor_split(ed.pane_root, SAG_SPLIT_H));
    sag_ed_free(&ed);
}

static void count_visit(Pane *p, void *ctx)
{
    (void)p;
    (*(u32 *)ctx)++;
}

/* Sprint 25 serializes through the walk, so it must reach every node —
 * splits included, not only leaves. */
void test_layout_tree_walk_visits_every_node(void)
{
    Ed ed;
    Pane *b;
    u32 n = 0U;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    b = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_H);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    SAG_ASSERT_NOT_NULL(sag_pane_split(&ed, b, SAG_SPLIT_V));

    sag_pane_tree_walk(ed.pane_root, count_visit, &n);
    /* 3 leaves + 2 split nodes. */
    SAG_ASSERT_EQ_U64(n, 5U);
    sag_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Commands (§4/§5) — dispatched through the registry, as bound      */
/* ---------------------------------------------------------------- */

static CmdStatus ly_invoke(Ed *ed, const char *name, u32 count)
{
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx;

    SAG_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    /* count 0 here means "the user gave none"; the registry requires a
     * repeat count of at least one regardless. */
    cx.count = count == 0U ? 1U : count;
    cx.count_given = count != 0U;
    cx.source = SAG_SRC_TEST;
    return sag_ed_invoke(ed, id, &cx);
}

void test_layout_split_command_focuses_the_new_pane(void)
{
    Ed ed;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 2U);
    /* The NEW pane takes focus: a split means "open a view here". */
    SAG_ASSERT(ed.focus == ed.pane_root->b);
    /* And Ed's active window follows focus, so edits land in it. */
    SAG_ASSERT(ed.win == ed.focus->win);
    /* Both views show the same buffer. */
    SAG_ASSERT(ed.pane_root->a->win->buf == ed.pane_root->b->win->buf);
    sag_ed_free(&ed);
}

void test_layout_split_command_refuses_with_a_message(void)
{
    Ed ed;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 10U, 24U});
    SAG_ASSERT(ly_invoke(&ed, "ed.pane.split_h", 0U) != SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 1U);
    sag_ed_free(&ed);
}

/* Closing the focused pane must leave focus on a leaf that still
 * exists — the classic dangling-pointer case. */
void test_layout_close_command_moves_focus_to_a_live_leaf(void)
{
    Ed ed;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.close", 0U), SAG_CMD_OK);

    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 1U);
    SAG_ASSERT_NOT_NULL(ed.focus);
    SAG_ASSERT(ed.focus->is_leaf);
    SAG_ASSERT(ed.focus == ed.pane_root);
    SAG_ASSERT(ed.win == ed.focus->win);
    /* The last pane refuses, naming Sprint 23. */
    SAG_ASSERT(ly_invoke(&ed, "ed.pane.close", 0U) != SAG_CMD_OK);
    sag_ed_free(&ed);
}

void test_layout_focus_commands_move_and_stop_at_edges(void)
{
    Ed ed;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    SAG_ASSERT(ed.focus == ed.pane_root->b);
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_left", 0U),
                      SAG_CMD_OK);
    SAG_ASSERT(ed.focus == ed.pane_root->a);
    /* Left again is a no-op, not a wrap. */
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_left", 0U),
                      SAG_CMD_OK);
    SAG_ASSERT(ed.focus == ed.pane_root->a);
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_right", 0U),
                      SAG_CMD_OK);
    SAG_ASSERT(ed.focus == ed.pane_root->b);
    sag_ed_free(&ed);
}

/* Grow means "make MY pane bigger" whichever side of the split it is
 * on — the sign flips for the second child. */
void test_layout_grow_widens_the_focused_pane_on_either_side(void)
{
    Ed ed;
    u16 a0;
    u16 b0;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b0 = ed.pane_root->b->rect.w;

    /* Focus is on b, the second child. */
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.grow", 0U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ed.pane_root->b->rect.w, (u64)b0 + 2U);

    /* Now from the first child. */
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_left", 0U),
                      SAG_CMD_OK);
    a0 = ed.pane_root->a->rect.w;
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.grow", 0U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ed.pane_root->a->rect.w, (u64)a0 + 2U);

    /* A count overrides the two-cell default. */
    a0 = ed.pane_root->a->rect.w;
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.shrink", 5U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ed.pane_root->a->rect.w, (u64)a0 - 5U);
    sag_ed_free(&ed);
}

/* DoD 6: a drag moves the border by exactly the motion delta, and Esc
 * restores the ratio the drag began with. */
void test_layout_drag_moves_by_delta_and_esc_restores(void)
{
    Ed ed;
    u16 before;
    float entry;

    ly_fixture(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), SAG_CMD_OK);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    before = ed.pane_root->a->rect.w;
    entry = ed.pane_root->ratio;

    /* Press on the border column, then move three cells right. */
    sag_pane_drag_begin(&ed, ed.pane_root, before, 5U);
    SAG_ASSERT(ed.drag.active);
    sag_pane_drag_motion(&ed, (u16)(before + 3U), 5U);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT_EQ_U64(ed.pane_root->a->rect.w, (u64)before + 3U);
    sag_pane_drag_end(&ed);
    SAG_ASSERT(!ed.drag.active);

    /* A second drag, cancelled: the entry ratio comes back exactly. */
    entry = ed.pane_root->ratio;
    before = ed.pane_root->a->rect.w;
    sag_pane_drag_begin(&ed, ed.pane_root, before, 5U);
    sag_pane_drag_motion(&ed, (u16)(before + 6U), 5U);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT(ed.pane_root->a->rect.w != before);
    sag_pane_drag_cancel(&ed);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT(ed.pane_root->ratio == entry);
    SAG_ASSERT_EQ_U64(ed.pane_root->a->rect.w, before);
    sag_ed_free(&ed);
}
