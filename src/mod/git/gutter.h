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

#define YEW_DIFF_MAX_LINES 200000U
#define YEW_DIFF_MAX_BYTES (16U * 1024U * 1024U)
#define YEW_DIFF_MAX_D 4096U
#define YEW_DIFF_BUDGET_US 4000U
#define YEW_GIT_HUNK_CONTEXT 3U

bool yew_diff_lines(Arena *a, const u64 *ah, u32 an, const u64 *bh, u32 bn,
                    u32 budget_d, GitHunkVec *out);
u64 yew_git_line_hash(const u8 *bytes, size_t len);
bool yew_git_hash_lines(const u8 *bytes, size_t len, Arena *a,
                        u64 **hashes, u32 *count, bool *missing_final_nl);
bool yew_git_hunk_patch(Bytebuf *out, const char *path, const u8 *base,
                        size_t base_len, const u8 *buf, size_t buf_len,
                        const GitHunk *h);

#endif
