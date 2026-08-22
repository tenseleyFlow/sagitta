#ifndef YEW_MOD_GIT_DIFFVIEW_H
#define YEW_MOD_GIT_DIFFVIEW_H

#include <stdbool.h>
#include <stddef.h>

#include "mod/git/gutter.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/vec.h"

enum {
    YEW_DIFF_FILLER_ROW = -1,
    YEW_DIFF_INTRALINE_MAX = 512
};

typedef enum {
    YEW_DIFF_LEFT,
    YEW_DIFF_RIGHT
} DiffSide;

VEC_DECL(DiffI32Vec, i32);

/* One entry per displayed row.  -1 denotes a filler on that side. */
typedef struct DiffRowMap {
    DiffI32Vec left;
    DiffI32Vec right;
} DiffRowMap;

void yew_diff_rowmap_init(DiffRowMap *map);
void yew_diff_rowmap_drop(DiffRowMap *map);
bool yew_diff_rowmap_build(DiffRowMap *map, u32 base_lines, u32 buf_lines,
                           const GitHunk *hunks, size_t nhunks);
size_t yew_diff_rowmap_len(const DiffRowMap *map);
i32 yew_diff_row_source(const DiffRowMap *map, DiffSide side,
                        size_t display_row);

/*
 * Scratch bytes contain one logical line per row-map entry.  Source line
 * terminators are replaced by separators between scratch rows; a missing
 * final newline therefore remains missing on the final scratch row.  The
 * source_rows vector distinguishes a literal "~" source line from a filler.
 */
typedef struct DiffScratch {
    Bytebuf bytes;
    DiffI32Vec source_rows;
} DiffScratch;

typedef struct DiffScratchPair {
    DiffScratch left;
    DiffScratch right;
} DiffScratchPair;

void yew_diff_scratch_init(DiffScratch *scratch);
void yew_diff_scratch_drop(DiffScratch *scratch);
bool yew_diff_scratch_build(DiffScratch *scratch, const u8 *source,
                            size_t source_len, const DiffI32Vec *rows);
i32 yew_diff_scratch_source_row(const DiffScratch *scratch,
                                size_t display_row);
void yew_diff_scratch_pair_init(DiffScratchPair *pair);
void yew_diff_scratch_pair_drop(DiffScratchPair *pair);
bool yew_diff_scratch_pair_build(DiffScratchPair *pair,
                                 const u8 *base, size_t base_len,
                                 const u8 *buf, size_t buf_len,
                                 const DiffRowMap *map);

typedef struct DiffIntraSpan {
    u32 off;
    u32 len;
} DiffIntraSpan;

VEC_DECL(DiffIntraSpanVec, DiffIntraSpan);

typedef struct DiffIntraline {
    DiffIntraSpanVec left;
    DiffIntraSpanVec right;
} DiffIntraline;

typedef enum {
    YEW_DIFF_INTRA_ERROR,
    YEW_DIFF_INTRA_EQUAL,
    YEW_DIFF_INTRA_SPANS,
    YEW_DIFF_INTRA_WHOLE_LINE
} DiffIntraResult;

void yew_diff_intraline_init(DiffIntraline *diff);
void yew_diff_intraline_drop(DiffIntraline *diff);
DiffIntraResult yew_diff_intraline_build(DiffIntraline *diff,
                                         const u8 *left, size_t left_len,
                                         const u8 *right, size_t right_len);

typedef void (*DiffScrollApplyFn)(void *ctx, u32 aligned_top);

typedef struct DiffScrollMember {
    u32 link_id;
    u32 member_id;
    DiffScrollApplyFn apply;
    void *ctx;
} DiffScrollMember;

VEC_DECL(DiffScrollMemberVec, DiffScrollMember);

typedef struct DiffScrollRegistry {
    DiffScrollMemberVec members;
} DiffScrollRegistry;

void yew_diff_scroll_registry_init(DiffScrollRegistry *registry);
void yew_diff_scroll_registry_drop(DiffScrollRegistry *registry);
bool yew_diff_scroll_register(DiffScrollRegistry *registry, u32 link_id,
                              u32 member_id, DiffScrollApplyFn apply,
                              void *ctx);
void yew_diff_scroll_unregister(DiffScrollRegistry *registry, u32 member_id);
/* False means invalid input or a recursive propagation suppressed by guard. */
bool yew_diff_scroll_sync(DiffScrollRegistry *registry, u32 link_id,
                          u32 source_member_id, u32 aligned_top);

#endif
