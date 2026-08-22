#include "mod/git/gutter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum DiffOp {
    DIFF_EQUAL,
    DIFF_DELETE,
    DIFF_INSERT
} DiffOp;

VEC_DECL(DiffOpVec, DiffOp);

typedef struct PatchLine {
    size_t off;
    size_t len;
    bool has_nl;
} PatchLine;

VEC_DECL(PatchLineVec, PatchLine);

u64 yew_git_line_hash(const u8 *bytes, size_t len)
{
    u64 hash = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool yew_git_hash_lines(const u8 *bytes, size_t len, Arena *arena,
                        u64 **hashes, u32 *count, bool *missing_final_nl)
{
    size_t at;
    u32 nlines = 0U;
    u32 line = 0U;
    u64 *result;

    if ((bytes == NULL && len != 0U) || arena == NULL || hashes == NULL ||
        count == NULL || missing_final_nl == NULL)
        return false;
    *missing_final_nl = len != 0U && bytes[len - 1U] != '\n';
    for (at = 0U; at < len; at++) {
        if (bytes[at] == '\n')
            nlines++;
    }
    if (*missing_final_nl)
        nlines++;
    result = nlines == 0U ? NULL :
             arena_alloc(arena, (size_t)nlines * sizeof(*result),
                         _Alignof(u64));
    at = 0U;
    while (at < len) {
        size_t end = at;

        while (end < len && bytes[end] != '\n')
            end++;
        if (end < len)
            end++;
        result[line++] = yew_git_line_hash(bytes + at, end - at);
        at = end;
    }
    *hashes = result;
    *count = nlines;
    return true;
}

static void diff_ops_reverse(DiffOpVec *ops)
{
    size_t lo = 0U;
    size_t hi = ops->len;

    while (lo < hi) {
        DiffOp tmp;

        hi--;
        if (lo >= hi)
            break;
        tmp = ops->data[lo];
        ops->data[lo] = ops->data[hi];
        ops->data[hi] = tmp;
        lo++;
    }
}

static bool diff_backtrack(i32 **trace, u32 distance, const u64 *left,
                           u32 left_n, const u64 *right, u32 right_n,
                           u32 prefix, i32 offset, DiffOpVec *ops)
{
    i32 x = (i32)left_n;
    i32 y = (i32)right_n;
    i32 d;

    for (d = (i32)distance; d >= 0; d--) {
        i32 *v = trace[d];
        i32 k = x - y;
        i32 prev_k;
        i32 prev_x;
        i32 prev_y;

        if (k == -d || (k != d && v[offset + k - 1] <
                                      v[offset + k + 1]))
            prev_k = k + 1;
        else
            prev_k = k - 1;
        prev_x = v[offset + prev_k];
        prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            if (left[prefix + (u32)x - 1U] !=
                right[prefix + (u32)y - 1U])
                return false;
            DiffOpVec_push(ops, DIFF_EQUAL);
            x--;
            y--;
        }
        if (d == 0)
            break;
        if (x == prev_x) {
            DiffOpVec_push(ops, DIFF_INSERT);
            y--;
        } else {
            DiffOpVec_push(ops, DIFF_DELETE);
            x--;
        }
    }
    return x == 0 && y == 0;
}

static void diff_emit_hunks(const DiffOpVec *ops, u32 prefix,
                            GitHunkVec *out)
{
    u32 left = prefix;
    u32 right = prefix;
    size_t at = 0U;

    while (at < ops->len) {
        u32 base_lo;
        u32 buf_lo;

        if (ops->data[at] == DIFF_EQUAL) {
            left++;
            right++;
            at++;
            continue;
        }
        base_lo = left;
        buf_lo = right;
        while (at < ops->len && ops->data[at] != DIFF_EQUAL) {
            if (ops->data[at] == DIFF_DELETE)
                left++;
            else
                right++;
            at++;
        }
        GitHunkVec_push(out, ((GitHunk){
            LINENO(base_lo), LINENO(left - base_lo),
            LINENO(buf_lo), LINENO(right - buf_lo),
            left == base_lo ? YEW_HUNK_ADD :
            right == buf_lo ? YEW_HUNK_DEL : YEW_HUNK_MOD
        }));
    }
}

/* Standard Myers frontier search.  We retain the bounded frontier history
 * needed for deterministic reconstruction.  The cap is D, never N*M, so a
 * 100 kLOC file with one changed row takes a tiny trace. */
bool yew_diff_lines(Arena *arena, const u64 *left, u32 left_n,
                    const u64 *right, u32 right_n, u32 budget_d,
                    GitHunkVec *out)
{
    u32 prefix = 0U;
    u32 suffix = 0U;
    u32 middle_left;
    u32 middle_right;
    u32 max_d;
    size_t width;
    i32 *frontier;
    i32 **trace;
    DiffOpVec ops = {0};
    u32 distance;
    u32 trace_count = 0U;
    bool found = false;
    bool ok = false;

    (void)arena;
    if (out == NULL || (left == NULL && left_n != 0U) ||
        (right == NULL && right_n != 0U))
        return false;
    out->len = 0U;
    while (prefix < left_n && prefix < right_n &&
           left[prefix] == right[prefix])
        prefix++;
    while (suffix < left_n - prefix && suffix < right_n - prefix &&
           left[left_n - suffix - 1U] == right[right_n - suffix - 1U])
        suffix++;
    middle_left = left_n - prefix - suffix;
    middle_right = right_n - prefix - suffix;
    if (middle_left == 0U && middle_right == 0U)
        return true;
    if (middle_left > UINT32_MAX - middle_right)
        return false;
    max_d = middle_left + middle_right;
    if (max_d > budget_d)
        max_d = budget_d;
    width = (size_t)max_d * 2U + 3U;
    frontier = yew_xmalloc(width * sizeof(*frontier));
    trace = yew_xcalloc((size_t)max_d + 1U, sizeof(*trace));
    for (distance = 0U; distance < width; distance++)
        frontier[distance] = -1;
    frontier[max_d + 2U] = 0;
    for (distance = 0U; distance <= max_d; distance++) {
        i32 d = (i32)distance;
        i32 offset = (i32)max_d + 1;
        i32 k;

        trace[distance] = yew_xmalloc(width * sizeof(**trace));
        trace_count = distance + 1U;
        (void)memcpy(trace[distance], frontier,
                     width * sizeof(**trace));
        for (k = -d; k <= d; k += 2) {
            i32 x;
            i32 y;

            if (k == -d || (k != d && frontier[offset + k - 1] <
                                          frontier[offset + k + 1]))
                x = frontier[offset + k + 1];
            else
                x = frontier[offset + k - 1] + 1;
            y = x - k;
            while (x < (i32)middle_left && y < (i32)middle_right &&
                   left[prefix + (u32)x] == right[prefix + (u32)y]) {
                x++;
                y++;
            }
            frontier[offset + k] = x;
            if (x >= (i32)middle_left && y >= (i32)middle_right) {
                found = true;
                break;
            }
        }
        if (found)
            break;
    }
    if (found && diff_backtrack(trace, distance, left, middle_left, right,
                                middle_right, prefix, (i32)max_d + 1,
                                &ops)) {
        diff_ops_reverse(&ops);
        diff_emit_hunks(&ops, prefix, out);
        ok = true;
    }
    DiffOpVec_free(&ops);
    for (distance = 0U; distance < trace_count; distance++)
        free(trace[distance]);
    free(trace);
    free(frontier);
    if (!ok)
        out->len = 0U;
    return ok;
}

static bool patch_lines(const u8 *bytes, size_t len, PatchLineVec *out)
{
    size_t at = 0U;

    while (at < len) {
        size_t end = at;
        bool has_nl;

        while (end < len && bytes[end] != '\n')
            end++;
        has_nl = end < len;
        if (has_nl)
            end++;
        PatchLineVec_push(out, ((PatchLine){at, end - at, has_nl}));
        at = end;
    }
    return out->len <= UINT32_MAX;
}

static void patch_emit_line(Bytebuf *out, u8 prefix, const u8 *bytes,
                            PatchLine line)
{
    bytebuf_push_u8(out, prefix);
    bytebuf_append(out, bytes + line.off, line.len);
    if (!line.has_nl) {
        bytebuf_push_u8(out, '\n');
        bytebuf_append(out, "\\ No newline at end of file\n",
                       sizeof("\\ No newline at end of file\n") - 1U);
    }
}

static u64 patch_header_start(u64 zero_based, u64 count)
{
    return count == 0U ? zero_based : zero_based + 1U;
}

bool yew_git_hunk_patch(Bytebuf *out, const char *path, const u8 *base,
                        size_t base_len, const u8 *buf, size_t buf_len,
                        const GitHunk *hunk)
{
    PatchLineVec base_lines = {0};
    PatchLineVec buf_lines = {0};
    u64 before;
    u64 after;
    u64 base_start;
    u64 buf_start;
    u64 base_count;
    u64 buf_count;
    u64 i;
    bool ok = false;

    if (out == NULL || path == NULL || strchr(path, '\n') != NULL ||
        (base == NULL && base_len != 0U) || (buf == NULL && buf_len != 0U) ||
        hunk == NULL)
        return false;
    bytebuf_init(out);
    if (!patch_lines(base, base_len, &base_lines) ||
        !patch_lines(buf, buf_len, &buf_lines))
        goto done;
    if (hunk->base_lo.v > base_lines.len || hunk->buf_lo.v > buf_lines.len ||
        hunk->base_n.v > base_lines.len - hunk->base_lo.v ||
        hunk->buf_n.v > buf_lines.len - hunk->buf_lo.v)
        goto done;
    before = hunk->base_lo.v < hunk->buf_lo.v ? hunk->base_lo.v :
                                                   hunk->buf_lo.v;
    if (before > YEW_GIT_HUNK_CONTEXT)
        before = YEW_GIT_HUNK_CONTEXT;
    after = (u64)base_lines.len - hunk->base_lo.v - hunk->base_n.v;
    if ((u64)buf_lines.len - hunk->buf_lo.v - hunk->buf_n.v < after)
        after = (u64)buf_lines.len - hunk->buf_lo.v - hunk->buf_n.v;
    if (after > YEW_GIT_HUNK_CONTEXT)
        after = YEW_GIT_HUNK_CONTEXT;
    base_start = hunk->base_lo.v - before;
    buf_start = hunk->buf_lo.v - before;
    base_count = before + hunk->base_n.v + after;
    buf_count = before + hunk->buf_n.v + after;
    bytebuf_printf(out,
                   "diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n"
                   "@@ -%llu,%llu +%llu,%llu @@\n",
                   path, path, path, path,
                   (unsigned long long)patch_header_start(base_start,
                                                          base_count),
                   (unsigned long long)base_count,
                   (unsigned long long)patch_header_start(buf_start,
                                                          buf_count),
                   (unsigned long long)buf_count);
    for (i = 0U; i < before; i++)
        patch_emit_line(out, ' ', base,
                        base_lines.data[(size_t)(base_start + i)]);
    for (i = 0U; i < hunk->base_n.v; i++)
        patch_emit_line(out, '-', base,
                        base_lines.data[(size_t)(hunk->base_lo.v + i)]);
    for (i = 0U; i < hunk->buf_n.v; i++)
        patch_emit_line(out, '+', buf,
                        buf_lines.data[(size_t)(hunk->buf_lo.v + i)]);
    for (i = 0U; i < after; i++)
        patch_emit_line(out, ' ', base,
                        base_lines.data[(size_t)(hunk->base_lo.v +
                                                hunk->base_n.v + i)]);
    ok = true;
done:
    PatchLineVec_free(&base_lines);
    PatchLineVec_free(&buf_lines);
    if (!ok)
        bytebuf_free(out);
    return ok;
}
