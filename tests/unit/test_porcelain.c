#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/git/git.h"

#define OID_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OID_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define OID_C "cccccccccccccccccccccccccccccccccccccccc"

static void snapshot_begin(GitSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    arena_init(&snap->a);
}

static const GitEntry *entry_named(const GitSnapshot *snap, const char *name)
{
    size_t i;
    size_t n = strlen(name);
    for (i = 0U; i < snap->entries.len; i++)
        if (snap->entries.data[i].path_len == n &&
            memcmp(snap->entries.data[i].path, name, n) == 0)
            return &snap->entries.data[i];
    return NULL;
}

void test_porcelain_status_all_records_headers_and_sort(void)
{
    static const u8 input[] =
        "# branch.oid " OID_A "\0"
        "# branch.head feature/parser branch\0"
        "# branch.upstream origin/trunk\0"
        "# branch.ab +12 -3\0"
        "# stash 4\0"
        "# future.header accepted\0"
        "x future record\0"
        "1 .M N... 100644 100644 100644 " OID_A " " OID_B
        " z ordinary\0"
        "2 R. S.M. 100644 100644 100644 " OID_A " " OID_C
        " R075 b renamed\0a original\0"
        "u UU N... 100644 100644 100644 100644 " OID_A " " OID_B " " OID_C
        " conflict\0"
        "? dir/\0"
        "! ignored file\0";
    GitSnapshot snap;
    GitParseErr err;
    const GitEntry *entry;

    snapshot_begin(&snap);
    YEW_ASSERT(yew_git_parse_status(&snap, input, sizeof(input) - 1U, &err));
    YEW_ASSERT_EQ_U64(snap.entries.len, 5U);
    YEW_ASSERT_EQ_I64(snap.state, YEW_GIT_CONFLICTED);
    YEW_ASSERT_EQ_STR(snap.branch, "feature/parser branch");
    YEW_ASSERT_EQ_STR(snap.upstream, "origin/trunk");
    YEW_ASSERT_EQ_STR(snap.head_oid, OID_A);
    YEW_ASSERT_EQ_I64(snap.ahead, 12);
    YEW_ASSERT_EQ_I64(snap.behind, 3);
    YEW_ASSERT(!snap.detached);
    YEW_ASSERT(!snap.unborn);
    YEW_ASSERT(snap.conflicted);
    YEW_ASSERT_EQ_STR(snap.entries.data[0].path, "b renamed");
    YEW_ASSERT_EQ_STR(snap.entries.data[4].path, "z ordinary");

    entry = entry_named(&snap, "z ordinary");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_I64(entry->kind, GIT_E_ORDINARY);
    YEW_ASSERT(!entry->staged);
    YEW_ASSERT(entry->unstaged);
    YEW_ASSERT_EQ_STR(entry->index_oid, OID_B);

    entry = entry_named(&snap, "b renamed");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_I64(entry->kind, GIT_E_RENAME);
    YEW_ASSERT_EQ_I64(entry->score, 75);
    YEW_ASSERT_EQ_U64(entry->orig_len, strlen("a original"));
    YEW_ASSERT_EQ_MEM(entry->orig_path, "a original", entry->orig_len);
    YEW_ASSERT(entry->staged);
    YEW_ASSERT(!entry->unstaged);
    YEW_ASSERT(entry->submodule);

    entry = entry_named(&snap, "conflict");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_I64(entry->kind, GIT_E_UNMERGED);
    YEW_ASSERT(entry->conflicted);
    YEW_ASSERT_EQ_I64(entry->x, 'U');
    YEW_ASSERT_EQ_I64(entry->y, 'U');

    entry = entry_named(&snap, "dir/");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT(entry->untracked);
    YEW_ASSERT(entry->is_dir);
    entry = entry_named(&snap, "ignored file");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_I64(entry->kind, GIT_E_IGNORED);
    YEW_ASSERT(!entry->untracked);
    arena_free_all(&snap.a);
}

void test_porcelain_xy_mapping_complete(void)
{
    static const char alphabet[] = ".MTADRC";
    static const char *const conflicts[] = {
        "DD", "AU", "UD", "UA", "DU", "AA", "UU"
    };
    size_t x;
    size_t y;

    for (x = 0U; x < strlen(alphabet); x++) {
        for (y = 0U; y < strlen(alphabet); y++) {
            char input[256];
            int n;
            GitSnapshot snap;
            GitParseErr err;
            snapshot_begin(&snap);
            n = snprintf(input, sizeof(input),
                         "1 %c%c N... 100644 100644 100644 %s %s p",
                         alphabet[x], alphabet[y], OID_A, OID_B);
            YEW_ASSERT(n > 0 && (size_t)n + 1U < sizeof(input));
            input[n] = '\0';
            YEW_ASSERT(yew_git_parse_status(&snap, (const u8 *)input,
                                            (u64)n + 1U, &err));
            YEW_ASSERT_EQ_U64(snap.entries.len, 1U);
            YEW_ASSERT_EQ_I64(snap.entries.data[0].staged,
                              alphabet[x] != '.');
            YEW_ASSERT_EQ_I64(snap.entries.data[0].unstaged,
                              alphabet[y] != '.');
            arena_free_all(&snap.a);
        }
    }
    for (x = 0U; x < sizeof(conflicts) / sizeof(conflicts[0]); x++) {
        char input[320];
        int n;
        GitSnapshot snap;
        GitParseErr err;
        snapshot_begin(&snap);
        n = snprintf(input, sizeof(input),
                     "u %s N... 100644 100644 100644 100644 %s %s %s p",
                     conflicts[x], OID_A, OID_B, OID_C);
        YEW_ASSERT(n > 0 && (size_t)n + 1U < sizeof(input));
        input[n] = '\0';
        YEW_ASSERT(yew_git_parse_status(&snap, (const u8 *)input,
                                        (u64)n + 1U, &err));
        YEW_ASSERT_EQ_U64(snap.entries.len, 1U);
        YEW_ASSERT(snap.entries.data[0].conflicted);
        YEW_ASSERT_EQ_I64(snap.entries.data[0].x, conflicts[x][0]);
        YEW_ASSERT_EQ_I64(snap.entries.data[0].y, conflicts[x][1]);
        arena_free_all(&snap.a);
    }
}

void test_porcelain_rename_consumes_two_nuls_and_preserves_newline(void)
{
    static const u8 input[] =
        "2 R. N... 100644 100644 100644 " OID_A " " OID_B
        " R100 renamed\npath\0old\npath\0"
        "1 M. N... 100644 100644 100644 " OID_A " " OID_B " one\0"
        "1 .M N... 100644 100644 100644 " OID_A " " OID_B " two\0"
        "1 A. N... 000000 100644 100644 " OID_A " " OID_B " three\0"
        "1 D. N... 100644 000000 000000 " OID_A " " OID_B " four\0"
        "1 T. N... 100644 120000 120000 " OID_A " " OID_B " five\0";
    GitSnapshot snap;
    GitParseErr err;
    const GitEntry *renamed;

    snapshot_begin(&snap);
    YEW_ASSERT(yew_git_parse_status(&snap, input, sizeof(input) - 1U, &err));
    YEW_ASSERT_EQ_U64(snap.entries.len, 6U);
    YEW_ASSERT_NOT_NULL(entry_named(&snap, "one"));
    YEW_ASSERT_NOT_NULL(entry_named(&snap, "two"));
    YEW_ASSERT_NOT_NULL(entry_named(&snap, "three"));
    YEW_ASSERT_NOT_NULL(entry_named(&snap, "four"));
    YEW_ASSERT_NOT_NULL(entry_named(&snap, "five"));
    renamed = entry_named(&snap, "renamed\npath");
    YEW_ASSERT_NOT_NULL(renamed);
    YEW_ASSERT_EQ_U64(renamed->path_len, strlen("renamed\npath"));
    YEW_ASSERT_EQ_MEM(renamed->path, "renamed\npath", renamed->path_len);
    YEW_ASSERT_EQ_MEM(renamed->orig_path, "old\npath", renamed->orig_len);
    arena_free_all(&snap.a);
}

void test_porcelain_header_state_matrices(void)
{
    static const u8 normal[] =
        "# branch.oid " OID_A "\0# branch.head trunk\0"
        "# branch.upstream origin/trunk\0# branch.ab +0 -0\0";
    static const u8 unborn[] =
        "# branch.oid (initial)\0# branch.head new\0";
    static const u8 detached[] =
        "# branch.oid " OID_A "\0# branch.head (detached)\0"
        "# branch.upstream odd/upstream\0";
    static const u8 no_upstream[] =
        "# branch.oid " OID_A "\0# branch.head trunk\0";
    GitSnapshot snap;
    GitParseErr err;

    snapshot_begin(&snap);
    YEW_ASSERT(yew_git_parse_status(&snap, normal, sizeof(normal) - 1U, &err));
    YEW_ASSERT_EQ_I64(snap.state, YEW_GIT_OK);
    YEW_ASSERT_EQ_I64(snap.ahead, 0);
    YEW_ASSERT_EQ_I64(snap.behind, 0);
    arena_free_all(&snap.a);

    snapshot_begin(&snap);
    YEW_ASSERT(yew_git_parse_status(&snap, unborn, sizeof(unborn) - 1U, &err));
    YEW_ASSERT_EQ_I64(snap.state, YEW_GIT_NO_HEAD);
    YEW_ASSERT(snap.unborn);
    YEW_ASSERT_NULL(snap.head_oid);
    YEW_ASSERT_EQ_I64(snap.ahead, -1);
    YEW_ASSERT_EQ_I64(snap.behind, -1);
    arena_free_all(&snap.a);

    snapshot_begin(&snap);
    YEW_ASSERT(yew_git_parse_status(&snap, detached, sizeof(detached) - 1U,
                                    &err));
    YEW_ASSERT_EQ_I64(snap.state, YEW_GIT_DETACHED);
    YEW_ASSERT(snap.detached);
    YEW_ASSERT_NULL(snap.branch);
    arena_free_all(&snap.a);

    snapshot_begin(&snap);
    YEW_ASSERT(yew_git_parse_status(&snap, no_upstream,
                                    sizeof(no_upstream) - 1U, &err));
    YEW_ASSERT_EQ_I64(snap.state, YEW_GIT_NO_UPSTREAM);
    arena_free_all(&snap.a);
}

void test_porcelain_every_byte_truncation_and_rejections(void)
{
    static const u8 record[] =
        "1 M. N... 100644 100644 100644 " OID_A " " OID_B " path\0";
    size_t cut;
    GitParseErr err;

    for (cut = 0U; cut + 1U < sizeof(record) - 1U; cut++) {
        GitSnapshot snap;
        snapshot_begin(&snap);
        YEW_ASSERT(!yew_git_parse_status(&snap, record, cut + 1U, &err));
        YEW_ASSERT(err.off <= cut + 1U);
        arena_free_all(&snap.a);
    }
    {
        static const u8 bad_conflict[] =
            "u ZZ N... 100644 100644 100644 100644 " OID_A " " OID_B " " OID_C
            " bad\0";
        GitSnapshot snap;
        snapshot_begin(&snap);
        YEW_ASSERT(!yew_git_parse_status(&snap, bad_conflict,
                                         sizeof(bad_conflict) - 1U, &err));
        arena_free_all(&snap.a);
    }
}

void test_porcelain_z_paths_ignore_prefixes(void)
{
    static const u8 input[] = "z-file\0node_modules/\0a/b/\0exact.bin\0";
    Arena arena;
    GitPathList paths;
    GitIgnoreSet ignored;
    GitParseErr err;

    arena_init(&arena);
    YEW_ASSERT(yew_git_parse_z_paths(&arena, input, sizeof(input) - 1U,
                                     &paths, &err));
    YEW_ASSERT_EQ_U64(paths.len, 4U);
    YEW_ASSERT_EQ_STR(paths.data[0].path, "a/b/");
    YEW_ASSERT(paths.data[0].is_dir);
    YEW_ASSERT_EQ_STR(paths.data[3].path, "z-file");
    YEW_ASSERT(!paths.data[3].is_dir);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_git_parse_ignore(&arena, input, sizeof(input) - 1U,
                                    &ignored, &err));
    YEW_ASSERT(yew_git_ignored(&ignored, "exact.bin", 9U));
    YEW_ASSERT(yew_git_ignored(&ignored, "node_modules/pkg/x.js", 21U));
    YEW_ASSERT(yew_git_ignored(&ignored, "a/b/c/d", 7U));
    YEW_ASSERT(!yew_git_ignored(&ignored, "a/bad", 5U));
    YEW_ASSERT(!yew_git_ignored(&ignored, "node_module", 11U));
    arena_free_all(&arena);
}

void test_porcelain_blame_metadata_reuse_and_incremental(void)
{
    static const u8 input[] =
        OID_A " 1 10 2\n"
        "author Jane Doe\n"
        "author-mail <jane@example.test>\n"
        "author-time 1700000000\n"
        "author-tz -0530\n"
        "committer Nobody\n"
        "summary First summary\n"
        "boundary\n"
        "filename source.c\n"
        "\tline ten\n"
        OID_A " 2 11\n"
        "\tline eleven\n"
        OID_A " 15 15 2\n"
        "\tline fifteen\n"
        OID_A " 16 16\n"
        "\tline sixteen\n"
        OID_A " 8 20 1\n"
        "previous " OID_B " old.c\n"
        "filename source.c\n"
        OID_A " 9 30 1\n"
        "filename source.c\n";
    Arena arena;
    GitBlameLineList lines;
    GitCommitMetaList commits;
    GitParseErr err;

    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_git_parse_blame(&arena, input, sizeof(input) - 1U,
                                          &lines, &commits, &err), 6U);
    YEW_ASSERT_EQ_U64(lines.len, 6U);
    YEW_ASSERT_EQ_U64(commits.len, 1U);
    YEW_ASSERT_EQ_STR(commits.data[0].sha, OID_A);
    YEW_ASSERT_EQ_STR(commits.data[0].author, "Jane Doe");
    YEW_ASSERT_EQ_STR(commits.data[0].author_mail, "<jane@example.test>");
    YEW_ASSERT_EQ_STR(commits.data[0].summary, "First summary");
    YEW_ASSERT_EQ_I64(commits.data[0].author_time, 1700000000);
    YEW_ASSERT_EQ_I64(commits.data[0].author_tz_min, -330);
    YEW_ASSERT(commits.data[0].boundary);
    YEW_ASSERT_EQ_U64(lines.data[0].lineno, 10U);
    YEW_ASSERT_EQ_U64(lines.data[1].lineno, 11U);
    YEW_ASSERT_EQ_U64(lines.data[2].lineno, 15U);
    YEW_ASSERT_EQ_U64(lines.data[3].lineno, 16U);
    YEW_ASSERT_EQ_U64(lines.data[4].lineno, 20U);
    YEW_ASSERT_EQ_U64(lines.data[5].lineno, 30U);
    YEW_ASSERT_EQ_U64(lines.data[0].commit, 0U);
    YEW_ASSERT_EQ_U64(lines.data[5].commit, 0U);
    arena_free_all(&arena);
}

void test_porcelain_log_reflog_format_edges(void)
{
    static const u8 log_input[] =
        OID_A "\0aaaaaaa\0" "1700000000\0Jane\0jane@example.test\0"
        OID_B "\0HEAD -> trunk\0subject\037with-us\0body\037with-us\0"
        OID_B "\0bbbbbbb\0-1\0Bob\0bob@example.test\0\0\0"
        "last\0final body";
    static const u8 reflog_input[] =
        OID_C "\0ccccccc\0HEAD@{0}\0commit: message\0" "42\0"
        "subject\037with-us";
    Arena arena;
    GitLogRecordList logs;
    GitReflogRecordList reflogs;
    GitParseErr err;

    arena_init(&arena);
    YEW_ASSERT(yew_git_parse_log(&arena, log_input, sizeof(log_input) - 1U,
                                 &logs, &err));
    YEW_ASSERT_EQ_U64(logs.len, 2U);
    YEW_ASSERT_EQ_U64(strlen(logs.data[0].subject),
                      strlen("subject\037with-us"));
    YEW_ASSERT_EQ_MEM(logs.data[0].subject, "subject\037with-us",
                      strlen("subject\037with-us"));
    YEW_ASSERT_EQ_U64(strlen(logs.data[0].body), strlen("body\037with-us"));
    YEW_ASSERT_EQ_MEM(logs.data[0].body, "body\037with-us",
                      strlen("body\037with-us"));
    YEW_ASSERT_EQ_I64(logs.data[1].author_time, -1);
    YEW_ASSERT_EQ_STR(logs.data[1].body, "final body");
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_git_parse_reflog(&arena, reflog_input,
                                    sizeof(reflog_input) - 1U,
                                    &reflogs, &err));
    YEW_ASSERT_EQ_U64(reflogs.len, 1U);
    YEW_ASSERT_EQ_STR(reflogs.data[0].selector, "HEAD@{0}");
    YEW_ASSERT_EQ_STR(reflogs.data[0].message, "commit: message");
    YEW_ASSERT_EQ_I64(reflogs.data[0].author_time, 42);
    YEW_ASSERT_EQ_U64(strlen(reflogs.data[0].subject),
                      strlen("subject\037with-us"));
    arena_free_all(&arena);
}
