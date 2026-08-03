#ifndef SAG_TEXT_UNDO_H
#define SAG_TEXT_UNDO_H

#include <stdbool.h>
#include <stdio.h>

#include "text/cursor.h"
#include "text/mark.h"
#include "text/piece.h"
#include "util/buf.h"
#include "util/vec.h"

#define SAG_UNDO_BURST_MS 400U
#define SAG_UNDO_BURST_BYTES 4096U
#define SAG_UNDO_BYTES_MAX (UINT64_C(64) * 1024U * 1024U)
#define SAG_UNDO_MIN_NODES 200U
#define SAG_UNDO_PERSIST_BYTES_MAX (UINT64_C(16) * 1024U * 1024U)

typedef struct EditCtx EditCtx;

typedef enum {
    SAG_OP_INS = 1,
    SAG_OP_DEL = 2
} SagOpKind;

typedef enum {
    SAG_TXN_TYPE = 0,
    SAG_TXN_ERASE,
    SAG_TXN_PASTE,
    SAG_TXN_CUT,
    SAG_TXN_MULTI,
    SAG_TXN_MACRO,
    SAG_TXN_FILTER,
    SAG_TXN_REPLACE,
    SAG_TXN_LSP,
    SAG_TXN_EXTERNAL,
    SAG_TXN_REASON_MAX
} SagTxnReason;

enum {
    SAG_TXN_TRIMMED = 1U << 0,
    SAG_TXN_SAVED = 1U << 1,
    SAG_TXN_DEAD = 1U << 2
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

VEC_DECL(SagUndoNodeVec, UndoNode);
VEC_DECL(SagUndoOpVec, UndoOp);
VEC_DECL(SagCursorRecVec, CursorRec);
VEC_DECL(SagMarkRepairVec, MarkRepair);
VEC_DECL(SagUndoRepairRunVec, UndoRepairRun);
VEC_DECL(SagUndoReplaySpanVec, UndoReplaySpan);

typedef u64 (*SagUndoMonoClock)(void *ctx);
typedef i64 (*SagUndoWallClock)(void *ctx);

typedef struct UndoTree {
    SagUndoNodeVec nodes;
    SagUndoOpVec ops;
    SagCursorRecVec cursors;
    SagMarkRepairVec repairs;
    SagUndoRepairRunVec repair_runs;
    SagUndoReplaySpanVec replay_spans;
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
    SagTxnReason pending_reason;
    bool boundary;
    bool over_budget_logged;
    bool reopened;
    u32 reopen_n_ops;
    u32 reopen_n_rep;
    u32 reopen_cur_after;
    u32 reopen_n_after;
    u64 reopen_blob_hi;
    u64 reopen_t_last_ms;
    SagUndoMonoClock mono_clock;
    SagUndoWallClock wall_clock;
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
    SAG_UNDO_WRITE_OK = 0,
    SAG_UNDO_WRITE_IO,
    SAG_UNDO_WRITE_TOO_LARGE
} SagUndoWriteResult;

typedef enum {
    SAG_UNDO_READ_CURRENT = 0,
    SAG_UNDO_READ_ANCHOR,
    SAG_UNDO_READ_DROPPED,
    SAG_UNDO_READ_IO
} SagUndoReadResult;

UndoTree *sag_undo_new(const TextBuf *tb);
void sag_undo_free(UndoTree *ut);
void sag_undo_set_clock(UndoTree *ut, SagUndoMonoClock mono_clock,
                        SagUndoWallClock wall_clock, void *ctx);
void sag_undo_set_limits(UndoTree *ut, u64 bytes_max, u32 min_nodes,
                         u64 persist_bytes_max);

void sag_undo_begin(EditCtx *ec, SagTxnReason why);
void sag_undo_promote_multi(EditCtx *ec);
void sag_undo_end(EditCtx *ec);
void sag_undo_boundary(UndoTree *ut);
void sag_undo_abort(EditCtx *ec);
bool sag_undo_reopen(EditCtx *ec, SagTxnReason expect);

bool sag_undo(EditCtx *ec);
bool sag_redo(EditCtx *ec);
bool sag_undo_to(EditCtx *ec, u32 node_id);
bool sag_undo_state(EditCtx *ec, i64 delta);
bool sag_undo_time(EditCtx *ec, i64 wall_secs);
u32 sag_undo_branch_cycle(UndoTree *ut, i32 delta);
u32 sag_undo_current(const UndoTree *ut);
bool sag_undo_at_save_point(const UndoTree *ut);
void sag_undo_mark_saved(UndoTree *ut);
bool sag_undo_last_insert(const UndoTree *ut, Bytebuf *out, i64 *t_wall);

u32 sag_undo_list(const UndoTree *ut, UndoNodeInfo *out, u32 max);
u32 sag_undo_children(const UndoTree *ut, u32 id, u32 *out, u32 max);
void sag_undo_describe(const UndoTree *ut, u32 id, i64 now, char *buf,
                       u64 len);
void sag_undo_dump(const UndoTree *ut, FILE *out);

SagUndoWriteResult sag_undo_write(EditCtx *ec, const char *path);
SagUndoReadResult sag_undo_read(EditCtx *ec, const char *path);

/* Internal edit-choke-point hooks; callers mutate only through edit.c. */
void sag_undo_prepare_insert(EditCtx *ec, ByteOff at, u64 len);
void sag_undo_prepare_delete(EditCtx *ec, Span range);
void sag_undo_record_insert(EditCtx *ec, ByteOff at, u64 len, u64 payload);
void sag_undo_record_delete(EditCtx *ec, Span range);

#endif
