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
        equal = first_err.off == second_err.off &&
                strcmp(first_err.message, second_err.message) == 0;
    snapshot_end(&second);
    snapshot_end(&first);
    if (!equal) {
        (void)snprintf(why, why_cap,
                       "same porcelain bytes produced different results");
        return false;
    }
    return true;
}

static bool check_prefixed(const u8 *data, size_t len,
                           char *why, size_t why_cap)
{
    static const u8 prefix[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0"
        "? prefix-ok\0";
    size_t total = sizeof(prefix) - 1U + len;
    u8 *buf = malloc(total == 0U ? 1U : total);
    bool ok;

    if (buf == NULL) {
        (void)snprintf(why, why_cap, "out of memory");
        return false;
    }
    (void)memcpy(buf, prefix, sizeof(prefix) - 1U);
    if (len != 0U)
        (void)memcpy(buf + sizeof(prefix) - 1U, data, len);
    ok = parse_deterministic(buf, total, why, why_cap);
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

static bool check_porcelain(const u8 *data, size_t len,
                            char *why, size_t why_cap)
{
    u8 selector = len == 0U ? 0U : data[0];

    if (!parse_deterministic(data, len, why, why_cap))
        return false;
    /* Arena blocks are deliberately large.  Sampling the structured
     * wrappers keeps the 800,000-case four-seed lane bounded while still
     * driving tens of thousands of prefix-plus-garbage mutations. */
    if ((selector & 15U) == 0U &&
        !check_prefixed(data, len, why, why_cap))
        return false;
    if ((selector & 63U) == 1U &&
        !check_embedded_separators(data, len, why, why_cap))
        return false;
    return true;
}

int main(int argc, char **argv)
{
    static const u8 garbage[] = {0xffU, '\n', '2', ' ', 0U, 'x'};
    char why[256];

    if (!check_prefixed(garbage, sizeof(garbage), why, sizeof(why)) ||
        !check_embedded_separators(garbage, sizeof(garbage),
                                   why, sizeof(why)) ||
        !check_long_path(why, sizeof(why))) {
        (void)fprintf(stderr, "fuzz_porcelain: fixed case failed: %s\n", why);
        return 1;
    }
    return yew_fuzz_main(argc, argv, "fuzz_porcelain", NULL,
                         check_porcelain);
}
