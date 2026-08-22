#include "harness.h"

#include <string.h>

#include "mod/git/gutter.h"

static u32 oracle_distance(const u64 *left, u32 left_n,
                           const u64 *right, u32 right_n)
{
    u32 lcs[41][41] = {{0}};
    u32 i;
    u32 j;

    for (i = 1U; i <= left_n; i++) {
        for (j = 1U; j <= right_n; j++) {
            lcs[i][j] = left[i - 1U] == right[j - 1U] ?
                        lcs[i - 1U][j - 1U] + 1U :
                        lcs[i - 1U][j] > lcs[i][j - 1U] ?
                        lcs[i - 1U][j] : lcs[i][j - 1U];
        }
    }
    return left_n + right_n - 2U * lcs[left_n][right_n];
}

static u32 hunk_distance(const GitHunkVec *hunks)
{
    u64 total = 0U;
    size_t i;

    for (i = 0U; i < hunks->len; i++)
        total += hunks->data[i].base_n.v + hunks->data[i].buf_n.v;
    YEW_ASSERT(total <= UINT32_MAX);
    return (u32)total;
}

static u64 rng_next(u64 *state)
{
    u64 x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

void test_git_line_hash_and_split(void)
{
    Arena arena;
    u64 *hashes;
    u32 count;
    bool missing;
    static const u8 mixed[] = "a\r\nb\nlast";
    static const u8 complete[] = "a\nb\n";

    arena_init(&arena);
    YEW_ASSERT(yew_git_hash_lines(mixed, sizeof(mixed) - 1U, &arena,
                                  &hashes, &count, &missing));
    YEW_ASSERT_EQ_U64(count, 3U);
    YEW_ASSERT(missing);
    YEW_ASSERT_EQ_U64(hashes[0], yew_git_line_hash((const u8 *)"a\r\n", 3U));
    YEW_ASSERT_EQ_U64(hashes[1], yew_git_line_hash((const u8 *)"b\n", 2U));
    YEW_ASSERT_EQ_U64(hashes[2], yew_git_line_hash((const u8 *)"last", 4U));
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_git_hash_lines(complete, sizeof(complete) - 1U, &arena,
                                  &hashes, &count, &missing));
    YEW_ASSERT_EQ_U64(count, 2U);
    YEW_ASSERT(!missing);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_git_hash_lines(NULL, 0U, &arena, &hashes, &count,
                                  &missing));
    YEW_ASSERT_EQ_U64(count, 0U);
    YEW_ASSERT_NULL(hashes);
    YEW_ASSERT(!missing);
    arena_free_all(&arena);
}

void test_git_diff_basic_shapes(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    u64 left[] = {1U, 2U, 3U};
    u64 modified[] = {1U, 4U, 3U};
    u64 added[] = {1U, 2U, 5U, 3U};
    u64 deleted[] = {1U, 3U};

    arena_init(&arena);
    YEW_ASSERT(yew_diff_lines(&arena, left, 3U, modified, 3U, 8U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_n.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_n.v, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_MOD);

    YEW_ASSERT(yew_diff_lines(&arena, left, 3U, added, 4U, 8U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 2U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 2U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_n.v, 0U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_n.v, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_ADD);

    YEW_ASSERT(yew_diff_lines(&arena, left, 3U, deleted, 2U, 8U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_n.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_n.v, 0U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_DEL);
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
}

void test_git_diff_matches_random_minimal_oracle(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    u64 left[40];
    u64 right[40];
    u64 seed = UINT64_C(0x53d1ff53c0ffee11);
    u32 trial;

    arena_init(&arena);
    for (trial = 0U; trial < 5000U; trial++) {
        u32 left_n = (u32)(rng_next(&seed) % 41U);
        u32 right_n = (u32)(rng_next(&seed) % 41U);
        u32 i;
        u32 expected;

        for (i = 0U; i < left_n; i++)
            left[i] = rng_next(&seed) % 9U;
        for (i = 0U; i < right_n; i++)
            right[i] = rng_next(&seed) % 9U;
        expected = oracle_distance(left, left_n, right, right_n);
        YEW_ASSERT(yew_diff_lines(&arena, left, left_n, right, right_n,
                                  80U, &hunks));
        YEW_ASSERT_EQ_U64(hunk_distance(&hunks), expected);
        for (i = 1U; i < hunks.len; i++) {
            YEW_ASSERT(hunks.data[i - 1U].buf_lo.v <=
                       hunks.data[i].buf_lo.v);
            YEW_ASSERT(hunks.data[i - 1U].buf_lo.v +
                       hunks.data[i - 1U].buf_n.v <=
                       hunks.data[i].buf_lo.v);
        }
    }
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
}

void test_git_diff_budget_and_raw_terminators(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    u64 left[12];
    u64 right[12];
    u32 i;
    u64 lf = yew_git_line_hash((const u8 *)"x\n", 2U);
    u64 crlf = yew_git_line_hash((const u8 *)"x\r\n", 3U);

    for (i = 0U; i < YEW_ARRAY_LEN(left); i++) {
        left[i] = i + 1U;
        right[i] = 100U + i;
    }
    arena_init(&arena);
    YEW_ASSERT(!yew_diff_lines(&arena, left, 12U, right, 12U, 8U,
                               &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 0U);
    YEW_ASSERT(yew_diff_lines(&arena, &lf, 1U, &crlf, 1U, 2U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_MOD);
    YEW_ASSERT_EQ_U64(hunk_distance(&hunks), 2U);
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
}

void test_git_diff_identical_large_is_allocation_free_path(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    u64 lines[10000];
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(lines); i++)
        lines[i] = i;
    arena_init(&arena);
    YEW_ASSERT(yew_diff_lines(&arena, lines, YEW_ARRAY_LEN(lines), lines,
                              YEW_ARRAY_LEN(lines), 0U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 0U);
    YEW_ASSERT_NULL(hunks.data);
    arena_free_all(&arena);
}
