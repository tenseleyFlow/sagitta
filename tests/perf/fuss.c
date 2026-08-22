#define _POSIX_C_SOURCE 200809L

/* Sprint 52: FUSS tree work must remain below the keypress budget. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod/git/fusstree.h"
#include "util/base.h"

enum {
    FUSS_PERF_ENTRIES = 20000,
    FUSS_PERF_PATH_CAP = 48,
    FUSS_PERF_BUILD_SAMPLES = 9,
    FUSS_PERF_KEY_SAMPLES = 2001,
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

int main(void)
{
    PerfFixture fixture;
    u64 build_median = 0U;
    u64 nav_p99 = 0U;
    u64 toggle_p99 = 0U;
    u64 unchanged = 0U;
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
    (void)printf("fuss build+flatten 20000  %.3f ms (limit 12.000 ms)\n",
                 (double)build_median / 1000000.0);
    (void)printf("fuss navigation p99       %.3f ms (limit 5.000 ms)\n",
                 (double)nav_p99 / 1000000.0);
    (void)printf("fuss toggle+flatten p99    %.3f ms (limit 5.000 ms)\n",
                 (double)toggle_p99 / 1000000.0);
    (void)printf("fuss unchanged-gen x5000  %.3f ms (zero rebuilds)\n",
                 (double)unchanged / 1000000.0);
    (void)printf("fuss collapse refresh x100 stable (7 paths)\n");
    if (build_median > FUSS_PERF_BUILD_BUDGET_NS ||
        nav_p99 > FUSS_PERF_KEY_BUDGET_NS ||
        toggle_p99 > FUSS_PERF_KEY_BUDGET_NS)
        status = 1;
    fixture_drop(&fixture);
    return status;
}
