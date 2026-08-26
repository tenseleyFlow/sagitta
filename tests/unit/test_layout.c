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
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT_NOT_NULL(ed->pane_root);
    YEW_ASSERT(ed->pane_root->is_leaf);
    YEW_ASSERT(ed->focus == ed->pane_root);
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
    yew_layout_compute(ed.pane_root, area);
    YEW_ASSERT(rect_eq(rect_of(ed.pane_root), area));
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 1U);
    yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(b);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    YEW_ASSERT(rect_eq(rect_of(ed.pane_root->a),
                       (Rect){0U, 0U, 40U, 24U}));
    /* Column 40 is the border; b starts at 41. */
    YEW_ASSERT(rect_eq(rect_of(ed.pane_root->b),
                       (Rect){41U, 0U, 39U, 24U}));
    YEW_ASSERT_EQ_U64(ed.pane_root->a->rect.w + ed.pane_root->b->rect.w +
                          1U,
                      80U);
    yew_ed_free(&ed);
}

void test_layout_v_split_gives_the_border_row_to_the_split(void)
{
    Ed ed;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT(rect_eq(rect_of(ed.pane_root->a),
                       (Rect){0U, 0U, 80U, 12U}));
    YEW_ASSERT(rect_eq(rect_of(ed.pane_root->b),
                       (Rect){0U, 13U, 80U, 11U}));
    yew_ed_free(&ed);
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

    for (i = 0U; i < YEW_ARRAY_LEN(widths); i++) {
        Ed ed;
        u16 w = widths[i];
        Rect a;
        Rect b;

        ly_fixture(&ed);
        yew_layout_compute(ed.pane_root, (Rect){0U, 0U, w, 24U});
        YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, ed.pane_root,
                                           YEW_SPLIT_H));
        yew_layout_compute(ed.pane_root, (Rect){0U, 0U, w, 24U});
        a = ed.pane_root->a->rect;
        b = ed.pane_root->b->rect;
        /* Every column is accounted for exactly once: the two leaves
         * plus the single border column. */
        YEW_ASSERT_EQ_U64((u64)a.w + b.w + 1U, w);
        /* And b begins immediately after the border. */
        YEW_ASSERT_EQ_U64(b.x, (u64)a.x + a.w + 1U);
        yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, b, YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    snap[0] = ed.pane_root->rect;
    snap[1] = ed.pane_root->a->rect;
    snap[2] = ed.pane_root->b->rect;

    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 81U, 25U});
    YEW_ASSERT(memcmp(&snap[0], &ed.pane_root->rect, sizeof(Rect)) == 0);
    YEW_ASSERT(memcmp(&snap[1], &ed.pane_root->a->rect,
                      sizeof(Rect)) == 0);
    YEW_ASSERT(memcmp(&snap[2], &ed.pane_root->b->rect,
                      sizeof(Rect)) == 0);
    yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, b, YEW_SPLIT_V));

    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    before[0] = ed.pane_root->a->rect;
    before[1] = ed.pane_root->b->a->rect;
    before[2] = ed.pane_root->b->b->rect;

    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 40U, 12U});
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    after[0] = ed.pane_root->a->rect;
    after[1] = ed.pane_root->b->a->rect;
    after[2] = ed.pane_root->b->b->rect;
    YEW_ASSERT(memcmp(before, after, sizeof(before)) == 0);
    yew_ed_free(&ed);
}

/* A subtree that cannot fit collapses to zero size and is SKIPPED, not
 * freed: growing the terminal back has to bring it back. */
void test_layout_collapses_rather_than_destroying_on_shrink(void)
{
    Ed ed;
    Pane *b;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(b);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 20U, 24U});
    /* 20 columns cannot hold two 12-column minima plus a border. */
    YEW_ASSERT_EQ_U64(ed.pane_root->b->rect.w, 0U);
    /* The node is still there, with its window. */
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 2U);
    YEW_ASSERT_NOT_NULL(ed.pane_root->b->win);

    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT(ed.pane_root->b->rect.w >= YEW_PANE_MIN_W);
    yew_ed_free(&ed);
}

/* DoD 3: refusal at exactly min*2+1, acceptance at min*2+2. */
void test_layout_split_refuses_below_minimum(void)
{
    Ed ed;

    ly_fixture(&ed);
    /* One column short of holding two minima plus a border. */
    yew_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, (u16)(YEW_PANE_MIN_W * 2), 24U});
    YEW_ASSERT_NULL(yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H));

    /* Exactly enough. */
    yew_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, (u16)(YEW_PANE_MIN_W * 2 + 1), 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H));
    yew_ed_free(&ed);
}

void test_layout_split_refuses_vertically_below_minimum(void)
{
    Ed ed;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, 80U, (u16)(YEW_PANE_MIN_H * 2)});
    YEW_ASSERT_NULL(yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root,
                       (Rect){0U, 0U, 80U, (u16)(YEW_PANE_MIN_H * 2 + 1)});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_V));
    yew_ed_free(&ed);
}

void test_layout_split_refuses_past_the_leaf_cap(void)
{
    Ed ed;
    Rect area = {0U, 0U, 400U, 100U};
    Pane *leaves[YEW_PANE_MAX_LEAVES * 2];
    u32 n;
    u32 round;

    ly_fixture(&ed);
    /*
     * Split BREADTH-first, alternating axes.  Always splitting the
     * newest leaf halves it every time and runs out of room around the
     * eighth split, so it would hit the minimum-size refusal and never
     * reach the cap this test is about.
     */
    yew_layout_compute(ed.pane_root, area);
    leaves[0] = ed.pane_root;
    n = 1U;
    for (round = 0U; round < 4U; round++) {
        u32 have = n;
        u32 i;

        for (i = 0U; i < have; i++) {
            Pane *nu = yew_pane_split(&ed, leaves[i],
                                      (round & 1U) != 0U ? YEW_SPLIT_V
                                                         : YEW_SPLIT_H);

            YEW_ASSERT_NOT_NULL(nu);
            /* The old leaf became the split node; its `a` child is the
             * original view. */
            leaves[i] = leaves[i]->a;
            leaves[n++] = nu;
            yew_layout_compute(ed.pane_root, area);
        }
    }
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root),
                      YEW_PANE_MAX_LEAVES);
    /* At the cap, a split with ample room is still refused. */
    YEW_ASSERT(leaves[0]->rect.w >= (u16)(YEW_PANE_MIN_W * 2 + 1));
    YEW_ASSERT_NULL(yew_pane_split(&ed, leaves[0], YEW_SPLIT_H));
    yew_ed_free(&ed);
}

/* Parent pointers stay right through split and close sequences. */
void test_layout_tree_invariants_survive_split_and_close(void)
{
    Ed ed;
    Pane *b;
    Pane *c;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(b);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    c = yew_pane_split(&ed, b, YEW_SPLIT_V);
    YEW_ASSERT_NOT_NULL(c);

    YEW_ASSERT(ed.pane_root->a->parent == ed.pane_root);
    YEW_ASSERT(ed.pane_root->b->parent == ed.pane_root);
    YEW_ASSERT(ed.pane_root->b->a->parent == ed.pane_root->b);
    YEW_ASSERT(ed.pane_root->b->b->parent == ed.pane_root->b);
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 3U);

    /* Closing collapses the parent into the sibling. */
    YEW_ASSERT(yew_pane_close(&ed, c));
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 2U);
    YEW_ASSERT(ed.pane_root->b->is_leaf);
    YEW_ASSERT(ed.pane_root->b->parent == ed.pane_root);
    yew_ed_free(&ed);
}

/* The root leaf refuses to close; Sprint 23 owns that case. */
void test_layout_root_leaf_refuses_to_close(void)
{
    Ed ed;

    ly_fixture(&ed);
    YEW_ASSERT(!yew_pane_close(&ed, ed.pane_root));
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 1U);
    yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, b, YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    for (y = 0U; y < 24U; y++) {
        for (x = 0U; x < 80U; x++) {
            Pane *leaf = yew_pane_leaf_at(ed.pane_root, x, y);

            if (leaf == NULL) {
                borders++;
                continue;
            }
            YEW_ASSERT(leaf->is_leaf);
            YEW_ASSERT(x >= leaf->rect.x);
            YEW_ASSERT(x < (u32)leaf->rect.x + leaf->rect.w);
            YEW_ASSERT(y >= leaf->rect.y);
            YEW_ASSERT(y < (u32)leaf->rect.y + leaf->rect.h);
            covered++;
        }
    }
    /* One border column over the full height, plus one border row
     * across the right-hand pane only. */
    YEW_ASSERT_EQ_U64(borders, 24U + ed.pane_root->b->rect.w);
    YEW_ASSERT_EQ_U64(covered + borders, 80U * 24U);
    /* Off the tree is NULL, not a wild pointer. */
    YEW_ASSERT_NULL(yew_pane_leaf_at(ed.pane_root, 80U, 0U));
    YEW_ASSERT_NULL(yew_pane_leaf_at(ed.pane_root, 0U, 24U));
    yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    right = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, ed.pane_root->a,
                                       YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, right, YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    tl = ed.pane_root->a->a;
    bl = ed.pane_root->a->b;
    tr = ed.pane_root->b->a;
    br = ed.pane_root->b->b;

    YEW_ASSERT(yew_pane_dir(ed.pane_root, tl, YEW_DIR_RIGHT) == tr);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, tr, YEW_DIR_LEFT) == tl);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, tl, YEW_DIR_DOWN) == bl);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, bl, YEW_DIR_UP) == tl);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, br, YEW_DIR_LEFT) == bl);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, br, YEW_DIR_UP) == tr);

    /* It never wraps: off the edge is a no-op. */
    YEW_ASSERT_NULL(yew_pane_dir(ed.pane_root, tl, YEW_DIR_LEFT));
    YEW_ASSERT_NULL(yew_pane_dir(ed.pane_root, tl, YEW_DIR_UP));
    YEW_ASSERT_NULL(yew_pane_dir(ed.pane_root, br, YEW_DIR_RIGHT));
    YEW_ASSERT_NULL(yew_pane_dir(ed.pane_root, br, YEW_DIR_DOWN));
    yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    right = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, right, YEW_SPLIT_V));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    left = ed.pane_root->a;
    tr = ed.pane_root->b->a;
    br = ed.pane_root->b->b;

    /* From the tall left pane, RIGHT picks the nearer facing edge; both
     * are equidistant in x, so the tie breaks on the centre line, which
     * the top pane wins on a 24-row split (12 vs 11). */
    YEW_ASSERT(yew_pane_dir(ed.pane_root, left, YEW_DIR_RIGHT) == tr);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, tr, YEW_DIR_LEFT) == left);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, br, YEW_DIR_LEFT) == left);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, tr, YEW_DIR_DOWN) == br);
    YEW_ASSERT(yew_pane_dir(ed.pane_root, br, YEW_DIR_UP) == tr);
    yew_ed_free(&ed);
}

void test_layout_focus_next_cycles_in_tree_order(void)
{
    Ed ed;
    Pane *b;
    Pane *first;
    Pane *second;
    Pane *third;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 24U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, b, YEW_SPLIT_H));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 24U});

    first = ed.pane_root->a;
    second = ed.pane_root->b->a;
    third = ed.pane_root->b->b;
    YEW_ASSERT(yew_pane_next(ed.pane_root, first) == second);
    YEW_ASSERT(yew_pane_next(ed.pane_root, second) == third);
    /* And round, which is what a cycle means. */
    YEW_ASSERT(yew_pane_next(ed.pane_root, third) == first);
    YEW_ASSERT(yew_pane_prev(ed.pane_root, first) == third);
    YEW_ASSERT(yew_pane_prev(ed.pane_root, third) == second);
    YEW_ASSERT(yew_pane_prev(ed.pane_root, second) == first);
    yew_ed_free(&ed);
}

/* Resize moves the boundary by the requested cells, and refuses rather
 * than producing an under-minimum pane. */
void test_layout_resize_moves_by_cells_and_clamps(void)
{
    Ed ed;
    Pane *split;
    u16 before;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    split = ed.pane_root;
    before = split->a->rect.w;

    YEW_ASSERT(yew_pane_resize(split, 2));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(split->a->rect.w, (u64)before + 2U);

    YEW_ASSERT(yew_pane_resize(split, -5));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(split->a->rect.w, (u64)before - 3U);

    /* Past the minimum it refuses, and the ratio is untouched. */
    {
        float kept = split->ratio;

        YEW_ASSERT(!yew_pane_resize(split, -1000));
        YEW_ASSERT(split->ratio == kept);
    }
    yew_ed_free(&ed);
}

void test_layout_ancestor_split_walks_to_the_matching_axis(void)
{
    Ed ed;
    Pane *b;
    Pane *deep;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    deep = yew_pane_split(&ed, b, YEW_SPLIT_V);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});

    /* The nearest V ancestor is the inner split; the nearest H
     * ancestor is the root, which is further up. */
    YEW_ASSERT(yew_pane_ancestor_split(deep, YEW_SPLIT_V) == b);
    YEW_ASSERT(yew_pane_ancestor_split(deep, YEW_SPLIT_H) == ed.pane_root);
    /* The root leaf of a one-pane tree has neither. */
    YEW_ASSERT_NULL(yew_pane_ancestor_split(ed.pane_root, YEW_SPLIT_H));
    yew_ed_free(&ed);
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
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    b = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 120U, 40U});
    YEW_ASSERT_NOT_NULL(yew_pane_split(&ed, b, YEW_SPLIT_V));

    yew_pane_tree_walk(ed.pane_root, count_visit, &n);
    /* 3 leaves + 2 split nodes. */
    YEW_ASSERT_EQ_U64(n, 5U);
    yew_ed_free(&ed);
}

void test_layout_keeps_inactive_linked_viewport_detached_from_cursor(void)
{
    Ed ed;
    Pane *right;
    Bytebuf text;
    u32 i;

    ly_fixture(&ed);
    bytebuf_init(&text);
    for (i = 0U; i < 80U; i++)
        bytebuf_printf(&text, "row-%03u\n", i);
    yew_textbuf_insert(ed.buffer.tb, BYTEOFF(0U), text.data,
                       (u64)text.len);
    bytebuf_free(&text);
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 24U, 80U));
    ed.grid_ready = true;
    yew_layout(&ed);
    right = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(right);
    ed.focus = ed.pane_root->a;
    ed.win = ed.focus->win;
    ed.pane_root->a->win->scroll_link = 7U;
    right->win->scroll_link = 7U;
    right->win->vp.top = LINENO(20U);
    yew_layout(&ed);
    YEW_ASSERT_EQ_U64(right->win->vp.top.v, 20U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_of(
                          right->win->buf->tb,
                          right->win->cs.curs.data[0].pos).v,
                      0U);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Commands (§4/§5) — dispatched through the registry, as bound      */
/* ---------------------------------------------------------------- */

static CmdStatus ly_invoke(Ed *ed, const char *name, u32 count)
{
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx;

    YEW_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    /* count 0 here means "the user gave none"; the registry requires a
     * repeat count of at least one regardless. */
    cx.count = count == 0U ? 1U : count;
    cx.count_given = count != 0U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

void test_layout_split_command_focuses_the_new_pane(void)
{
    Ed ed;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 2U);
    /* The NEW pane takes focus: a split means "open a view here". */
    YEW_ASSERT(ed.focus == ed.pane_root->b);
    /* And Ed's active window follows focus, so edits land in it. */
    YEW_ASSERT(ed.win == ed.focus->win);
    /* Both views show the same buffer. */
    YEW_ASSERT(ed.pane_root->a->win->buf == ed.pane_root->b->win->buf);
    yew_ed_free(&ed);
}

void test_layout_split_command_refuses_with_a_message(void)
{
    Ed ed;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 10U, 24U});
    YEW_ASSERT(ly_invoke(&ed, "ed.pane.split_h", 0U) != YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 1U);
    yew_ed_free(&ed);
}

/* Closing the focused pane must leave focus on a leaf that still
 * exists — the classic dangling-pointer case. */
void test_layout_close_command_moves_focus_to_a_live_leaf(void)
{
    Ed ed;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.close", 0U), YEW_CMD_OK);

    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 1U);
    YEW_ASSERT_NOT_NULL(ed.focus);
    YEW_ASSERT(ed.focus->is_leaf);
    YEW_ASSERT(ed.focus == ed.pane_root);
    YEW_ASSERT(ed.win == ed.focus->win);
    /* The last pane refuses, naming Sprint 23. */
    YEW_ASSERT(ly_invoke(&ed, "ed.pane.close", 0U) != YEW_CMD_OK);
    yew_ed_free(&ed);
}

void test_layout_focus_commands_move_and_stop_at_edges(void)
{
    Ed ed;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});

    YEW_ASSERT(ed.focus == ed.pane_root->b);
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_left", 0U),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.focus == ed.pane_root->a);
    /* Left again is a no-op, not a wrap. */
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_left", 0U),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.focus == ed.pane_root->a);
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_right", 0U),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.focus == ed.pane_root->b);
    yew_ed_free(&ed);
}

/* Grow means "make MY pane bigger" whichever side of the split it is
 * on — the sign flips for the second child. */
void test_layout_grow_widens_the_focused_pane_on_either_side(void)
{
    Ed ed;
    u16 a0;
    u16 b0;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    b0 = ed.pane_root->b->rect.w;

    /* Focus is on b, the second child. */
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.grow", 0U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ed.pane_root->b->rect.w, (u64)b0 + 2U);

    /* Now from the first child. */
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.focus_left", 0U),
                      YEW_CMD_OK);
    a0 = ed.pane_root->a->rect.w;
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.grow", 0U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ed.pane_root->a->rect.w, (u64)a0 + 2U);

    /* A count overrides the two-cell default. */
    a0 = ed.pane_root->a->rect.w;
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.shrink", 5U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ed.pane_root->a->rect.w, (u64)a0 - 5U);
    yew_ed_free(&ed);
}

/* DoD 6: a drag moves the border by exactly the motion delta, and Esc
 * restores the ratio the drag began with. */
void test_layout_drag_moves_by_delta_and_esc_restores(void)
{
    Ed ed;
    u16 before;
    float entry;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ly_invoke(&ed, "ed.pane.split_h", 0U), YEW_CMD_OK);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    before = ed.pane_root->a->rect.w;
    entry = ed.pane_root->ratio;

    /* Press on the border column, then move three cells right. */
    yew_pane_drag_begin(&ed, ed.pane_root, before, 5U);
    YEW_ASSERT(ed.drag.active);
    yew_pane_drag_motion(&ed, (u16)(before + 3U), 5U);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT_EQ_U64(ed.pane_root->a->rect.w, (u64)before + 3U);
    yew_pane_drag_end(&ed);
    YEW_ASSERT(!ed.drag.active);

    /* A second drag, cancelled: the entry ratio comes back exactly. */
    entry = ed.pane_root->ratio;
    before = ed.pane_root->a->rect.w;
    yew_pane_drag_begin(&ed, ed.pane_root, before, 5U);
    yew_pane_drag_motion(&ed, (u16)(before + 6U), 5U);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT(ed.pane_root->a->rect.w != before);
    yew_pane_drag_cancel(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT(ed.pane_root->ratio == entry);
    YEW_ASSERT_EQ_U64(ed.pane_root->a->rect.w, before);
    yew_ed_free(&ed);
}

/*
 * Closing a pane frees the SIBLING node after moving its content into
 * the parent, so a focus pointer aimed at the sibling is left on freed
 * memory and the next keystroke uses it.  fuzz_panes found this in
 * eight operations; the row keeps it found.
 */
void test_layout_close_repairs_focus_on_the_freed_sibling(void)
{
    Ed ed;
    Pane *right;
    Pane *left;

    ly_fixture(&ed);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    right = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(right);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    left = ed.pane_root->a;

    /* Focus the SIBLING of the pane about to close. */
    ed.focus = right;
    YEW_ASSERT(yew_pane_close(&ed, left));
    /* Focus must name a leaf that still exists — not `right`, which was
     * the sibling node and is now freed. */
    YEW_ASSERT_NOT_NULL(ed.focus);
    YEW_ASSERT(ed.focus == ed.pane_root);
    YEW_ASSERT(ed.focus->is_leaf);
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 1U);
    yew_ed_free(&ed);
}
