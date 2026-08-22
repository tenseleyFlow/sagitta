#define _POSIX_C_SOURCE 200809L

/* Sprint 52: arbitrary tree shapes and navigation/state transitions. */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/git/fusstree.h"

enum { FUSS_FUZZ_MAX_ENTRIES = 128, FUSS_FUZZ_PATH_CAP = 48 };

typedef struct FussFixture {
    GitSnapshot snap;
    GitEntry *entries;
    char *paths;
} FussFixture;

static bool fixture_make(FussFixture *f, const u8 *data, size_t len)
{
    size_t count = len == 0U ? 1U : len / 3U + 1U;
    size_t i;

    if (count > FUSS_FUZZ_MAX_ENTRIES)
        count = FUSS_FUZZ_MAX_ENTRIES;
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->snap.a);
    f->entries = calloc(count, sizeof(*f->entries));
    f->paths = malloc(count * FUSS_FUZZ_PATH_CAP);
    if (f->entries == NULL || f->paths == NULL)
        return false;
    for (i = 0U; i < count; i++) {
        GitEntry *e = &f->entries[i];
        char *path = f->paths + i * FUSS_FUZZ_PATH_CAP;
        u8 a = len == 0U ? 0U : data[(i * 3U) % len];
        u8 b = len == 0U ? 0U : data[(i * 3U + 1U) % len];
        u8 flags = len == 0U ? 1U : data[(i * 3U + 2U) % len];
        int wrote;

        if ((flags & 0x40U) != 0U)
            wrote = snprintf(path, FUSS_FUZZ_PATH_CAP,
                             ".d%02x/sub%02x/f%03zu.c", a, b, i);
        else
            wrote = snprintf(path, FUSS_FUZZ_PATH_CAP,
                             "d%02x/sub%02x/f%03zu.c", a, b, i);
        if (wrote <= 0 || wrote >= FUSS_FUZZ_PATH_CAP)
            return false;
        e->kind = (flags & 0x20U) != 0U ? GIT_E_IGNORED : GIT_E_ORDINARY;
        e->path = path;
        e->path_len = (u32)wrote;
        e->staged = (flags & 0x01U) != 0U;
        e->unstaged = (flags & 0x02U) != 0U;
        e->untracked = (flags & 0x04U) != 0U;
        e->incoming = (flags & 0x08U) != 0U;
        e->conflicted = (flags & 0x10U) != 0U;
    }
    f->snap.entries.data = f->entries;
    f->snap.entries.len = count;
    f->snap.gen = 1U;
    return true;
}

static void fixture_drop(FussFixture *f)
{
    free(f->paths);
    free(f->entries);
    arena_free_all(&f->snap.a);
}

static bool tree_valid(const FussTree *t, char *why, size_t why_cap)
{
    size_t i;

    if (t->nodes.len == 0U) {
        (void)snprintf(why, why_cap, "tree has no root node");
        return false;
    }
    for (i = 1U; i < t->nodes.len; i++) {
        const FussNode *node = &t->nodes.data[i];

        if (node->name == NULL || node->parent >= t->nodes.len ||
            node->first_child >= t->nodes.len ||
            node->next_sibling >= t->nodes.len) {
            (void)snprintf(why, why_cap, "invalid node links at %zu", i);
            return false;
        }
    }
    for (i = 0U; i < t->items.len; i++) {
        const FussItem *item = &t->items.data[i];

        if (item->path == NULL || item->node == 0U ||
            item->node >= t->nodes.len ||
            item->is_file != t->nodes.data[item->node].is_file) {
            (void)snprintf(why, why_cap, "invalid flat item at %zu", i);
            return false;
        }
    }
    return true;
}

static bool check_fuss(const u8 *data, size_t len, char *why,
                       size_t why_cap)
{
    FussFixture fixture;
    FussOpts opts = {true, true};
    FussTree tree;
    FussSel sel = {0};
    i32 row = -1;
    size_t at;
    bool ok = false;

    if (!fixture_make(&fixture, data, len)) {
        (void)snprintf(why, why_cap, "fixture allocation failed");
        fixture_drop(&fixture);
        return false;
    }
    yew_fuss_tree_init(&tree);
    yew_fuss_build(&tree, &fixture.snap, &opts);
    if (!tree_valid(&tree, why, why_cap))
        goto done;
    if (tree.items.len != 0U) {
        row = 0;
        yew_fuss_sel_from_row(&sel, &tree, row);
    }
    for (at = 0U; at < len; at++) {
        u8 op = data[at] % 9U;

        if (op == 0U)
            row = yew_fuss_nav_step(&tree, row, 1);
        else if (op == 1U)
            row = yew_fuss_nav_step(&tree, row, -1);
        else if (op == 2U)
            row = yew_fuss_nav_raw(&tree, row,
                                   (data[at] & 1U) != 0U ? 1 : -1);
        else if (op == 3U)
            row = yew_fuss_nav_parent(&tree, row);
        else if (op == 4U)
            row = yew_fuss_nav_enter(&tree, row);
        else if (op == 5U) {
            (void)yew_fuss_nav_toggle(&tree, row);
            row = yew_fuss_row_of(&tree, &sel);
        } else if (op == 6U && row >= 0) {
            yew_fuss_sel_from_row(&sel, &tree, row);
        } else if (op == 7U) {
            FussNode *nodes = tree.nodes.data;
            FussItem *items = tree.items.data;
            size_t node_len = tree.nodes.len;
            size_t item_len = tree.items.len;
            u16 saved_depth = 0U;

            /* Pointer equality alone cannot prove flatten was skipped: the
             * item vector normally reuses its allocation.  A temporary
             * sentinel makes an in-place flatten observable. */
            if (item_len != 0U) {
                saved_depth = tree.items.data[0].depth;
                tree.items.data[0].depth = UINT16_MAX;
            }

            yew_fuss_build(&tree, &fixture.snap, &opts);
            if (tree.nodes.data != nodes || tree.items.data != items ||
                tree.nodes.len != node_len || tree.items.len != item_len ||
                (item_len != 0U &&
                 tree.items.data[0].depth != UINT16_MAX)) {
                (void)snprintf(why, why_cap,
                               "unchanged generation rebuilt or flattened");
                goto done;
            }
            if (item_len != 0U)
                tree.items.data[0].depth = saved_depth;
        } else {
            Arena paths_arena;
            char **paths = NULL;
            u32 n;

            arena_init(&paths_arena);
            n = yew_fuss_harvest_collapsed(&tree, &paths_arena, &paths);
            yew_fuss_restore_collapsed(&tree, paths, n);
            arena_free_all(&paths_arena);
        }
        if (!tree_valid(&tree, why, why_cap))
            goto done;
        if (tree.items.len == 0U) {
            row = -1;
            yew_fuss_sel_clear(&sel);
        } else {
            if (row < 0 || (size_t)row >= tree.items.len)
                row = 0;
            yew_fuss_sel_from_row(&sel, &tree, row);
            if (yew_fuss_row_of(&tree, &sel) != row) {
                (void)snprintf(why, why_cap,
                               "selection path did not resolve at byte %zu",
                               at);
                goto done;
            }
        }
    }
    ok = true;
done:
    yew_fuss_sel_clear(&sel);
    yew_fuss_tree_drop(&tree);
    fixture_drop(&fixture);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_fuss", NULL, check_fuss);
}
