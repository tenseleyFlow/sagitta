#define _POSIX_C_SOURCE 200809L

/* Sprint 51: porcelain v2 is a byte protocol, so fuzz bytes as bytes. */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/git/git.h"

static void snapshot_begin(GitSnapshot *snap)
{
    (void)memset(snap, 0, sizeof(*snap));
    arena_init(&snap->a);
}

static void snapshot_end(GitSnapshot *snap)
{
    arena_free_all(&snap->a);
}

static bool bytes_eq(const char *a, u32 an, const char *b, u32 bn)
{
    return an == bn && (an == 0U || memcmp(a, b, an) == 0);
}

static bool text_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
}

static bool error_eq(const GitParseErr *a, const GitParseErr *b)
{
    return a->off == b->off && strcmp(a->message, b->message) == 0;
}

static bool entry_eq(const GitEntry *a, const GitEntry *b)
{
    return a->kind == b->kind && a->x == b->x && a->y == b->y &&
           bytes_eq(a->path, a->path_len, b->path, b->path_len) &&
           bytes_eq(a->orig_path, a->orig_len, b->orig_path, b->orig_len) &&
           a->score == b->score &&
           strcmp(a->index_oid, b->index_oid) == 0 &&
           a->is_dir == b->is_dir && a->submodule == b->submodule &&
           a->staged == b->staged && a->unstaged == b->unstaged &&
           a->untracked == b->untracked && a->conflicted == b->conflicted &&
           a->incoming == b->incoming;
}

static bool snapshot_eq(const GitSnapshot *a, const GitSnapshot *b)
{
    size_t i;

    if (a->state != b->state || !text_eq(a->branch, b->branch) ||
        !text_eq(a->upstream, b->upstream) ||
        !text_eq(a->head_oid, b->head_oid) || a->ahead != b->ahead ||
        a->behind != b->behind || a->detached != b->detached ||
        a->unborn != b->unborn || a->conflicted != b->conflicted ||
        a->entries.len != b->entries.len)
        return false;
    for (i = 0U; i < a->entries.len; i++)
        if (!entry_eq(&a->entries.data[i], &b->entries.data[i]))
            return false;
    return true;
}

static bool parse_deterministic(const u8 *data, size_t len,
                                char *why, size_t why_cap)
{
    GitSnapshot first;
    GitSnapshot second;
    GitParseErr first_err;
    GitParseErr second_err;
    bool first_ok;
    bool second_ok;
    bool equal;

    (void)memset(&first_err, 0, sizeof(first_err));
    (void)memset(&second_err, 0, sizeof(second_err));
    snapshot_begin(&first);
    snapshot_begin(&second);
    first_ok = yew_git_parse_status(&first, data, (u64)len, &first_err);
    second_ok = yew_git_parse_status(&second, data, (u64)len, &second_err);
    equal = first_ok == second_ok;
    if (equal && first_ok)
        equal = snapshot_eq(&first, &second);
    else if (equal)
        equal = error_eq(&first_err, &second_err);
    snapshot_end(&second);
    snapshot_end(&first);
    if (!equal) {
        (void)snprintf(why, why_cap,
                       "same porcelain bytes produced different results");
        return false;
    }
    return true;
}

static bool path_array_eq(const GitPath *a, size_t a_len,
                          const GitPath *b, size_t b_len)
{
    size_t i;

    if (a_len != b_len)
        return false;
    for (i = 0U; i < a_len; i++) {
        if (a[i].len != b[i].len || a[i].is_dir != b[i].is_dir ||
            (a[i].len != 0U &&
             memcmp(a[i].path, b[i].path, a[i].len) != 0))
            return false;
    }
    return true;
}

static bool paths_deterministic(const u8 *data, size_t len,
                                char *why, size_t why_cap)
{
    Arena first_arena;
    Arena second_arena;
    GitPathList first = {0};
    GitPathList second = {0};
    GitParseErr first_err = {0};
    GitParseErr second_err = {0};
    bool first_ok;
    bool second_ok;
    bool equal;

    arena_init(&first_arena);
    arena_init(&second_arena);
    first_ok = yew_git_parse_z_paths(&first_arena, data, (u64)len,
                                     &first, &first_err);
    second_ok = yew_git_parse_z_paths(&second_arena, data, (u64)len,
                                      &second, &second_err);
    equal = first_ok == second_ok &&
            (first_ok ? path_array_eq(first.data, first.len,
                                      second.data, second.len) :
                        error_eq(&first_err, &second_err));
    arena_free_all(&second_arena);
    arena_free_all(&first_arena);
    if (!equal)
        (void)snprintf(why, why_cap, "z-path parser is nondeterministic");
    return equal;
}

static bool ignore_deterministic(const u8 *data, size_t len,
                                 char *why, size_t why_cap)
{
    Arena first_arena;
    Arena second_arena;
    GitIgnoreSet first = {0};
    GitIgnoreSet second = {0};
    GitParseErr first_err = {0};
    GitParseErr second_err = {0};
    bool first_ok;
    bool second_ok;
    bool equal;

    arena_init(&first_arena);
    arena_init(&second_arena);
    first_ok = yew_git_parse_ignore(&first_arena, data, (u64)len,
                                    &first, &first_err);
    second_ok = yew_git_parse_ignore(&second_arena, data, (u64)len,
                                     &second, &second_err);
    equal = first_ok == second_ok &&
            (first_ok ? path_array_eq(first.data, first.len,
                                      second.data, second.len) :
                        error_eq(&first_err, &second_err));
    if (equal && first_ok) {
        static const char query[] = "dir/child\nname";
        equal = yew_git_ignored(&first, query, sizeof(query) - 1U) ==
                yew_git_ignored(&second, query, sizeof(query) - 1U);
    }
    arena_free_all(&second_arena);
    arena_free_all(&first_arena);
    if (!equal)
        (void)snprintf(why, why_cap, "ignore parser is nondeterministic");
    return equal;
}

static bool commit_eq(const GitCommitMeta *a, const GitCommitMeta *b)
{
    return strcmp(a->sha, b->sha) == 0 &&
           text_eq(a->author, b->author) &&
           text_eq(a->author_mail, b->author_mail) &&
           text_eq(a->summary, b->summary) &&
           a->author_time == b->author_time &&
           a->author_tz_min == b->author_tz_min &&
           a->boundary == b->boundary;
}

static bool blame_deterministic(const u8 *data, size_t len,
                                char *why, size_t why_cap)
{
    Arena first_arena;
    Arena second_arena;
    GitBlameLineList first_lines = {0};
    GitBlameLineList second_lines = {0};
    GitCommitMetaList first_commits = {0};
    GitCommitMetaList second_commits = {0};
    GitParseErr first_err = {0};
    GitParseErr second_err = {0};
    u32 first_count;
    u32 second_count;
    bool equal;
    size_t i;

    arena_init(&first_arena);
    arena_init(&second_arena);
    first_count = yew_git_parse_blame(&first_arena, data, (u64)len,
                                      &first_lines, &first_commits,
                                      &first_err);
    second_count = yew_git_parse_blame(&second_arena, data, (u64)len,
                                       &second_lines, &second_commits,
                                       &second_err);
    equal = first_count == second_count &&
            first_lines.len == second_lines.len &&
            first_commits.len == second_commits.len &&
            error_eq(&first_err, &second_err);
    for (i = 0U; equal && i < first_lines.len; i++)
        equal = first_lines.data[i].lineno == second_lines.data[i].lineno &&
                first_lines.data[i].commit == second_lines.data[i].commit;
    for (i = 0U; equal && i < first_commits.len; i++)
        equal = commit_eq(&first_commits.data[i], &second_commits.data[i]);
    arena_free_all(&second_arena);
    arena_free_all(&first_arena);
    if (!equal)
        (void)snprintf(why, why_cap, "blame parser is nondeterministic");
    return equal;
}

static bool log_record_eq(const GitLogRecord *a, const GitLogRecord *b)
{
    return text_eq(a->oid, b->oid) && text_eq(a->short_oid, b->short_oid) &&
           a->author_time == b->author_time &&
           text_eq(a->author, b->author) &&
           text_eq(a->author_mail, b->author_mail) &&
           text_eq(a->parents, b->parents) && text_eq(a->refs, b->refs) &&
           text_eq(a->subject, b->subject) && text_eq(a->body, b->body);
}

static bool log_deterministic(const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    Arena first_arena;
    Arena second_arena;
    GitLogRecordList first = {0};
    GitLogRecordList second = {0};
    GitParseErr first_err = {0};
    GitParseErr second_err = {0};
    bool first_ok;
    bool second_ok;
    bool equal;
    size_t i;

    arena_init(&first_arena);
    arena_init(&second_arena);
    first_ok = yew_git_parse_log(&first_arena, data, (u64)len,
                                 &first, &first_err);
    second_ok = yew_git_parse_log(&second_arena, data, (u64)len,
                                  &second, &second_err);
    equal = first_ok == second_ok && first.len == second.len &&
            (first_ok || error_eq(&first_err, &second_err));
    for (i = 0U; equal && i < first.len; i++)
        equal = log_record_eq(&first.data[i], &second.data[i]);
    arena_free_all(&second_arena);
    arena_free_all(&first_arena);
    if (!equal)
        (void)snprintf(why, why_cap, "log parser is nondeterministic");
    return equal;
}

static bool reflog_record_eq(const GitReflogRecord *a,
                             const GitReflogRecord *b)
{
    return text_eq(a->oid, b->oid) && text_eq(a->short_oid, b->short_oid) &&
           text_eq(a->selector, b->selector) &&
           text_eq(a->message, b->message) &&
           a->author_time == b->author_time &&
           text_eq(a->subject, b->subject);
}

static bool reflog_deterministic(const u8 *data, size_t len,
                                 char *why, size_t why_cap)
{
    Arena first_arena;
    Arena second_arena;
    GitReflogRecordList first = {0};
    GitReflogRecordList second = {0};
    GitParseErr first_err = {0};
    GitParseErr second_err = {0};
    bool first_ok;
    bool second_ok;
    bool equal;
    size_t i;

    arena_init(&first_arena);
    arena_init(&second_arena);
    first_ok = yew_git_parse_reflog(&first_arena, data, (u64)len,
                                    &first, &first_err);
    second_ok = yew_git_parse_reflog(&second_arena, data, (u64)len,
                                     &second, &second_err);
    equal = first_ok == second_ok && first.len == second.len &&
            (first_ok || error_eq(&first_err, &second_err));
    for (i = 0U; equal && i < first.len; i++)
        equal = reflog_record_eq(&first.data[i], &second.data[i]);
    arena_free_all(&second_arena);
    arena_free_all(&first_arena);
    if (!equal)
        (void)snprintf(why, why_cap, "reflog parser is nondeterministic");
    return equal;
}

typedef bool (*DeterminismCheck)(const u8 *, size_t, char *, size_t);

static bool check_prefixed(DeterminismCheck check, const u8 *prefix,
                           size_t prefix_len, const u8 *data, size_t len,
                           char *why, size_t why_cap)
{
    size_t total = prefix_len + len;
    u8 *buf = malloc(total == 0U ? 1U : total);
    bool ok;

    if (buf == NULL) {
        (void)snprintf(why, why_cap, "out of memory");
        return false;
    }
    (void)memcpy(buf, prefix, prefix_len);
    if (len != 0U)
        (void)memcpy(buf + prefix_len, data, len);
    ok = check(buf, total, why, why_cap);
    free(buf);
    return ok;
}

static bool check_long_path(char *why, size_t why_cap)
{
    enum { PATH_BYTES = 4095 };
    u8 record[2U + PATH_BYTES + 1U];
    size_t i;

    record[0] = (u8)'?';
    record[1] = (u8)' ';
    for (i = 0U; i < PATH_BYTES; i++)
        record[2U + i] = (u8)('a' + (i % 23U));
    record[2U + 17U] = (u8)'\n';
    record[2U + PATH_BYTES] = 0U;
    return parse_deterministic(record, sizeof(record), why, why_cap);
}

static bool check_embedded_separators(const u8 *data, size_t len,
                                      char *why, size_t why_cap)
{
    static const u8 fixed[] = {
        '?', ' ', 'a', '\n', 'b', 0,
        '?', ' ', 'c', 0,
        '1', ' ', 'g', 'a', 'r', 'b', 'a', 'g', 'e', 0
    };
    u8 mixed[sizeof(fixed) + 64U];
    size_t take = len < 64U ? len : 64U;

    (void)memcpy(mixed, fixed, sizeof(fixed));
    if (take != 0U)
        (void)memcpy(mixed + sizeof(fixed), data, take);
    return parse_deterministic(mixed, sizeof(fixed) + take, why, why_cap);
}

static bool check_valid_prefixes(const u8 *status, size_t status_len,
                                 const u8 *paths, size_t paths_len,
                                 const u8 *blame, size_t blame_len,
                                 const u8 *log, size_t log_len,
                                 const u8 *reflog, size_t reflog_len,
                                 char *why, size_t why_cap)
{
    GitSnapshot snap;
    GitPathList path_list = {0};
    GitIgnoreSet ignored = {0};
    GitBlameLineList lines = {0};
    GitCommitMetaList commits = {0};
    GitLogRecordList logs = {0};
    GitReflogRecordList reflogs = {0};
    GitParseErr err = {0};
    Arena a;
    bool ok;

    snapshot_begin(&snap);
    ok = yew_git_parse_status(&snap, status, (u64)status_len, &err) &&
         snap.entries.len == 1U;
    snapshot_end(&snap);
    arena_init(&a);
    ok = ok && yew_git_parse_z_paths(&a, paths, (u64)paths_len,
                                     &path_list, &err) &&
         path_list.len == 3U;
    arena_free_all(&a);
    arena_init(&a);
    ok = ok && yew_git_parse_ignore(&a, paths, (u64)paths_len,
                                    &ignored, &err) && ignored.len == 3U;
    arena_free_all(&a);
    arena_init(&a);
    ok = ok && yew_git_parse_blame(&a, blame, (u64)blame_len,
                                   &lines, &commits, &err) == 1U;
    arena_free_all(&a);
    arena_init(&a);
    ok = ok && yew_git_parse_log(&a, log, (u64)log_len, &logs, &err) &&
         logs.len == 1U;
    arena_free_all(&a);
    arena_init(&a);
    ok = ok && yew_git_parse_reflog(&a, reflog, (u64)reflog_len,
                                    &reflogs, &err) && reflogs.len == 1U;
    arena_free_all(&a);
    if (!ok)
        (void)snprintf(why, why_cap, "supposed valid parser prefix rejected");
    return ok;
}

static bool check_porcelain(const u8 *data, size_t len,
                            char *why, size_t why_cap)
{
    u8 selector = len == 0U ? 0U : data[0];
    bool ok;

    switch (selector % 6U) {
    case 0U: ok = parse_deterministic(data, len, why, why_cap); break;
    case 1U: ok = paths_deterministic(data, len, why, why_cap); break;
    case 2U: ok = ignore_deterministic(data, len, why, why_cap); break;
    case 3U: ok = blame_deterministic(data, len, why, why_cap); break;
    case 4U: ok = log_deterministic(data, len, why, why_cap); break;
    default: ok = reflog_deterministic(data, len, why, why_cap); break;
    }
    if (!ok)
        return false;
    if ((selector & 63U) == 1U &&
        !check_embedded_separators(data, len, why, why_cap))
        return false;
    return true;
}

int main(int argc, char **argv)
{
    static const u8 status_prefix[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0? prefix-ok\0";
    static const u8 path_prefix[] = "alpha\0dir/\0line\nname\0";
    static const u8 blame_prefix[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 1\n"
        "author Jane\n"
        "author-mail <jane@example.test>\n"
        "author-time 1\n"
        "author-tz +0000\n"
        "summary subject\n"
        "filename file.c\n";
    static const u8 log_prefix[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\0aaaaaaa\0" "1\0Jane\0"
        "jane@example.test\0\0HEAD -> trunk\0subject\0body\0";
    static const u8 reflog_prefix[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\0aaaaaaa\0HEAD@{0}\0"
        "commit: subject\0" "1\0subject\0";
    static const u8 garbage[] = {0xffU, '\n', '2', ' ', 0U, 'x'};
    char why[256];

    if (!check_valid_prefixes(status_prefix, sizeof(status_prefix) - 1U,
                              path_prefix, sizeof(path_prefix) - 1U,
                              blame_prefix, sizeof(blame_prefix) - 1U,
                              log_prefix, sizeof(log_prefix) - 1U,
                              reflog_prefix, sizeof(reflog_prefix) - 1U,
                              why, sizeof(why)) ||
        !check_prefixed(parse_deterministic, status_prefix,
                        sizeof(status_prefix) - 1U,
                        garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_prefixed(paths_deterministic, path_prefix,
                        sizeof(path_prefix) - 1U,
                        garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_prefixed(ignore_deterministic, path_prefix,
                        sizeof(path_prefix) - 1U,
                        garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_prefixed(blame_deterministic, blame_prefix,
                        sizeof(blame_prefix) - 1U,
                        garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_prefixed(log_deterministic, log_prefix,
                        sizeof(log_prefix) - 1U,
                        garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_prefixed(reflog_deterministic, reflog_prefix,
                        sizeof(reflog_prefix) - 1U,
                        garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_embedded_separators(garbage, sizeof(garbage),
                                   why, sizeof(why)) ||
        !check_long_path(why, sizeof(why))) {
        (void)fprintf(stderr, "fuzz_porcelain: fixed case failed: %s\n", why);
        return 1;
    }
    return yew_fuzz_main(argc, argv, "fuzz_porcelain", NULL,
                         check_porcelain);
}
