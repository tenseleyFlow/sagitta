/* Sprint 53: arbitrary line sequences must produce a minimal, replayable,
 * deterministic hunk list. */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/git/gutter.h"
#include "mod/git/diffview.h"
#include "mod/git/conflict.h"

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

static bool check_unified(const u8 *data, size_t len)
{
    Bytebuf patch = {0}, expected = {0};
    DiffUnified view = {0};
    size_t i, count = len > 64U ? 64U : len;
    u32 rows = (u32)count + 1U;
    bool ok;

    bytebuf_printf(&patch,
        "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1,%u +1,%u @@\n",
        rows, rows);
    for (i = 0U; i < (size_t)rows * 2U; i++) {
        u8 byte = i == 0U || len == 0U ? 'x' : data[(i - 1U) % len];

        if (byte == 0U || byte == '\n')
            byte = 'x';
        bytebuf_push_u8(&patch, i < rows ? '-' : '+');
        bytebuf_push_u8(&patch, byte);
        bytebuf_push_u8(&patch, '\n');
        if (i != 0U)
            bytebuf_push_u8(&expected, '\n');
        bytebuf_push_u8(&expected, byte);
    }
    ok = yew_diff_unified_build(&view, patch.data, patch.len) ==
             YEW_DIFF_UNIFIED_OK && view.rows.left.len == (size_t)rows * 2U &&
         view.bytes.len == expected.len &&
         memcmp(view.bytes.data, expected.data, expected.len) == 0;
    for (i = 0U; ok && i < (size_t)rows * 2U; i++)
        ok = view.rows.left.data[i] == (i < rows ? (i32)i : -1) &&
             view.rows.right.data[i] == (i < rows ? -1 : (i32)(i - rows));
    /* Mutate both framing and content. Failure must keep the last complete
     * projection; success must still have one source pair per output row. */
    for (i = 0U; i < count; i++)
        patch.data[((size_t)data[i] + i * 17U) % patch.len] = data[i];
    {
        DiffUnifiedResult result = yew_diff_unified_build(&view, patch.data,
                                                          patch.len);

        if (result == YEW_DIFF_UNIFIED_INVALID || result == YEW_DIFF_UNIFIED_LIMIT)
            ok = ok && view.bytes.len == expected.len &&
                 memcmp(view.bytes.data, expected.data, expected.len) == 0;
        ok = ok && view.rows.left.len == view.rows.right.len;
    }
    yew_diff_unified_drop(&view);
    bytebuf_free(&expected);
    bytebuf_free(&patch);
    return ok;
}

static u8 conflict_byte(const u8 *data, size_t len, size_t at)
{
    return len == 0U ? 0U : data[at % len];
}

static bool check_conflict_match(const u8 *data, size_t len)
{
    Bytebuf source = {0}, arm = {0};
    u8 source_lines[32], arm_lines[16];
    size_t offsets[33] = {0};
    size_t sn = conflict_byte(data, len, 0U) % 33U;
    size_t an = conflict_byte(data, len, 1U) % 17U;
    size_t i, j, hits = 0U, hit = 0U, steps = 0U;
    GitConflictMatch *work;
    GitConflictParseResult result;
    Span span;
    bool missing, ok;

    for (i = 0U; i < sn; i++) {
        u8 byte = conflict_byte(data, len, i + 2U);
        source_lines[i] = byte & 3U;
        bytebuf_push_u8(&source, (u8)('a' + source_lines[i]));
        if (i + 1U != sn || (byte & 16U) == 0U)
            bytebuf_append(&source, byte & 4U ? "\r\n" : "\n",
                             byte & 4U ? 2U : 1U);
        offsets[i + 1U] = source.len;
    }
    for (i = 0U; i < an; i++) {
        u8 byte = conflict_byte(data, len, i + 7U);
        arm_lines[i] = byte & 3U;
        bytebuf_push_u8(&arm, (u8)('a' + arm_lines[i]));
        bytebuf_append(&arm, byte & 8U ? "\r\n" : "\n", byte & 8U ? 2U : 1U);
    }
    if (an == 0U)
        hits = 1U;
    else if (an <= sn)
        for (i = 0U; i <= sn - an; i++) {
            for (j = 0U; j < an && source_lines[i + j] == arm_lines[j]; j++)
                ;
            if (j == an) {
                hits++;
                hit = i;
            }
        }
    work = yew_git_conflict_match_new(source.data, source.len, arm.data, arm.len);
    do {
        result = yew_git_conflict_match_step(work,
            1U + conflict_byte(data, len, 2U) % 17U);
    } while (result == YEW_CONFLICT_PENDING && ++steps < 10000U);
    ok = (result == YEW_CONFLICT_OK) == (hits == 1U) &&
         result != YEW_CONFLICT_PENDING;
    if (hits == 1U) {
        ok = ok && yew_git_conflict_match_span(work, &span, &missing) &&
             span.lo == offsets[hit] && span.hi == offsets[hit + an] &&
             missing == (an != 0U && hit + an == sn &&
                          source.data[source.len - 1U] != '\n');
    }
    yew_git_conflict_match_free(work);
    bytebuf_free(&source);
    bytebuf_free(&arm);
    return ok;
}

static void conflict_marker(Bytebuf *b, u8 c, u32 width, bool crlf)
{
    u32 i;
    for (i = 0U; i < width; i++)
        bytebuf_push_u8(b, c);
    bytebuf_append(b, crlf ? "\r\n" : "\n", crlf ? 2U : 1U);
}

static bool check_conflict_choices(const u8 *data, size_t len)
{
    Bytebuf text = {0}, current = {0}, incoming = {0}, out = {0};
    GitConflictParse p = {0};
    u32 width = 3U + conflict_byte(data, len, 0U) % 12U;
    bool crlf = (conflict_byte(data, len, 1U) & 1U) != 0U;
    size_t i, n = conflict_byte(data, len, 2U) % 64U;
    bool ok;
    for (i = 0U; i < n; i++) {
        bytebuf_push_u8(&current, (u8)('a' + conflict_byte(data, len, i) % 26U));
        bytebuf_push_u8(&incoming, (u8)('A' + conflict_byte(data, len, i) % 26U));
    }
    if (n != 0U) {
        bytebuf_append(&current, crlf ? "\r\n" : "\n", crlf ? 2U : 1U);
        bytebuf_append(&incoming, crlf ? "\r\n" : "\n", crlf ? 2U : 1U);
    }
    conflict_marker(&text, '<', width, crlf);
    bytebuf_append(&text, current.data, current.len);
    if (conflict_byte(data, len, 3U) & 1U) {
        conflict_marker(&text, '|', width, crlf);
        bytebuf_append(&text, "base\n", 5U);
    }
    conflict_marker(&text, '=', width, crlf);
    bytebuf_append(&text, incoming.data, incoming.len);
    conflict_marker(&text, '>', width, crlf);
    yew_git_conflict_parse_start(&p, text.data, text.len, width);
    while (yew_git_conflict_parse_step(&p, 1U) == YEW_CONFLICT_PENDING)
        ;
    ok = p.result == YEW_CONFLICT_OK && p.ranges.len == 1U;
    for (i = 0U; ok && i < 3U; i++) {
        Bytebuf expected = {0};
        if (i != YEW_CONFLICT_INCOMING)
            bytebuf_append(&expected, current.data, current.len);
        if (i != YEW_CONFLICT_CURRENT)
            bytebuf_append(&expected, incoming.data, incoming.len);
        ok = yew_git_conflict_choice(&out, text.data, text.len, &p.ranges.data[0],
                                     (GitConflictChoice)i, crlf) &&
             out.len == expected.len && (out.len == 0U ||
             memcmp(out.data, expected.data, out.len) == 0);
        bytebuf_free(&expected);
    }
    text.data[conflict_byte(data, len, 4U) % text.len] = 0U;
    yew_git_conflict_parse_start(&p, text.data, text.len, width);
    ok = ok && yew_git_conflict_parse_step(&p, SIZE_MAX) != YEW_CONFLICT_OK;
    yew_git_conflict_parse_drop(&p);
    bytebuf_free(&text);
    bytebuf_free(&current);
    bytebuf_free(&incoming);
    bytebuf_free(&out);
    return ok;
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

    if (!check_conflict_match(data, len) || !check_conflict_choices(data, len)) {
        (void)snprintf(why, why_cap, "conflict match/choice oracle mismatch");
        return false;
    }

    {
        GitConflictParse a = {0}, b = {0};
        size_t i;
        yew_git_conflict_parse_start(&a, data, len, 0U);
        yew_git_conflict_parse_start(&b, data, len, 0U);
        while (yew_git_conflict_parse_step(&a, 17U) == YEW_CONFLICT_PENDING)
            ;
        (void)yew_git_conflict_parse_step(&b, SIZE_MAX);
        ok = a.result == b.result && a.ranges.len == b.ranges.len;
        for (i = 0U; ok && i < a.ranges.len; i++) {
            const GitConflictRange *r = &a.ranges.data[i];
            ok = r->envelope.lo <= r->current.lo &&
                 r->current.lo <= r->current.hi &&
                 r->current.hi <= r->incoming.lo &&
                 r->incoming.lo <= r->incoming.hi &&
                 r->incoming.hi <= r->envelope.hi && r->envelope.hi <= len &&
                 r->envelope.lo == b.ranges.data[i].envelope.lo &&
                 r->envelope.hi == b.ranges.data[i].envelope.hi;
        }
        yew_git_conflict_parse_drop(&a);
        yew_git_conflict_parse_drop(&b);
        if (!ok) {
            (void)snprintf(why, why_cap, "conflict parse/resumption mismatch");
            return false;
        }
    }

    if (!check_unified(data, len)) {
        (void)snprintf(why, why_cap, "unified projection or atomic failure mismatch");
        return false;
    }
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
