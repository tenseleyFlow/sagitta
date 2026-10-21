/* Sprint 53: pure side-by-side row alignment and scroll synchronization. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "mod/git/editor.h"
#include "mod/git/diffview.h"
#include "text/piece.h"
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

static DiffUnifiedResult dv_unified(DiffUnified *view, const char *patch)
{
    return yew_diff_unified_build(view, (const u8 *)patch, strlen(patch));
}

void test_diffview_unified_source_rows_and_bytes(void)
{
    static const char patch[] =
        "diff --git a/x b/x\nindex 123..456 100644\n--- a/x\n+++ b/x\n"
        "@@ -1,3 +1,4 @@\n one\r\n-old\n+漢字\n++new\n last\n"
        "\\ No newline at end of file\n";
    static const char want[] = "one\r\nold\n漢字\n+new\nlast";
    const i32 before[] = {0, 1, -1, -1, 2};
    const i32 after[] = {0, -1, 1, 2, 3};
    DiffUnified view = {0};
    size_t i;

    YEW_ASSERT_EQ_I64(dv_unified(&view, patch), YEW_DIFF_UNIFIED_OK);
    YEW_ASSERT_EQ_U64(view.bytes.len, sizeof(want) - 1U);
    YEW_ASSERT(memcmp(view.bytes.data, want, view.bytes.len) == 0);
    YEW_ASSERT_EQ_U64(view.rows.left.len, YEW_ARRAY_LEN(before));
    for (i = 0U; i < YEW_ARRAY_LEN(before); i++) {
        YEW_ASSERT_EQ_I64(view.rows.left.data[i], before[i]);
        YEW_ASSERT_EQ_I64(view.rows.right.data[i], after[i]);
    }
    YEW_ASSERT(view.before_no_lf && view.after_no_lf);
    yew_diff_unified_drop(&view);
    YEW_ASSERT_EQ_I64(dv_unified(&view,
        "diff --git a/x b/x\n--- /dev/null\n+++ b/x\n@@ -0,0 +1 @@\n+\n"),
        YEW_DIFF_UNIFIED_OK);
    YEW_ASSERT_EQ_U64(view.bytes.len, 0U);
    YEW_ASSERT_EQ_U64(view.rows.left.len, 1U);
    YEW_ASSERT_EQ_I64(view.rows.left.data[0], -1);
    YEW_ASSERT_EQ_I64(view.rows.right.data[0], 0);
    YEW_ASSERT_EQ_I64(dv_unified(&view,
        "diff --git a/x b/x\n--- a/x\n+++ /dev/null\n@@ -1 +0,0 @@\n-x\n"
        "\\ No newline at end of file\n"), YEW_DIFF_UNIFIED_OK);
    YEW_ASSERT(view.before_no_lf && !view.after_no_lf);
    YEW_ASSERT_EQ_I64(view.rows.right.data[0], -1);
    yew_diff_unified_drop(&view);
}

void test_diffview_unified_rejects_partial_and_malformed_atomically(void)
{
    const char *bad[] = {
        "@@ -1 +1 @@\n-x\n+y\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -2 +2 @@\n-x\n+y\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1,2 +1 @@\n-x\n+y\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-x\n+y",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-x\n+y\n+z\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-x\n+y\n"
        "@@ -3 +3 @@\n-z\n+q\n",
        "diff --git a/x b/x\ndiff --git a/y b/y\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -999999999999 +1 @@\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1,2 +1 @@\n-x\n"
        "\\ No newline at end of file\n-y\n+z\n",
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n",
        "diff --cc x\n@@@ -1 -1 +1 @@@\n",
        "diff --git a/x b/x\nBinary files a/x and b/x differ\n"
        "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-x\n+y\n"
    };
    DiffUnified view = {0};
    size_t i;

    YEW_ASSERT_EQ_I64(dv_unified(&view,
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-x\n+y\n"),
        YEW_DIFF_UNIFIED_OK);
    for (i = 0U; i < YEW_ARRAY_LEN(bad); i++) {
        YEW_ASSERT_EQ_I64(dv_unified(&view, bad[i]), YEW_DIFF_UNIFIED_INVALID);
        YEW_ASSERT_EQ_U64(view.bytes.len, 3U);
        YEW_ASSERT(memcmp(view.bytes.data, "x\ny", 3U) == 0);
        YEW_ASSERT_EQ_U64(view.rows.left.len, 2U);
    }
    YEW_ASSERT_EQ_I64(yew_diff_unified_build(&view, (const u8 *)"x",
        (size_t)YEW_DIFF_UNIFIED_MAX_BYTES + 1U), YEW_DIFF_UNIFIED_LIMIT);
    YEW_ASSERT_EQ_I64(yew_diff_unified_build(&view, NULL, 1U),
                      YEW_DIFF_UNIFIED_INVALID);
    YEW_ASSERT_EQ_I64(dv_unified(&view,
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1,200001 +1 @@\n"),
        YEW_DIFF_UNIFIED_LIMIT);
    yew_diff_unified_drop(&view);
}

void test_diffview_unified_binary_metadata_empty_and_row_cap(void)
{
    DiffUnified view = {0};
    Bytebuf patch = {0};
    u32 i;

    YEW_ASSERT_EQ_I64(dv_unified(&view, ""), YEW_DIFF_UNIFIED_EMPTY);
    YEW_ASSERT_EQ_I64(dv_unified(&view,
        "diff --git a/x b/x\nindex 123..456 100644\nBinary files a/x and b/x differ\n"),
        YEW_DIFF_UNIFIED_BINARY);
    YEW_ASSERT_EQ_I64(dv_unified(&view,
        "diff --git a/x b/x\nold mode 100644\nnew mode 100755\n"),
        YEW_DIFF_UNIFIED_METADATA);
    bytebuf_printf(&patch,
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1,%u +1 @@\n",
        (unsigned)YEW_DIFF_UNIFIED_MAX_ROWS);
    for (i = 0U; i < YEW_DIFF_UNIFIED_MAX_ROWS; i++)
        bytebuf_append(&patch, "-x\n", 3U);
    bytebuf_append(&patch, "+y\n", 3U);
    YEW_ASSERT_EQ_I64(yew_diff_unified_build(&view, patch.data, patch.len),
                      YEW_DIFF_UNIFIED_LIMIT);
    YEW_ASSERT_EQ_U64(view.bytes.len, 0U);
    bytebuf_free(&patch);
    yew_diff_unified_drop(&view);
}

void test_diffview_unified_clone_gutter_and_hunk_motion(void)
{
    Ed ed;
    Win *clone;
    CmdCtx cx = {0};
    const YewGitDiffRowStyle styles[] = {
        YEW_GIT_DIFF_ROW_NONE, YEW_GIT_DIFF_ROW_DEL, YEW_GIT_DIFF_ROW_ADD,
        YEW_GIT_DIFF_ROW_NONE, YEW_GIT_DIFF_ROW_ADD
    };
    size_t i;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"a\nb\nc\nd\ne", 9U, "diff"));
    for (i = 0U; i < YEW_ARRAY_LEN(styles); i++) {
        YewGitDiffSource source = {(i32)i, (i32)i};

        YewGitDiffRowStyleVec_push(&ed.win->git_diff_rows, styles[i]);
        YewGitDiffSourceVec_push(&ed.win->git_diff_sources, source);
    }
    YEW_ASSERT_EQ_U64(yew_gutter_width(ed.win), 10U);
    clone = yew_ed_win_clone(&ed, ed.win);
    YEW_ASSERT(clone != NULL);
    YEW_ASSERT_EQ_U64(clone->git_diff_sources.len, 5U);
    YEW_ASSERT(clone->git_diff_sources.data != ed.win->git_diff_sources.data);
    YEW_ASSERT_EQ_I64(clone->git_diff_rows.data[1], YEW_GIT_DIFF_ROW_DEL);
    {
        Buffer *other = yew_ws_scratch_new(&ed, "other", 0U);

        YEW_ASSERT(other != NULL);
        yew_ed_win_set_buffer(&ed, clone, other);
        YEW_ASSERT_EQ_U64(clone->git_diff_sources.len, 0U);
        YEW_ASSERT_EQ_U64(clone->git_diff_rows.len, 0U);
        YEW_ASSERT_EQ_U64(yew_gutter_width(clone), 6U);
    }
    yew_ed_win_release(&ed, clone);
    cx.ed = &ed;
    cx.win = ed.win;
    YEW_ASSERT_EQ_I64(yew_git_cmd_hunk_next(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_I64(yew_git_cmd_hunk_next(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 8U);
    YEW_ASSERT_EQ_I64(yew_git_cmd_hunk_next(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_I64(yew_git_cmd_hunk_prev(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 8U);
    YEW_ASSERT_EQ_I64(yew_git_cmd_hunk_first(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 2U);
    YEW_ASSERT_EQ_I64(yew_git_cmd_hunk_last(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 8U);
    yew_ed_free(&ed);
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
