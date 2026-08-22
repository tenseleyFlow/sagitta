#include "mod/git/gutter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum DiffOp {
    DIFF_EQUAL,
    DIFF_DELETE,
    DIFF_INSERT
} DiffOp;

typedef struct DiffRun {
    DiffOp op;
    u32 count;
} DiffRun;

VEC_DECL(DiffRunVec, DiffRun);

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

typedef enum DiffTaskKind {
    DIFF_TASK_RANGE,
    DIFF_TASK_EQUAL,
    DIFF_TASK_DELETE,
    DIFF_TASK_INSERT
} DiffTaskKind;

typedef struct DiffTask {
    DiffTaskKind kind;
    u32 left_lo;
    u32 left_hi;
    u32 right_lo;
    u32 right_hi;
    u32 count;
    bool budgeted;
} DiffTask;

VEC_DECL(DiffTaskVec, DiffTask);

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
    WORK_REFINE_POP,
    WORK_REFINE_PREFIX,
    WORK_REFINE_SUFFIX,
    WORK_BISECT_INIT,
    WORK_BISECT_CLEAR,
    WORK_BISECT_FORWARD,
    WORK_BISECT_REVERSE,
    WORK_EMIT_OP,
    WORK_VALIDATE,
    WORK_EMIT,
    WORK_FINISH,
    WORK_DONE
} DiffWorkPhase;

typedef enum EqualProgress {
    EQUAL_MORE,
    EQUAL_YES,
    EQUAL_NO
} EqualProgress;

/* Myers' linear-space refinement, expressed as a resumable state machine.
 * One active subrange owns two bidirectional frontiers; completed splits are
 * retained only as constant-size tasks.  Together with line metadata and the
 * final script this is O(N + D), rather than one O(D) frontier for every d.
 * The root overlap also proves the capped edit distance, so a separate
 * forward search would duplicate the dominant work. */
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
    size_t clear_at;
    i32 k;
    i32 x;
    i32 y;
    bool snake_active;
    bool equal_active;
    u32 equal_left;
    u32 equal_right;
    size_t equal_at;
    bool owns_sequences;
    DiffTaskVec tasks;
    DiffTask task;
    u32 range_suffix;
    DiffOp emit_op;
    u32 emit_op_count;
    i32 *forward;
    i32 *reverse;
    size_t bisect_width;
    i32 bisect_offset;
    u32 bisect_max;
    u32 bisect_d;
    u32 bisect_limit;
    i32 bisect_delta;
    bool bisect_odd;
    bool strict_equal;
    size_t peak_bytes;
    size_t validate_at;
    u32 validate_run_at;
    u32 validate_left;
    u32 validate_right;
    size_t emit_at;
    u32 emit_left;
    u32 emit_right;
    bool emit_hunk;
    u32 emit_base_lo;
    u32 emit_buf_lo;
    DiffRunVec ops;
    GitHunkVec result;
    YewDiffOutcome outcome;
    DiffWorkPhase phase;
    bool taken;
};

static void work_memory_peak(YewDiffWork *work)
{
    size_t bytes = sizeof(*work);

    if (work->owns_sequences) {
        bytes += (size_t)work->left_n *
                 (sizeof(*work->left_lines) + sizeof(*work->left_hashes));
        bytes += (size_t)work->right_n *
                 (sizeof(*work->right_lines) + sizeof(*work->right_hashes));
    }
    bytes += work->bisect_width *
             (sizeof(*work->forward) + sizeof(*work->reverse));
    bytes += work->tasks.cap * sizeof(*work->tasks.data);
    bytes += work->ops.cap * sizeof(*work->ops.data);
    bytes += work->result.cap * sizeof(*work->result.data);
    if (bytes > work->peak_bytes)
        work->peak_bytes = bytes;
}

static void work_push_task(YewDiffWork *work, DiffTask task)
{
    size_t cap = work->tasks.cap;

    DiffTaskVec_push(&work->tasks, task);
    if (work->tasks.cap != cap)
        work_memory_peak(work);
}

static void work_push_op(YewDiffWork *work, DiffOp op)
{
    size_t cap = work->ops.cap;

    if (work->ops.len != 0U &&
        work->ops.data[work->ops.len - 1U].op == op) {
        work->ops.data[work->ops.len - 1U].count++;
        return;
    }
    DiffRunVec_push(&work->ops, ((DiffRun){op, 1U}));
    if (work->ops.cap != cap)
        work_memory_peak(work);
}

static void work_push_ops(YewDiffWork *work, DiffOp op, u32 count)
{
    size_t cap = work->ops.cap;

    if (count == 0U)
        return;
    if (work->ops.len != 0U &&
        work->ops.data[work->ops.len - 1U].op == op) {
        work->ops.data[work->ops.len - 1U].count += count;
        return;
    }
    DiffRunVec_push(&work->ops, ((DiffRun){op, count}));
    if (work->ops.cap != cap)
        work_memory_peak(work);
}

static bool work_tick_expired(YewDiffNowUsFn now_us, void *ctx,
                              i64 deadline)
{
    return now_us(ctx) >= deadline;
}

static EqualProgress work_equal_mode(YewDiffWork *work, u32 left_at,
                                     u32 right_at, bool raw)
{
    const DiffLine *a;
    const DiffLine *b;
    size_t limit;

    if (!work->equal_active) {
        if (work->left_hashes[left_at] != work->right_hashes[right_at])
            return EQUAL_NO;
        if (!raw || work->left_lines == NULL || work->right_lines == NULL)
            return EQUAL_YES;
        a = &work->left_lines[left_at];
        b = &work->right_lines[right_at];
        if (a->len != b->len)
            return EQUAL_NO;
        work->equal_active = true;
        work->equal_left = left_at;
        work->equal_right = right_at;
        work->equal_at = 0U;
    }
    if (work->equal_left != left_at || work->equal_right != right_at)
        return EQUAL_NO;
    a = &work->left_lines[left_at];
    b = &work->right_lines[right_at];
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

static EqualProgress work_equal(YewDiffWork *work, u32 left_at,
                                u32 right_at)
{
    return work_equal_mode(work, left_at, right_at, work->strict_equal);
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
    }
    return work->at == len;
}

static void work_truncated(YewDiffWork *work)
{
    GitHunkVec_push(&work->result, ((GitHunk){
        LINENO(0U), LINENO(work->left_n), LINENO(0U), LINENO(work->right_n),
        YEW_HUNK_MOD
    }));
    work_memory_peak(work);
    work->outcome = YEW_DIFF_TRUNCATED;
    work->phase = WORK_DONE;
}

static void work_free_bisect(YewDiffWork *work)
{
    free(work->forward);
    free(work->reverse);
    work->forward = NULL;
    work->reverse = NULL;
    work->bisect_width = 0U;
}

static void work_refine_start(YewDiffWork *work)
{
    if (work->suffix != 0U)
        work_push_task(work, ((DiffTask){DIFF_TASK_EQUAL, 0U, 0U, 0U, 0U,
                                         work->suffix, false}));
    work_push_task(work, ((DiffTask){DIFF_TASK_RANGE, work->prefix,
                                     work->left_n - work->suffix,
                                     work->prefix,
                                     work->right_n - work->suffix, 0U,
                                     true}));
    if (work->prefix != 0U)
        work_push_task(work, ((DiffTask){DIFF_TASK_EQUAL, 0U, 0U, 0U, 0U,
                                         work->prefix, false}));
    work->phase = WORK_REFINE_POP;
}

static void work_bisect_split(YewDiffWork *work, u32 x, u32 y)
{
    u32 left = work->task.left_lo + x;
    u32 right = work->task.right_lo + y;

    work_free_bisect(work);
    if ((left == work->task.left_lo && right == work->task.right_lo) ||
        (left == work->task.left_hi && right == work->task.right_hi)) {
        /* A boundary overlap means this minimal path has no middle snake.
         * Peel the deletion-preferred edge used by the forward search. */
        work_push_task(work, ((DiffTask){DIFF_TASK_RANGE,
                                         work->task.left_lo + 1U,
                                         work->task.left_hi,
                                         work->task.right_lo,
                                         work->task.right_hi, 0U, false}));
        work_push_task(work, ((DiffTask){DIFF_TASK_DELETE, 0U, 0U, 0U, 0U,
                                         1U, false}));
    } else {
        work_push_task(work, ((DiffTask){DIFF_TASK_RANGE, left,
                                         work->task.left_hi, right,
                                         work->task.right_hi, 0U, false}));
        work_push_task(work, ((DiffTask){DIFF_TASK_RANGE,
                                         work->task.left_lo, left,
                                         work->task.right_lo, right, 0U,
                                         false}));
    }
    work->phase = WORK_REFINE_POP;
}

static void work_bisect_accept(YewDiffWork *work, u32 distance,
                               u32 x, u32 y)
{
    if (work->task.budgeted && distance > work->budget_d) {
        work_truncated(work);
        return;
    }
    work_bisect_split(work, x, y);
}

static void work_retry_strict(YewDiffWork *work)
{
    work_free_bisect(work);
    work->tasks.len = 0U;
    work->ops.len = 0U;
    work->result.len = 0U;
    work->prefix = 0U;
    work->suffix = 0U;
    work->strict_equal = true;
    work_equal_reset(work);
    work->phase = WORK_PREFIX;
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
    work->owns_sequences = true;
    work->strict_equal = hash_fn != yew_git_line_hash;
    work->budget_d = budget_d;
    work->hash = UINT64_C(1469598103934665603);
    work->outcome = YEW_DIFF_OK;
    work->phase = left_len > YEW_DIFF_MAX_BYTES ||
                  right_len > YEW_DIFF_MAX_BYTES ?
                  WORK_DONE : WORK_COUNT_LEFT;
    if (work->phase == WORK_DONE)
        work->outcome = YEW_DIFF_TOO_LARGE;
    work_memory_peak(work);
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
    u32 transitions = 0U;
    bool expired = false;

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
            work_memory_peak(work);
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
            work->middle_left = work->left_n - work->prefix - work->suffix;
            work->middle_right = work->right_n - work->prefix - work->suffix;
            if (work->middle_left == 0U && work->middle_right == 0U) {
                if (!work->strict_equal && work->left_lines != NULL &&
                    work->right_lines != NULL)
                    work_refine_start(work);
                else
                    work->phase = WORK_DONE;
                break;
            }
            work_refine_start(work);
            break;
        }
        case WORK_REFINE_POP:
            if (work->tasks.len == 0U) {
                if (work->strict_equal || work->left_lines == NULL ||
                    work->right_lines == NULL) {
                    work->emit_left = 0U;
                    work->emit_right = 0U;
                    work->phase = WORK_EMIT;
                } else {
                    work->validate_at = 0U;
                    work->validate_run_at = 0U;
                    work->validate_left = 0U;
                    work->validate_right = 0U;
                    work->phase = WORK_VALIDATE;
                }
                break;
            }
            work->task = work->tasks.data[--work->tasks.len];
            if (work->task.kind == DIFF_TASK_RANGE) {
                work->range_suffix = 0U;
                work->phase = WORK_REFINE_PREFIX;
            } else {
                work->emit_op = work->task.kind == DIFF_TASK_EQUAL ?
                                DIFF_EQUAL :
                                work->task.kind == DIFF_TASK_DELETE ?
                                DIFF_DELETE : DIFF_INSERT;
                work->emit_op_count = work->task.count;
                work->phase = WORK_EMIT_OP;
            }
            break;
        case WORK_EMIT_OP:
            work_push_ops(work, work->emit_op, work->emit_op_count);
            work->emit_op_count = 0U;
            work->phase = WORK_REFINE_POP;
            break;
        case WORK_REFINE_PREFIX:
            if (work->task.left_lo >= work->task.left_hi ||
                work->task.right_lo >= work->task.right_hi) {
                work->phase = WORK_REFINE_SUFFIX;
            } else {
                EqualProgress eq = work_equal(work, work->task.left_lo,
                                              work->task.right_lo);
                if (eq == EQUAL_YES) {
                    work_push_op(work, DIFF_EQUAL);
                    work->task.left_lo++;
                    work->task.right_lo++;
                    work_equal_reset(work);
                } else if (eq == EQUAL_NO) {
                    work_equal_reset(work);
                    work->phase = WORK_REFINE_SUFFIX;
                }
            }
            break;
        case WORK_REFINE_SUFFIX:
            if (work->range_suffix <
                    work->task.left_hi - work->task.left_lo &&
                work->range_suffix <
                    work->task.right_hi - work->task.right_lo) {
                u32 li = work->task.left_hi - work->range_suffix - 1U;
                u32 ri = work->task.right_hi - work->range_suffix - 1U;
                EqualProgress eq = work_equal(work, li, ri);

                if (eq == EQUAL_YES) {
                    work->range_suffix++;
                    work_equal_reset(work);
                    break;
                }
                if (eq == EQUAL_MORE)
                    break;
                work_equal_reset(work);
            }
            work->task.left_hi -= work->range_suffix;
            work->task.right_hi -= work->range_suffix;
            if (work->range_suffix != 0U)
                work_push_task(work, ((DiffTask){DIFF_TASK_EQUAL, 0U, 0U,
                                                 0U, 0U,
                                                 work->range_suffix, false}));
            if (work->task.left_lo == work->task.left_hi) {
                if (work->task.budgeted &&
                    work->task.right_hi - work->task.right_lo >
                        work->budget_d) {
                    work_truncated(work);
                    break;
                }
                work_push_task(work, ((DiffTask){DIFF_TASK_INSERT, 0U, 0U,
                                                 0U, 0U,
                                                 work->task.right_hi -
                                                 work->task.right_lo, false}));
                work->phase = WORK_REFINE_POP;
            } else if (work->task.right_lo == work->task.right_hi) {
                if (work->task.budgeted &&
                    work->task.left_hi - work->task.left_lo >
                        work->budget_d) {
                    work_truncated(work);
                    break;
                }
                work_push_task(work, ((DiffTask){DIFF_TASK_DELETE, 0U, 0U,
                                                 0U, 0U,
                                                 work->task.left_hi -
                                                 work->task.left_lo, false}));
                work->phase = WORK_REFINE_POP;
            } else {
                work->phase = WORK_BISECT_INIT;
            }
            break;
        case WORK_BISECT_INIT:
        {
            u32 n = work->task.left_hi - work->task.left_lo;
            u32 m = work->task.right_hi - work->task.right_lo;

            work->bisect_max = (n + m + 1U) / 2U;
            work->bisect_limit = work->task.budgeted ?
                work->budget_d / 2U + (work->budget_d & 1U) :
                work->bisect_max;
            if (work->bisect_limit > work->bisect_max)
                work->bisect_limit = work->bisect_max;
            work->bisect_width = (size_t)work->bisect_max * 2U + 3U;
            work->bisect_offset = (i32)work->bisect_max + 1;
            work->bisect_delta = (i32)n - (i32)m;
            work->bisect_odd = (work->bisect_delta & 1) != 0;
            work->forward = yew_xmalloc(work->bisect_width *
                                        sizeof(*work->forward));
            work->reverse = yew_xmalloc(work->bisect_width *
                                        sizeof(*work->reverse));
            work_memory_peak(work);
            work->clear_at = 0U;
            work->phase = WORK_BISECT_CLEAR;
            break;
        }
        case WORK_BISECT_CLEAR:
            if (work->clear_at < work->bisect_width) {
                size_t limit = work->clear_at + 1024U;

                if (limit < work->clear_at || limit > work->bisect_width)
                    limit = work->bisect_width;
                while (work->clear_at < limit) {
                    work->forward[work->clear_at] = -1;
                    work->reverse[work->clear_at] = -1;
                    work->clear_at++;
                }
            } else {
                work->forward[work->bisect_offset + 1] = 0;
                work->reverse[work->bisect_offset + 1] = 0;
                work->bisect_d = 0U;
                work->k = 0;
                work->snake_active = false;
                work->phase = WORK_BISECT_FORWARD;
            }
            break;
        case WORK_BISECT_FORWARD:
        {
            i32 d = (i32)work->bisect_d;
            u32 n = work->task.left_hi - work->task.left_lo;
            u32 m = work->task.right_hi - work->task.right_lo;

            if (!work->snake_active) {
                if (work->k > d) {
                    work->k = -d;
                    work->phase = WORK_BISECT_REVERSE;
                    break;
                }
                if (work->k == -d ||
                    (work->k != d &&
                     work->forward[work->bisect_offset + work->k - 1] <
                     work->forward[work->bisect_offset + work->k + 1]))
                    work->x = work->forward[work->bisect_offset + work->k + 1];
                else
                    work->x = work->forward[work->bisect_offset + work->k - 1] + 1;
                work->y = work->x - work->k;
                work->snake_active = true;
            }
            if (work->x < (i32)n && work->y < (i32)m) {
                EqualProgress eq = work_equal(
                    work, work->task.left_lo + (u32)work->x,
                    work->task.right_lo + (u32)work->y);
                if (eq == EQUAL_MORE)
                    break;
                work_equal_reset(work);
                if (eq == EQUAL_YES) {
                    work->x++;
                    work->y++;
                    break;
                }
            }
            work->forward[work->bisect_offset + work->k] = work->x;
            work->snake_active = false;
            if (work->bisect_odd) {
                i32 reverse_k = work->bisect_delta - work->k;

                if (reverse_k >= -d + 1 && reverse_k <= d - 1 &&
                    work->reverse[work->bisect_offset + reverse_k] != -1 &&
                    work->x >= (i32)n -
                               work->reverse[work->bisect_offset + reverse_k]) {
                    work_bisect_accept(work, work->bisect_d * 2U - 1U,
                                       (u32)work->x, (u32)work->y);
                    break;
                }
            }
            work->k += 2;
            break;
        }
        case WORK_BISECT_REVERSE:
        {
            i32 d = (i32)work->bisect_d;
            u32 n = work->task.left_hi - work->task.left_lo;
            u32 m = work->task.right_hi - work->task.right_lo;

            if (!work->snake_active) {
                if (work->k > d) {
                    if (work->bisect_d == work->bisect_limit) {
                        if (work->task.budgeted)
                            work_truncated(work);
                        else {
                            work->outcome = YEW_DIFF_INVALID;
                            work->phase = WORK_DONE;
                        }
                    } else {
                        work->bisect_d++;
                        work->k = -(i32)work->bisect_d;
                        work->phase = WORK_BISECT_FORWARD;
                    }
                    break;
                }
                if (work->k == -d ||
                    (work->k != d &&
                     work->reverse[work->bisect_offset + work->k - 1] <
                     work->reverse[work->bisect_offset + work->k + 1]))
                    work->x = work->reverse[work->bisect_offset + work->k + 1];
                else
                    work->x = work->reverse[work->bisect_offset + work->k - 1] + 1;
                work->y = work->x - work->k;
                work->snake_active = true;
            }
            if (work->x < (i32)n && work->y < (i32)m) {
                EqualProgress eq = work_equal(
                    work, work->task.left_hi - (u32)work->x - 1U,
                    work->task.right_hi - (u32)work->y - 1U);
                if (eq == EQUAL_MORE)
                    break;
                work_equal_reset(work);
                if (eq == EQUAL_YES) {
                    work->x++;
                    work->y++;
                    break;
                }
            }
            work->reverse[work->bisect_offset + work->k] = work->x;
            work->snake_active = false;
            if (!work->bisect_odd) {
                i32 forward_k = work->bisect_delta - work->k;

                if (forward_k >= -d && forward_k <= d &&
                    work->forward[work->bisect_offset + forward_k] != -1 &&
                    work->forward[work->bisect_offset + forward_k] >=
                        (i32)n - work->x) {
                    i32 split_x =
                        work->forward[work->bisect_offset + forward_k];
                    work_bisect_accept(work, work->bisect_d * 2U,
                                       (u32)split_x,
                                       (u32)(split_x - forward_k));
                    break;
                }
            }
            work->k += 2;
            break;
        }
        case WORK_VALIDATE:
            if (work->validate_at >= work->ops.len) {
                work->emit_left = 0U;
                work->emit_right = 0U;
                work->phase = WORK_EMIT;
            } else if (work->ops.data[work->validate_at].op == DIFF_DELETE) {
                work->validate_left += work->ops.data[work->validate_at].count;
                work->validate_at++;
                work->validate_run_at = 0U;
            } else if (work->ops.data[work->validate_at].op == DIFF_INSERT) {
                work->validate_right += work->ops.data[work->validate_at].count;
                work->validate_at++;
                work->validate_run_at = 0U;
            } else {
                EqualProgress eq = work_equal_mode(
                    work, work->validate_left + work->validate_run_at,
                    work->validate_right + work->validate_run_at, true);

                if (eq == EQUAL_YES) {
                    work->validate_run_at++;
                    work_equal_reset(work);
                    if (work->validate_run_at ==
                        work->ops.data[work->validate_at].count) {
                        work->validate_left += work->validate_run_at;
                        work->validate_right += work->validate_run_at;
                        work->validate_at++;
                        work->validate_run_at = 0U;
                    }
                } else if (eq == EQUAL_NO) {
                    work_retry_strict(work);
                }
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
                    work_memory_peak(work);
                    work->emit_hunk = false;
                }
                work->phase = WORK_FINISH;
                break;
            }
            if (work->ops.data[work->emit_at].op == DIFF_EQUAL) {
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
                    work_memory_peak(work);
                    work->emit_hunk = false;
                }
                work->emit_left += work->ops.data[work->emit_at].count;
                work->emit_right += work->ops.data[work->emit_at].count;
            } else {
                if (!work->emit_hunk) {
                    work->emit_base_lo = work->emit_left;
                    work->emit_buf_lo = work->emit_right;
                    work->emit_hunk = true;
                }
                if (work->ops.data[work->emit_at].op == DIFF_DELETE)
                    work->emit_left += work->ops.data[work->emit_at].count;
                else
                    work->emit_right += work->ops.data[work->emit_at].count;
            }
            work->emit_at++;
            break;
        case WORK_FINISH:
            work_free_bisect(work);
            work->phase = WORK_DONE;
            break;
        case WORK_DONE:
            break;
        }
        transitions++;
        if (transitions == 24U) {
            expired = work_tick_expired(now_us, clock_ctx, deadline);
            transitions = 0U;
        }
    } while (work->phase != WORK_DONE && !expired);
    if (transitions != 0U)
        (void)now_us(clock_ctx);
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

size_t yew_diff_work_peak_bytes(const YewDiffWork *work)
{
    return work == NULL ? 0U : work->peak_bytes;
}

void yew_diff_work_free(YewDiffWork *work)
{
    if (work == NULL)
        return;
    if (work->owns_sequences) {
        free(work->left_lines);
        free(work->right_lines);
        free(work->left_hashes);
        free(work->right_hashes);
    }
    work_free_bisect(work);
    DiffTaskVec_free(&work->tasks);
    DiffRunVec_free(&work->ops);
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

bool yew_diff_lines(Arena *arena, const u64 *left, u32 left_n,
                    const u64 *right, u32 right_n, u32 budget_d,
                    GitHunkVec *out)
{
    YewDiffWork *work;
    YewDiffOutcome outcome;
    i64 ticks = 0;

    if (arena == NULL || out == NULL ||
        (left == NULL && left_n != 0U) ||
        (right == NULL && right_n != 0U))
        return false;
    work = yew_xcalloc(1U, sizeof(*work));
    work->left_hashes = (u64 *)left;
    work->right_hashes = (u64 *)right;
    work->left_n = left_n;
    work->right_n = right_n;
    work->budget_d = budget_d;
    work->outcome = YEW_DIFF_OK;
    work->phase = WORK_PREFIX;
    work_memory_peak(work);
    while (yew_diff_work_step(work, UINT32_MAX, sync_clock, &ticks) ==
           YEW_DIFF_MORE)
        ;
    outcome = yew_diff_work_take(work, out);
    yew_diff_work_free(work);
    if (outcome != YEW_DIFF_OK)
        out->len = 0U;
    return outcome == YEW_DIFF_OK;
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
