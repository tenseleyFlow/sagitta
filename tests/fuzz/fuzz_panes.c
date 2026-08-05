/*
 * Sprint 22 fuzz: random split/close/resize/focus sequences.
 *
 * The tree invariants are cheap to state and expensive to lose: a
 * parent pointer that stops matching, a focus pointer left on a freed
 * node, a leaf whose rect escapes its parent.  Each of those produces a
 * crash or a mis-routed click much later, so they are asserted after
 * EVERY operation rather than at the end.
 *
 * Layout determinism gets the same treatment: laying out twice with no
 * intervening change must produce byte-identical rects (invariant 5).
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/pane_cmds.h"
#include "ui/layout.h"
#include "ui/region.h"

typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(r->s >> 33);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

static bool rect_inside(Rect inner, Rect outer)
{
    if (inner.w == 0U || inner.h == 0U)
        return true; /* collapsed subtrees are skipped, not placed */
    return inner.x >= outer.x && inner.y >= outer.y &&
           (u32)inner.x + inner.w <= (u32)outer.x + outer.w &&
           (u32)inner.y + inner.h <= (u32)outer.y + outer.h;
}

static bool check_tree(Pane *p, Pane *parent, char *why, size_t cap)
{
    if (p == NULL)
        return true;
    if (p->parent != parent) {
        (void)snprintf(why, cap, "parent pointer does not match");
        return false;
    }
    if (p->is_leaf) {
        if (p->win == NULL) {
            (void)snprintf(why, cap, "leaf without a window");
            return false;
        }
        return true;
    }
    if (p->a == NULL || p->b == NULL) {
        (void)snprintf(why, cap, "split node missing a child");
        return false;
    }
    if (p->win != NULL) {
        (void)snprintf(why, cap, "split node still holds a window");
        return false;
    }
    if (!rect_inside(p->a->rect, p->rect) ||
        !rect_inside(p->b->rect, p->rect)) {
        (void)snprintf(why, cap, "child rect escapes its parent");
        return false;
    }
    /* Children never overlap: the border cell between them belongs to
     * neither, which is what makes leaf_at unambiguous. */
    if (p->a->rect.w != 0U && p->b->rect.w != 0U) {
        if (p->dir == SAG_SPLIT_H &&
            (u32)p->a->rect.x + p->a->rect.w >= p->b->rect.x) {
            (void)snprintf(why, cap, "horizontal children overlap");
            return false;
        }
        if (p->dir == SAG_SPLIT_V &&
            (u32)p->a->rect.y + p->a->rect.h >= p->b->rect.y) {
            (void)snprintf(why, cap, "vertical children overlap");
            return false;
        }
    }
    return check_tree(p->a, p, why, cap) && check_tree(p->b, p, why, cap);
}

static void collect(Pane *p, Pane **out, u32 *n, u32 cap)
{
    if (p == NULL || *n >= cap)
        return;
    if (p->is_leaf) {
        out[(*n)++] = p;
        return;
    }
    collect(p->a, out, n, cap);
    collect(p->b, out, n, cap);
}

static bool focus_is_live(Ed *ed)
{
    Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
    u32 n = 0U;
    u32 i;

    collect(ed->pane_root, leaves, &n, SAG_ARRAY_LEN(leaves));
    for (i = 0U; i < n; i++) {
        if (leaves[i] == ed->focus)
            return true;
    }
    return false;
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Ed ed;
    Rng rng;
    Rect area = {0U, 0U, 120U, 40U};
    size_t op;
    size_t ops;
    bool ok = false;

    rng.s = 0x9E3779B97F4A7C15ULL;
    {
        size_t i;

        for (i = 0U; i < len; i++)
            rng.s = rng.s * 31U + data[i];
    }
    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "cannot open a buffer");
        return false;
    }
    sag_layout_compute(ed.pane_root, area);

    ops = 60U + (len % 60U);
    for (op = 0U; op < ops; op++) {
        Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
        u32 n = 0U;
        Pane *pick;

        collect(ed.pane_root, leaves, &n, SAG_ARRAY_LEN(leaves));
        if (n == 0U) {
            (void)snprintf(why, why_cap, "tree lost every leaf");
            goto done;
        }
        pick = leaves[rng_below(&rng, n)];
        switch (rng_below(&rng, 6U)) {
        case 0:
        case 1: {
            Pane *nu = sag_pane_split(&ed, pick,
                                      rng_below(&rng, 2U) != 0U
                                          ? SAG_SPLIT_V
                                          : SAG_SPLIT_H);

            if (nu != NULL)
                ed.focus = nu;
            break;
        }
        case 2:
            if (pick->parent != NULL) {
                Pane *parent = pick->parent;

                if (ed.focus == pick)
                    ed.focus = NULL;
                (void)sag_pane_close(&ed, pick);
                if (ed.focus == NULL)
                    ed.focus = sag_pane_first_leaf(parent);
            }
            break;
        case 3: {
            Pane *split = sag_pane_ancestor_split(pick, SAG_SPLIT_H);

            if (split == NULL)
                split = sag_pane_ancestor_split(pick, SAG_SPLIT_V);
            if (split != NULL)
                (void)sag_pane_resize(split,
                                      (i32)rng_below(&rng, 9U) - 4);
            break;
        }
        case 4: {
            Pane *to = sag_pane_dir(ed.pane_root, ed.focus,
                                    (SagDir)rng_below(&rng, 4U));

            if (to != NULL)
                ed.focus = to;
            break;
        }
        default:
            /* Resize the screen, including sizes too small to hold the
             * tree — collapse must be survivable and reversible. */
            area.w = (u16)(20U + rng_below(&rng, 200U));
            area.h = (u16)(6U + rng_below(&rng, 60U));
            break;
        }
        sag_layout_compute(ed.pane_root, area);
        if (!check_tree(ed.pane_root, NULL, why, why_cap))
            goto done;
        if (!focus_is_live(&ed)) {
            (void)snprintf(why, why_cap,
                           "focus is not a live leaf after op %zu", op);
            goto done;
        }
        /* Determinism: a second layout with nothing changed must not
         * move a single cell. */
        {
            Pane *before[SAG_PANE_MAX_LEAVES * 2];
            Rect snap[SAG_PANE_MAX_LEAVES * 2];
            u32 m = 0U;
            u32 i;

            collect(ed.pane_root, before, &m, SAG_ARRAY_LEN(before));
            for (i = 0U; i < m; i++)
                snap[i] = before[i]->rect;
            sag_layout_compute(ed.pane_root, area);
            for (i = 0U; i < m; i++) {
                if (memcmp(&snap[i], &before[i]->rect,
                           sizeof(Rect)) != 0) {
                    (void)snprintf(why, why_cap,
                                   "layout is not idempotent at op %zu",
                                   op);
                    goto done;
                }
            }
        }
        /* Every cell either names a leaf that contains it or is a
         * border; leaf_at must never point outside the rect it came
         * from. */
        {
            u16 x = (u16)rng_below(&rng, area.w);
            u16 y = (u16)rng_below(&rng, area.h);
            Pane *leaf = sag_pane_leaf_at(ed.pane_root, x, y);

            if (leaf != NULL &&
                (x < leaf->rect.x ||
                 x >= (u32)leaf->rect.x + leaf->rect.w ||
                 y < leaf->rect.y ||
                 y >= (u32)leaf->rect.y + leaf->rect.h)) {
                (void)snprintf(why, why_cap,
                               "leaf_at returned a pane that does not "
                               "contain the cell");
                goto done;
            }
        }
    }
    ok = true;
done:
    sag_ed_free(&ed);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_panes", NULL, run_session);
}
