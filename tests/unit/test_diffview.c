/* Sprint 53: pure side-by-side row alignment and scroll synchronization. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "mod/git/editor.h"
#include "mod/git/diffview.h"
#include "unicode/grapheme.h"

static GitHunk dv_hunk(u64 base_lo, u64 base_n, u64 buf_lo, u64 buf_n,
                       HunkKind kind)
{
    GitHunk h;

    h.base_lo = LINENO(base_lo);
    h.base_n = LINENO(base_n);
    h.buf_lo = LINENO(buf_lo);
    h.buf_n = LINENO(buf_n);
    h.kind = kind;
    return h;
}

void test_diffview_blame_clock_uses_injected_anchor(void)
{
    Ed ed;

    yew_ed_init(&ed);
    ed.now_ms = 1000;
    yew_git_editor_clock_anchor(&ed, 1000, 1700000000);
    YEW_ASSERT_EQ_I64(yew_git_editor_wall_now(&ed), 1700000000);
    ed.now_ms = 4999;
    YEW_ASSERT_EQ_I64(yew_git_editor_wall_now(&ed), 1700000003);
    ed.now_ms = 999;
    YEW_ASSERT_EQ_I64(yew_git_editor_wall_now(&ed), 1700000000);
    yew_git_editor_clock_anchor(&ed, -1, 1800000000);
    YEW_ASSERT_EQ_I64(yew_git_editor_wall_now(&ed), 1700000000);
    yew_ed_free(&ed);
}

void test_diffview_rowmap_aligns_unbalanced_hunks(void)
{
    GitHunk hunks[] = {
        dv_hunk(1U, 1U, 1U, 3U, YEW_HUNK_MOD),
        dv_hunk(4U, 1U, 6U, 0U, YEW_HUNK_DEL)
    };
    const i32 want_left[] = {0, 1, -1, -1, 2, 3, 4};
    const i32 want_right[] = {0, 1, 2, 3, 4, 5, -1};
    DiffRowMap map;
    size_t i;

    yew_diff_rowmap_init(&map);
    YEW_ASSERT(yew_diff_rowmap_build(&map, 5U, 6U, hunks,
                                     YEW_ARRAY_LEN(hunks)));
    YEW_ASSERT_EQ_U64(yew_diff_rowmap_len(&map), YEW_ARRAY_LEN(want_left));
    for (i = 0U; i < YEW_ARRAY_LEN(want_left); i++) {
        YEW_ASSERT_EQ_I64(yew_diff_row_source(&map, YEW_DIFF_LEFT, i),
                          want_left[i]);
        YEW_ASSERT_EQ_I64(yew_diff_row_source(&map, YEW_DIFF_RIGHT, i),
                          want_right[i]);
    }
    YEW_ASSERT_EQ_I64(yew_diff_row_source(&map, YEW_DIFF_LEFT, 99U), -1);
    yew_diff_rowmap_drop(&map);
}

void test_diffview_rowmap_rejects_noncoherent_hunks_atomically(void)
{
    GitHunk good = dv_hunk(1U, 1U, 1U, 1U, YEW_HUNK_MOD);
    GitHunk bad = dv_hunk(1U, 1U, 2U, 1U, YEW_HUNK_MOD);
    DiffRowMap map;

    yew_diff_rowmap_init(&map);
    YEW_ASSERT(yew_diff_rowmap_build(&map, 3U, 3U, &good, 1U));
    YEW_ASSERT(!yew_diff_rowmap_build(&map, 3U, 3U, &bad, 1U));
    YEW_ASSERT_EQ_U64(yew_diff_rowmap_len(&map), 3U);
    YEW_ASSERT_EQ_I64(yew_diff_row_source(&map, YEW_DIFF_LEFT, 2U), 2);
    YEW_ASSERT_EQ_I64(yew_diff_row_source(&map, YEW_DIFF_RIGHT, 2U), 2);
    yew_diff_rowmap_drop(&map);
}

void test_diffview_scratch_builds_fillers_and_source_rows(void)
{
    static const u8 base[] = "one\nold\nend";
    static const u8 buf[] = "one\nnew-a\nnew-b\nnew-c\nend";
    static const char want_left[] = "one\nold\n~\n~\nend";
    GitHunk h = dv_hunk(1U, 1U, 1U, 3U, YEW_HUNK_MOD);
    DiffRowMap map;
    DiffScratchPair pair;
    size_t i;

    yew_diff_rowmap_init(&map);
    yew_diff_scratch_pair_init(&pair);
    YEW_ASSERT(yew_diff_rowmap_build(&map, 3U, 5U, &h, 1U));
    YEW_ASSERT(yew_diff_scratch_pair_build(&pair, base, sizeof(base) - 1U,
                                           buf, sizeof(buf) - 1U, &map));
    YEW_ASSERT_EQ_U64(pair.left.bytes.len, sizeof(want_left) - 1U);
    YEW_ASSERT_EQ_MEM(pair.left.bytes.data, want_left, sizeof(want_left) - 1U);
    YEW_ASSERT_EQ_U64(pair.right.bytes.len, sizeof(buf) - 1U);
    YEW_ASSERT_EQ_MEM(pair.right.bytes.data, buf, sizeof(buf) - 1U);
    for (i = 0U; i < yew_diff_rowmap_len(&map); i++) {
        YEW_ASSERT_EQ_I64(yew_diff_scratch_source_row(&pair.left, i),
                          yew_diff_row_source(&map, YEW_DIFF_LEFT, i));
        YEW_ASSERT_EQ_I64(yew_diff_scratch_source_row(&pair.right, i),
                          yew_diff_row_source(&map, YEW_DIFF_RIGHT, i));
    }
    yew_diff_scratch_pair_drop(&pair);
    yew_diff_rowmap_drop(&map);
}

void test_diffview_intraline_preserves_zwj_and_cjk_clusters(void)
{
    static const u8 left[] =
        "A\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA7\xE4\xB8\xADZ";
    static const u8 right[] =
        "A\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA6\xE5\x9B\xBDZ";
    DiffIntraline diff;
    size_t family_off = 1U;
    size_t family_len = 18U;

    yew_diff_intraline_init(&diff);
    YEW_ASSERT_EQ_U64(yew_diff_intraline_build(&diff, left,
                                               sizeof(left) - 1U, right,
                                               sizeof(right) - 1U),
                      YEW_DIFF_INTRA_SPANS);
    YEW_ASSERT_EQ_U64(diff.left.len, 1U);
    YEW_ASSERT_EQ_U64(diff.right.len, 1U);
    YEW_ASSERT_EQ_U64(diff.left.data[0].off, family_off);
    YEW_ASSERT_EQ_U64(diff.left.data[0].len, family_len + 3U);
    YEW_ASSERT_EQ_U64(diff.right.data[0].off, family_off);
    YEW_ASSERT_EQ_U64(diff.right.data[0].len, family_len + 3U);
    YEW_ASSERT_EQ_U64(yew_gb_next_bytes(left, sizeof(left) - 1U,
                                        diff.left.data[0].off),
                      family_off + family_len);
    YEW_ASSERT_EQ_U64(yew_gb_next_bytes(right, sizeof(right) - 1U,
                                        diff.right.data[0].off),
                      family_off + family_len);
    yew_diff_intraline_drop(&diff);
}

void test_diffview_intraline_equal_and_over_cap_fallback(void)
{
    u8 long_line[YEW_DIFF_INTRALINE_MAX + 1U];
    DiffIntraline diff;

    (void)memset(long_line, 'x', sizeof(long_line));
    yew_diff_intraline_init(&diff);
    YEW_ASSERT_EQ_U64(yew_diff_intraline_build(&diff, (const u8 *)"same", 4U,
                                               (const u8 *)"same", 4U),
                      YEW_DIFF_INTRA_EQUAL);
    YEW_ASSERT_EQ_U64(diff.left.len, 0U);
    YEW_ASSERT_EQ_U64(diff.right.len, 0U);
    YEW_ASSERT_EQ_U64(yew_diff_intraline_build(&diff, long_line,
                                               sizeof(long_line),
                                               (const u8 *)"x", 1U),
                      YEW_DIFF_INTRA_WHOLE_LINE);
    YEW_ASSERT_EQ_U64(diff.left.len, 0U);
    YEW_ASSERT_EQ_U64(diff.right.len, 0U);
    yew_diff_intraline_drop(&diff);
}

typedef struct ScrollProbe {
    DiffScrollRegistry *registry;
    u32 link_id;
    u32 member_id;
    u32 calls;
    u32 top;
    bool recurse;
    bool nested_result;
} ScrollProbe;

static void dv_scroll_apply(void *ctx, u32 aligned_top)
{
    ScrollProbe *probe = ctx;

    probe->calls++;
    probe->top = aligned_top;
    if (probe->recurse)
        probe->nested_result = yew_diff_scroll_sync(probe->registry,
                                                    probe->link_id,
                                                    probe->member_id,
                                                    aligned_top);
}

void test_diffview_scroll_sync_guard_blocks_feedback_loop(void)
{
    DiffScrollRegistry registry;
    ScrollProbe a = {0};
    ScrollProbe b = {0};
    ScrollProbe other = {0};

    yew_diff_scroll_registry_init(&registry);
    a = (ScrollProbe){&registry, 7U, 1U, 0U, 0U, false, true};
    b = (ScrollProbe){&registry, 7U, 2U, 0U, 0U, true, true};
    other = (ScrollProbe){&registry, 9U, 3U, 0U, 0U, false, true};
    YEW_ASSERT(yew_diff_scroll_register(&registry, 7U, 1U,
                                        dv_scroll_apply, &a));
    YEW_ASSERT(yew_diff_scroll_register(&registry, 7U, 2U,
                                        dv_scroll_apply, &b));
    YEW_ASSERT(yew_diff_scroll_register(&registry, 9U, 3U,
                                        dv_scroll_apply, &other));
    YEW_ASSERT(yew_diff_scroll_sync(&registry, 7U, 1U, 42U));
    YEW_ASSERT_EQ_U64(a.calls, 0U);
    YEW_ASSERT_EQ_U64(b.calls, 1U);
    YEW_ASSERT_EQ_U64(b.top, 42U);
    YEW_ASSERT(!b.nested_result);
    YEW_ASSERT_EQ_U64(other.calls, 0U);
    yew_diff_scroll_unregister(&registry, 2U);
    YEW_ASSERT(yew_diff_scroll_sync(&registry, 7U, 1U, 9U));
    YEW_ASSERT_EQ_U64(b.calls, 1U);
    yew_diff_scroll_registry_drop(&registry);
}
