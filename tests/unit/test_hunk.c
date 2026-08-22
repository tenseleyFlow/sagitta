#include "harness.h"

#include <string.h>

#include "mod/git/gutter.h"

static void assert_patch(const char *expected, const u8 *base,
                         size_t base_len, const u8 *buf, size_t buf_len,
                         GitHunk hunk)
{
    Bytebuf patch;

    YEW_ASSERT(yew_git_hunk_patch(&patch, "file.c", base, base_len, buf,
                                  buf_len, &hunk));
    bytebuf_push_u8(&patch, '\0');
    YEW_ASSERT_EQ_STR((const char *)patch.data, expected);
    bytebuf_free(&patch);
}

void test_git_hunk_patch_headers_and_path_guard(void)
{
    Bytebuf patch;
    GitHunk hunk = {
        LINENO(0U), LINENO(1U), LINENO(0U), LINENO(1U), YEW_HUNK_MOD
    };
    static const char expected[] =
        "diff --git a/file.c b/file.c\n"
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-a\n"
        "+b\n";

    assert_patch(expected, (const u8 *)"a\n", 2U,
                 (const u8 *)"b\n", 2U, hunk);
    YEW_ASSERT(!yew_git_hunk_patch(&patch, "bad\nname", NULL, 0U, NULL,
                                   0U, &hunk));
    YEW_ASSERT(!yew_git_hunk_patch(&patch, "file.c", (const u8 *)"a\n",
                                   2U, (const u8 *)"b\n", 2U,
                                   &(GitHunk){LINENO(9U), LINENO(1U),
                                              LINENO(0U), LINENO(1U),
                                              YEW_HUNK_MOD}));
}

void test_git_hunk_patch_exact_three_line_context(void)
{
    static const u8 base[] = "0\n1\n2\n3\nold\n5\n6\n7\n8\n";
    static const u8 buf[] = "0\n1\n2\n3\nnew\n5\n6\n7\n8\n";
    static const char expected[] =
        "diff --git a/file.c b/file.c\n"
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -2,7 +2,7 @@\n"
        " 1\n 2\n 3\n-old\n+new\n 5\n 6\n 7\n";
    GitHunk hunk = {
        LINENO(4U), LINENO(1U), LINENO(4U), LINENO(1U), YEW_HUNK_MOD
    };

    assert_patch(expected, base, sizeof(base) - 1U, buf, sizeof(buf) - 1U,
                 hunk);
}

void test_git_hunk_patch_add_delete_at_bounds(void)
{
    static const char add_expected[] =
        "diff --git a/file.c b/file.c\n--- a/file.c\n+++ b/file.c\n"
        "@@ -0,0 +1,2 @@\n+a\n+b\n";
    static const char del_expected[] =
        "diff --git a/file.c b/file.c\n--- a/file.c\n+++ b/file.c\n"
        "@@ -1,2 +0,0 @@\n-a\n-b\n";

    assert_patch(add_expected, NULL, 0U, (const u8 *)"a\nb\n", 4U,
                 (GitHunk){LINENO(0U), LINENO(0U), LINENO(0U), LINENO(2U),
                           YEW_HUNK_ADD});
    assert_patch(del_expected, (const u8 *)"a\nb\n", 4U, NULL, 0U,
                 (GitHunk){LINENO(0U), LINENO(2U), LINENO(0U), LINENO(0U),
                           YEW_HUNK_DEL});
}

void test_git_hunk_patch_missing_final_newline_matrix(void)
{
    static const char both_missing[] =
        "diff --git a/file.c b/file.c\n--- a/file.c\n+++ b/file.c\n"
        "@@ -1,1 +1,1 @@\n-old\n\\ No newline at end of file\n"
        "+new\n\\ No newline at end of file\n";
    static const char base_missing[] =
        "diff --git a/file.c b/file.c\n--- a/file.c\n+++ b/file.c\n"
        "@@ -1,1 +1,1 @@\n-old\n\\ No newline at end of file\n+new\n";
    static const char buf_missing[] =
        "diff --git a/file.c b/file.c\n--- a/file.c\n+++ b/file.c\n"
        "@@ -1,1 +1,1 @@\n-old\n+new\n\\ No newline at end of file\n";
    GitHunk hunk = {
        LINENO(0U), LINENO(1U), LINENO(0U), LINENO(1U), YEW_HUNK_MOD
    };

    assert_patch(both_missing, (const u8 *)"old", 3U,
                 (const u8 *)"new", 3U, hunk);
    assert_patch(base_missing, (const u8 *)"old", 3U,
                 (const u8 *)"new\n", 4U, hunk);
    assert_patch(buf_missing, (const u8 *)"old\n", 4U,
                 (const u8 *)"new", 3U, hunk);
    assert_patch("diff --git a/file.c b/file.c\n--- a/file.c\n"
                 "+++ b/file.c\n@@ -1,1 +1,1 @@\n-old\n+new\n",
                 (const u8 *)"old\n", 4U, (const u8 *)"new\n", 4U, hunk);
}

void test_git_hunk_patch_preserves_crlf_bytes(void)
{
    static const u8 base[] = "before\r\nold\r\nafter\r\n";
    static const u8 buf[] = "before\r\nnew\r\nafter\r\n";
    static const char expected[] =
        "diff --git a/file.c b/file.c\n--- a/file.c\n+++ b/file.c\n"
        "@@ -1,3 +1,3 @@\n before\r\n-old\r\n+new\r\n after\r\n";

    assert_patch(expected, base, sizeof(base) - 1U, buf, sizeof(buf) - 1U,
                 (GitHunk){LINENO(1U), LINENO(1U), LINENO(1U), LINENO(1U),
                           YEW_HUNK_MOD});
}
