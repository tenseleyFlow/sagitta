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

typedef struct DiffLine {
    size_t off;
    size_t len;
} DiffLine;

typedef struct DiffSeq {
    const u64 *hashes;
    const u8 *bytes;
    const DiffLine *lines;
} DiffSeq;

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

static bool diff_equal(const DiffSeq *left, u32 left_at,
                       const DiffSeq *right, u32 right_at)
{
    const DiffLine *a;
    const DiffLine *b;

    if (left->hashes[left_at] != right->hashes[right_at])
        return false;
    if (left->lines == NULL || right->lines == NULL)
        return true;
    a = &left->lines[left_at];
    b = &right->lines[right_at];
    return a->len == b->len &&
           (a->len == 0U ||
            memcmp(left->bytes + a->off, right->bytes + b->off,
                   a->len) == 0);
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

static bool diff_backtrack(i32 **trace, u32 distance, const DiffSeq *left,
                           u32 left_n, const DiffSeq *right, u32 right_n,
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
            if (!diff_equal(left, prefix + (u32)x - 1U,
                            right, prefix + (u32)y - 1U))
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
static bool diff_sequences(Arena *arena, const DiffSeq *left, u32 left_n,
                           const DiffSeq *right, u32 right_n, u32 budget_d,
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
    if (out == NULL || left == NULL || right == NULL ||
        (left->hashes == NULL && left_n != 0U) ||
        (right->hashes == NULL && right_n != 0U))
        return false;
    out->len = 0U;
    while (prefix < left_n && prefix < right_n &&
           diff_equal(left, prefix, right, prefix))
        prefix++;
    while (suffix < left_n - prefix && suffix < right_n - prefix &&
           diff_equal(left, left_n - suffix - 1U,
                      right, right_n - suffix - 1U))
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
                   diff_equal(left, prefix + (u32)x,
                              right, prefix + (u32)y)) {
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

bool yew_diff_lines(Arena *arena, const u64 *left, u32 left_n,
                    const u64 *right, u32 right_n, u32 budget_d,
                    GitHunkVec *out)
{
    DiffSeq a = {left, NULL, NULL};
    DiffSeq b = {right, NULL, NULL};

    return diff_sequences(arena, &a, left_n, &b, right_n, budget_d, out);
}

bool yew_diff_within_size_limits(size_t left_bytes, u32 left_lines,
                                 size_t right_bytes, u32 right_lines)
{
    return left_bytes <= YEW_DIFF_MAX_BYTES &&
           right_bytes <= YEW_DIFF_MAX_BYTES &&
           left_lines <= YEW_DIFF_MAX_LINES &&
           right_lines <= YEW_DIFF_MAX_LINES;
}

typedef enum DiffWorkPhase {
    WORK_COUNT_LEFT,
    WORK_COUNT_RIGHT,
    WORK_ALLOC_LINES,
    WORK_HASH_LEFT,
    WORK_HASH_RIGHT,
    WORK_PREFIX,
    WORK_SUFFIX,
    WORK_INIT_SEARCH,
    WORK_COPY_TRACE,
    WORK_SEARCH,
    WORK_BACKTRACK,
    WORK_REVERSE,
    WORK_EMIT,
    WORK_FINISH,
    WORK_DONE
} DiffWorkPhase;

typedef enum EqualProgress {
    EQUAL_MORE,
    EQUAL_YES,
    EQUAL_NO
} EqualProgress;

struct YewDiffWork {
    const u8 *left;
    const u8 *right;
    size_t left_len;
    size_t right_len;
    YewGitLineHashFn hash_fn;
    u32 budget_d;
    DiffLine *left_lines;
    DiffLine *right_lines;
    u64 *left_hashes;
    u64 *right_hashes;
    u32 left_n;
    u32 right_n;
    size_t at;
    size_t line_start;
    u32 line_at;
    u64 hash;
    u32 prefix;
    u32 suffix;
    u32 middle_left;
    u32 middle_right;
    u32 max_d;
    size_t width;
    i32 offset;
    i32 *frontier;
    i32 **trace;
    u32 distance;
    u32 trace_count;
    size_t copy_at;
    i32 k;
    i32 x;
    i32 y;
    bool snake_active;
    bool equal_active;
    u32 equal_left;
    u32 equal_right;
    size_t equal_at;
    i32 back_d;
    i32 back_x;
    i32 back_y;
    i32 prev_x;
    i32 prev_y;
    bool back_selected;
    size_t reverse_at;
    size_t emit_at;
    u32 emit_left;
    u32 emit_right;
    bool emit_hunk;
    u32 emit_base_lo;
    u32 emit_buf_lo;
    DiffOpVec ops;
    GitHunkVec result;
    YewDiffOutcome outcome;
    DiffWorkPhase phase;
    bool taken;
};

static bool work_tick_expired(YewDiffNowUsFn now_us, void *ctx,
                              i64 deadline)
{
    return now_us(ctx) >= deadline;
}

static EqualProgress work_equal(YewDiffWork *work, u32 left_at,
                                u32 right_at)
{
    const DiffLine *a = &work->left_lines[left_at];
    const DiffLine *b = &work->right_lines[right_at];
    size_t limit;

    if (!work->equal_active) {
        if (work->left_hashes[left_at] != work->right_hashes[right_at] ||
            a->len != b->len)
            return EQUAL_NO;
        work->equal_active = true;
        work->equal_left = left_at;
        work->equal_right = right_at;
        work->equal_at = 0U;
    }
    if (work->equal_left != left_at || work->equal_right != right_at)
        return EQUAL_NO;
    limit = work->equal_at + 4096U;
    if (limit < work->equal_at || limit > a->len)
        limit = a->len;
    while (work->equal_at < limit) {
        if (work->left[a->off + work->equal_at] !=
            work->right[b->off + work->equal_at]) {
            work->equal_active = false;
            return EQUAL_NO;
        }
        work->equal_at++;
    }
    return work->equal_at < a->len ? EQUAL_MORE : EQUAL_YES;
}

static void work_equal_reset(YewDiffWork *work)
{
    work->equal_active = false;
}

static void work_hash_byte(YewDiffWork *work, const u8 *bytes)
{
    work->hash ^= bytes[work->at];
    work->hash *= UINT64_C(1099511628211);
    work->at++;
}

static bool work_hash_line(YewDiffWork *work, const u8 *bytes, size_t len,
                           DiffLine *lines, u64 *hashes)
{
    size_t limit = work->at + 4096U;

    if (limit < work->at || limit > len)
        limit = len;
    while (work->at < limit) {
        bool end;

        work_hash_byte(work, bytes);
        end = bytes[work->at - 1U] == '\n' || work->at == len;
        if (!end)
            continue;
        lines[work->line_at] = (DiffLine){work->line_start,
                                          work->at - work->line_start};
        hashes[work->line_at] = work->hash_fn == yew_git_line_hash ?
                                work->hash :
                                work->hash_fn(bytes + work->line_start,
                                              work->at - work->line_start);
        work->line_at++;
        work->line_start = work->at;
        work->hash = UINT64_C(1469598103934665603);
        break;
    }
    return work->at == len;
}

static void work_truncated(YewDiffWork *work)
{
    GitHunkVec_push(&work->result, ((GitHunk){
        LINENO(0U), LINENO(work->left_n), LINENO(0U), LINENO(work->right_n),
        YEW_HUNK_MOD
    }));
    work->outcome = YEW_DIFF_TRUNCATED;
    work->phase = WORK_DONE;
}

static void work_free_search(YewDiffWork *work)
{
    u32 i;

    for (i = 0U; i < work->trace_count; i++)
        free(work->trace[i]);
    free(work->trace);
    free(work->frontier);
    work->trace = NULL;
    work->frontier = NULL;
    work->trace_count = 0U;
}

YewDiffWork *yew_diff_work_begin_bytes_with_hash(
    const u8 *left, size_t left_len, const u8 *right, size_t right_len,
    u32 budget_d, YewGitLineHashFn hash_fn)
{
    YewDiffWork *work;

    if (hash_fn == NULL || (left == NULL && left_len != 0U) ||
        (right == NULL && right_len != 0U))
        return NULL;
    work = yew_xcalloc(1U, sizeof(*work));
    work->left = left;
    work->right = right;
    work->left_len = left_len;
    work->right_len = right_len;
    work->hash_fn = hash_fn;
    work->budget_d = budget_d;
    work->hash = UINT64_C(1469598103934665603);
    work->outcome = YEW_DIFF_OK;
    work->phase = left_len > YEW_DIFF_MAX_BYTES ||
                  right_len > YEW_DIFF_MAX_BYTES ?
                  WORK_DONE : WORK_COUNT_LEFT;
    if (work->phase == WORK_DONE)
        work->outcome = YEW_DIFF_TOO_LARGE;
    return work;
}

YewDiffWork *yew_diff_work_begin_bytes(const u8 *left, size_t left_len,
                                       const u8 *right, size_t right_len,
                                       u32 budget_d)
{
    return yew_diff_work_begin_bytes_with_hash(left, left_len, right,
                                               right_len, budget_d,
                                               yew_git_line_hash);
}

YewDiffProgress yew_diff_work_step(YewDiffWork *work, u32 budget_us,
                                   YewDiffNowUsFn now_us, void *clock_ctx)
{
    i64 start;
    i64 deadline;

    if (work == NULL || now_us == NULL || work->phase == WORK_DONE)
        return YEW_DIFF_DONE;
    start = now_us(clock_ctx);
    deadline = start > INT64_MAX - (i64)budget_us ? INT64_MAX :
               start + (i64)budget_us;
    do {
        switch (work->phase) {
        case WORK_COUNT_LEFT:
            if (work->at < work->left_len) {
                size_t limit = work->at + 4096U;

                if (limit < work->at || limit > work->left_len)
                    limit = work->left_len;
                while (work->at < limit) {
                    if (work->left[work->at++] == '\n')
                        work->left_n++;
                }
            } else {
                if (work->left_len != 0U &&
                    work->left[work->left_len - 1U] != '\n')
                    work->left_n++;
                work->at = 0U;
                work->phase = WORK_COUNT_RIGHT;
            }
            break;
        case WORK_COUNT_RIGHT:
            if (work->at < work->right_len) {
                size_t limit = work->at + 4096U;

                if (limit < work->at || limit > work->right_len)
                    limit = work->right_len;
                while (work->at < limit) {
                    if (work->right[work->at++] == '\n')
                        work->right_n++;
                }
            } else {
                if (work->right_len != 0U &&
                    work->right[work->right_len - 1U] != '\n')
                    work->right_n++;
                work->phase = WORK_ALLOC_LINES;
            }
            break;
        case WORK_ALLOC_LINES:
            if (!yew_diff_within_size_limits(work->left_len, work->left_n,
                                              work->right_len,
                                              work->right_n)) {
                work->outcome = YEW_DIFF_TOO_LARGE;
                work->phase = WORK_DONE;
                break;
            }
            work->left_lines = work->left_n == 0U ? NULL :
                yew_xmalloc((size_t)work->left_n * sizeof(*work->left_lines));
            work->left_hashes = work->left_n == 0U ? NULL :
                yew_xmalloc((size_t)work->left_n * sizeof(*work->left_hashes));
            work->right_lines = work->right_n == 0U ? NULL :
                yew_xmalloc((size_t)work->right_n * sizeof(*work->right_lines));
            work->right_hashes = work->right_n == 0U ? NULL :
                yew_xmalloc((size_t)work->right_n * sizeof(*work->right_hashes));
            work->at = 0U;
            work->line_start = 0U;
            work->line_at = 0U;
            work->phase = WORK_HASH_LEFT;
            break;
        case WORK_HASH_LEFT:
            if (work_hash_line(work, work->left, work->left_len,
                               work->left_lines, work->left_hashes)) {
                work->at = 0U;
                work->line_start = 0U;
                work->line_at = 0U;
                work->hash = UINT64_C(1469598103934665603);
                work->phase = WORK_HASH_RIGHT;
            }
            break;
        case WORK_HASH_RIGHT:
            if (work_hash_line(work, work->right, work->right_len,
                               work->right_lines, work->right_hashes))
                work->phase = WORK_PREFIX;
            break;
        case WORK_PREFIX:
            if (work->prefix >= work->left_n ||
                work->prefix >= work->right_n) {
                work->phase = WORK_SUFFIX;
            } else {
                EqualProgress eq = work_equal(work, work->prefix,
                                              work->prefix);
                if (eq == EQUAL_YES) {
                    work->prefix++;
                    work_equal_reset(work);
                } else if (eq == EQUAL_NO) {
                    work_equal_reset(work);
                    work->phase = WORK_SUFFIX;
                }
            }
            break;
        case WORK_SUFFIX:
            if (work->suffix >= work->left_n - work->prefix ||
                work->suffix >= work->right_n - work->prefix) {
                work->phase = WORK_INIT_SEARCH;
            } else {
                u32 li = work->left_n - work->suffix - 1U;
                u32 ri = work->right_n - work->suffix - 1U;
                EqualProgress eq = work_equal(work, li, ri);
                if (eq == EQUAL_YES) {
                    work->suffix++;
                    work_equal_reset(work);
                } else if (eq == EQUAL_NO) {
                    work_equal_reset(work);
                    work->phase = WORK_INIT_SEARCH;
                }
            }
            break;
        case WORK_INIT_SEARCH:
        {
            size_t i;

            work->middle_left = work->left_n - work->prefix - work->suffix;
            work->middle_right = work->right_n - work->prefix - work->suffix;
            if (work->middle_left == 0U && work->middle_right == 0U) {
                work->phase = WORK_DONE;
                break;
            }
            if (work->middle_left > UINT32_MAX - work->middle_right) {
                work->outcome = YEW_DIFF_INVALID;
                work->phase = WORK_DONE;
                break;
            }
            work->max_d = work->middle_left + work->middle_right;
            if (work->max_d > work->budget_d)
                work->max_d = work->budget_d;
            work->width = (size_t)work->max_d * 2U + 3U;
            work->offset = (i32)work->max_d + 1;
            work->frontier = yew_xmalloc(work->width * sizeof(*work->frontier));
            work->trace = yew_xcalloc((size_t)work->max_d + 1U,
                                      sizeof(*work->trace));
            for (i = 0U; i < work->width; i++)
                work->frontier[i] = -1;
            work->frontier[work->max_d + 2U] = 0;
            work->copy_at = 0U;
            work->distance = 0U;
            work->k = 0;
            work->phase = WORK_COPY_TRACE;
            break;
        }
        case WORK_COPY_TRACE:
            if (work->trace[work->distance] == NULL) {
                work->trace[work->distance] =
                    yew_xmalloc(work->width * sizeof(**work->trace));
                work->trace_count = work->distance + 1U;
            }
            if (work->copy_at < work->width) {
                size_t limit = work->copy_at + 1024U;

                if (limit < work->copy_at || limit > work->width)
                    limit = work->width;
                while (work->copy_at < limit) {
                    work->trace[work->distance][work->copy_at] =
                        work->frontier[work->copy_at];
                    work->copy_at++;
                }
            } else {
                work->k = -(i32)work->distance;
                work->snake_active = false;
                work->phase = WORK_SEARCH;
            }
            break;
        case WORK_SEARCH:
            if (!work->snake_active) {
                i32 d = (i32)work->distance;
                if (work->k > d) {
                    if (work->distance == work->max_d) {
                        work_truncated(work);
                    } else {
                        work->distance++;
                        work->copy_at = 0U;
                        work->phase = WORK_COPY_TRACE;
                    }
                    break;
                }
                if (work->k == -d ||
                    (work->k != d &&
                     work->frontier[work->offset + work->k - 1] <
                     work->frontier[work->offset + work->k + 1]))
                    work->x = work->frontier[work->offset + work->k + 1];
                else
                    work->x = work->frontier[work->offset + work->k - 1] + 1;
                work->y = work->x - work->k;
                work->snake_active = true;
            }
            if (work->x < (i32)work->middle_left &&
                work->y < (i32)work->middle_right) {
                EqualProgress eq = work_equal(
                    work, work->prefix + (u32)work->x,
                    work->prefix + (u32)work->y);
                if (eq == EQUAL_MORE)
                    break;
                work_equal_reset(work);
                if (eq == EQUAL_YES) {
                    work->x++;
                    work->y++;
                    break;
                }
            }
            work->frontier[work->offset + work->k] = work->x;
            work->snake_active = false;
            if (work->x >= (i32)work->middle_left &&
                work->y >= (i32)work->middle_right) {
                work->back_d = (i32)work->distance;
                work->back_x = (i32)work->middle_left;
                work->back_y = (i32)work->middle_right;
                work->back_selected = false;
                work->phase = WORK_BACKTRACK;
            } else {
                work->k += 2;
            }
            break;
        case WORK_BACKTRACK:
            if (!work->back_selected) {
                i32 *v;
                i32 bk;
                i32 prev_k;

                if (work->back_d < 0) {
                    work->reverse_at = 0U;
                    work->phase = WORK_REVERSE;
                    break;
                }
                v = work->trace[work->back_d];
                bk = work->back_x - work->back_y;
                if (bk == -work->back_d ||
                    (bk != work->back_d &&
                     v[work->offset + bk - 1] <
                     v[work->offset + bk + 1]))
                    prev_k = bk + 1;
                else
                    prev_k = bk - 1;
                work->prev_x = v[work->offset + prev_k];
                work->prev_y = work->prev_x - prev_k;
                work->back_selected = true;
            }
            if (work->back_x > work->prev_x &&
                work->back_y > work->prev_y) {
                DiffOpVec_push(&work->ops, DIFF_EQUAL);
                work->back_x--;
                work->back_y--;
            } else {
                if (work->back_d != 0) {
                    if (work->back_x == work->prev_x) {
                        DiffOpVec_push(&work->ops, DIFF_INSERT);
                        work->back_y--;
                    } else {
                        DiffOpVec_push(&work->ops, DIFF_DELETE);
                        work->back_x--;
                    }
                }
                work->back_d--;
                work->back_selected = false;
            }
            break;
        case WORK_REVERSE:
            if (work->reverse_at < work->ops.len / 2U) {
                size_t hi = work->ops.len - work->reverse_at - 1U;
                DiffOp tmp = work->ops.data[work->reverse_at];
                work->ops.data[work->reverse_at] = work->ops.data[hi];
                work->ops.data[hi] = tmp;
                work->reverse_at++;
            } else {
                work->emit_left = work->prefix;
                work->emit_right = work->prefix;
                work->phase = WORK_EMIT;
            }
            break;
        case WORK_EMIT:
            if (work->emit_at >= work->ops.len) {
                if (work->emit_hunk) {
                    GitHunkVec_push(&work->result, ((GitHunk){
                        LINENO(work->emit_base_lo),
                        LINENO(work->emit_left - work->emit_base_lo),
                        LINENO(work->emit_buf_lo),
                        LINENO(work->emit_right - work->emit_buf_lo),
                        work->emit_left == work->emit_base_lo ? YEW_HUNK_ADD :
                        work->emit_right == work->emit_buf_lo ? YEW_HUNK_DEL :
                        YEW_HUNK_MOD
                    }));
                    work->emit_hunk = false;
                }
                work->phase = WORK_FINISH;
                break;
            }
            if (work->ops.data[work->emit_at] == DIFF_EQUAL) {
                if (work->emit_hunk) {
                    GitHunkVec_push(&work->result, ((GitHunk){
                        LINENO(work->emit_base_lo),
                        LINENO(work->emit_left - work->emit_base_lo),
                        LINENO(work->emit_buf_lo),
                        LINENO(work->emit_right - work->emit_buf_lo),
                        work->emit_left == work->emit_base_lo ? YEW_HUNK_ADD :
                        work->emit_right == work->emit_buf_lo ? YEW_HUNK_DEL :
                        YEW_HUNK_MOD
                    }));
                    work->emit_hunk = false;
                }
                work->emit_left++;
                work->emit_right++;
            } else {
                if (!work->emit_hunk) {
                    work->emit_base_lo = work->emit_left;
                    work->emit_buf_lo = work->emit_right;
                    work->emit_hunk = true;
                }
                if (work->ops.data[work->emit_at] == DIFF_DELETE)
                    work->emit_left++;
                else
                    work->emit_right++;
            }
            work->emit_at++;
            break;
        case WORK_FINISH:
            work_free_search(work);
            work->phase = WORK_DONE;
            break;
        case WORK_DONE:
            break;
        }
    } while (work->phase != WORK_DONE &&
             !work_tick_expired(now_us, clock_ctx, deadline));
    return work->phase == WORK_DONE ? YEW_DIFF_DONE : YEW_DIFF_MORE;
}

YewDiffOutcome yew_diff_work_take(YewDiffWork *work, GitHunkVec *out)
{
    if (work == NULL || out == NULL || work->phase != WORK_DONE ||
        work->taken)
        return YEW_DIFF_INVALID;
    GitHunkVec_free(out);
    *out = work->result;
    work->result = (GitHunkVec){0};
    work->taken = true;
    return work->outcome;
}

void yew_diff_work_free(YewDiffWork *work)
{
    if (work == NULL)
        return;
    free(work->left_lines);
    free(work->right_lines);
    free(work->left_hashes);
    free(work->right_hashes);
    work_free_search(work);
    DiffOpVec_free(&work->ops);
    GitHunkVec_free(&work->result);
    free(work);
}

bool yew_git_hunk_sign_placement(const GitHunk *h, u64 buffer_line_count,
                                 bool terminal_line_is_synthetic,
                                 LineNo *line, bool *delete_below)
{
    u64 real_lines;
    u64 target;

    if (h == NULL || line == NULL || delete_below == NULL ||
        buffer_line_count == 0U)
        return false;
    real_lines = buffer_line_count;
    if (terminal_line_is_synthetic && real_lines != 0U)
        real_lines--;
    *delete_below = h->kind == YEW_HUNK_DEL && h->buf_n.v == 0U &&
                    h->buf_lo.v >= real_lines && real_lines != 0U;
    if (*delete_below)
        target = real_lines - 1U;
    else if (h->buf_lo.v < buffer_line_count)
        target = h->buf_lo.v;
    else
        target = buffer_line_count - 1U;
    *line = LINENO(target);
    return true;
}

static i64 sync_clock(void *ctx)
{
    i64 *ticks = ctx;

    return (*ticks)++;
}

YewDiffOutcome yew_diff_bytes_with_hash(Arena *arena,
                                        const u8 *left, size_t left_len,
                                        const u8 *right, size_t right_len,
                                        u32 budget_d,
                                        YewGitLineHashFn hash_fn,
                                        GitHunkVec *out)
{
    YewDiffWork *work;
    YewDiffOutcome outcome;
    i64 ticks = 0;

    if (arena == NULL || out == NULL || hash_fn == NULL ||
        (left == NULL && left_len != 0U) ||
        (right == NULL && right_len != 0U))
        return YEW_DIFF_INVALID;
    work = yew_diff_work_begin_bytes_with_hash(left, left_len, right,
                                               right_len, budget_d, hash_fn);
    if (work == NULL)
        return YEW_DIFF_INVALID;
    while (yew_diff_work_step(work, UINT32_MAX, sync_clock, &ticks) ==
           YEW_DIFF_MORE)
        ;
    outcome = yew_diff_work_take(work, out);
    yew_diff_work_free(work);
    return outcome;
}

YewDiffOutcome yew_diff_bytes(Arena *arena,
                              const u8 *left, size_t left_len,
                              const u8 *right, size_t right_len,
                              u32 budget_d, GitHunkVec *out)
{
    return yew_diff_bytes_with_hash(arena, left, left_len, right, right_len,
                                    budget_d, yew_git_line_hash, out);
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
