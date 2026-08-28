#define _POSIX_C_SOURCE 200809L

/* Sprint 52: FUSS tree work must remain below the keypress budget. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "mod/git/fussmode.h"
#include "mod/git/fusstree.h"
#include "mod/git/git_int.h"
#include "util/base.h"

enum {
    FUSS_PERF_ENTRIES = 20000,
    FUSS_PERF_PATH_CAP = 48,
    FUSS_PERF_BUILD_SAMPLES = 9,
    FUSS_PERF_KEY_SAMPLES = 2001,
    FUSS_PERF_DRAWER_KEYS = 1000,
    FUSS_PERF_UNCHANGED_CALLS = 5000,
    FUSS_PERF_REFRESHES = 100,
    FUSS_PERF_COLLAPSED = 7,
    FUSS_PERF_BUILD_BUDGET_NS = 12000000,
    FUSS_PERF_KEY_BUDGET_NS = 5000000
};

typedef struct PerfFixture {
    GitSnapshot snap;
    GitEntry *entries;
    char *paths;
} PerfFixture;

static volatile u64 fuss_perf_sink;

static bool perf_advisory(void)
{
    const char *value = getenv("YEW_PERF_ADVISORY");

    return value != NULL && strcmp(value, "0") != 0;
}

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_fuss: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static void sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at > 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool fixture_make(PerfFixture *f)
{
    u32 i;

    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->snap.a);
    f->entries = calloc(FUSS_PERF_ENTRIES, sizeof(*f->entries));
    f->paths = malloc((size_t)FUSS_PERF_ENTRIES * FUSS_PERF_PATH_CAP);
    if (f->entries == NULL || f->paths == NULL)
        return false;
    for (i = 0U; i < FUSS_PERF_ENTRIES; i++) {
        GitEntry *entry = &f->entries[i];
        char *path = f->paths + (size_t)i * FUSS_PERF_PATH_CAP;
        int wrote = snprintf(path, FUSS_PERF_PATH_CAP,
                             "d%03u/sub%03u/file-%05u.c",
                             i / 100U, i / 20U, i);

        if (wrote <= 0 || wrote >= FUSS_PERF_PATH_CAP)
            return false;
        entry->kind = GIT_E_ORDINARY;
        entry->path = path;
        entry->path_len = (u32)wrote;
        entry->staged = (i & 1U) == 0U;
        entry->unstaged = (i & 2U) != 0U;
        entry->untracked = (i & 4U) != 0U;
        entry->incoming = (i & 8U) != 0U;
        entry->conflicted = i % 127U == 0U;
    }
    f->snap.entries.data = f->entries;
    f->snap.entries.len = FUSS_PERF_ENTRIES;
    f->snap.gen = 7U;
    return true;
}

static void fixture_drop(PerfFixture *f)
{
    free(f->paths);
    free(f->entries);
    arena_free_all(&f->snap.a);
}

static bool measure_build(const PerfFixture *fixture, u64 *median)
{
    const FussOpts opts = {true, true};
    u64 samples[FUSS_PERF_BUILD_SAMPLES];
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(samples); i++) {
        FussTree tree;
        u64 start;
        u64 end;

        yew_fuss_tree_init(&tree);
        start = now_ns();
        yew_fuss_build(&tree, &fixture->snap, &opts);
        end = now_ns();
        if (start == 0U || end == 0U || tree.items.len < FUSS_PERF_ENTRIES) {
            yew_fuss_tree_drop(&tree);
            return false;
        }
        samples[i] = end - start;
        fuss_perf_sink ^= tree.nodes.len + tree.items.len;
        yew_fuss_tree_drop(&tree);
    }
    sort_u64(samples, YEW_ARRAY_LEN(samples));
    *median = samples[YEW_ARRAY_LEN(samples) / 2U];
    return true;
}

static i32 first_directory(const FussTree *tree)
{
    size_t i;

    for (i = 0U; i < tree->items.len; i++)
        if (!tree->items.data[i].is_file)
            return (i32)i;
    return -1;
}

static i32 last_expanded_directory(const FussTree *tree)
{
    size_t i = tree->items.len;

    while (i != 0U) {
        const FussItem *item = &tree->items.data[--i];

        if (!item->is_file && tree->nodes.data[item->node].expanded)
            return (i32)i;
    }
    return -1;
}

static bool collapsed_equal(char *const *a, u32 an,
                            char *const *b, u32 bn)
{
    u32 i;

    if (an != bn)
        return false;
    for (i = 0U; i < an; i++)
        if (strcmp(a[i], b[i]) != 0)
            return false;
    return true;
}

static bool check_refresh_stability(const PerfFixture *fixture)
{
    const FussOpts opts = {true, true};
    GitSnapshot snap = fixture->snap;
    Arena expected_arena;
    FussTree tree;
    char **expected = NULL;
    u32 expected_n;
    u32 i;
    bool ok = false;

    yew_fuss_tree_init(&tree);
    yew_fuss_build(&tree, &snap, &opts);
    for (i = 0U; i < FUSS_PERF_COLLAPSED; i++) {
        i32 row = last_expanded_directory(&tree);

        if (row < 0 || !yew_fuss_nav_toggle(&tree, row))
            goto done_tree;
    }
    arena_init(&expected_arena);
    expected_n = yew_fuss_harvest_collapsed(&tree, &expected_arena,
                                             &expected);
    if (expected_n != FUSS_PERF_COLLAPSED)
        goto done_expected;
    for (i = 0U; i < FUSS_PERF_REFRESHES; i++) {
        Arena current_arena;
        char **current = NULL;
        u32 current_n;

        snap.gen++;
        yew_fuss_build(&tree, &snap, &opts);
        arena_init(&current_arena);
        current_n = yew_fuss_harvest_collapsed(&tree, &current_arena,
                                                &current);
        if (!collapsed_equal(expected, expected_n, current, current_n)) {
            arena_free_all(&current_arena);
            goto done_expected;
        }
        arena_free_all(&current_arena);
    }
    ok = true;
done_expected:
    arena_free_all(&expected_arena);
done_tree:
    yew_fuss_tree_drop(&tree);
    return ok;
}

static bool measure_keys(const PerfFixture *fixture, u64 *nav_p99,
                         u64 *toggle_p99)
{
    const FussOpts opts = {true, true};
    u64 nav[FUSS_PERF_KEY_SAMPLES];
    u64 toggles[33];
    FussTree tree;
    i32 row;
    size_t i;

    yew_fuss_tree_init(&tree);
    yew_fuss_build(&tree, &fixture->snap, &opts);
    row = first_directory(&tree);
    if (row < 0) {
        yew_fuss_tree_drop(&tree);
        return false;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(nav); i++) {
        u64 start = now_ns();

        /* Model the 500 ms status tick landing on the same frame as the
         * keypress.  An unchanged generation must be constant-time. */
        yew_fuss_build(&tree, &fixture->snap, &opts);
        switch (i % 4U) {
        case 0U: row = yew_fuss_nav_step(&tree, row, 1); break;
        case 1U: row = yew_fuss_nav_step(&tree, row, -1); break;
        case 2U: row = yew_fuss_nav_raw(&tree, row, 1); break;
        default: row = yew_fuss_nav_parent(&tree, row); break;
        }
        nav[i] = now_ns() - start;
        fuss_perf_sink ^= (u64)(row + 1);
    }
    for (i = 0U; i < YEW_ARRAY_LEN(toggles); i++) {
        u64 start;

        row = first_directory(&tree);
        if (row < 0) {
            yew_fuss_tree_drop(&tree);
            return false;
        }
        start = now_ns();
        yew_fuss_build(&tree, &fixture->snap, &opts);
        if (!yew_fuss_nav_toggle(&tree, row)) {
            yew_fuss_tree_drop(&tree);
            return false;
        }
        toggles[i] = now_ns() - start;
        fuss_perf_sink ^= tree.items.len;
    }
    sort_u64(nav, YEW_ARRAY_LEN(nav));
    sort_u64(toggles, YEW_ARRAY_LEN(toggles));
    *nav_p99 = nav[(YEW_ARRAY_LEN(nav) * 99U + 99U) / 100U - 1U];
    *toggle_p99 = toggles[(YEW_ARRAY_LEN(toggles) * 99U + 99U) / 100U - 1U];
    yew_fuss_tree_drop(&tree);
    return true;
}

static bool measure_unchanged(const PerfFixture *fixture, u64 *elapsed)
{
    const FussOpts opts = {true, true};
    FussTree tree;
    FussNode *nodes;
    FussItem *items;
    size_t node_len;
    size_t item_len;
    u16 saved_depth;
    u64 start;
    u32 i;

    yew_fuss_tree_init(&tree);
    yew_fuss_build(&tree, &fixture->snap, &opts);
    nodes = tree.nodes.data;
    items = tree.items.data;
    node_len = tree.nodes.len;
    item_len = tree.items.len;
    if (item_len == 0U) {
        yew_fuss_tree_drop(&tree);
        return false;
    }
    /* Re-flattening normally reuses `items.data`, so stable pointers alone
     * are insufficient evidence.  The sentinel survives only when flatten
     * is skipped altogether. */
    saved_depth = tree.items.data[0].depth;
    tree.items.data[0].depth = UINT16_MAX;
    start = now_ns();
    for (i = 0U; i < FUSS_PERF_UNCHANGED_CALLS; i++)
        yew_fuss_build(&tree, &fixture->snap, &opts);
    *elapsed = now_ns() - start;
    if (tree.nodes.data != nodes || tree.items.data != items ||
        tree.nodes.len != node_len || tree.items.len != item_len ||
        tree.items.data[0].depth != UINT16_MAX) {
        yew_fuss_tree_drop(&tree);
        return false;
    }
    tree.items.data[0].depth = saved_depth;
    fuss_perf_sink ^= tree.snap_gen;
    yew_fuss_tree_drop(&tree);
    return true;
}

static u32 drawer_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                        const GitReq *req, void *opaque, char *err,
                        size_t errsz)
{
    u32 *next = opaque;

    (void)ed;
    (void)verb;
    (void)argv;
    (void)req;
    (void)err;
    (void)errsz;
    return ++*next;
}

static bool drawer_root_make(char *root, size_t root_cap)
{
    const char *tmp = getenv("TMPDIR");
    char resolved[PATH_MAX];
    size_t len;
    int wrote;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "build/tmp";
    wrote = snprintf(root, root_cap, "%s/yew-perf-fuss-XXXXXX", tmp);
    if (wrote <= 0 || (size_t)wrote >= root_cap || mkdtemp(root) == NULL)
        return false;
    if (realpath(root, resolved) == NULL) {
        (void)rmdir(root);
        return false;
    }
    len = strlen(resolved);
    if (len >= root_cap) {
        (void)rmdir(root);
        errno = ENAMETOOLONG;
        return false;
    }
    (void)memcpy(root, resolved, len + 1U);
    return true;
}

static bool drawer_snapshot_fill(Ed *ed)
{
    GitSnapshot *snap = yew_git_test_snapshot_mut(ed);
    GitEntry *entries;
    u32 i;

    if (snap == NULL)
        return false;
    entries = arena_alloc(&snap->a,
                          (size_t)FUSS_PERF_ENTRIES * sizeof(*entries),
                          _Alignof(GitEntry));
    if (entries == NULL)
        return false;
    (void)memset(entries, 0,
                 (size_t)FUSS_PERF_ENTRIES * sizeof(*entries));
    for (i = 0U; i < FUSS_PERF_ENTRIES; i++) {
        char path[32];
        int wrote = snprintf(path, sizeof(path), "file-%05u.c", i);

        if (wrote <= 0 || (size_t)wrote >= sizeof(path))
            return false;
        entries[i].kind = GIT_E_ORDINARY;
        entries[i].path = arena_strndup(&snap->a, path, (size_t)wrote);
        if (entries[i].path == NULL)
            return false;
        entries[i].path_len = (u32)wrote;
        entries[i].unstaged = true;
    }
    snap->state = YEW_GIT_OK;
    snap->entries.data = entries;
    snap->entries.len = FUSS_PERF_ENTRIES;
    snap->gen++;
    return true;
}

static bool measure_drawer(u64 *entry_ns, u64 *input_p99,
                           u64 *open_ns)
{
    char root[PATH_MAX];
    char selected[PATH_MAX];
    u64 samples[FUSS_PERF_DRAWER_KEYS];
    CmdCtx cx = {0};
    Ed ed;
    FILE *file = NULL;
    u32 next_job = 0U;
    u64 start;
    size_t i;
    bool initialized = false;
    bool ok = false;

    if (!drawer_root_make(root, sizeof(root)) ||
        snprintf(selected, sizeof(selected), "%s/file-19999.c", root) <= 0)
        return false;
    file = fopen(selected, "wb");
    if (file == NULL || fputs("drawer open target\n", file) < 0 ||
        fclose(file) != 0)
        goto done;
    file = NULL;
    yew_ed_init(&ed);
    initialized = true;
    if (!yew_ed_open_scratch(&ed))
        goto done;
    ed.ws.dir = arena_strdup(&ed.arena, root);
    if (ed.ws.dir == NULL || !drawer_snapshot_fill(&ed) ||
        !yew_grid_init(&ed.grid, &ed.interner, 24U, 80U))
        goto done;
    ed.grid_ready = true;
    ed.tab_strip_rect = (Rect){0U, 0U, 80U, 1U};
    ed.footer_rect = (Rect){0U, 22U, 80U, 2U};
    ed.win->rect = (Rect){0U, 1U, 80U, 21U};
    yew_git_test_spawn_set(drawer_spawn, &next_job);
    start = now_ns();
    if (yew_mode_enter(&ed, YEW_MODE_F) != YEW_CMD_OK)
        goto done_spawn;
    *entry_ns = now_ns() - start;
    yew_fuss_draw(&ed);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    for (i = 0U; i < YEW_ARRAY_LEN(samples); i++) {
        ed.full_damage = false;
        ed.layout_dirty = false;
        ed.footer_dirty = false;
        start = now_ns();
        if (yew_fuss_cmd_nav_row_next(&cx) != YEW_CMD_OK)
            goto done_spawn;
        samples[i] = now_ns() - start;
        if (ed.full_damage || ed.layout_dirty || !ed.footer_dirty ||
            !yew_fuss_draw_dirty(&ed))
            goto done_spawn;
        yew_fuss_draw(&ed);
    }
    sort_u64(samples, YEW_ARRAY_LEN(samples));
    *input_p99 = samples[(YEW_ARRAY_LEN(samples) * 99U + 99U) / 100U - 1U];
    cx.sarg = "file-19999.c";
    cx.sarg_len = 12U;
    start = now_ns();
    if (yew_fuss_cmd_open(&cx) != YEW_CMD_OK)
        goto done_spawn;
    *open_ns = now_ns() - start;
    ok = ed.mode == YEW_MODE_L && ed.win != NULL && ed.win->buf != NULL &&
         ed.win->buf->meta.realpath != NULL &&
         strcmp(ed.win->buf->meta.realpath, selected) == 0;
done_spawn:
    yew_git_test_spawn_set(NULL, NULL);
done:
    if (file != NULL)
        (void)fclose(file);
    if (initialized)
        yew_ed_free(&ed);
    (void)unlink(selected);
    (void)rmdir(root);
    return ok;
}

int main(void)
{
    PerfFixture fixture;
    u64 build_median = 0U;
    u64 nav_p99 = 0U;
    u64 toggle_p99 = 0U;
    u64 unchanged = 0U;
    u64 drawer_entry = 0U;
    u64 drawer_input_p99 = 0U;
    u64 drawer_open = 0U;
    bool advisory = perf_advisory();
    int status = 0;

    if (!fixture_make(&fixture)) {
        (void)fputs("perf_fuss: could not build fixture\n", stderr);
        fixture_drop(&fixture);
        return 2;
    }
    if (!measure_build(&fixture, &build_median)) {
        (void)fputs("perf_fuss: build fixture failed\n", stderr);
        status = 1;
    }
    if (!check_refresh_stability(&fixture)) {
        (void)fputs("perf_fuss: collapsed state drifted across refreshes\n",
                    stderr);
        status = 1;
    }
    if (!measure_keys(&fixture, &nav_p99, &toggle_p99)) {
        (void)fputs("perf_fuss: navigation fixture failed\n", stderr);
        status = 1;
    }
    if (!measure_unchanged(&fixture, &unchanged)) {
        (void)fputs("perf_fuss: unchanged generation rebuilt tree\n", stderr);
        status = 1;
    }
    if (!measure_drawer(&drawer_entry, &drawer_input_p99, &drawer_open)) {
        (void)fputs("perf_fuss: end-to-end drawer fixture failed\n", stderr);
        status = 1;
    }
    (void)printf("fuss build+flatten 20000  %.3f ms (limit 12.000 ms)\n",
                 (double)build_median / 1000000.0);
    (void)printf("fuss navigation p99       %.3f ms (limit 5.000 ms)\n",
                 (double)nav_p99 / 1000000.0);
    (void)printf("fuss toggle+flatten p99    %.3f ms (limit 5.000 ms)\n",
                 (double)toggle_p99 / 1000000.0);
    (void)printf("fuss unchanged-gen x5000  %.3f ms (zero rebuilds)\n",
                 (double)unchanged / 1000000.0);
    (void)printf("fuss collapse refresh x100 stable (7 paths)\n");
    (void)printf("fuss drawer entry 20000   %.3f ms (limit 5.000 ms)%s\n",
                 (double)drawer_entry / 1000000.0,
                 advisory ? " ADVISORY" : "");
    (void)printf("fuss input-to-damage p99  %.3f ms (1000 selections)%s\n",
                 (double)drawer_input_p99 / 1000000.0,
                 advisory ? " ADVISORY" : "");
    (void)printf("fuss open resolve 20000   %.3f ms (limit 5.000 ms)%s\n",
                 (double)drawer_open / 1000000.0,
                 advisory ? " ADVISORY" : "");
    if (build_median > FUSS_PERF_BUILD_BUDGET_NS ||
        nav_p99 > FUSS_PERF_KEY_BUDGET_NS ||
        toggle_p99 > FUSS_PERF_KEY_BUDGET_NS ||
        (!advisory &&
         (drawer_entry > FUSS_PERF_KEY_BUDGET_NS ||
          drawer_input_p99 > FUSS_PERF_KEY_BUDGET_NS ||
          drawer_open > FUSS_PERF_KEY_BUDGET_NS)))
        status = 1;
    fixture_drop(&fixture);
    return status;
}
