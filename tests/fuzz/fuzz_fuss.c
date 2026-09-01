#define _POSIX_C_SOURCE 200809L

/* Sprint 52: arbitrary tree shapes plus live F-mode key dispatch. */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "mod/git/fussmode.h"
#include "mod/git/fusstree.h"
#include "mod/git/git_int.h"
#include "ui/tabs.h"

enum {
    FUSS_FUZZ_MAX_ENTRIES = 128,
    FUSS_FUZZ_PATH_CAP = 48,
    FUSS_FUZZ_LIVE_KEYS = 48
};

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

static bool layout_valid(u16 content, u16 natural, char *why,
                         size_t why_cap)
{
    FussDrawerLayout layout = yew_fuss_drawer_layout(content, natural);

    if (layout.width > content || layout.tree_width > layout.width) {
        (void)snprintf(why, why_cap, "drawer escaped %u-cell content",
                       content);
        return false;
    }
    if (layout.fullscreen) {
        if (layout.width != content || layout.tree_width != content ||
            layout.edge_col != UINT16_MAX) {
            (void)snprintf(why, why_cap, "invalid full-screen drawer");
            return false;
        }
    } else if (content < YEW_FUSS_DRAWER_MIN_CELLS ||
               layout.width + YEW_FUSS_EDITOR_RETAIN_CELLS > content ||
               layout.width == 0U ||
               layout.tree_width + YEW_FUSS_DRAWER_EDGE_CELLS !=
                   layout.width ||
               layout.edge_col != layout.tree_width) {
        (void)snprintf(why, why_cap, "invalid overlay drawer");
        return false;
    }
    return true;
}

typedef struct LiveSpawn {
    u32 next_id;
    u32 calls;
} LiveSpawn;

static u32 live_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                      const GitReq *req, void *opaque, char *err,
                      size_t errsz)
{
    LiveSpawn *spawn = opaque;

    (void)ed;
    (void)req;
    (void)err;
    (void)errsz;
    if (spawn == NULL || verb == NULL || argv == NULL || argv[0] == NULL)
        return 0U;
    spawn->calls++;
    if (spawn->next_id == UINT32_MAX)
        spawn->next_id = 1000U;
    return spawn->next_id++;
}

static Key live_key(u8 byte)
{
    static const u32 named[] = {
        YEW_KEY_UP, YEW_KEY_DOWN, YEW_KEY_LEFT, YEW_KEY_RIGHT,
        YEW_KEY_PAGE_UP, YEW_KEY_PAGE_DOWN, YEW_KEY_HOME, YEW_KEY_END,
        YEW_KEY_ENTER, YEW_KEY_ESCAPE
    };
    static const char printable[] =
        "kjhl auUSmMplfdswhLcbnRGOIyvzZtxrNT./q0123456789";
    Key key = {0};
    size_t named_count = YEW_ARRAY_LEN(named);
    size_t printable_count = sizeof(printable) - 1U;
    size_t choice = (size_t)byte % (named_count + printable_count + 3U);

    key.kind = YEW_EV_KEY;
    key.ev = (byte & 0x40U) != 0U ? YEW_KEY_REPEAT : YEW_KEY_PRESS;
    if (choice < named_count) {
        key.code = named[choice];
        if ((byte & 0x80U) != 0U &&
            (key.code == YEW_KEY_UP || key.code == YEW_KEY_DOWN))
            key.mods = YEW_MOD_CTRL;
    } else if (choice < named_count + printable_count) {
        u8 text = (u8)printable[choice - named_count];

        key.code = text;
        key.ntext = 1U;
        key.text[0] = text;
    } else {
        key.code = (u32)'r';
        key.mods = YEW_MOD_CTRL;
        key.ntext = 1U;
        key.text[0] = (u8)'r';
    }
    return key;
}

static bool live_views_valid(Ed *ed)
{
    Tab *active;
    size_t tab;

    if (ed == NULL || ed->tabs.v.len == 0U || ed->tabs.active < 0 ||
        (size_t)ed->tabs.active >= ed->tabs.v.len)
        return false;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        Tab *item = &ed->tabs.v.data[tab];
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;
        bool focus_live = false;

        if (item->root == NULL || item->focus == NULL)
            return false;
        yew_pane_collect_leaves(item->root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        if (n == 0U)
            return false;
        for (i = 0U; i < n; i++) {
            if (leaves[i] == NULL || !leaves[i]->is_leaf ||
                leaves[i]->win == NULL)
                return false;
            if (leaves[i] == item->focus)
                focus_live = true;
        }
        if (!focus_live)
            return false;
    }
    active = &ed->tabs.v.data[ed->tabs.active];
    return ed->pane_root == active->root && ed->focus == active->focus &&
           ed->focus != NULL && ed->win == ed->focus->win;
}

static bool live_teardown(Ed *ed, char *why, size_t why_cap)
{
    bool handled = false;
    bool views_valid;

    /* Direct teardown helpers bypass the normal command-dispatch barrier.
     * Close a typing transaction exactly as a user-issued mode/close command
     * would before any scratch buffer can be released. */
    yew_ed_insert_barrier(ed);
    if (yew_fuss_active(ed) && ed->win != NULL && ed->win->buf != NULL)
        (void)yew_fuss_commit_close(ed, ed->win->buf, &handled);
    if (ed->cmdline.active)
        yew_cmdline_close(ed, false);
    if (ed->prompt != YEW_PROMPT_NONE)
        (void)yew_mode_escape(ed);
    if (yew_fuss_active(ed)) {
        if (ed->mode == YEW_MODE_F)
            (void)yew_mode_enter(ed, YEW_MODE_L);
        else
            yew_fuss_mode_leave(ed);
    }
    if (ed->mode != YEW_MODE_L)
        (void)yew_mode_enter(ed, YEW_MODE_L);
    views_valid = live_views_valid(ed);
    if (ed->mode != YEW_MODE_L || yew_fuss_active(ed) ||
        !views_valid ||
        ed->keys.n == 0U || ed->keys.l[0] != &ed->mode_keys[YEW_MODE_L] ||
        (ed->keys.n >= 3U && ed->keys.l[2] != &ed->bind_keys[YEW_MODE_L]) ||
        ed->chord.n != 0U || ed->chord.layer != -1 ||
        ed->chord.count_given || ed->chord.deadline != 0) {
        (void)snprintf(why, why_cap,
                       "F teardown mode=%u active=%d views=%d "
                       "keys=%u base=%d bind=%d chord=%u/%d/%d/%lld",
                       (unsigned)ed->mode, yew_fuss_active(ed) ? 1 : 0,
                       views_valid ? 1 : 0, (unsigned)ed->keys.n,
                       ed->keys.n != 0U &&
                               ed->keys.l[0] == &ed->mode_keys[YEW_MODE_L] ?
                           1 : 0,
                       ed->keys.n < 3U ||
                               ed->keys.l[2] == &ed->bind_keys[YEW_MODE_L] ?
                           1 : 0,
                       (unsigned)ed->chord.n, (int)ed->chord.layer,
                       ed->chord.count_given ? 1 : 0,
                       (long long)ed->chord.deadline);
        return false;
    }
    return true;
}

static bool check_live_fuss(const u8 *data, size_t len, char *why,
                            size_t why_cap)
{
    LiveSpawn spawn = {1000U, 0U};
    Ed ed;
    size_t count = len < FUSS_FUZZ_LIVE_KEYS ? len : FUSS_FUZZ_LIVE_KEYS;
    size_t at;
    bool ok;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        yew_ed_free(&ed);
        (void)snprintf(why, why_cap, "live editor initialization failed");
        return false;
    }
    yew_git_test_spawn_set(live_spawn, &spawn);
    if (yew_mode_enter(&ed, YEW_MODE_F) != YEW_CMD_OK ||
        !yew_fuss_active(&ed) || ed.mode != YEW_MODE_F) {
        yew_git_test_spawn_set(NULL, NULL);
        yew_ed_free(&ed);
        (void)snprintf(why, why_cap, "could not enter live F mode");
        return false;
    }
    for (at = 0U; at < count; at++) {
        Key key = live_key(data[at]);

        yew_ed_handle_key(&ed, key, 1000 + (i64)at * 17);
        yew_fuss_tick(&ed, 1000 + (i64)at * 17);
        if (ed.jobs.len != 0U) {
            (void)snprintf(why, why_cap,
                           "live F key spawned a real subprocess at byte %zu",
                           at);
            yew_git_test_spawn_set(NULL, NULL);
            yew_ed_free(&ed);
            return false;
        }
        if (!yew_fuss_active(&ed) && at + 1U < count &&
            yew_mode_enter(&ed, YEW_MODE_F) != YEW_CMD_OK) {
            (void)snprintf(why, why_cap,
                           "could not re-enter F mode after byte %zu", at);
            yew_git_test_spawn_set(NULL, NULL);
            yew_ed_free(&ed);
            return false;
        }
    }
    ok = live_teardown(&ed, why, why_cap);
    yew_git_test_spawn_set(NULL, NULL);
    yew_ed_free(&ed);
    return ok;
}

static bool check_fuss(const u8 *data, size_t len, char *why,
                       size_t why_cap)
{
    FussFixture fixture;
    FussOpts opts = {true, true};
    FussTree tree;
    FussOpenMemory manual;
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
    yew_fuss_open_memory_init(&manual);
    yew_fuss_build(&tree, &fixture.snap, &opts);
    yew_fuss_apply_expansion(&tree, &manual, NULL, 0U);
    if (!tree_valid(&tree, why, why_cap))
        goto done;
    if (!layout_valid(0U, yew_fuss_tree_natural_width(&tree),
                      why, why_cap))
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
        else if (op == 4U) {
            if (row >= 0 && (size_t)row < tree.items.len) {
                const FussItem *item = &tree.items.data[row];
                const FussNode *node = &tree.nodes.data[item->node];

                if (!node->is_file && !node->expanded) {
                    (void)yew_fuss_open_memory_set(&manual, item->path,
                                                    item->path_len, true);
                    yew_fuss_apply_expansion(&tree, &manual, NULL, 0U);
                    row = yew_fuss_row_of(&tree, &sel);
                } else {
                    row = yew_fuss_nav_enter(&tree, row);
                }
            }
        }
        else if (op == 5U) {
            if (row >= 0 && (size_t)row < tree.items.len) {
                const FussItem *item = &tree.items.data[row];
                const FussNode *node = &tree.nodes.data[item->node];

                if (!node->is_file) {
                    bool remembered = yew_fuss_open_memory_has(
                        &manual, item->path, item->path_len);

                    (void)yew_fuss_open_memory_set(&manual, item->path,
                                                    item->path_len,
                                                    !remembered);
                    yew_fuss_apply_expansion(&tree, &manual, NULL, 0U);
                }
            }
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
        } else
            yew_fuss_apply_expansion(&tree, &manual, NULL, 0U);
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
    {
        u16 content = len == 0U ? 0U : (u16)((u16)data[0] << 8U);
        u16 random_natural = 0U;
        u16 measured = yew_fuss_tree_natural_width(&tree);

        if (len > 1U)
            content = (u16)(content | data[1]);
        if (len > 2U)
            random_natural = (u16)((u16)data[2] << 8U);
        if (len > 3U)
            random_natural = (u16)(random_natural | data[3]);
        if (!layout_valid(content, measured, why, why_cap) ||
            !layout_valid((u16)~content, random_natural, why, why_cap))
            goto done;
    }
    ok = true;
done:
    yew_fuss_sel_clear(&sel);
    yew_fuss_open_memory_drop(&manual);
    yew_fuss_tree_drop(&tree);
    fixture_drop(&fixture);
    if (!ok)
        return false;
    /* The Sprint 52 gate is explicitly 20k live sequences: retain the model
     * oracle above and drive every generated input through the editor too. */
    return check_live_fuss(data, len, why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_fuss", NULL, check_fuss);
}
