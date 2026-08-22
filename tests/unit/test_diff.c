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

static u64 colliding_hash(const u8 *bytes, size_t len)
{
    (void)bytes;
    (void)len;
    return UINT64_C(0x5353535353535353);
}

typedef struct DiffFakeClock {
    i64 now;
    i64 first;
    i64 last;
    u32 calls;
} DiffFakeClock;

static i64 diff_fake_now(void *ctx)
{
    DiffFakeClock *clock = ctx;
    i64 value = clock->now;

    clock->now += 250;
    if (clock->calls == 0U)
        clock->first = value;
    clock->last = value;
    clock->calls++;
    return value;
}

static void assert_same_hunks(const GitHunkVec *left,
                              const GitHunkVec *right)
{
    size_t i;

    YEW_ASSERT_EQ_U64(left->len, right->len);
    for (i = 0U; i < left->len; i++) {
        YEW_ASSERT_EQ_U64(left->data[i].base_lo.v,
                          right->data[i].base_lo.v);
        YEW_ASSERT_EQ_U64(left->data[i].base_n.v,
                          right->data[i].base_n.v);
        YEW_ASSERT_EQ_U64(left->data[i].buf_lo.v,
                          right->data[i].buf_lo.v);
        YEW_ASSERT_EQ_U64(left->data[i].buf_n.v,
                          right->data[i].buf_n.v);
        YEW_ASSERT(left->data[i].kind == right->data[i].kind);
    }
}

static void assert_hunks_align_equal_regions(const u64 *left, u32 left_n,
                                             const u64 *right, u32 right_n,
                                             const GitHunkVec *hunks)
{
    u32 left_at = 0U;
    u32 right_at = 0U;
    size_t h;

    for (h = 0U; h < hunks->len; h++) {
        const GitHunk *cur = &hunks->data[h];
        u32 gap;
        u32 i;

        YEW_ASSERT(cur->base_lo.v >= left_at);
        YEW_ASSERT(cur->buf_lo.v >= right_at);
        gap = (u32)cur->base_lo.v - left_at;
        YEW_ASSERT_EQ_U64(gap, cur->buf_lo.v - right_at);
        for (i = 0U; i < gap; i++)
            YEW_ASSERT_EQ_U64(left[left_at + i], right[right_at + i]);
        left_at = (u32)(cur->base_lo.v + cur->base_n.v);
        right_at = (u32)(cur->buf_lo.v + cur->buf_n.v);
    }
    YEW_ASSERT_EQ_U64(left_n - left_at, right_n - right_at);
    while (left_at < left_n) {
        YEW_ASSERT_EQ_U64(left[left_at], right[right_at]);
        left_at++;
        right_at++;
    }
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
        assert_hunks_align_equal_regions(left, left_n, right, right_n,
                                         &hunks);
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
    YEW_ASSERT(yew_diff_lines(&arena, left, 4U, right, 4U, 8U, &hunks));
    YEW_ASSERT_EQ_U64(hunk_distance(&hunks), 8U);
    YEW_ASSERT(!yew_diff_lines(&arena, left, 4U, right, 4U, 7U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 0U);
    YEW_ASSERT(yew_diff_lines(&arena, left, 4U, right, 5U, 9U, &hunks));
    YEW_ASSERT_EQ_U64(hunk_distance(&hunks), 9U);
    YEW_ASSERT(!yew_diff_lines(&arena, left, 4U, right, 5U, 8U, &hunks));
    YEW_ASSERT_EQ_U64(hunks.len, 0U);
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

void test_git_diff_raw_bytes_resolve_hash_collisions(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    static const u8 left[] = "same\nleft\r\ntail\n";
    static const u8 right[] = "same\nright\ntail\n";

    arena_init(&arena);
    YEW_ASSERT(yew_diff_bytes_with_hash(&arena,
                                        left, sizeof(left) - 1U,
                                        right, sizeof(right) - 1U,
                                        8U, colliding_hash, &hunks) ==
               YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_n.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_n.v, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_MOD);

    YEW_ASSERT(yew_diff_bytes_with_hash(&arena,
                                        left, sizeof(left) - 1U,
                                        left, sizeof(left) - 1U,
                                        0U, colliding_hash, &hunks) ==
               YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 0U);
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
}

void test_git_diff_raw_bytes_preserve_line_terminators(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    static const u8 lf[] = "x\ny\n";
    static const u8 crlf[] = "x\r\ny\n";
    static const u8 base[] = "alpha\nbeta\ngamma\n";
    static const u8 added[] = "alpha\ninserted\nbeta\ngamma\n";
    static const u8 modified[] = "alpha\nchanged\ngamma\n";
    static const u8 deleted[] = "beta\ngamma\n";
    static const u8 deleted_eof[] = "alpha\nbeta\n";
    LineNo sign_line;
    bool delete_below;

    arena_init(&arena);
    YEW_ASSERT(yew_diff_bytes(&arena, lf, sizeof(lf) - 1U,
                              crlf, sizeof(crlf) - 1U, 2U, &hunks) ==
               YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 0U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 0U);
    YEW_ASSERT_EQ_U64(hunk_distance(&hunks), 2U);

    YEW_ASSERT(yew_diff_bytes(&arena, base, sizeof(base) - 1U,
                              added, sizeof(added) - 1U, 4U, &hunks) ==
               YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_ADD);

    YEW_ASSERT(yew_diff_bytes(&arena, base, sizeof(base) - 1U,
                              modified, sizeof(modified) - 1U, 4U,
                              &hunks) == YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_MOD);

    YEW_ASSERT(yew_diff_bytes(&arena, base, sizeof(base) - 1U,
                              deleted, sizeof(deleted) - 1U, 4U,
                              &hunks) == YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 0U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 0U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_DEL);

    YEW_ASSERT(yew_diff_bytes(&arena, base, sizeof(base) - 1U,
                              deleted_eof, sizeof(deleted_eof) - 1U, 4U,
                              &hunks) == YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 2U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 2U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_DEL);
    YEW_ASSERT(yew_git_hunk_sign_placement(&hunks.data[0], 3U, true,
                                           &sign_line, &delete_below));
    YEW_ASSERT_EQ_U64(sign_line.v, 1U);
    YEW_ASSERT(delete_below);

    YEW_ASSERT(yew_git_hunk_sign_placement(&hunks.data[0], 2U, false,
                                           &sign_line, &delete_below));
    YEW_ASSERT_EQ_U64(sign_line.v, 1U);
    YEW_ASSERT(delete_below);
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
}

void test_git_diff_raw_budget_and_size_outcomes(void)
{
    Arena arena;
    GitHunkVec hunks = {0};
    Bytebuf left;
    Bytebuf right;
    YewDiffWork *work;
    DiffFakeClock clock = {0};
    size_t left_at_4096 = 0U;
    size_t right_at_4096 = 0U;
    u32 i;

    arena_init(&arena);
    bytebuf_init(&left);
    bytebuf_init(&right);
    for (i = 0U; i < YEW_DIFF_MAX_D / 2U + 1U; i++) {
        if (i == YEW_DIFF_MAX_D / 2U) {
            left_at_4096 = left.len;
            right_at_4096 = right.len;
        }
        bytebuf_printf(&left, "left-%u\n", i);
        bytebuf_printf(&right, "right-%u\n", i);
    }
    YEW_ASSERT(yew_diff_bytes(&arena, left.data, left_at_4096,
                              right.data, right_at_4096,
                              YEW_DIFF_MAX_D, &hunks) == YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunk_distance(&hunks), YEW_DIFF_MAX_D);
    YEW_ASSERT(yew_diff_bytes(&arena, left.data, left_at_4096,
                              right.data, right.len,
                              YEW_DIFF_MAX_D, &hunks) ==
               YEW_DIFF_TRUNCATED);
    YEW_ASSERT(yew_diff_bytes(&arena, left.data, left.len,
                              right.data, right.len,
                              YEW_DIFF_MAX_D, &hunks) ==
               YEW_DIFF_TRUNCATED);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 0U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_lo.v, 0U);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_n.v,
                      YEW_DIFF_MAX_D / 2U + 1U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_n.v,
                      YEW_DIFF_MAX_D / 2U + 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_MOD);

    work = yew_diff_work_begin_bytes(left.data, left.len, right.data,
                                     right.len, YEW_DIFF_MAX_D);
    YEW_ASSERT_NOT_NULL(work);
    while (yew_diff_work_step(work, YEW_DIFF_BUDGET_US,
                              diff_fake_now, &clock) == YEW_DIFF_MORE)
        ;
    /* At D=4096 the former full-frontier trace retained about 128 MiB.
     * Line metadata plus two active frontiers remains below 1 MiB. */
    YEW_ASSERT(yew_diff_work_peak_bytes(work) < 1024U * 1024U);
    yew_diff_work_free(work);

    YEW_ASSERT(yew_diff_within_size_limits(YEW_DIFF_MAX_BYTES,
                                            YEW_DIFF_MAX_LINES,
                                            YEW_DIFF_MAX_BYTES,
                                            YEW_DIFF_MAX_LINES));
    YEW_ASSERT(!yew_diff_within_size_limits(YEW_DIFF_MAX_BYTES + 1U, 0U,
                                             0U, 0U));
    YEW_ASSERT(!yew_diff_within_size_limits(0U, YEW_DIFF_MAX_LINES + 1U,
                                             0U, 0U));
    YEW_ASSERT(yew_diff_bytes(&arena, left.data, YEW_DIFF_MAX_BYTES + 1U,
                              right.data, right.len, 3U, &hunks) ==
               YEW_DIFF_TOO_LARGE);
    YEW_ASSERT_EQ_U64(hunks.len, 0U);
    bytebuf_free(&left);
    bytebuf_free(&right);
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
}

void test_git_diff_incremental_matches_sync_and_publishes_atomically(void)
{
    Arena arena;
    GitHunkVec sync = {0};
    GitHunkVec incremental = {0};
    GitHunkVec sentinel = {0};
    YewDiffWork *work;
    Bytebuf left;
    Bytebuf right;
    DiffFakeClock clock = {0};
    YewDiffProgress progress;
    u32 slices = 0U;
    u32 i;

    arena_init(&arena);
    bytebuf_init(&left);
    bytebuf_init(&right);
    for (i = 0U; i < 512U; i++) {
        bytebuf_printf(&left, "shared-%04u\n", i);
        bytebuf_printf(&right, "shared-%04u\n", i);
    }
    bytebuf_append(&left, "alpha\r\nlong collision-safe payload\ntail\n",
                   sizeof("alpha\r\nlong collision-safe payload\ntail\n") - 1U);
    bytebuf_append(&right, "beta\nlong collision-safe payload\nadded\ntail\n",
                   sizeof("beta\nlong collision-safe payload\nadded\ntail\n") - 1U);
    YEW_ASSERT(yew_diff_bytes_with_hash(
        &arena, left.data, left.len, right.data, right.len,
        16U, colliding_hash, &sync) == YEW_DIFF_OK);
    GitHunkVec_push(&sentinel, ((GitHunk){LINENO(91U), LINENO(92U),
                                         LINENO(93U), LINENO(94U),
                                         YEW_HUNK_DEL}));
    work = yew_diff_work_begin_bytes_with_hash(
        left.data, left.len, right.data, right.len, 16U,
        colliding_hash);
    YEW_ASSERT_NOT_NULL(work);
    YEW_ASSERT(yew_diff_work_take(work, &sentinel) == YEW_DIFF_INVALID);
    YEW_ASSERT_EQ_U64(sentinel.len, 1U);
    YEW_ASSERT_EQ_U64(sentinel.data[0].base_lo.v, 91U);
    do {
        clock.calls = 0U;
        progress = yew_diff_work_step(work, YEW_DIFF_BUDGET_US,
                                      diff_fake_now, &clock);
        YEW_ASSERT(clock.last - clock.first <= YEW_DIFF_BUDGET_US);
        slices++;
    } while (progress == YEW_DIFF_MORE);
    YEW_ASSERT(slices > 1U);
    YEW_ASSERT(yew_diff_work_take(work, &incremental) == YEW_DIFF_OK);
    assert_same_hunks(&sync, &incremental);
    YEW_ASSERT(yew_diff_work_take(work, &sentinel) == YEW_DIFF_INVALID);

    yew_diff_work_free(work);
    GitHunkVec_free(&sentinel);
    GitHunkVec_free(&incremental);
    GitHunkVec_free(&sync);
    bytebuf_free(&right);
    bytebuf_free(&left);
    arena_free_all(&arena);
}

void test_git_diff_incremental_budget_covers_hash_and_compare(void)
{
    Bytebuf left;
    Bytebuf right;
    GitHunkVec hunks = {0};
    YewDiffWork *work;
    DiffFakeClock clock = {0};
    YewDiffProgress progress;
    u32 slices = 0U;
    u32 i;

    bytebuf_init(&left);
    bytebuf_init(&right);
    for (i = 0U; i < 512U; i++) {
        bytebuf_printf(&left, "shared-%04u-abcdefghijklmnopqrstuvwxyz\n", i);
        bytebuf_printf(&right, "shared-%04u-abcdefghijklmnopqrstuvwxyz\n", i);
    }
    bytebuf_append(&right, "one-more-line\n", sizeof("one-more-line\n") - 1U);
    work = yew_diff_work_begin_bytes(left.data, left.len, right.data,
                                     right.len, 4U);
    YEW_ASSERT_NOT_NULL(work);
    do {
        clock.calls = 0U;
        progress = yew_diff_work_step(work, YEW_DIFF_BUDGET_US,
                                      diff_fake_now, &clock);
        YEW_ASSERT(clock.calls >= 2U);
        YEW_ASSERT(clock.last - clock.first <= YEW_DIFF_BUDGET_US);
        slices++;
    } while (progress == YEW_DIFF_MORE);
    YEW_ASSERT(slices > 2U);
    YEW_ASSERT(yew_diff_work_take(work, &hunks) == YEW_DIFF_OK);
    YEW_ASSERT_EQ_U64(hunks.len, 1U);
    YEW_ASSERT(hunks.data[0].kind == YEW_HUNK_ADD);
    YEW_ASSERT_EQ_U64(hunks.data[0].base_lo.v, 512U);
    YEW_ASSERT_EQ_U64(hunks.data[0].buf_n.v, 1U);

    yew_diff_work_free(work);
    GitHunkVec_free(&hunks);
    bytebuf_free(&right);
    bytebuf_free(&left);
}
