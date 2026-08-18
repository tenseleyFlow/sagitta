#ifndef YEW_TEXT_UNDO_H
#define YEW_TEXT_UNDO_H

#include <stdbool.h>
#include <stdio.h>

#include "text/cursor.h"
#include "text/mark.h"
#include "text/piece.h"
#include "util/buf.h"
#include "util/vec.h"

#define YEW_UNDO_BURST_MS 400U
#define YEW_UNDO_BURST_BYTES 4096U
#define YEW_UNDO_BYTES_MAX (UINT64_C(64) * 1024U * 1024U)
#define YEW_UNDO_MIN_NODES 200U
#define YEW_UNDO_PERSIST_BYTES_MAX (UINT64_C(16) * 1024U * 1024U)

typedef struct EditCtx EditCtx;

typedef enum {
    YEW_OP_INS = 1,
    YEW_OP_DEL = 2
} YewOpKind;

typedef enum {
    YEW_TXN_TYPE = 0,
    YEW_TXN_ERASE,
    YEW_TXN_PASTE,
    YEW_TXN_CUT,
    YEW_TXN_MULTI,
    YEW_TXN_MACRO,
    YEW_TXN_FILTER,
    YEW_TXN_REPLACE,
    YEW_TXN_LSP,
    YEW_TXN_EXTERNAL,
    YEW_TXN_REASON_MAX
} YewTxnReason;

enum {
    YEW_TXN_TRIMMED = 1U << 0,
    YEW_TXN_SAVED = 1U << 1,
    YEW_TXN_DEAD = 1U << 2
};

typedef struct UndoOp {
    u8 kind;
    u8 src;
    u64 off;
    u64 len;
    u64 payload;
} UndoOp;

_Static_assert(sizeof(UndoOp) <= 32U, "op bloat");

typedef struct {
    u64 pos;
    u64 anchor;
    u64 goal;
} CursorRec;

typedef struct {
    u32 mark_id;
    u32 mark_gen;
    u64 rel_off;
} MarkRepair;

/* Parallel to ops: each delete owns exactly one repair slice. */
typedef struct {
    u32 at;
    u32 len;
} UndoRepairRun;

/* Parallel to ops: a delete's first undo populates its replay span. */
typedef struct {
    Span span;
    u8 src;
    bool valid;
} UndoReplaySpan;

typedef struct UndoNode {
    u32 id;
    u32 parent;
    u32 first_child;
    u32 next_sibling;
    u32 redo_child;
    u32 depth;
    u32 ops_at;
    u32 n_ops;
    u64 blob_lo;
    u64 blob_hi;
    u32 cur_before;
    u32 n_before;
    u32 cur_after;
    u32 n_after;
    u32 rep_at;
    u32 n_rep;
    u32 win_id;
    i64 t_wall;
    u64 t_last_ms;
    u8 reason;
    u8 flags;
} UndoNode;

VEC_DECL(YewUndoNodeVec, UndoNode);
VEC_DECL(YewUndoOpVec, UndoOp);
VEC_DECL(YewCursorRecVec, CursorRec);
VEC_DECL(YewMarkRepairVec, MarkRepair);
VEC_DECL(YewUndoRepairRunVec, UndoRepairRun);
VEC_DECL(YewUndoReplaySpanVec, UndoReplaySpan);

typedef u64 (*YewUndoMonoClock)(void *ctx);
typedef i64 (*YewUndoWallClock)(void *ctx);

typedef struct UndoTree {
    YewUndoNodeVec nodes;
    YewUndoOpVec ops;
    YewCursorRecVec cursors;
    YewMarkRepairVec repairs;
    YewUndoRepairRunVec repair_runs;
    YewUndoReplaySpanVec replay_spans;
    Bytebuf blobs;
    u32 root;
    u32 cur;
    u32 saved;
    u32 open;
    u32 depth;
    u64 bytes_live;
    u64 bytes_dead;
    u64 gen;
    u64 root_len;
    u64 root_hash;
    u64 saved_len;
    u64 saved_hash;
    u64 bytes_max;
    u64 persist_bytes_max;
    u32 min_nodes;
    YewTxnReason pending_reason;
    bool boundary;
    bool over_budget_logged;
    bool reopened;
    u32 reopen_n_ops;
    u32 reopen_n_rep;
    u32 reopen_cur_after;
    u32 reopen_n_after;
    u64 reopen_blob_hi;
    u64 reopen_t_last_ms;
    YewUndoMonoClock mono_clock;
    YewUndoWallClock wall_clock;
    void *clock_ctx;
    const TextBuf *owner;
} UndoTree;

typedef struct {
    u32 id;
    u32 parent;
    u32 depth;
    u32 n_children;
    u8 reason;
    i64 t_wall;
    u64 bytes_ins;
    u64 bytes_del;
    bool on_current_path;
    bool is_current;
    bool is_saved;
    bool is_trimmed;
} UndoNodeInfo;

typedef enum {
    YEW_UNDO_WRITE_OK = 0,
    YEW_UNDO_WRITE_IO,
    YEW_UNDO_WRITE_TOO_LARGE
} YewUndoWriteResult;

typedef enum {
    YEW_UNDO_READ_CURRENT = 0,
    YEW_UNDO_READ_ANCHOR,
    YEW_UNDO_READ_DROPPED,
    YEW_UNDO_READ_IO
} YewUndoReadResult;

UndoTree *yew_undo_new(const TextBuf *tb);
void yew_undo_free(UndoTree *ut);
void yew_undo_set_clock(UndoTree *ut, YewUndoMonoClock mono_clock,
                        YewUndoWallClock wall_clock, void *ctx);
void yew_undo_set_limits(UndoTree *ut, u64 bytes_max, u32 min_nodes,
                         u64 persist_bytes_max);

void yew_undo_begin(EditCtx *ec, YewTxnReason why);
void yew_undo_promote_multi(EditCtx *ec);
void yew_undo_end(EditCtx *ec);
void yew_undo_boundary(UndoTree *ut);
void yew_undo_abort(EditCtx *ec);
bool yew_undo_reopen(EditCtx *ec, YewTxnReason expect);

bool yew_undo(EditCtx *ec);
bool yew_redo(EditCtx *ec);
bool yew_undo_prune_redo(UndoTree *ut, YewTxnReason expect);
bool yew_undo_to(EditCtx *ec, u32 node_id);
bool yew_undo_state(EditCtx *ec, i64 delta);
bool yew_undo_time(EditCtx *ec, i64 wall_secs);
u32 yew_undo_branch_cycle(UndoTree *ut, i32 delta);
u32 yew_undo_current(const UndoTree *ut);
bool yew_undo_at_save_point(const UndoTree *ut);
void yew_undo_mark_saved(UndoTree *ut);
bool yew_undo_last_insert(const UndoTree *ut, Bytebuf *out, i64 *t_wall);

u32 yew_undo_list(const UndoTree *ut, UndoNodeInfo *out, u32 max);
u32 yew_undo_children(const UndoTree *ut, u32 id, u32 *out, u32 max);
void yew_undo_describe(const UndoTree *ut, u32 id, i64 now, char *buf,
                       u64 len);
void yew_undo_dump(const UndoTree *ut, FILE *out);

YewUndoWriteResult yew_undo_write(EditCtx *ec, const char *path);
YewUndoReadResult yew_undo_read(EditCtx *ec, const char *path);

/* Internal edit-choke-point hooks; callers mutate only through edit.c. */
void yew_undo_prepare_insert(EditCtx *ec, ByteOff at, u64 len);
void yew_undo_prepare_delete(EditCtx *ec, Span range);
void yew_undo_record_insert(EditCtx *ec, ByteOff at, u64 len, u64 payload);
void yew_undo_record_delete(EditCtx *ec, Span range);

#endif
