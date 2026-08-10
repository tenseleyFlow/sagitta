#define _POSIX_C_SOURCE 200809L

#include "text/undo.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "text/edit.h"
#include "text/journal.h"
#include "util/log.h"

enum { YEWU_VERSION = 1U, YEWU_HEADER_LEN = 64U, YEWU_TRUNCATED = 1U };

VEC_DECL(YewU32Vec, u32);

static void trim_tree(EditCtx *ec);
static bool is_ancestor(const UndoTree *ut, u32 ancestor, u32 id);
static void account_live(UndoTree *ut);

static UndoNode *node_mut(UndoTree *ut, u32 id)
{
    if (ut == NULL || id == 0U || (size_t)id > ut->nodes.len)
        return NULL;
    return &ut->nodes.data[id - 1U];
}

static const UndoNode *node_get(const UndoTree *ut, u32 id)
{
    if (ut == NULL || id == 0U || (size_t)id > ut->nodes.len)
        return NULL;
    return &ut->nodes.data[id - 1U];
}

static bool node_live(const UndoTree *ut, u32 id)
{
    const UndoNode *node = node_get(ut, id);
    return node != NULL && (node->flags & YEW_TXN_DEAD) == 0U;
}

static u64 default_mono(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("undo: monotonic clock failed");
    return (u64)ts.tv_sec * 1000U + (u64)ts.tv_nsec / 1000000U;
}

static i64 default_wall(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        YEW_BUG("undo: realtime clock failed");
    return (i64)ts.tv_sec;
}

static u64 fnv_add(u64 hash, const u8 *bytes, size_t len)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static u64 text_hash(const TextBuf *tb)
{
    TextIter it;
    u64 hash = UINT64_C(14695981039346656037);

    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return hash;
    do {
        const u8 *bytes;
        u64 len;
        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            YEW_BUG("undo: text iterator failed");
        hash = fnv_add(hash, bytes, (size_t)len);
    } while (yew_textiter_advance(&it, tb));
    return hash;
}

static void require_ctx(const EditCtx *ec)
{
    if (ec == NULL || ec->tb == NULL || ec->undo == NULL)
        YEW_BUG("undo: NULL edit context");
}

static void snapshot_cursors(UndoTree *ut, const CursorSet *cs,
                             u32 *at, u32 *count)
{
    size_t i;

    *at = (u32)ut->cursors.len;
    *count = 0U;
    if (cs == NULL)
        return;
    yew_cset_check(cs);
    for (i = 0U; i < cs->curs.len; i++) {
        size_t index = i == 0U ? (size_t)cs->primary
                               : (i <= (size_t)cs->primary ? i - 1U : i);
        const Cursor *cursor = &cs->curs.data[index];
        CursorRec rec;
        rec.pos = cursor->pos.v;
        rec.anchor = cursor->anchor.v;
        rec.goal = cursor->goal_col.v;
        YewCursorRecVec_push(&ut->cursors, rec);
        (*count)++;
    }
}

static void restore_cursors(EditCtx *ec, u32 at, u32 count)
{
    CursorSet *cs = ec->cset;
    u32 i;

    if (cs == NULL || count == 0U)
        return;
    if ((size_t)at + count > ec->undo->cursors.len)
        YEW_BUG("undo: corrupt cursor slice");
    YewCursorVec_reserve(&cs->curs, count);
    cs->curs.len = count;
    cs->primary = 0U;
    for (i = 0U; i < count; i++) {
        const CursorRec *rec = &ec->undo->cursors.data[at + i];
        cs->curs.data[i].pos = BYTEOFF(rec->pos);
        cs->curs.data[i].anchor = BYTEOFF(rec->anchor);
        cs->curs.data[i].goal_col = (GCol){rec->goal};
    }
    yew_cset_reseed(cs);
    for (i = 0U; i < count; i++)
        cs->selstacks.data[i].n = 0U;
    yew_cset_normalize(ec->tb, cs);
}

UndoTree *yew_undo_new(const TextBuf *tb)
{
    UndoTree *ut;
    UndoNode root;

    if (tb == NULL)
        YEW_BUG("yew_undo_new: NULL buffer");
    ut = yew_xcalloc(1U, sizeof(*ut));
    bytebuf_init(&ut->blobs);
    ut->root = 1U;
    ut->cur = 1U;
    ut->saved = 0U;
    ut->bytes_max = YEW_UNDO_BYTES_MAX;
    ut->persist_bytes_max = YEW_UNDO_PERSIST_BYTES_MAX;
    ut->min_nodes = YEW_UNDO_MIN_NODES;
    ut->pending_reason = YEW_TXN_REASON_MAX;
    ut->boundary = true;
    ut->mono_clock = default_mono;
    ut->wall_clock = default_wall;
    ut->root_len = yew_textbuf_len(tb);
    ut->root_hash = text_hash(tb);
    ut->saved_len = ut->root_len;
    ut->saved_hash = ut->root_hash;
    ut->owner = tb;
    (void)memset(&root, 0, sizeof(root));
    root.id = 1U;
    root.t_wall = ut->wall_clock(ut->clock_ctx);
    root.t_last_ms = ut->mono_clock(ut->clock_ctx);
    YewUndoNodeVec_push(&ut->nodes, root);
    ut->bytes_live = sizeof(root);
    return ut;
}

void yew_undo_free(UndoTree *ut)
{
    if (ut == NULL)
        return;
    if (ut->depth != 0U || ut->open != 0U)
        YEW_BUG("undo: free with an open transaction");
    YewUndoNodeVec_free(&ut->nodes);
    YewUndoOpVec_free(&ut->ops);
    YewCursorRecVec_free(&ut->cursors);
    YewMarkRepairVec_free(&ut->repairs);
    YewUndoRepairRunVec_free(&ut->repair_runs);
    YewUndoReplaySpanVec_free(&ut->replay_spans);
    bytebuf_free(&ut->blobs);
    free(ut);
}

void yew_undo_set_clock(UndoTree *ut, YewUndoMonoClock mono_clock,
                        YewUndoWallClock wall_clock, void *ctx)
{
    if (ut == NULL)
        YEW_BUG("yew_undo_set_clock: NULL tree");
    ut->mono_clock = mono_clock != NULL ? mono_clock : default_mono;
    ut->wall_clock = wall_clock != NULL ? wall_clock : default_wall;
    ut->clock_ctx = ctx;
}

void yew_undo_set_limits(UndoTree *ut, u64 bytes_max, u32 min_nodes,
                         u64 persist_bytes_max)
{
    if (ut == NULL)
        YEW_BUG("yew_undo_set_limits: NULL tree");
    ut->bytes_max = bytes_max;
    ut->min_nodes = min_nodes;
    ut->persist_bytes_max = persist_bytes_max;
}

static void require_reason(EditCtx *ec, YewTxnReason reason)
{
    if (reason >= YEW_TXN_REASON_MAX)
        YEW_BUG("undo: invalid transaction reason");
    if (reason == YEW_TXN_LSP)
        YEW_BUG("LSP transactions land in Sprint 47");
    if (reason == YEW_TXN_MULTI) {
        if (ec->cset == NULL || ec->cset->curs.len < 2U)
            YEW_BUG("multi-cursor transaction requires multiple cursors");
        yew_cset_check_text(ec->tb, ec->cset);
    } else if (ec->cset != NULL)
        yew_cset_require_single_edit(ec->cset);
}

void yew_undo_begin(EditCtx *ec, YewTxnReason why)
{
    UndoTree *ut;
    require_ctx(ec);
    require_reason(ec, why);
    ut = ec->undo;
    if (ut->depth == 0U) {
        ut->pending_reason = why;
        ut->boundary = true;
        ut->reopened = false;
    } else if (ut->pending_reason != why) {
        YEW_BUG("undo: nested transaction reason mismatch");
    }
    if (ut->depth == UINT32_MAX)
        YEW_BUG("undo: transaction nesting overflow");
    ut->depth++;
}

void yew_undo_promote_multi(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *node;

    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth == 0U || ec->cset == NULL || ec->cset->curs.len < 2U)
        YEW_BUG("undo: invalid multi-cursor transaction promotion");
    yew_cset_check_text(ec->tb, ec->cset);
    if (ut->pending_reason == YEW_TXN_MULTI)
        return;
    if (ut->pending_reason != YEW_TXN_TYPE &&
        ut->pending_reason != YEW_TXN_ERASE)
        YEW_BUG("undo: cannot promote transaction reason %u",
                (u32)ut->pending_reason);
    node = node_mut(ut, ut->open);
    if (node != NULL)
        node->reason = YEW_TXN_MULTI;
    ut->pending_reason = YEW_TXN_MULTI;
}

void yew_undo_boundary(UndoTree *ut)
{
    if (ut == NULL)
        YEW_BUG("yew_undo_boundary: NULL tree");
    ut->boundary = true;
}

static bool reason_mergeable(YewTxnReason reason)
{
    return reason == YEW_TXN_TYPE || reason == YEW_TXN_ERASE;
}

static bool op_contiguous(YewTxnReason reason, const UndoOp *prev,
                          u8 kind, u64 off, u64 len)
{
    if (reason == YEW_TXN_TYPE && kind == YEW_OP_INS &&
        prev->kind == YEW_OP_INS)
        return prev->off <= UINT64_MAX - prev->len &&
               off == prev->off + prev->len;
    if (reason == YEW_TXN_ERASE && kind == YEW_OP_DEL &&
        prev->kind == YEW_OP_DEL)
        return off == prev->off ||
               (off <= UINT64_MAX - len && off + len == prev->off);
    return false;
}

static u64 node_payload_bytes(const UndoTree *ut, const UndoNode *node)
{
    u64 total = 0U;
    u32 i;
    for (i = 0U; i < node->n_ops; i++)
        total += ut->ops.data[node->ops_at + i].len;
    return total;
}

static bool may_merge(UndoTree *ut, YewTxnReason reason, u8 kind,
                      u64 off, u64 len, u64 now)
{
    UndoNode *node;
    const UndoOp *prev;

    if (ut->depth != 0U || ut->boundary || !reason_mergeable(reason) ||
        ut->cur != ut->nodes.len)
        return false;
    node = node_mut(ut, ut->cur);
    if (node == NULL || node->reason != (u8)reason || node->n_ops == 0U ||
        node->first_child != 0U || now < node->t_last_ms ||
        now - node->t_last_ms >= YEW_UNDO_BURST_MS)
        return false;
    prev = &ut->ops.data[node->ops_at + node->n_ops - 1U];
    return op_contiguous(reason, prev, kind, off, len) &&
           node_payload_bytes(ut, node) < YEW_UNDO_BURST_BYTES;
}

static void link_child(UndoTree *ut, UndoNode *node)
{
    UndoNode *parent = node_mut(ut, node->parent);
    u32 *link;
    if (parent == NULL)
        YEW_BUG("undo: missing parent");
    link = &parent->first_child;
    while (*link != 0U)
        link = &node_mut(ut, *link)->next_sibling;
    *link = node->id;
    parent->redo_child = node->id;
}

static UndoNode *open_node(EditCtx *ec, YewTxnReason reason, u8 kind,
                           u64 off, u64 len)
{
    UndoTree *ut = ec->undo;
    u64 now = ut->mono_clock(ut->clock_ctx);
    UndoNode node;
    UndoNode *parent;
    i64 wall;

    if (ut->open != 0U)
        return node_mut(ut, ut->open);
    if (may_merge(ut, reason, kind, off, len, now)) {
        ut->open = ut->cur;
        return node_mut(ut, ut->open);
    }
    (void)memset(&node, 0, sizeof(node));
    node.id = (u32)ut->nodes.len + 1U;
    node.parent = ut->cur;
    parent = node_mut(ut, node.parent);
    if (parent == NULL)
        YEW_BUG("undo: invalid current node");
    node.depth = parent->depth + 1U;
    node.ops_at = (u32)ut->ops.len;
    node.rep_at = (u32)ut->repairs.len;
    node.blob_lo = ut->blobs.len;
    node.blob_hi = ut->blobs.len;
    node.win_id = ec->win_id;
    node.reason = (u8)reason;
    node.t_last_ms = now;
    wall = ut->wall_clock(ut->clock_ctx);
    if (ut->nodes.len != 0U &&
        wall < ut->nodes.data[ut->nodes.len - 1U].t_wall) {
        yew_log(YEW_LOG_WARN, "undo: wall clock stepped backwards; clamped");
        wall = ut->nodes.data[ut->nodes.len - 1U].t_wall;
    }
    node.t_wall = wall;
    snapshot_cursors(ut, ec->cset, &node.cur_before, &node.n_before);
    YewUndoNodeVec_push(&ut->nodes, node);
    link_child(ut, &ut->nodes.data[ut->nodes.len - 1U]);
    ut->open = node.id;
    ut->boundary = false;
    ut->bytes_live += sizeof(node) +
                      (u64)node.n_before * sizeof(CursorRec);
    return node_mut(ut, node.id);
}

static YewTxnReason edit_reason(const UndoTree *ut, u8 kind)
{
    if (ut->depth != 0U)
        return ut->pending_reason;
    return kind == YEW_OP_INS ? YEW_TXN_TYPE : YEW_TXN_ERASE;
}

static void capture_delete_bytes(UndoTree *ut, const TextBuf *tb, Span range)
{
    TextIter it;
    u64 left = range.hi - range.lo;
    if (left == 0U)
        return;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        YEW_BUG("undo: cannot capture delete bytes");
    while (left != 0U) {
        const u8 *bytes;
        u64 len;
        u64 take;
        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            YEW_BUG("undo: delete capture truncated");
        take = len < left ? len : left;
        bytebuf_append(&ut->blobs, bytes, (size_t)take);
        left -= take;
        if (left != 0U && !yew_textiter_advance(&it, tb))
            YEW_BUG("undo: delete capture advance failed");
    }
}

typedef struct {
    UndoTree *ut;
    u64 base;
} RepairCapture;

static void capture_repair(void *ctx, MarkId id, u64 rel_off)
{
    RepairCapture *capture = ctx;
    MarkRepair repair;
    repair.mark_id = id.id;
    repair.mark_gen = id.gen;
    repair.rel_off = rel_off;
    YewMarkRepairVec_push(&capture->ut->repairs, repair);
    (void)capture->base;
}

void yew_undo_prepare_insert(EditCtx *ec, ByteOff at, u64 len)
{
    UndoTree *ut;
    require_ctx(ec);
    ut = ec->undo;
    (void)open_node(ec, edit_reason(ut, YEW_OP_INS), YEW_OP_INS, at.v, len);
}

void yew_undo_prepare_delete(EditCtx *ec, Span range)
{
    UndoTree *ut;
    UndoNode *node;
    RepairCapture capture;

    require_ctx(ec);
    ut = ec->undo;
    node = open_node(ec, edit_reason(ut, YEW_OP_DEL), YEW_OP_DEL, range.lo,
                     range.hi - range.lo);
    capture_delete_bytes(ut, ec->tb, range);
    node = node_mut(ut, node->id);
    node->blob_hi = ut->blobs.len;
    capture.ut = ut;
    capture.base = range.lo;
    if (ec->marks != NULL)
        yew_marks_observe_collapse(ec->marks, range, capture_repair, &capture);
}

static void finish_record(EditCtx *ec, UndoNode *node)
{
    UndoTree *ut = ec->undo;
    if (ut->depth != 0U && ut->pending_reason == YEW_TXN_MULTI) {
        node->t_last_ms = ut->mono_clock(ut->clock_ctx);
        ut->cur = node->id;
        ut->bytes_live += sizeof(UndoOp) + sizeof(UndoRepairRun) +
                          sizeof(UndoReplaySpan);
        return;
    }
    if (node->n_after != 0U &&
        (size_t)node->cur_after + node->n_after == ut->cursors.len &&
        (!ut->reopened || node->cur_after != ut->reopen_cur_after)) {
        ut->cursors.len = node->cur_after;
        ut->bytes_live -= (u64)node->n_after * sizeof(CursorRec);
        node->n_after = 0U;
    }
    node->t_last_ms = ut->mono_clock(ut->clock_ctx);
    snapshot_cursors(ut, ec->cset, &node->cur_after, &node->n_after);
    ut->cur = node->id;
    ut->bytes_live += sizeof(UndoOp) + sizeof(UndoRepairRun) +
                      sizeof(UndoReplaySpan) +
                      (u64)node->n_after * sizeof(CursorRec);
    if (ut->depth == 0U) {
        ut->open = 0U;
        if (ec->jrnl != NULL)
            yew_journal_sync(ec->jrnl);
        trim_tree(ec);
    }
}

static void replace_after_snapshot(EditCtx *ec, UndoNode *node)
{
    UndoTree *ut = ec->undo;
    if (node->n_after != 0U &&
        (size_t)node->cur_after + node->n_after == ut->cursors.len &&
        (!ut->reopened || node->cur_after != ut->reopen_cur_after)) {
        ut->cursors.len = node->cur_after;
        ut->bytes_live -= (u64)node->n_after * sizeof(CursorRec);
        node->n_after = 0U;
    }
    snapshot_cursors(ut, ec->cset, &node->cur_after, &node->n_after);
    ut->bytes_live += (u64)node->n_after * sizeof(CursorRec);
}

static void commit_reopened_snapshot(UndoTree *ut, UndoNode *node)
{
    if (!ut->reopened)
        return;
    if (node->cur_after < ut->reopen_cur_after + ut->reopen_n_after ||
        (size_t)node->cur_after + node->n_after != ut->cursors.len)
        YEW_BUG("undo: invalid reopened cursor snapshots");
    (void)memmove(ut->cursors.data + ut->reopen_cur_after,
                  ut->cursors.data + node->cur_after,
                  (size_t)node->n_after * sizeof(CursorRec));
    node->cur_after = ut->reopen_cur_after;
    ut->cursors.len = (size_t)node->cur_after + node->n_after;
    ut->bytes_live -= (u64)ut->reopen_n_after * sizeof(CursorRec);
}

void yew_undo_record_insert(EditCtx *ec, ByteOff at, u64 len, u64 payload)
{
    UndoTree *ut;
    UndoNode *node;
    UndoOp op;
    UndoRepairRun run;
    UndoReplaySpan replay;

    require_ctx(ec);
    ut = ec->undo;
    node = node_mut(ut, ut->open);
    if (node == NULL)
        YEW_BUG("undo: insert recorded without prepare");
    op.kind = YEW_OP_INS;
    op.src = YEW_STORE_ADD;
    op.off = at.v;
    op.len = len;
    op.payload = payload;
    run.at = node->rep_at + node->n_rep;
    run.len = 0U;
    (void)memset(&replay, 0, sizeof(replay));
    YewUndoOpVec_push(&ut->ops, op);
    YewUndoRepairRunVec_push(&ut->repair_runs, run);
    YewUndoReplaySpanVec_push(&ut->replay_spans, replay);
    node = node_mut(ut, ut->open);
    node->n_ops++;
    finish_record(ec, node);
}

void yew_undo_record_delete(EditCtx *ec, Span range)
{
    UndoTree *ut;
    UndoNode *node;
    UndoOp op;
    UndoRepairRun run;
    UndoReplaySpan replay;

    require_ctx(ec);
    ut = ec->undo;
    node = node_mut(ut, ut->open);
    if (node == NULL)
        YEW_BUG("undo: delete recorded without prepare");
    op.kind = YEW_OP_DEL;
    op.src = 0U;
    op.off = range.lo;
    op.len = range.hi - range.lo;
    op.payload = node->blob_hi - op.len;
    run.at = node->rep_at + node->n_rep;
    run.len = (u32)ut->repairs.len - run.at;
    node->n_rep += run.len;
    (void)memset(&replay, 0, sizeof(replay));
    YewUndoOpVec_push(&ut->ops, op);
    YewUndoRepairRunVec_push(&ut->repair_runs, run);
    YewUndoReplaySpanVec_push(&ut->replay_spans, replay);
    node = node_mut(ut, ut->open);
    node->n_ops++;
    ut->bytes_live += op.len + (u64)run.len * sizeof(MarkRepair);
    finish_record(ec, node);
}

static const u8 *store_bytes(const TextBuf *tb, u8 src, Span span)
{
    const TextStore *store;
    store = src == YEW_STORE_ORIG ? &tb->orig : &tb->add;
    if ((src != YEW_STORE_ORIG && src != YEW_STORE_ADD) ||
        span.lo > span.hi || span.hi > store->len)
        YEW_BUG("undo: invalid store span");
    return store->bytes + (size_t)span.lo;
}

typedef struct {
    u64 off;
    u64 len;
    u64 payload;
} LastInsertSpan;

VEC_DECL(YewLastInsertSpanVec, LastInsertSpan);

static void last_insert_push(YewLastInsertSpanVec *spans, u64 off,
                             u64 len, u64 payload)
{
    LastInsertSpan span;

    if (len == 0U)
        return;
    span.off = off;
    span.len = len;
    span.payload = payload;
    YewLastInsertSpanVec_push(spans, span);
}

static void last_insert_apply_insert(YewLastInsertSpanVec *spans,
                                     const UndoOp *op)
{
    YewLastInsertSpanVec next = {0};
    bool placed = false;
    size_t i;

    for (i = 0U; i < spans->len; i++) {
        LastInsertSpan span = spans->data[i];
        u64 hi = span.off + span.len;

        if (hi <= op->off) {
            last_insert_push(&next, span.off, span.len, span.payload);
            continue;
        }
        if (!placed) {
            if (span.off < op->off) {
                u64 left = op->off - span.off;
                last_insert_push(&next, span.off, left, span.payload);
                last_insert_push(&next, op->off, op->len, op->payload);
                last_insert_push(&next, op->off + op->len,
                                 span.len - left, span.payload + left);
                placed = true;
                continue;
            }
            last_insert_push(&next, op->off, op->len, op->payload);
            placed = true;
        }
        last_insert_push(&next, span.off + op->len,
                         span.len, span.payload);
    }
    if (!placed)
        last_insert_push(&next, op->off, op->len, op->payload);
    YewLastInsertSpanVec_free(spans);
    *spans = next;
}

static void last_insert_apply_delete(YewLastInsertSpanVec *spans,
                                     const UndoOp *op)
{
    YewLastInsertSpanVec next = {0};
    u64 hi = op->off + op->len;
    size_t i;

    for (i = 0U; i < spans->len; i++) {
        LastInsertSpan span = spans->data[i];
        u64 span_hi = span.off + span.len;

        if (span_hi <= op->off) {
            last_insert_push(&next, span.off, span.len, span.payload);
        } else if (span.off >= hi) {
            last_insert_push(&next, span.off - op->len,
                             span.len, span.payload);
        } else {
            u64 left = span.off < op->off ? op->off - span.off : 0U;
            u64 right = span_hi > hi ? span_hi - hi : 0U;

            last_insert_push(&next, span.off, left, span.payload);
            last_insert_push(&next, op->off, right,
                             span.payload + span.len - right);
        }
    }
    YewLastInsertSpanVec_free(spans);
    *spans = next;
}

bool yew_undo_last_insert(const UndoTree *ut, Bytebuf *out, i64 *t_wall)
{
    size_t at;

    if (ut == NULL || out == NULL || ut->owner == NULL)
        YEW_BUG("yew_undo_last_insert: NULL context");
    out->len = 0U;
    if (t_wall != NULL)
        *t_wall = 0;
    at = ut->nodes.len;
    while (at > 1U) {
        const UndoNode *node = &ut->nodes.data[--at];
        YewLastInsertSpanVec spans = {0};
        u32 i;

        if (!node_live(ut, node->id) || node->reason != YEW_TXN_TYPE)
            continue;
        for (i = 0U; i < node->n_ops; i++) {
            const UndoOp *op = &ut->ops.data[node->ops_at + i];

            if (op->kind == YEW_OP_INS)
                last_insert_apply_insert(&spans, op);
            else if (op->kind == YEW_OP_DEL)
                last_insert_apply_delete(&spans, op);
            else
                YEW_BUG("undo: invalid type transaction operation");
        }
        for (i = 0U; i < spans.len; i++) {
            const LastInsertSpan *span = &spans.data[i];
            const u8 *bytes = store_bytes(
                ut->owner, YEW_STORE_ADD,
                (Span){span->payload, span->payload + span->len});

            bytebuf_append(out, bytes, (size_t)span->len);
        }
        YewLastInsertSpanVec_free(&spans);
        if (t_wall != NULL)
            *t_wall = node->t_wall;
        return true;
    }
    return false;
}

static u8 *copy_live_range(const TextBuf *tb, Span range)
{
    TextIter it;
    u64 done = 0U;
    u64 len = range.hi - range.lo;
    u8 *bytes = yew_xmalloc(len == 0U ? 1U : (size_t)len);

    if (len == 0U)
        return bytes;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        YEW_BUG("undo replay: invalid delete range");
    while (done < len) {
        const u8 *chunk;
        u64 avail;
        u64 take;
        if (!yew_textiter_chunk(&it, tb, &chunk, &avail))
            YEW_BUG("undo replay: delete iterator failed");
        take = avail < len - done ? avail : len - done;
        (void)memcpy(bytes + (size_t)done, chunk, (size_t)take);
        done += take;
        if (done < len && !yew_textiter_advance(&it, tb))
            YEW_BUG("undo replay: delete iterator truncated");
    }
    return bytes;
}

static void replay_insert_span(EditCtx *ec, ByteOff at, u8 src, Span span)
{
    LineNo line = yew_textbuf_line_of(ec->tb, at);
    u64 old_lines = yew_textbuf_line_count(ec->tb);
    u64 len = span.hi - span.lo;
    const u8 *bytes = store_bytes(ec->tb, src, span);

    yew_textbuf_insert_span(ec->tb, at, src, span);
    if (ec->marks != NULL)
        yew_marks_adjust(ec->marks, YEW_JOURNAL_INS, at, len);
    if (ec->cset != NULL)
        yew_cset_adjust(ec->cset, YEW_JOURNAL_INS, at, len);
    if (ec->jrnl != NULL)
        yew_journal_record(ec->jrnl, YEW_JOURNAL_INS, at.v, bytes, len);
    if (ec->on_change != NULL)
        ec->on_change(ec->on_change_ctx, at, line, 0U,
                      yew_textbuf_line_count(ec->tb) - old_lines,
                      ec->now_ms, false);
}

static void replay_insert_blob(EditCtx *ec, u32 op_index, const UndoOp *op)
{
    UndoTree *ut = ec->undo;
    UndoReplaySpan *cache = &ut->replay_spans.data[op_index];

    if (cache->valid) {
        replay_insert_span(ec, BYTEOFF(op->off), cache->src, cache->span);
    } else {
        LineNo line = yew_textbuf_line_of(ec->tb, BYTEOFF(op->off));
        u64 old_lines = yew_textbuf_line_count(ec->tb);
        u64 payload = ec->tb->add.len;
        const u8 *bytes;
        if (op->payload > ut->blobs.len || op->len > ut->blobs.len - op->payload)
            YEW_BUG("undo: corrupt delete payload");
        bytes = ut->blobs.data + (size_t)op->payload;
        yew_textbuf_insert(ec->tb, BYTEOFF(op->off), bytes, op->len);
        if (ec->marks != NULL)
            yew_marks_adjust(ec->marks, YEW_JOURNAL_INS, BYTEOFF(op->off),
                             op->len);
        if (ec->cset != NULL)
            yew_cset_adjust(ec->cset, YEW_JOURNAL_INS, BYTEOFF(op->off),
                            op->len);
        if (ec->jrnl != NULL)
            yew_journal_record(ec->jrnl, YEW_JOURNAL_INS, op->off, bytes,
                               op->len);
        cache->src = YEW_STORE_ADD;
        cache->span = (Span){payload, payload + op->len};
        cache->valid = true;
        if (ec->on_change != NULL)
            ec->on_change(ec->on_change_ctx, BYTEOFF(op->off), line, 0U,
                          yew_textbuf_line_count(ec->tb) - old_lines,
                          ec->now_ms, false);
    }
}

static void replay_delete(EditCtx *ec, const UndoOp *op)
{
    Span range = {op->off, op->off + op->len};
    LineNo line;
    u64 old_lines;
    u8 *bytes;

    if (op->off > yew_textbuf_len(ec->tb) ||
        op->len > yew_textbuf_len(ec->tb) - op->off)
        YEW_BUG("undo replay: delete out of bounds");
    line = yew_textbuf_line_of(ec->tb, BYTEOFF(op->off));
    old_lines = yew_textbuf_line_count(ec->tb);
    bytes = copy_live_range(ec->tb, range);
    yew_textbuf_delete(ec->tb, range);
    if (ec->marks != NULL)
        yew_marks_adjust(ec->marks, YEW_JOURNAL_DEL, BYTEOFF(op->off), op->len);
    if (ec->cset != NULL)
        yew_cset_adjust(ec->cset, YEW_JOURNAL_DEL, BYTEOFF(op->off), op->len);
    if (ec->jrnl != NULL)
        yew_journal_record(ec->jrnl, YEW_JOURNAL_DEL, op->off, bytes, op->len);
    if (ec->on_change != NULL)
        ec->on_change(ec->on_change_ctx, BYTEOFF(op->off), line,
                      old_lines - yew_textbuf_line_count(ec->tb), 0U,
                      ec->now_ms, false);
    free(bytes);
}

static void apply_inverse(EditCtx *ec, const UndoNode *node)
{
    UndoTree *ut = ec->undo;
    u32 i = node->n_ops;

    while (i != 0U) {
        u32 index = node->ops_at + --i;
        const UndoOp *op = &ut->ops.data[index];
        if (op->kind == YEW_OP_INS) {
            replay_delete(ec, op);
        } else if (op->kind == YEW_OP_DEL) {
            const UndoRepairRun *run;
            u32 r;
            replay_insert_blob(ec, index, op);
            run = &ut->repair_runs.data[index];
            for (r = 0U; r < run->len; r++) {
                const MarkRepair *repair = &ut->repairs.data[run->at + r];
                if (ec->marks != NULL) {
                    (void)yew_mark_repair(
                        ec->marks,
                        (MarkId){repair->mark_id, repair->mark_gen},
                        BYTEOFF(op->off + repair->rel_off));
                }
            }
        } else {
            YEW_BUG("undo: invalid operation kind");
        }
    }
}

static void apply_forward(EditCtx *ec, const UndoNode *node)
{
    UndoTree *ut = ec->undo;
    u32 i;
    for (i = 0U; i < node->n_ops; i++) {
        u32 index = node->ops_at + i;
        const UndoOp *op = &ut->ops.data[index];
        if (op->kind == YEW_OP_INS) {
            replay_insert_span(ec, BYTEOFF(op->off), op->src,
                               (Span){op->payload, op->payload + op->len});
        } else if (op->kind == YEW_OP_DEL) {
            replay_delete(ec, op);
        } else {
            YEW_BUG("redo: invalid operation kind");
        }
    }
}

static void sync_navigation(EditCtx *ec)
{
    if (ec->jrnl != NULL)
        yew_journal_sync(ec->jrnl);
}

void yew_undo_end(EditCtx *ec)
{
    UndoTree *ut;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth == 0U)
        YEW_BUG("undo: unbalanced end");
    ut->depth--;
    if (ut->depth != 0U)
        return;
    if (ut->open != 0U)
        replace_after_snapshot(ec, node_mut(ut, ut->open));
    if (ut->open != 0U)
        commit_reopened_snapshot(ut, node_mut(ut, ut->open));
    ut->pending_reason = YEW_TXN_REASON_MAX;
    ut->open = 0U;
    ut->reopened = false;
    ut->boundary = true;
    sync_navigation(ec);
    trim_tree(ec);
}

static void unlink_child(UndoTree *ut, u32 parent_id, u32 child_id)
{
    UndoNode *parent = node_mut(ut, parent_id);
    u32 *link;
    if (parent == NULL)
        YEW_BUG("undo: abort parent missing");
    link = &parent->first_child;
    while (*link != 0U && *link != child_id)
        link = &node_mut(ut, *link)->next_sibling;
    if (*link == child_id)
        *link = node_mut(ut, child_id)->next_sibling;
    if (parent->redo_child == child_id)
        parent->redo_child = parent->first_child;
}

void yew_undo_abort(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *node;
    u32 parent;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth == 0U)
        YEW_BUG("undo: abort outside transaction");
    if (ut->open == 0U) {
        ut->depth = 0U;
        ut->pending_reason = YEW_TXN_REASON_MAX;
        ut->boundary = true;
        return;
    }
    node = node_mut(ut, ut->open);
    parent = node->parent;
    yew_edit_ensure_journal(ec);
    if (ut->reopened) {
        u32 i = node->n_ops;

        while (i > ut->reopen_n_ops) {
            u32 index = node->ops_at + --i;
            const UndoOp *op = &ut->ops.data[index];

            if (op->kind == YEW_OP_INS) {
                replay_delete(ec, op);
            } else if (op->kind == YEW_OP_DEL) {
                const UndoRepairRun *run;
                u32 r;

                replay_insert_blob(ec, index, op);
                run = &ut->repair_runs.data[index];
                for (r = 0U; r < run->len; r++) {
                    const MarkRepair *repair =
                        &ut->repairs.data[run->at + r];
                    if (ec->marks != NULL) {
                        (void)yew_mark_repair(
                            ec->marks,
                            (MarkId){repair->mark_id, repair->mark_gen},
                            BYTEOFF(op->off + repair->rel_off));
                    }
                }
            } else {
                YEW_BUG("undo: invalid reopened operation kind");
            }
        }
        restore_cursors(ec, ut->reopen_cur_after, ut->reopen_n_after);
        ut->ops.len = node->ops_at + ut->reopen_n_ops;
        ut->repair_runs.len = ut->ops.len;
        ut->replay_spans.len = ut->ops.len;
        ut->repairs.len = node->rep_at + ut->reopen_n_rep;
        ut->blobs.len = (size_t)ut->reopen_blob_hi;
        ut->cursors.len = (size_t)ut->reopen_cur_after +
                          ut->reopen_n_after;
        node->n_ops = ut->reopen_n_ops;
        node->n_rep = ut->reopen_n_rep;
        node->blob_hi = ut->reopen_blob_hi;
        node->cur_after = ut->reopen_cur_after;
        node->n_after = ut->reopen_n_after;
        node->t_last_ms = ut->reopen_t_last_ms;
        ut->open = 0U;
        ut->depth = 0U;
        ut->pending_reason = YEW_TXN_REASON_MAX;
        ut->boundary = true;
        ut->reopened = false;
        account_live(ut);
        sync_navigation(ec);
        return;
    }
    apply_inverse(ec, node);
    restore_cursors(ec, node->cur_before, node->n_before);
    unlink_child(ut, parent, node->id);
    if (node->id == ut->nodes.len && !ut->reopened) {
        ut->ops.len = node->ops_at;
        ut->repair_runs.len = node->ops_at;
        ut->replay_spans.len = node->ops_at;
        ut->repairs.len = node->rep_at;
        ut->blobs.len = (size_t)node->blob_lo;
        ut->cursors.len = node->cur_before;
        ut->nodes.len--;
    } else {
        node->flags |= YEW_TXN_DEAD;
        ut->bytes_dead += node->blob_hi - node->blob_lo;
    }
    ut->cur = parent;
    ut->open = 0U;
    ut->depth = 0U;
    ut->pending_reason = YEW_TXN_REASON_MAX;
    ut->boundary = true;
    ut->reopened = false;
    account_live(ut);
    sync_navigation(ec);
}

bool yew_undo_reopen(EditCtx *ec, YewTxnReason expect)
{
    UndoTree *ut;
    UndoNode *node;
    require_ctx(ec);
    require_reason(ec, expect);
    ut = ec->undo;
    if (ut->depth != 0U)
        return false;
    node = node_mut(ut, ut->cur);
    if (node == NULL || node->id == ut->root || node->reason != (u8)expect ||
        node->first_child != 0U ||
        (size_t)node->ops_at + node->n_ops != ut->ops.len ||
        ut->repair_runs.len != ut->ops.len ||
        ut->replay_spans.len != ut->ops.len ||
        (size_t)node->rep_at + node->n_rep != ut->repairs.len ||
        node->blob_hi != ut->blobs.len ||
        (size_t)node->cur_after + node->n_after != ut->cursors.len)
        return false;
    ut->reopen_n_ops = node->n_ops;
    ut->reopen_n_rep = node->n_rep;
    ut->reopen_cur_after = node->cur_after;
    ut->reopen_n_after = node->n_after;
    ut->reopen_blob_hi = node->blob_hi;
    ut->reopen_t_last_ms = node->t_last_ms;
    ut->open = node->id;
    ut->depth = 1U;
    ut->pending_reason = expect;
    ut->boundary = false;
    ut->reopened = true;
    return true;
}

bool yew_undo(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *node;
    UndoNode *parent;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth != 0U)
        YEW_BUG("undo navigation inside transaction");
    if (ut->cur == ut->root)
        return false;
    yew_edit_ensure_journal(ec);
    node = node_mut(ut, ut->cur);
    parent = node_mut(ut, node->parent);
    if (parent == NULL)
        YEW_BUG("undo: broken parent chain");
    apply_inverse(ec, node);
    restore_cursors(ec, node->cur_before, node->n_before);
    parent->redo_child = node->id;
    ut->cur = parent->id;
    ut->boundary = true;
    sync_navigation(ec);
    return true;
}

bool yew_redo(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *parent;
    UndoNode *child;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth != 0U)
        YEW_BUG("redo navigation inside transaction");
    parent = node_mut(ut, ut->cur);
    if (parent == NULL || !node_live(ut, parent->redo_child))
        return false;
    yew_edit_ensure_journal(ec);
    child = node_mut(ut, parent->redo_child);
    apply_forward(ec, child);
    restore_cursors(ec, child->cur_after, child->n_after);
    parent->redo_child = child->id;
    ut->cur = child->id;
    ut->boundary = true;
    sync_navigation(ec);
    return true;
}

bool yew_undo_to(EditCtx *ec, u32 target_id)
{
    UndoTree *ut;
    u32 from;
    u32 target;
    YewU32Vec path = {0};

    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth != 0U || !node_live(ut, target_id))
        return false;
    if (target_id != ut->cur)
        yew_edit_ensure_journal(ec);
    from = ut->cur;
    target = target_id;
    while (node_get(ut, from)->depth > node_get(ut, target)->depth) {
        if (!yew_undo(ec))
            YEW_BUG("undo_to: failed ascent");
        from = ut->cur;
    }
    while (node_get(ut, target)->depth > node_get(ut, from)->depth) {
        YewU32Vec_push(&path, target);
        target = node_get(ut, target)->parent;
    }
    while (from != target) {
        YewU32Vec_push(&path, target);
        target = node_get(ut, target)->parent;
        if (!yew_undo(ec))
            YEW_BUG("undo_to: failed LCA ascent");
        from = ut->cur;
    }
    while (path.len != 0U) {
        UndoNode *child = node_mut(ut, path.data[--path.len]);
        UndoNode *parent = node_mut(ut, ut->cur);
        apply_forward(ec, child);
        restore_cursors(ec, child->cur_after, child->n_after);
        parent->redo_child = child->id;
        ut->cur = child->id;
    }
    YewU32Vec_free(&path);
    ut->boundary = true;
    sync_navigation(ec);
    return true;
}

bool yew_undo_state(EditCtx *ec, i64 delta)
{
    UndoTree *ut;
    u32 id;
    require_ctx(ec);
    ut = ec->undo;
    if (delta == 0)
        return true;
    id = ut->cur;
    while (delta < 0) {
        do {
            if (id <= 1U)
                return false;
            id--;
        } while (!node_live(ut, id));
        delta++;
    }
    while (delta > 0) {
        do {
            if ((size_t)id >= ut->nodes.len)
                return false;
            id++;
        } while (!node_live(ut, id));
        delta--;
    }
    return yew_undo_to(ec, id);
}

bool yew_undo_time(EditCtx *ec, i64 wall_secs)
{
    UndoTree *ut;
    u32 best = 0U;
    u32 id;
    require_ctx(ec);
    ut = ec->undo;
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        const UndoNode *node = node_get(ut, id);
        if (node_live(ut, id) && node->t_wall <= wall_secs)
            best = id;
    }
    return best != 0U && yew_undo_to(ec, best);
}

static u32 live_count(const UndoTree *ut)
{
    u32 count = 0U;
    u32 id;
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        if (node_live(ut, id))
            count++;
    }
    return count;
}

static u32 recent_floor(const UndoTree *ut)
{
    u32 seen = 0U;
    u32 id = (u32)ut->nodes.len;
    while (id != 0U) {
        if (node_live(ut, id) && ++seen >= ut->min_nodes)
            return id;
        id--;
    }
    return 1U;
}

static bool protected_node(const UndoTree *ut, u32 id, u32 floor)
{
    return id >= floor || is_ancestor(ut, id, ut->cur) ||
           (ut->saved != 0U && is_ancestor(ut, id, ut->saved));
}

static u64 node_storage_bytes(const UndoNode *node)
{
    return sizeof(*node) +
           (u64)node->n_ops *
               (sizeof(UndoOp) + sizeof(UndoRepairRun) +
                sizeof(UndoReplaySpan)) +
           (node->blob_hi - node->blob_lo) +
           ((u64)node->n_before + node->n_after) * sizeof(CursorRec) +
           (u64)node->n_rep * sizeof(MarkRepair);
}

static void account_live(UndoTree *ut)
{
    u64 total = 0U;
    u32 id;
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        const UndoNode *node;
        if (!node_live(ut, id))
            continue;
        node = node_get(ut, id);
        total += node_storage_bytes(node);
    }
    ut->bytes_live = total;
}

static void compact_history(UndoTree *ut)
{
    YewUndoOpVec ops = {0};
    YewCursorRecVec cursors = {0};
    YewMarkRepairVec repairs = {0};
    YewUndoRepairRunVec repair_runs = {0};
    YewUndoReplaySpanVec replay_spans = {0};
    Bytebuf blobs;
    u32 id;

    if (ut->bytes_dead <= ut->bytes_live)
        return;
    bytebuf_init(&blobs);
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        UndoNode *node;
        u32 old_ops_at;
        u32 old_rep_at;
        u32 old_n_rep;
        u32 old_cur_before;
        u32 old_cur_after;
        u32 copied_repairs = 0U;
        u32 i;

        if (!node_live(ut, id)) {
            node = node_mut(ut, id);
            node->ops_at = 0U;
            node->n_ops = 0U;
            node->rep_at = 0U;
            node->n_rep = 0U;
            node->blob_lo = 0U;
            node->blob_hi = 0U;
            node->cur_before = 0U;
            node->n_before = 0U;
            node->cur_after = 0U;
            node->n_after = 0U;
            continue;
        }
        node = node_mut(ut, id);
        old_ops_at = node->ops_at;
        old_rep_at = node->rep_at;
        old_n_rep = node->n_rep;
        old_cur_before = node->cur_before;
        old_cur_after = node->cur_after;
        if ((size_t)old_ops_at > ut->ops.len ||
            node->n_ops > ut->ops.len - old_ops_at ||
            (size_t)old_ops_at > ut->repair_runs.len ||
            node->n_ops > ut->repair_runs.len - old_ops_at ||
            (size_t)old_ops_at > ut->replay_spans.len ||
            node->n_ops > ut->replay_spans.len - old_ops_at ||
            (size_t)old_rep_at > ut->repairs.len ||
            old_n_rep > ut->repairs.len - old_rep_at ||
            (size_t)old_cur_before > ut->cursors.len ||
            node->n_before > ut->cursors.len - old_cur_before ||
            (size_t)old_cur_after > ut->cursors.len ||
            node->n_after > ut->cursors.len - old_cur_after)
            YEW_BUG("undo compact: corrupt arena slice");
        if (ops.len > UINT32_MAX || repairs.len > UINT32_MAX ||
            cursors.len > UINT32_MAX)
            YEW_BUG("undo compact: arena index overflow");
        if (node->n_ops > UINT32_MAX - (u32)ops.len ||
            old_n_rep > UINT32_MAX - (u32)repairs.len ||
            node->n_before > UINT32_MAX - (u32)cursors.len ||
            node->n_after >
                UINT32_MAX - (u32)cursors.len - node->n_before)
            YEW_BUG("undo compact: arena length overflow");
        node->ops_at = (u32)ops.len;
        node->rep_at = (u32)repairs.len;
        node->n_rep = 0U;
        node->blob_lo = blobs.len;
        for (i = 0U; i < node->n_ops; i++) {
            u32 old_index = old_ops_at + i;
            UndoOp op = ut->ops.data[old_index];
            const UndoRepairRun *old_run =
                &ut->repair_runs.data[old_index];
            UndoRepairRun run;
            u32 r;

            if (old_run->at != old_rep_at + copied_repairs ||
                old_run->len > old_n_rep - copied_repairs)
                YEW_BUG("undo compact: inconsistent repair run");
            run.at = (u32)repairs.len;
            run.len = old_run->len;
            for (r = 0U; r < old_run->len; r++)
                YewMarkRepairVec_push(
                    &repairs, ut->repairs.data[old_run->at + r]);
            if (op.kind == YEW_OP_DEL) {
                if (op.payload > ut->blobs.len ||
                    op.len > ut->blobs.len - op.payload)
                    YEW_BUG("undo compact: corrupt blob slice");
                {
                    u64 old_payload = op.payload;

                    op.payload = blobs.len;
                    bytebuf_append(&blobs,
                                   ut->blobs.data + (size_t)old_payload,
                                   (size_t)op.len);
                }
            }
            if (run.len > UINT32_MAX - node->n_rep)
                YEW_BUG("undo compact: repair count overflow");
            node->n_rep += run.len;
            copied_repairs += run.len;
            YewUndoOpVec_push(&ops, op);
            YewUndoRepairRunVec_push(&repair_runs, run);
            YewUndoReplaySpanVec_push(
                &replay_spans, ut->replay_spans.data[old_index]);
        }
        if (copied_repairs != old_n_rep ||
            node->n_rep != (u32)(repairs.len - node->rep_at))
            YEW_BUG("undo compact: inconsistent repair slice");
        node->blob_hi = blobs.len;
        if (cursors.len > UINT32_MAX)
            YEW_BUG("undo compact: cursor index overflow");
        node->cur_before = (u32)cursors.len;
        for (i = 0U; i < node->n_before; i++)
            YewCursorRecVec_push(
                &cursors, ut->cursors.data[old_cur_before + i]);
        if (cursors.len > UINT32_MAX)
            YEW_BUG("undo compact: cursor index overflow");
        node->cur_after = (u32)cursors.len;
        for (i = 0U; i < node->n_after; i++)
            YewCursorRecVec_push(
                &cursors, ut->cursors.data[old_cur_after + i]);
    }
    YewUndoOpVec_free(&ut->ops);
    YewCursorRecVec_free(&ut->cursors);
    YewMarkRepairVec_free(&ut->repairs);
    YewUndoRepairRunVec_free(&ut->repair_runs);
    YewUndoReplaySpanVec_free(&ut->replay_spans);
    bytebuf_free(&ut->blobs);
    ut->ops = ops;
    ut->cursors = cursors;
    ut->repairs = repairs;
    ut->repair_runs = repair_runs;
    ut->replay_spans = replay_spans;
    ut->blobs = blobs;
    ut->bytes_dead = 0U;
    ut->gen++;
    account_live(ut);
}

static bool subtree_is_unprotected(const UndoTree *ut, u32 root, u32 floor)
{
    u32 id;
    for (id = root; (size_t)id <= ut->nodes.len; id++) {
        if (node_live(ut, id) && is_ancestor(ut, root, id) &&
            protected_node(ut, id, floor))
            return false;
    }
    return true;
}

static void tombstone_subtree(UndoTree *ut, u32 root)
{
    u32 id;
    UndoNode *first = node_mut(ut, root);
    unlink_child(ut, first->parent, root);
    for (id = root; (size_t)id <= ut->nodes.len; id++) {
        UndoNode *node;
        if (!node_live(ut, id) || !is_ancestor(ut, root, id))
            continue;
        node = node_mut(ut, id);
        node->flags |= YEW_TXN_DEAD;
        ut->bytes_dead += node_storage_bytes(node);
    }
}

static bool prune_branch(UndoTree *ut, u32 floor)
{
    u32 id;
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        if (!node_live(ut, id) || protected_node(ut, id, floor) ||
            !subtree_is_unprotected(ut, id, floor))
            continue;
        tombstone_subtree(ut, id);
        ut->gen++;
        return true;
    }
    return false;
}

static void state_identity_at(const EditCtx *ec, u32 target,
                              u64 *len_out, u64 *hash_out)
{
    const UndoTree *ut = ec->undo;
    u64 current_len = yew_textbuf_len(ec->tb);
    u8 *bytes = copy_live_range(ec->tb, (Span){0U, current_len});
    TextBuf *scratch = yew_textbuf_from_owned_bytes(bytes, current_len);
    u32 id = ut->cur;

    if (!is_ancestor(ut, target, id))
        YEW_BUG("undo: identity target is not on current path");
    while (id != target) {
        const UndoNode *node = node_get(ut, id);
        u32 i = node->n_ops;

        while (i != 0U) {
            const UndoOp *op = &ut->ops.data[node->ops_at + --i];

            if (op->kind == YEW_OP_INS) {
                u64 scratch_len = yew_textbuf_len(scratch);

                if (op->off > scratch_len ||
                    op->len > scratch_len - op->off)
                    YEW_BUG("undo: invalid insert while hashing state");
                yew_textbuf_delete(scratch,
                                   (Span){op->off, op->off + op->len});
            } else if (op->kind == YEW_OP_DEL) {
                if (op->payload > ut->blobs.len ||
                    op->len > ut->blobs.len - op->payload ||
                    op->off > yew_textbuf_len(scratch))
                    YEW_BUG("undo: invalid delete while hashing state");
                yew_textbuf_insert(scratch, BYTEOFF(op->off),
                                   ut->blobs.data + (size_t)op->payload,
                                   op->len);
            } else {
                YEW_BUG("undo: invalid operation while hashing state");
            }
        }
        id = node->parent;
    }
    *len_out = yew_textbuf_len(scratch);
    *hash_out = text_hash(scratch);
    yew_textbuf_free(scratch);
}

static bool reroot_one(UndoTree *ut)
{
    UndoNode *root = node_mut(ut, ut->root);
    u32 child = 0U;
    u32 at;

    if (root == NULL || ut->root == ut->saved ||
        live_count(ut) <= ut->min_nodes)
        return false;
    for (at = root->first_child; at != 0U;
         at = node_get(ut, at)->next_sibling) {
        if (!node_live(ut, at))
            continue;
        if (child != 0U)
            return false;
        child = at;
    }
    if (child == 0U || child == ut->cur)
        return false;
    {
        UndoNode *next = node_mut(ut, child);

        ut->bytes_dead += node_storage_bytes(root) +
            (u64)next->n_ops *
                (sizeof(UndoOp) + sizeof(UndoRepairRun) +
                 sizeof(UndoReplaySpan)) +
            (next->blob_hi - next->blob_lo) +
            (u64)next->n_before * sizeof(CursorRec) +
            (u64)next->n_rep * sizeof(MarkRepair);
        next->n_ops = 0U;
        next->n_before = 0U;
        next->n_rep = 0U;
        next->blob_lo = next->blob_hi;
        next->parent = 0U;
        next->flags |= YEW_TXN_TRIMMED;
        root->flags |= YEW_TXN_DEAD;
        ut->root = child;
        ut->gen++;
    }
    return true;
}

static void trim_tree(EditCtx *ec)
{
    UndoTree *ut = ec->undo;
    bool rerooted = false;

    if (ut->bytes_live <= ut->bytes_max)
        return;
    while (ut->bytes_live > ut->bytes_max) {
        u32 floor = recent_floor(ut);
        if (prune_branch(ut, floor)) {
            account_live(ut);
            continue;
        }
        if (reroot_one(ut)) {
            rerooted = true;
            account_live(ut);
            continue;
        }
        if (!ut->over_budget_logged) {
            yew_log(YEW_LOG_WARN, "undo: over budget, nothing trimmable");
            ut->over_budget_logged = true;
        }
        break;
    }
    if (rerooted) {
        u32 depth_base = node_get(ut, ut->root)->depth;
        u32 id;

        for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
            UndoNode *node;

            if (!node_live(ut, id))
                continue;
            node = node_mut(ut, id);
            if (node->depth < depth_base)
                YEW_BUG("undo trim: descendant depth underflow");
            node->depth -= depth_base;
        }
        state_identity_at(ec, ut->root, &ut->root_len, &ut->root_hash);
    }
    compact_history(ut);
}

u32 yew_undo_branch_cycle(UndoTree *ut, i32 delta)
{
    UndoNode *node;
    YewU32Vec children = {0};
    u32 child;
    size_t index = 0U;
    size_t i;

    if (ut == NULL)
        YEW_BUG("yew_undo_branch_cycle: NULL tree");
    node = node_mut(ut, ut->cur);
    if (node == NULL)
        return 0U;
    for (child = node->first_child; child != 0U;
         child = node_get(ut, child)->next_sibling) {
        if (node_live(ut, child))
            YewU32Vec_push(&children, child);
    }
    if (children.len == 0U) {
        YewU32Vec_free(&children);
        return 0U;
    }
    for (i = 0U; i < children.len; i++) {
        if (children.data[i] == node->redo_child) {
            index = i;
            break;
        }
    }
    if (delta >= 0) {
        index = (index + (size_t)(u32)delta % children.len) % children.len;
    } else {
        size_t back = (size_t)(u32)(-(i64)delta) % children.len;
        index = (index + children.len - back) % children.len;
    }
    node->redo_child = children.data[index];
    child = node->redo_child;
    YewU32Vec_free(&children);
    return child;
}

u32 yew_undo_current(const UndoTree *ut)
{
    return ut == NULL ? 0U : ut->cur;
}

bool yew_undo_at_save_point(const UndoTree *ut)
{
    return ut != NULL && ut->saved != 0U && ut->cur == ut->saved;
}

void yew_undo_mark_saved(UndoTree *ut)
{
    UndoNode *old;
    UndoNode *node;
    if (ut == NULL)
        YEW_BUG("yew_undo_mark_saved: NULL tree");
    old = node_mut(ut, ut->saved);
    if (old != NULL)
        old->flags &= (u8)~YEW_TXN_SAVED;
    ut->saved = ut->cur;
    node = node_mut(ut, ut->saved);
    if (node != NULL)
        node->flags |= YEW_TXN_SAVED;
    if (ut->owner == NULL)
        YEW_BUG("undo: save point has no buffer owner");
    ut->saved_len = yew_textbuf_len(ut->owner);
    ut->saved_hash = text_hash(ut->owner);
    ut->boundary = true;
}

static bool is_ancestor(const UndoTree *ut, u32 ancestor, u32 id)
{
    while (id != 0U) {
        if (id == ancestor)
            return true;
        id = node_get(ut, id)->parent;
    }
    return false;
}

u32 yew_undo_list(const UndoTree *ut, UndoNodeInfo *out, u32 max)
{
    u32 total = 0U;
    u32 id;
    if (ut == NULL)
        YEW_BUG("yew_undo_list: NULL tree");
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        const UndoNode *node;
        UndoNodeInfo info;
        u32 child;
        u32 i;
        if (!node_live(ut, id))
            continue;
        node = node_get(ut, id);
        (void)memset(&info, 0, sizeof(info));
        info.id = id;
        info.parent = node->parent;
        info.depth = node->depth;
        info.reason = node->reason;
        info.t_wall = node->t_wall;
        info.on_current_path = is_ancestor(ut, id, ut->cur);
        info.is_current = id == ut->cur;
        info.is_saved = id == ut->saved;
        info.is_trimmed = (node->flags & YEW_TXN_TRIMMED) != 0U;
        for (child = node->first_child; child != 0U;
             child = node_get(ut, child)->next_sibling) {
            if (node_live(ut, child))
                info.n_children++;
        }
        for (i = 0U; i < node->n_ops; i++) {
            const UndoOp *op = &ut->ops.data[node->ops_at + i];
            if (op->kind == YEW_OP_INS)
                info.bytes_ins += op->len;
            else
                info.bytes_del += op->len;
        }
        if (out != NULL && total < max)
            out[total] = info;
        total++;
    }
    return total;
}

u32 yew_undo_children(const UndoTree *ut, u32 id, u32 *out, u32 max)
{
    const UndoNode *node;
    u32 total = 0U;
    u32 child;
    if (!node_live(ut, id))
        return 0U;
    node = node_get(ut, id);
    for (child = node->first_child; child != 0U;
         child = node_get(ut, child)->next_sibling) {
        if (!node_live(ut, child))
            continue;
        if (out != NULL && total < max)
            out[total] = child;
        total++;
    }
    return total;
}

static const char *reason_name(u8 reason)
{
    static const char *const names[YEW_TXN_REASON_MAX] = {
        "typed", "erased", "pasted", "cut", "multi", "macro",
        "filtered", "replaced", "lsp", "external"
    };
    return reason < YEW_TXN_REASON_MAX ? names[reason] : "unknown";
}

void yew_undo_describe(const UndoTree *ut, u32 id, i64 now, char *buf,
                       u64 len)
{
    const UndoNode *node;
    u64 bytes_ins = 0U;
    u64 bytes_del = 0U;
    i64 age;
    char age_buf[32];
    u32 i;

    if (buf == NULL || len == 0U)
        return;
    if (!node_live(ut, id)) {
        buf[0] = '\0';
        return;
    }
    node = node_get(ut, id);
    for (i = 0U; i < node->n_ops; i++) {
        const UndoOp *op = &ut->ops.data[node->ops_at + i];
        if (op->kind == YEW_OP_INS)
            bytes_ins += op->len;
        else
            bytes_del += op->len;
    }
    age = now > node->t_wall ? now - node->t_wall : 0;
    if (age < 60)
        (void)snprintf(age_buf, sizeof(age_buf), "%" PRId64 "s ago", age);
    else if (age < 3600)
        (void)snprintf(age_buf, sizeof(age_buf), "%" PRId64 "m ago", age / 60);
    else if (age < 86400)
        (void)snprintf(age_buf, sizeof(age_buf), "%" PRId64 "h ago", age / 3600);
    else
        (void)snprintf(age_buf, sizeof(age_buf), "%" PRId64 "d ago", age / 86400);
    (void)snprintf(buf, (size_t)len, "#%u  +%" PRIu64 " -%" PRIu64
                   "  %s  %s%s", id, bytes_ins, bytes_del,
                   reason_name(node->reason), age_buf,
                   id == ut->saved ? "  *saved" : "");
}

void yew_undo_dump(const UndoTree *ut, FILE *out)
{
    u32 id;
    if (ut == NULL || out == NULL)
        YEW_BUG("yew_undo_dump: NULL argument");
    (void)fprintf(out, "undo root=%u cur=%u saved=%u gen=%" PRIu64 "\n",
                  ut->root, ut->cur, ut->saved, ut->gen);
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        const UndoNode *node = node_get(ut, id);
        u32 i;
        if (!node_live(ut, id))
            continue;
        (void)fprintf(out,
                      "node %u parent=%u redo=%u depth=%u reason=%u flags=%u"
                      " wall=%" PRId64 " ops=%u\n",
                      id, node->parent, node->redo_child, node->depth,
                      (unsigned)node->reason, (unsigned)node->flags,
                      node->t_wall, node->n_ops);
        for (i = 0U; i < node->n_ops; i++) {
            const UndoOp *op = &ut->ops.data[node->ops_at + i];
            (void)fprintf(out, "  op %u kind=%u off=%" PRIu64
                          " len=%" PRIu64 " repairs=%u\n", i,
                          (unsigned)op->kind, op->off, op->len,
                          ut->repair_runs.data[node->ops_at + i].len);
        }
    }
}

static void put_u16(Bytebuf *buf, u16 value)
{
    u8 bytes[2];
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8U);
    bytebuf_append(buf, bytes, sizeof(bytes));
}

static void put_u32(Bytebuf *buf, u32 value)
{
    u8 bytes[4];
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8U);
    bytes[2] = (u8)(value >> 16U);
    bytes[3] = (u8)(value >> 24U);
    bytebuf_append(buf, bytes, sizeof(bytes));
}

static void put_u64(Bytebuf *buf, u64 value)
{
    u8 bytes[8];
    unsigned int i;
    for (i = 0U; i < 8U; i++)
        bytes[i] = (u8)(value >> (i * 8U));
    bytebuf_append(buf, bytes, sizeof(bytes));
}

static void put_cursor(Bytebuf *buf, const CursorRec *cursor)
{
    put_u64(buf, cursor->pos);
    put_u64(buf, cursor->anchor);
    put_u64(buf, cursor->goal);
}

static void put_repair(Bytebuf *buf, const MarkRepair *repair)
{
    put_u32(buf, repair->mark_id);
    put_u32(buf, repair->mark_gen);
    put_u64(buf, repair->rel_off);
}

static void write_header(Bytebuf *file, u32 flags, u32 root, u32 cur,
                         u32 saved, u32 anchor, u32 count,
                         u64 anchor_len, u64 anchor_hash,
                         u64 cur_len, u64 cur_hash)
{
    bytebuf_append(file, "YEWU", 4U);
    put_u32(file, YEWU_VERSION);
    put_u32(file, flags);
    put_u32(file, root);
    put_u32(file, cur);
    put_u32(file, saved);
    put_u32(file, anchor);
    put_u32(file, count);
    put_u64(file, anchor_len);
    put_u64(file, anchor_hash);
    put_u64(file, cur_len);
    put_u64(file, cur_hash);
}

static u32 write_node_record(EditCtx *ec, Bytebuf *file,
                             const UndoNode *node, u32 parent,
                             u32 redo_child, u32 depth, u8 flags,
                             bool rerooted)
{
    const UndoTree *ut = ec->undo;
    size_t record_at;
    u32 n_ops = rerooted ? 0U : node->n_ops;
    u32 n_before = rerooted ? 0U : node->n_before;
    u32 i;
    u32 crc;

    record_at = file->len;
    put_u32(file, node->id);
    put_u32(file, parent);
    put_u32(file, redo_child);
    put_u32(file, depth);
    bytebuf_push_u8(file, node->reason);
    bytebuf_push_u8(file, flags);
    put_u16(file, 0U);
    put_u32(file, node->win_id);
    put_u64(file, (u64)node->t_wall);
    put_u32(file, n_ops);
    for (i = 0U; i < n_ops; i++) {
        u32 index = node->ops_at + i;
        const UndoOp *op = &ut->ops.data[index];
        const UndoRepairRun *run = &ut->repair_runs.data[index];
        const u8 *payload;
        u32 r;

        if (op->kind == YEW_OP_INS) {
            payload = store_bytes(ec->tb, op->src,
                                  (Span){op->payload,
                                         op->payload + op->len});
        } else {
            if (op->payload > ut->blobs.len ||
                op->len > ut->blobs.len - op->payload)
                YEW_BUG("undo write: corrupt delete payload");
            payload = ut->blobs.data + (size_t)op->payload;
        }
        bytebuf_push_u8(file, op->kind);
        bytebuf_push_u8(file, 0U);
        put_u16(file, 0U);
        put_u64(file, op->off);
        put_u64(file, op->len);
        put_u64(file, op->len);
        bytebuf_append(file, payload, (size_t)op->len);
        put_u32(file, run->len);
        for (r = 0U; r < run->len; r++)
            put_repair(file, &ut->repairs.data[run->at + r]);
    }
    put_u32(file, n_before);
    for (i = 0U; i < n_before; i++)
        put_cursor(file, &ut->cursors.data[node->cur_before + i]);
    put_u32(file, node->n_after);
    for (i = 0U; i < node->n_after; i++)
        put_cursor(file, &ut->cursors.data[node->cur_after + i]);
    crc = yew_crc32(file->data + record_at, file->len - record_at);
    put_u32(file, crc);
    return crc;
}

static u64 serialized_node_size(const UndoTree *ut, const UndoNode *node,
                                bool rerooted)
{
    u64 total = 36U + 12U + (u64)node->n_after * 24U;
    u32 i;

    if (!rerooted)
        total += (u64)node->n_before * 24U;
    if (rerooted)
        return total;
    for (i = 0U; i < node->n_ops; i++) {
        u32 index = node->ops_at + i;
        const UndoOp *op = &ut->ops.data[index];
        const UndoRepairRun *run = &ut->repair_runs.data[index];
        u64 repair_size = (u64)run->len * 16U;

        if (total > UINT64_MAX - 32U ||
            op->len > UINT64_MAX - total - 32U ||
            repair_size > UINT64_MAX - total - 32U - op->len)
            YEW_BUG("undo write: serialized node size overflow");
        total += 32U + op->len + repair_size;
    }
    return total;
}

static bool write_truncated_path(EditCtx *ec, Bytebuf *file,
                                 u64 cur_len, u64 cur_hash)
{
    UndoTree *ut = ec->undo;
    YewU32Vec path = {0};
    u64 tail_size = 0U;
    size_t first = SIZE_MAX;
    size_t i;
    u32 id = ut->cur;
    u32 saved = 0U;
    u32 crc_xor = 0U;
    u64 root_len;
    u64 root_hash;

    while (id != 0U) {
        YewU32Vec_push(&path, id);
        if (id == ut->root)
            break;
        id = node_get(ut, id)->parent;
    }
    if (path.len == 0U || path.data[path.len - 1U] != ut->root)
        YEW_BUG("undo write: current path does not reach root");
    for (i = 0U; i < path.len / 2U; i++) {
        u32 swap = path.data[i];
        path.data[i] = path.data[path.len - i - 1U];
        path.data[path.len - i - 1U] = swap;
    }
    for (i = path.len; i != 0U; ) {
        const UndoNode *node = node_get(ut, path.data[--i]);
        u64 root_size = serialized_node_size(ut, node, true);
        u64 total = YEWU_HEADER_LEN + 8U;

        if (tail_size <= UINT64_MAX - total - root_size)
            total += root_size + tail_size;
        else
            total = UINT64_MAX;
        if (total <= ut->persist_bytes_max)
            first = i;
        else
            break;
        root_size = serialized_node_size(ut, node, false);
        if (root_size > UINT64_MAX - tail_size)
            tail_size = UINT64_MAX;
        else
            tail_size += root_size;
    }
    if (first == SIZE_MAX) {
        YewU32Vec_free(&path);
        return false;
    }
    if (ut->saved != 0U) {
        for (i = first; i < path.len; i++) {
            if (path.data[i] == ut->saved) {
                saved = ut->saved;
                break;
            }
        }
    }
    state_identity_at(ec, path.data[first], &root_len, &root_hash);
    bytebuf_free(file);
    bytebuf_init(file);
    if (path.len - first > UINT32_MAX)
        YEW_BUG("undo write: too many nodes on truncated path");
    write_header(file, YEWU_TRUNCATED, path.data[first], ut->cur, saved,
                 saved != 0U ? saved : path.data[first],
                 (u32)(path.len - first),
                 saved != 0U ? ut->saved_len : root_len,
                 saved != 0U ? ut->saved_hash : root_hash,
                 cur_len, cur_hash);
    for (i = first; i < path.len; i++) {
        const UndoNode *node = node_get(ut, path.data[i]);
        u8 flags = node->flags & (u8)~YEW_TXN_SAVED;
        bool rerooted = i == first;

        if (rerooted)
            flags |= YEW_TXN_TRIMMED;
        if (node->id == saved)
            flags |= YEW_TXN_SAVED;
        crc_xor ^= write_node_record(
            ec, file, node, rerooted ? 0U : path.data[i - 1U],
            i + 1U < path.len ? path.data[i + 1U] : 0U,
            (u32)(i - first), flags, rerooted);
    }
    put_u32(file, crc_xor);
    bytebuf_append(file, "UGAS", 4U);
    YewU32Vec_free(&path);
    return (u64)file->len <= ut->persist_bytes_max;
}

static u32 selected_redo_child(const UndoTree *ut, const u8 *keep,
                               const UndoNode *node)
{
    u32 child;

    if (node->redo_child != 0U && keep[node->redo_child] != 0U)
        return node->redo_child;
    for (child = node->first_child; child != 0U;
         child = node_get(ut, child)->next_sibling) {
        if (keep[child] != 0U)
            return child;
    }
    return 0U;
}

static bool write_selected_tree(EditCtx *ec, Bytebuf *file,
                                const u8 *keep, u32 root, u32 count,
                                u64 cur_len, u64 cur_hash)
{
    UndoTree *ut = ec->undo;
    const UndoNode *root_node = node_get(ut, root);
    u32 saved = ut->saved != 0U && keep[ut->saved] != 0U ? ut->saved : 0U;
    u32 anchor = saved != 0U ? saved : root;
    u32 depth_base = root_node->depth;
    u32 crc_xor = 0U;
    u64 root_len;
    u64 root_hash;
    u32 id;

    state_identity_at(ec, root, &root_len, &root_hash);
    bytebuf_free(file);
    bytebuf_init(file);
    write_header(file, YEWU_TRUNCATED, root, ut->cur, saved, anchor, count,
                 saved != 0U ? ut->saved_len : root_len,
                 saved != 0U ? ut->saved_hash : root_hash,
                 cur_len, cur_hash);
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        const UndoNode *node;
        bool rerooted;
        u8 flags;

        if (keep[id] == 0U)
            continue;
        node = node_get(ut, id);
        if (node->depth < depth_base)
            YEW_BUG("undo write: selected depth underflow");
        rerooted = id == root && root != ut->root;
        flags = node->flags &
                (u8)~(YEW_TXN_DEAD | YEW_TXN_SAVED);
        if (rerooted)
            flags |= YEW_TXN_TRIMMED;
        if (id == saved)
            flags |= YEW_TXN_SAVED;
        crc_xor ^= write_node_record(
            ec, file, node, id == root ? 0U : node->parent,
            selected_redo_child(ut, keep, node),
            node->depth - depth_base, flags, rerooted);
    }
    put_u32(file, crc_xor);
    bytebuf_append(file, "UGAS", 4U);
    return (u64)file->len <= ut->persist_bytes_max;
}

static bool write_truncated_tree(EditCtx *ec, Bytebuf *file,
                                 u64 full_size, u64 cur_len, u64 cur_hash)
{
    UndoTree *ut = ec->undo;
    size_t slots = ut->nodes.len + 1U;
    u8 *keep = yew_xcalloc(slots, sizeof(*keep));
    u8 *protect = yew_xcalloc(slots, sizeof(*protect));
    u8 *subtree_protected = yew_xcalloc(slots, sizeof(*subtree_protected));
    u64 *node_sizes = yew_xcalloc(slots, sizeof(*node_sizes));
    u64 *subtree_sizes = yew_xcalloc(slots, sizeof(*subtree_sizes));
    u32 *subtree_counts = yew_xcalloc(slots, sizeof(*subtree_counts));
    u64 total = full_size;
    u64 root_size;
    u32 count = live_count(ut);
    u32 root = ut->root;
    u32 recent = 0U;
    u32 id;
    bool wrote = false;

    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        if (!node_live(ut, id))
            continue;
        keep[id] = 1U;
        node_sizes[id] = serialized_node_size(ut, node_get(ut, id), false);
    }
    root_size = node_sizes[root];
    id = ut->cur;
    while (id != 0U) {
        protect[id] = 1U;
        id = node_get(ut, id)->parent;
    }
    id = ut->saved;
    while (id != 0U) {
        protect[id] = 1U;
        id = node_get(ut, id)->parent;
    }
    id = (u32)ut->nodes.len;
    while (id != 0U && recent < ut->min_nodes) {
        if (keep[id] != 0U) {
            protect[id] = 1U;
            recent++;
        }
        id--;
    }
    {
        size_t at = ut->nodes.len;

        while (at != 0U) {
            const UndoNode *node;
            u32 parent;

            id = (u32)at--;
            if (keep[id] == 0U)
                continue;
            node = node_get(ut, id);
            subtree_sizes[id] += node_sizes[id];
            subtree_counts[id]++;
            if (protect[id] != 0U)
                subtree_protected[id] = 1U;
            parent = node->parent;
            if (parent != 0U) {
                if (subtree_sizes[id] >
                    UINT64_MAX - subtree_sizes[parent])
                    subtree_sizes[parent] = UINT64_MAX;
                else
                    subtree_sizes[parent] += subtree_sizes[id];
                if (subtree_counts[id] >
                    UINT32_MAX - subtree_counts[parent])
                    YEW_BUG("undo write: selected node count overflow");
                subtree_counts[parent] += subtree_counts[id];
                if (subtree_protected[id] != 0U)
                    subtree_protected[parent] = 1U;
            }
        }
    }
    for (id = 1U; (size_t)id <= ut->nodes.len &&
                       total > ut->persist_bytes_max;
         id++) {
        const UndoNode *node;
        u32 parent;
        u32 drop;

        if (keep[id] == 0U || subtree_protected[id] != 0U)
            continue;
        node = node_get(ut, id);
        parent = node->parent;
        if (parent != 0U && subtree_protected[parent] == 0U)
            continue;
        if (subtree_sizes[id] > total || subtree_counts[id] > count)
            YEW_BUG("undo write: invalid selected subtree accounting");
        total -= subtree_sizes[id];
        count -= subtree_counts[id];
        keep[id] = 0U;
        for (drop = id + 1U; (size_t)drop <= ut->nodes.len; drop++) {
            const UndoNode *candidate;

            if (keep[drop] == 0U)
                continue;
            candidate = node_get(ut, drop);
            if (candidate->parent != 0U &&
                keep[candidate->parent] == 0U)
                keep[drop] = 0U;
        }
    }
    while (total > ut->persist_bytes_max && count > ut->min_nodes &&
           root != ut->saved) {
        const UndoNode *root_node = node_get(ut, root);
        u32 child = 0U;
        u32 at;
        u64 old_child_size;
        u64 new_child_size;

        for (at = root_node->first_child; at != 0U;
             at = node_get(ut, at)->next_sibling) {
            if (keep[at] == 0U)
                continue;
            if (child != 0U) {
                child = 0U;
                break;
            }
            child = at;
        }
        if (child == 0U || child == ut->cur)
            break;
        old_child_size = node_sizes[child];
        new_child_size = serialized_node_size(
            ut, node_get(ut, child), true);
        if (root_size > total || old_child_size > total - root_size)
            YEW_BUG("undo write: invalid selected root accounting");
        total -= root_size + old_child_size;
        if (new_child_size > UINT64_MAX - total)
            total = UINT64_MAX;
        else
            total += new_child_size;
        keep[root] = 0U;
        root = child;
        root_size = new_child_size;
        count--;
    }
    if (total <= ut->persist_bytes_max)
        wrote = write_selected_tree(ec, file, keep, root, count,
                                    cur_len, cur_hash);
    free(subtree_counts);
    free(subtree_sizes);
    free(node_sizes);
    free(subtree_protected);
    free(protect);
    free(keep);
    return wrote;
}

static u64 serialized_tree_size(const UndoTree *ut)
{
    u64 total = YEWU_HEADER_LEN + 8U;
    u32 id;

    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        u64 node_size;

        if (!node_live(ut, id))
            continue;
        node_size = serialized_node_size(ut, node_get(ut, id), false);
        if (node_size > UINT64_MAX - total)
            return UINT64_MAX;
        total += node_size;
    }
    return total;
}

YewUndoWriteResult yew_undo_write(EditCtx *ec, const char *path)
{
    UndoTree *ut;
    Bytebuf file;
    u32 anchor;
    u32 count;
    u32 crc_xor = 0U;
    u32 id;
    u64 serialized_size;
    u64 current_len;
    u64 current_hash;
    YewSaveErr result;

    require_ctx(ec);
    if (path == NULL)
        YEW_BUG("yew_undo_write: NULL path");
    ut = ec->undo;
    if (ut->depth != 0U)
        YEW_BUG("undo: serialize inside transaction");
    current_len = yew_textbuf_len(ec->tb);
    current_hash = text_hash(ec->tb);
    anchor = ut->saved != 0U ? ut->saved : ut->root;
    count = live_count(ut);
    serialized_size = serialized_tree_size(ut);
    bytebuf_init(&file);
    if (serialized_size > ut->persist_bytes_max ||
        serialized_size > SIZE_MAX) {
        if (!write_truncated_tree(ec, &file, serialized_size,
                                  current_len, current_hash) &&
            !write_truncated_path(ec, &file, current_len, current_hash)) {
            bytebuf_free(&file);
            return YEW_UNDO_WRITE_TOO_LARGE;
        }
    } else {
        write_header(&file, 0U, ut->root, ut->cur, ut->saved, anchor, count,
                     ut->saved != 0U ? ut->saved_len : ut->root_len,
                     ut->saved != 0U ? ut->saved_hash : ut->root_hash,
                     current_len, current_hash);
        for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
            const UndoNode *node;

            if (!node_live(ut, id))
                continue;
            node = node_get(ut, id);
            crc_xor ^= write_node_record(ec, &file, node, node->parent,
                                         node->redo_child, node->depth,
                                         node->flags, false);
        }
        put_u32(&file, crc_xor);
        bytebuf_append(&file, "UGAS", 4U);
    }
    result = yew_file_write_atomic(path, file.data, file.len, 0600);
    bytebuf_free(&file);
    return result == YEW_SAVE_OK ? YEW_UNDO_WRITE_OK : YEW_UNDO_WRITE_IO;
}

typedef struct {
    const u8 *data;
    size_t len;
    size_t at;
} YewuReader;

typedef enum {
    YEWU_FILE_OK = 0,
    YEWU_FILE_IO,
    YEWU_FILE_INVALID
} YewuFileRead;

static bool take(YewuReader *reader, size_t len, const u8 **out)
{
    if (reader->at > reader->len || len > reader->len - reader->at)
        return false;
    *out = reader->data + reader->at;
    reader->at += len;
    return true;
}

static bool get_u16(YewuReader *reader, u16 *out)
{
    const u8 *bytes;
    if (!take(reader, 2U, &bytes))
        return false;
    *out = (u16)bytes[0] | (u16)((u16)bytes[1] << 8U);
    return true;
}

static bool get_u32(YewuReader *reader, u32 *out)
{
    const u8 *bytes;
    if (!take(reader, 4U, &bytes))
        return false;
    *out = (u32)bytes[0] | (u32)bytes[1] << 8U |
           (u32)bytes[2] << 16U | (u32)bytes[3] << 24U;
    return true;
}

static bool get_u64(YewuReader *reader, u64 *out)
{
    const u8 *bytes;
    unsigned int i;
    u64 value = 0U;
    if (!take(reader, 8U, &bytes))
        return false;
    for (i = 0U; i < 8U; i++)
        value |= (u64)bytes[i] << (i * 8U);
    *out = value;
    return true;
}

static YewuFileRead read_file_bytes(const char *path, u64 max_bytes,
                                    Bytebuf *buf)
{
    struct stat path_st;
    struct stat before;
    struct stat after;
    int flags = O_RDONLY;
    int fd;
    size_t size;
    size_t at = 0U;
    YewuFileRead result = YEWU_FILE_IO;

    bytebuf_init(buf);
    if (lstat(path, &path_st) != 0)
        return YEWU_FILE_IO;
    if (!S_ISREG(path_st.st_mode))
        return YEWU_FILE_INVALID;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0)
        return errno == ELOOP ? YEWU_FILE_INVALID : YEWU_FILE_IO;
    if (fstat(fd, &before) != 0)
        goto done;
    if (!S_ISREG(before.st_mode) || before.st_dev != path_st.st_dev ||
        before.st_ino != path_st.st_ino || before.st_size < 0 ||
        (u64)before.st_size > max_bytes ||
        (u64)before.st_size > SIZE_MAX) {
        result = YEWU_FILE_INVALID;
        goto done;
    }
    size = (size_t)before.st_size;
    bytebuf_reserve(buf, size);
    while (at < size) {
        ssize_t got = read(fd, buf->data + at, size - at);

        if (got > 0)
            at += (size_t)got;
        else if (got < 0 && errno == EINTR)
            continue;
        else
            goto done;
    }
    buf->len = size;
    if (fstat(fd, &after) != 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size)
        goto done;
    result = YEWU_FILE_OK;

done:
    if (close(fd) != 0 && result == YEWU_FILE_OK)
        result = YEWU_FILE_IO;
    if (result != YEWU_FILE_OK)
        bytebuf_free(buf);
    return result;
}

static void init_loaded_tree(UndoTree *ut, const TextBuf *tb)
{
    (void)memset(ut, 0, sizeof(*ut));
    bytebuf_init(&ut->blobs);
    ut->bytes_max = YEW_UNDO_BYTES_MAX;
    ut->persist_bytes_max = YEW_UNDO_PERSIST_BYTES_MAX;
    ut->min_nodes = YEW_UNDO_MIN_NODES;
    ut->pending_reason = YEW_TXN_REASON_MAX;
    ut->boundary = true;
    ut->mono_clock = default_mono;
    ut->wall_clock = default_wall;
    ut->owner = tb;
}

static void dispose_loaded_tree(UndoTree *ut)
{
    YewUndoNodeVec_free(&ut->nodes);
    YewUndoOpVec_free(&ut->ops);
    YewCursorRecVec_free(&ut->cursors);
    YewMarkRepairVec_free(&ut->repairs);
    YewUndoRepairRunVec_free(&ut->repair_runs);
    YewUndoReplaySpanVec_free(&ut->replay_spans);
    bytebuf_free(&ut->blobs);
}

static bool parse_cursor(YewuReader *reader, CursorRec *cursor)
{
    return get_u64(reader, &cursor->pos) &&
           get_u64(reader, &cursor->anchor) &&
           get_u64(reader, &cursor->goal);
}

static bool parse_repair(YewuReader *reader, MarkRepair *repair)
{
    return get_u32(reader, &repair->mark_id) &&
           get_u32(reader, &repair->mark_gen) &&
           get_u64(reader, &repair->rel_off);
}

static bool grow_node_ids(UndoTree *ut, u32 id)
{
    UndoNode dead;
    const u64 max_ids = YEW_UNDO_PERSIST_BYTES_MAX / 48U;

    if (id == 0U || id > max_ids)
        return false;
    (void)memset(&dead, 0, sizeof(dead));
    dead.flags = YEW_TXN_DEAD;
    while (ut->nodes.len < id)
        YewUndoNodeVec_push(&ut->nodes, dead);
    return true;
}

static bool parse_node(YewuReader *reader, UndoTree *ut, u32 *crc_xor,
                       u32 previous_id)
{
    size_t record_at = reader->at;
    UndoNode node;
    u32 n_ops;
    u32 i;
    u32 stored_crc;
    u32 actual_crc;
    u64 wall;
    u16 pad;

    (void)memset(&node, 0, sizeof(node));
    if (!get_u32(reader, &node.id) || !get_u32(reader, &node.parent) ||
        !get_u32(reader, &node.redo_child) || !get_u32(reader, &node.depth))
        return false;
    {
        const u8 *small;
        if (!take(reader, 2U, &small) || !get_u16(reader, &pad) ||
            !get_u32(reader, &node.win_id) || !get_u64(reader, &wall) ||
            !get_u32(reader, &n_ops))
            return false;
        node.reason = small[0];
        node.flags = small[1];
        node.t_wall = (i64)wall;
        if (pad != 0U ||
            (node.flags & (u8)~(YEW_TXN_TRIMMED | YEW_TXN_SAVED)) != 0U)
            return false;
    }
    if (node.id <= previous_id || node.reason >= YEW_TXN_REASON_MAX ||
        !grow_node_ids(ut, node.id) ||
        (node.parent != 0U && !node_live(ut, node.parent)) ||
        ut->ops.len > UINT32_MAX || ut->repairs.len > UINT32_MAX ||
        n_ops > (reader->len - reader->at) / 32U ||
        n_ops > UINT32_MAX - ut->ops.len)
        return false;
    node.ops_at = (u32)ut->ops.len;
    node.rep_at = (u32)ut->repairs.len;
    node.blob_lo = ut->blobs.len;
    for (i = 0U; i < n_ops; i++) {
        UndoOp op;
        UndoRepairRun run;
        UndoReplaySpan replay;
        const u8 *small;
        const u8 *payload;
        u32 repair_count;
        u64 plen;
        (void)memset(&op, 0, sizeof(op));
        (void)memset(&replay, 0, sizeof(replay));
        if (!take(reader, 2U, &small) || !get_u16(reader, &pad) ||
            !get_u64(reader, &op.off) || !get_u64(reader, &op.len) ||
            !get_u64(reader, &plen) || plen != op.len ||
            plen == 0U || plen > SIZE_MAX ||
            plen > SIZE_MAX - ut->blobs.len ||
            !take(reader, (size_t)plen, &payload))
            return false;
        op.kind = small[0];
        if ((op.kind != YEW_OP_INS && op.kind != YEW_OP_DEL) ||
            small[1] != 0U || pad != 0U)
            return false;
        op.payload = ut->blobs.len;
        bytebuf_append(&ut->blobs, payload, (size_t)plen);
        if (!get_u32(reader, &repair_count) ||
            repair_count > (reader->len - reader->at) / 16U ||
            repair_count > UINT32_MAX - ut->repairs.len ||
            repair_count > UINT32_MAX - node.n_rep ||
            (op.kind == YEW_OP_INS && repair_count != 0U))
            return false;
        run.at = (u32)ut->repairs.len;
        run.len = repair_count;
        {
            u32 r;
            for (r = 0U; r < repair_count; r++) {
                MarkRepair repair;
                if (!parse_repair(reader, &repair))
                    return false;
                YewMarkRepairVec_push(&ut->repairs, repair);
            }
        }
        node.n_rep += repair_count;
        YewUndoOpVec_push(&ut->ops, op);
        YewUndoRepairRunVec_push(&ut->repair_runs, run);
        YewUndoReplaySpanVec_push(&ut->replay_spans, replay);
    }
    node.n_ops = n_ops;
    if (!get_u32(reader, &node.n_before) ||
        ut->cursors.len > UINT32_MAX ||
        node.n_before > (reader->len - reader->at) / 24U ||
        node.n_before > UINT32_MAX - ut->cursors.len)
        return false;
    node.cur_before = (u32)ut->cursors.len;
    for (i = 0U; i < node.n_before; i++) {
        CursorRec cursor;
        if (!parse_cursor(reader, &cursor))
            return false;
        YewCursorRecVec_push(&ut->cursors, cursor);
    }
    if (!get_u32(reader, &node.n_after) ||
        ut->cursors.len > UINT32_MAX ||
        node.n_after > (reader->len - reader->at) / 24U ||
        node.n_after > UINT32_MAX - ut->cursors.len)
        return false;
    node.cur_after = (u32)ut->cursors.len;
    for (i = 0U; i < node.n_after; i++) {
        CursorRec cursor;
        if (!parse_cursor(reader, &cursor))
            return false;
        YewCursorRecVec_push(&ut->cursors, cursor);
    }
    node.blob_hi = ut->blobs.len;
    actual_crc = yew_crc32(reader->data + record_at, reader->at - record_at);
    if (!get_u32(reader, &stored_crc) || actual_crc != stored_crc)
        return false;
    *crc_xor ^= stored_crc;
    ut->nodes.data[node.id - 1U] = node;
    if (node.parent != 0U) {
        UndoNode *parent = node_mut(ut, node.parent);
        u32 *link = &parent->first_child;
        while (*link != 0U)
            link = &node_mut(ut, *link)->next_sibling;
        *link = node.id;
    }
    return true;
}

static bool reverse_node_len(const UndoTree *ut, const UndoNode *node,
                             u64 *len)
{
    u32 i = node->n_ops;

    while (i != 0U) {
        const UndoOp *op = &ut->ops.data[node->ops_at + --i];

        if (op->kind == YEW_OP_INS) {
            if (op->off > *len || op->len > *len - op->off)
                return false;
            *len -= op->len;
        } else if (op->kind == YEW_OP_DEL) {
            if (op->off > *len || op->len > UINT64_MAX - *len)
                return false;
            *len += op->len;
        } else {
            return false;
        }
    }
    return true;
}

static bool forward_node_len(const UndoTree *ut, const UndoNode *node,
                             u64 *len)
{
    u32 i;

    for (i = 0U; i < node->n_ops; i++) {
        const UndoOp *op = &ut->ops.data[node->ops_at + i];

        if (op->kind == YEW_OP_INS) {
            if (op->off > *len || op->len > UINT64_MAX - *len)
                return false;
            *len += op->len;
        } else if (op->kind == YEW_OP_DEL) {
            if (op->off > *len || op->len > *len - op->off)
                return false;
            *len -= op->len;
        } else {
            return false;
        }
    }
    return true;
}

static bool cursor_slice_valid(const UndoTree *ut, u32 at, u32 count,
                               u64 len)
{
    u32 i;

    if ((size_t)at > ut->cursors.len ||
        count > ut->cursors.len - (size_t)at)
        return false;
    for (i = 0U; i < count; i++) {
        const CursorRec *cursor = &ut->cursors.data[at + i];

        if (cursor->pos > len || cursor->anchor > len)
            return false;
    }
    return true;
}

static bool node_payloads_valid(const UndoTree *ut, const UndoNode *node)
{
    u32 i;

    if ((size_t)node->ops_at > ut->ops.len ||
        node->n_ops > ut->ops.len - (size_t)node->ops_at)
        return false;
    for (i = 0U; i < node->n_ops; i++) {
        u32 index = node->ops_at + i;
        const UndoOp *op = &ut->ops.data[index];
        const UndoRepairRun *run;
        u32 r;

        if ((size_t)index >= ut->repair_runs.len ||
            op->payload > ut->blobs.len ||
            op->len > ut->blobs.len - op->payload)
            return false;
        run = &ut->repair_runs.data[index];
        if ((size_t)run->at > ut->repairs.len ||
            run->len > ut->repairs.len - (size_t)run->at ||
            (op->kind == YEW_OP_INS && run->len != 0U))
            return false;
        for (r = 0U; r < run->len; r++) {
            if (ut->repairs.data[run->at + r].rel_off > op->len)
                return false;
        }
    }
    return true;
}

static bool validate_loaded(const UndoTree *ut, u32 root, u32 cur, u32 saved,
                            u32 anchor, u64 anchor_len, u64 cur_len,
                            u64 *root_len_out)
{
    u64 *lengths;
    u64 root_len = anchor_len;
    u32 id;

    if (!node_live(ut, root) || node_get(ut, root)->parent != 0U ||
        !node_live(ut, cur) || !node_live(ut, anchor) ||
        (saved != 0U && !node_live(ut, saved)) ||
        anchor != (saved != 0U ? saved : root) ||
        !is_ancestor(ut, root, cur) || !is_ancestor(ut, root, anchor) ||
        node_get(ut, root)->depth != 0U ||
        node_get(ut, root)->n_ops != 0U ||
        node_get(ut, root)->n_before != 0U)
        return false;
    id = anchor;
    while (id != root) {
        const UndoNode *node = node_get(ut, id);

        if (!reverse_node_len(ut, node, &root_len))
            return false;
        id = node->parent;
    }
    lengths = yew_xcalloc(ut->nodes.len, sizeof(*lengths));
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        const UndoNode *node;
        u64 before_len;
        u64 after_len;

        if (!node_live(ut, id))
            continue;
        node = node_get(ut, id);
        if ((id == root && node->parent != 0U) ||
            (id != root &&
             (!node_live(ut, node->parent) || node->parent == 0U ||
              node->depth != node_get(ut, node->parent)->depth + 1U)) ||
            ((node->flags & YEW_TXN_SAVED) != 0U) != (id == saved) ||
            !node_payloads_valid(ut, node))
            goto invalid;
        if (node->redo_child != 0U &&
            (!node_live(ut, node->redo_child) ||
             node_get(ut, node->redo_child)->parent != id))
            goto invalid;
        before_len = id == root ? root_len : lengths[node->parent - 1U];
        after_len = before_len;
        if (!forward_node_len(ut, node, &after_len) ||
            !cursor_slice_valid(ut, node->cur_before, node->n_before,
                                before_len) ||
            !cursor_slice_valid(ut, node->cur_after, node->n_after,
                                after_len))
            goto invalid;
        lengths[id - 1U] = after_len;
    }
    if (lengths[cur - 1U] != cur_len ||
        lengths[anchor - 1U] != anchor_len)
        goto invalid;
    free(lengths);
    *root_len_out = root_len;
    return true;

invalid:
    free(lengths);
    return false;
}

static void materialize_insert_payloads(EditCtx *ec, UndoTree *ut)
{
    size_t i;
    for (i = 0U; i < ut->ops.len; i++) {
        UndoOp *op = &ut->ops.data[i];
        u64 blob_at;
        u64 add_at;
        u64 end;
        if (op->kind != YEW_OP_INS)
            continue;
        blob_at = op->payload;
        add_at = ec->tb->add.len;
        end = yew_textbuf_len(ec->tb);
        yew_textbuf_insert(ec->tb, BYTEOFF(end),
                           ut->blobs.data + (size_t)blob_at, op->len);
        yew_textbuf_delete(ec->tb, (Span){end, end + op->len});
        op->src = YEW_STORE_ADD;
        op->payload = add_at;
    }
}

static void retain_delete_payloads(UndoTree *ut)
{
    Bytebuf deletes;
    u32 id;
    bytebuf_init(&deletes);
    for (id = 1U; (size_t)id <= ut->nodes.len; id++) {
        UndoNode *node;
        u32 i;
        if (!node_live(ut, id))
            continue;
        node = node_mut(ut, id);
        node->blob_lo = deletes.len;
        for (i = 0U; i < node->n_ops; i++) {
            UndoOp *op = &ut->ops.data[node->ops_at + i];
            u64 old;
            if (op->kind != YEW_OP_DEL)
                continue;
            old = op->payload;
            if (old > ut->blobs.len || op->len > ut->blobs.len - old)
                YEW_BUG("undo read: corrupt materialized delete payload");
            op->payload = deletes.len;
            bytebuf_append(&deletes, ut->blobs.data + (size_t)old,
                           (size_t)op->len);
        }
        node->blob_hi = deletes.len;
    }
    bytebuf_free(&ut->blobs);
    ut->blobs = deletes;
}

YewUndoReadResult yew_undo_read(EditCtx *ec, const char *path)
{
    Bytebuf file;
    YewuReader reader;
    UndoTree loaded;
    const u8 *magic;
    u32 version;
    u32 flags;
    u32 root;
    u32 cur;
    u32 saved;
    u32 anchor;
    u32 count;
    u64 anchor_len;
    u64 anchor_hash;
    u64 cur_len;
    u64 cur_hash;
    u64 root_len;
    u64 root_hash;
    u32 crc_xor = 0U;
    u32 stored_xor;
    u32 i;
    u32 previous_id = 0U;
    u64 len;
    u64 hash;
    YewUndoReadResult outcome;
    YewuFileRead file_result;
    UndoTree *old;

    require_ctx(ec);
    if (path == NULL)
        YEW_BUG("yew_undo_read: NULL path");
    if (ec->undo->depth != 0U || ec->undo->open != 0U)
        YEW_BUG("undo: deserialize inside transaction");
    file_result = read_file_bytes(
        path, ec->undo->persist_bytes_max < YEW_UNDO_PERSIST_BYTES_MAX
                  ? ec->undo->persist_bytes_max
                  : YEW_UNDO_PERSIST_BYTES_MAX,
        &file);
    if (file_result == YEWU_FILE_IO)
        return YEW_UNDO_READ_IO;
    if (file_result == YEWU_FILE_INVALID) {
        yew_log(YEW_LOG_WARN, "undo: dropped unsafe or oversized .yewu");
        return YEW_UNDO_READ_DROPPED;
    }
    reader = (YewuReader){file.data, file.len, 0U};
    init_loaded_tree(&loaded, ec->tb);
    if (!take(&reader, 4U, &magic) || memcmp(magic, "YEWU", 4U) != 0 ||
        !get_u32(&reader, &version) || version != YEWU_VERSION ||
        !get_u32(&reader, &flags) || !get_u32(&reader, &root) ||
        !get_u32(&reader, &cur) || !get_u32(&reader, &saved) ||
        !get_u32(&reader, &anchor) || !get_u32(&reader, &count) ||
        !get_u64(&reader, &anchor_len) || !get_u64(&reader, &anchor_hash) ||
        !get_u64(&reader, &cur_len) || !get_u64(&reader, &cur_hash) ||
        reader.at != YEWU_HEADER_LEN ||
        (flags & (u32)~YEWU_TRUNCATED) != 0U || count == 0U ||
        reader.len < YEWU_HEADER_LEN + 8U ||
        count > (reader.len - YEWU_HEADER_LEN - 8U) / 48U) {
        goto dropped;
    }
    for (i = 0U; i < count; i++) {
        size_t before = reader.at;
        if (!parse_node(&reader, &loaded, &crc_xor, previous_id))
            goto dropped;
        previous_id = (u32)loaded.nodes.len;
        if (reader.at <= before)
            goto dropped;
    }
    if (!get_u32(&reader, &stored_xor) || stored_xor != crc_xor ||
        !take(&reader, 4U, &magic) || memcmp(magic, "UGAS", 4U) != 0 ||
        reader.at != reader.len ||
        !validate_loaded(&loaded, root, cur, saved, anchor, anchor_len,
                         cur_len, &root_len))
        goto dropped;
    len = yew_textbuf_len(ec->tb);
    hash = text_hash(ec->tb);
    if (len == cur_len && hash == cur_hash) {
        outcome = YEW_UNDO_READ_CURRENT;
        loaded.cur = cur;
    } else if (len == anchor_len && hash == anchor_hash) {
        outcome = YEW_UNDO_READ_ANCHOR;
        loaded.cur = anchor;
    } else {
        goto dropped;
    }
    loaded.root = root;
    loaded.saved = saved;
    loaded.root_len = root_len;
    loaded.saved_len = saved != 0U ? anchor_len : root_len;
    loaded.saved_hash = saved != 0U ? anchor_hash : 0U;
    old = ec->undo;
    loaded.bytes_max = old->bytes_max;
    loaded.min_nodes = old->min_nodes;
    loaded.persist_bytes_max = old->persist_bytes_max;
    loaded.mono_clock = old->mono_clock;
    loaded.wall_clock = old->wall_clock;
    loaded.clock_ctx = old->clock_ctx;
    materialize_insert_payloads(ec, &loaded);
    retain_delete_payloads(&loaded);
    {
        EditCtx identity = *ec;

        identity.undo = &loaded;
        state_identity_at(&identity, root, &root_len, &root_hash);
    }
    if (root_len != loaded.root_len)
        YEW_BUG("undo read: validated root length changed");
    loaded.root_hash = root_hash;
    if (saved == 0U)
        loaded.saved_hash = root_hash;
    account_live(&loaded);
    dispose_loaded_tree(old);
    *old = loaded;
    ec->undo = old;
    bytebuf_free(&file);
    return outcome;

dropped:
    dispose_loaded_tree(&loaded);
    bytebuf_free(&file);
    yew_log(YEW_LOG_WARN, "undo: dropped invalid or stale .yewu");
    return YEW_UNDO_READ_DROPPED;
}
