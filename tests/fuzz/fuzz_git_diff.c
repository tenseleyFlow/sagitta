/* Sprint 53: arbitrary line sequences must produce a minimal, replayable,
 * deterministic hunk list. */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/git/gutter.h"

enum { GIT_DIFF_FUZZ_MAX_LINES = 64 };

static u32 min3(u32 a, u32 b, u32 c)
{
    u32 result = a < b ? a : b;

    return result < c ? result : c;
}

static u32 oracle_distance(const u64 *left, u32 left_n,
                           const u64 *right, u32 right_n)
{
    u32 prev[GIT_DIFF_FUZZ_MAX_LINES + 1U];
    u32 curr[GIT_DIFF_FUZZ_MAX_LINES + 1U];
    u32 i;
    u32 j;

    for (j = 0U; j <= right_n; j++)
        prev[j] = j;
    for (i = 1U; i <= left_n; i++) {
        curr[0] = i;
        for (j = 1U; j <= right_n; j++) {
            if (left[i - 1U] == right[j - 1U])
                curr[j] = prev[j - 1U];
            else
                curr[j] = min3(prev[j] + 1U, curr[j - 1U] + 1U,
                               prev[j - 1U] + 2U);
        }
        (void)memcpy(prev, curr, (right_n + 1U) * sizeof(prev[0]));
    }
    return prev[right_n];
}

static bool hunks_equal(const GitHunkVec *a, const GitHunkVec *b)
{
    size_t i;

    if (a->len != b->len)
        return false;
    for (i = 0U; i < a->len; i++) {
        const GitHunk *x = &a->data[i];
        const GitHunk *y = &b->data[i];

        if (x->base_lo.v != y->base_lo.v || x->base_n.v != y->base_n.v ||
            x->buf_lo.v != y->buf_lo.v || x->buf_n.v != y->buf_n.v ||
            x->kind != y->kind)
            return false;
    }
    return true;
}

static bool validate_hunks(const u64 *left, u32 left_n,
                           const u64 *right, u32 right_n,
                           const GitHunkVec *hunks,
                           char *why, size_t why_cap)
{
    u32 left_at = 0U;
    u32 right_at = 0U;
    u32 edits = 0U;
    size_t i;

    for (i = 0U; i < hunks->len; i++) {
        const GitHunk *h = &hunks->data[i];
        u32 equal_n;
        u32 j;

        if (h->base_lo.v < left_at || h->buf_lo.v < right_at ||
            h->base_lo.v > left_n || h->buf_lo.v > right_n ||
            h->base_n.v > left_n - h->base_lo.v ||
            h->buf_n.v > right_n - h->buf_lo.v) {
            (void)snprintf(why, why_cap, "hunk %zu is out of bounds", i);
            return false;
        }
        if (h->base_lo.v - left_at != h->buf_lo.v - right_at) {
            (void)snprintf(why, why_cap,
                           "hunk %zu leaves unequal equality spans", i);
            return false;
        }
        equal_n = h->base_lo.v - left_at;
        for (j = 0U; j < equal_n; j++) {
            if (left[left_at + j] != right[right_at + j]) {
                (void)snprintf(why, why_cap,
                               "hunk %zu skips a differing line", i);
                return false;
            }
        }
        if ((h->kind == YEW_HUNK_ADD &&
             (h->base_n.v != 0U || h->buf_n.v == 0U)) ||
            (h->kind == YEW_HUNK_DEL &&
             (h->base_n.v == 0U || h->buf_n.v != 0U)) ||
            (h->kind == YEW_HUNK_MOD &&
             (h->base_n.v == 0U || h->buf_n.v == 0U))) {
            (void)snprintf(why, why_cap, "hunk %zu kind disagrees with span",
                           i);
            return false;
        }
        edits += h->base_n.v + h->buf_n.v;
        left_at = h->base_lo.v + h->base_n.v;
        right_at = h->buf_lo.v + h->buf_n.v;
    }
    if (left_n - left_at != right_n - right_at) {
        (void)snprintf(why, why_cap, "tail equality spans have unequal length");
        return false;
    }
    while (left_at < left_n) {
        if (left[left_at++] != right[right_at++]) {
            (void)snprintf(why, why_cap, "tail marked equal is different");
            return false;
        }
    }
    if (edits != oracle_distance(left, left_n, right, right_n)) {
        (void)snprintf(why, why_cap,
                       "hunk edit length %u is not oracle-minimal", edits);
        return false;
    }
    return true;
}

static void make_sequences(const u8 *data, size_t len,
                           u64 *left, u32 *left_n,
                           u64 *right, u32 *right_n)
{
    size_t at = 0U;
    u32 i;

    *left_n = len == 0U ? 0U : data[at++] %
              (GIT_DIFF_FUZZ_MAX_LINES + 1U);
    *right_n = len < 2U ? 0U : data[at++] %
               (GIT_DIFF_FUZZ_MAX_LINES + 1U);
    for (i = 0U; i < *left_n; i++) {
        u8 byte = len == 0U ? 0U : data[at++ % len];

        left[i] = (u64)(byte & 15U);
    }
    for (i = 0U; i < *right_n; i++) {
        u8 byte = len == 0U ? 0U : data[at++ % len];

        right[i] = (u64)(byte & 15U);
    }
}

static bool check_git_diff(const u8 *data, size_t len,
                           char *why, size_t why_cap)
{
    u64 left[GIT_DIFF_FUZZ_MAX_LINES];
    u64 right[GIT_DIFF_FUZZ_MAX_LINES];
    u32 left_n;
    u32 right_n;
    Arena first_arena;
    Arena second_arena;
    GitHunkVec first = {0};
    GitHunkVec second = {0};
    bool ok;

    make_sequences(data, len, left, &left_n, right, &right_n);
    arena_init(&first_arena);
    arena_init(&second_arena);
    ok = yew_diff_lines(&first_arena, left, left_n, right, right_n,
                        left_n + right_n, &first) &&
         yew_diff_lines(&second_arena, left, left_n, right, right_n,
                        left_n + right_n, &second) &&
         hunks_equal(&first, &second) &&
         validate_hunks(left, left_n, right, right_n, &first,
                        why, why_cap);
    if (!ok && why[0] == '\0')
        (void)snprintf(why, why_cap,
                       "diff failed or differed across identical runs");
    GitHunkVec_free(&second);
    GitHunkVec_free(&first);
    arena_free_all(&second_arena);
    arena_free_all(&first_arena);
    return ok;
}

static bool check_budget_abort(char *why, size_t why_cap)
{
    static const u64 left[] = {1U, 2U, 3U, 4U};
    static const u64 right[] = {5U, 6U, 7U, 8U};
    Arena arena;
    GitHunkVec hunks = {0};
    bool aborted;

    arena_init(&arena);
    aborted = !yew_diff_lines(&arena, left, YEW_ARRAY_LEN(left), right,
                              YEW_ARRAY_LEN(right), 3U, &hunks) &&
              hunks.len == 0U;
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
    if (!aborted)
        (void)snprintf(why, why_cap,
                       "edit-distance budget produced a partial diff");
    return aborted;
}

int main(int argc, char **argv)
{
    static const u8 fixed[] = {
        6U, 7U, 1U, 2U, 3U, 2U, 4U, 5U,
        1U, 9U, 2U, 3U, 8U, 4U, 5U
    };
    char why[256] = {0};

    if (!check_budget_abort(why, sizeof(why)) ||
        !check_git_diff(NULL, 0U, why, sizeof(why)) ||
        !check_git_diff(fixed, sizeof(fixed), why, sizeof(why))) {
        (void)fprintf(stderr, "fuzz_git_diff: fixed case failed: %s\n", why);
        return 1;
    }
    return yew_fuzz_main(argc, argv, "fuzz_git_diff", NULL,
                         check_git_diff);
}
