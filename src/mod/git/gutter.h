#ifndef YEW_MOD_GIT_GUTTER_H
#define YEW_MOD_GIT_GUTTER_H

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/vec.h"

typedef enum HunkKind {
    YEW_HUNK_ADD,
    YEW_HUNK_DEL,
    YEW_HUNK_MOD
} HunkKind;

typedef struct GitHunk {
    LineNo base_lo;
    LineNo base_n;
    LineNo buf_lo;
    LineNo buf_n;
    HunkKind kind;
} GitHunk;

VEC_DECL(GitHunkVec, GitHunk);

typedef struct HunkList {
    Arena a;
    GitHunkVec h;
    u64 buf_gen;
    char base_oid[65];
    bool base_is_head;
    bool truncated;
} HunkList;

typedef u64 (*YewGitLineHashFn)(const u8 *bytes, size_t len);

typedef enum YewDiffOutcome {
    YEW_DIFF_OK,
    YEW_DIFF_TOO_LARGE,
    YEW_DIFF_TRUNCATED,
    YEW_DIFF_INVALID
} YewDiffOutcome;

typedef struct YewDiffWork YewDiffWork;
typedef i64 (*YewDiffNowUsFn)(void *ctx);

typedef enum YewDiffProgress {
    YEW_DIFF_MORE,
    YEW_DIFF_DONE
} YewDiffProgress;

#define YEW_DIFF_MAX_LINES 200000U
#define YEW_DIFF_MAX_BYTES (16U * 1024U * 1024U)
#define YEW_DIFF_MAX_D 4096U
#define YEW_DIFF_BUDGET_US 4000U
#define YEW_GIT_HUNK_CONTEXT 3U

bool yew_diff_lines(Arena *a, const u64 *ah, u32 an, const u64 *bh, u32 bn,
                    u32 budget_d, GitHunkVec *out);
bool yew_diff_within_size_limits(size_t left_bytes, u32 left_lines,
                                 size_t right_bytes, u32 right_lines);
YewDiffOutcome yew_diff_bytes_with_hash(Arena *a,
                                        const u8 *left, size_t left_len,
                                        const u8 *right, size_t right_len,
                                        u32 budget_d,
                                        YewGitLineHashFn hash_fn,
                                        GitHunkVec *out);
YewDiffOutcome yew_diff_bytes(Arena *a,
                              const u8 *left, size_t left_len,
                              const u8 *right, size_t right_len,
                              u32 budget_d, GitHunkVec *out);
YewDiffWork *yew_diff_work_begin_bytes_with_hash(
    const u8 *left, size_t left_len, const u8 *right, size_t right_len,
    u32 budget_d, YewGitLineHashFn hash_fn);
YewDiffWork *yew_diff_work_begin_bytes(const u8 *left, size_t left_len,
                                       const u8 *right, size_t right_len,
                                       u32 budget_d);
YewDiffProgress yew_diff_work_step(YewDiffWork *work, u32 budget_us,
                                   YewDiffNowUsFn now_us, void *clock_ctx);
YewDiffOutcome yew_diff_work_take(YewDiffWork *work, GitHunkVec *out);
/* Peak live storage owned by the work item, including line metadata,
 * refinement frontiers, the explicit range stack, and reconstructed output.
 * Exposed so the linear-space contract can be regression-tested. */
size_t yew_diff_work_peak_bytes(const YewDiffWork *work);
void yew_diff_work_free(YewDiffWork *work);
bool yew_git_hunk_sign_placement(const GitHunk *h, u64 buffer_line_count,
                                 bool terminal_line_is_synthetic,
                                 LineNo *line, bool *delete_below);
u64 yew_git_line_hash(const u8 *bytes, size_t len);
bool yew_git_hash_lines(const u8 *bytes, size_t len, Arena *a,
                        u64 **hashes, u32 *count, bool *missing_final_nl);
bool yew_git_hunk_patch(Bytebuf *out, const char *path, const u8 *base,
                        size_t base_len, const u8 *buf, size_t buf_len,
                        const GitHunk *h);

#endif
