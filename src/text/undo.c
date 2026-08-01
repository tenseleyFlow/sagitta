#define _POSIX_C_SOURCE 200809L

#include "text/undo.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "text/edit.h"
#include "text/journal.h"
#include "util/log.h"

enum { SAGU_VERSION = 1U, SAGU_HEADER_LEN = 64U, SAGU_TRUNCATED = 1U };

VEC_DECL(SagU32Vec, u32);

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
    return node != NULL && (node->flags & SAG_TXN_DEAD) == 0U;
}

static u64 default_mono(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        SAG_BUG("undo: monotonic clock failed");
    return (u64)ts.tv_sec * 1000U + (u64)ts.tv_nsec / 1000000U;
}

static i64 default_wall(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        SAG_BUG("undo: realtime clock failed");
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

    if (!sag_textiter_begin(&it, tb, BYTEOFF(0U)))
        return hash;
    do {
        const u8 *bytes;
        u64 len;
        if (!sag_textiter_chunk(&it, tb, &bytes, &len))
            SAG_BUG("undo: text iterator failed");
        hash = fnv_add(hash, bytes, (size_t)len);
    } while (sag_textiter_advance(&it, tb));
    return hash;
}

static void require_ctx(const EditCtx *ec)
{
    if (ec == NULL || ec->tb == NULL || ec->undo == NULL)
        SAG_BUG("undo: NULL edit context");
}

static void snapshot_cursors(UndoTree *ut, const CursorSet *cs,
                             u32 *at, u32 *count)
{
    size_t i;

    *at = (u32)ut->cursors.len;
    *count = 0U;
    if (cs == NULL)
        return;
    sag_cset_check(cs);
    for (i = 0U; i < cs->curs.len; i++) {
        size_t index = i == 0U ? (size_t)cs->primary
                               : (i <= (size_t)cs->primary ? i - 1U : i);
        const Cursor *cursor = &cs->curs.data[index];
        CursorRec rec;
        rec.pos = cursor->pos.v;
        rec.anchor = cursor->anchor.v;
        rec.goal = cursor->goal_col.v;
        SagCursorRecVec_push(&ut->cursors, rec);
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
        SAG_BUG("undo: corrupt cursor slice");
    SagCursorVec_reserve(&cs->curs, count);
    cs->curs.len = count;
    cs->primary = 0U;
    for (i = 0U; i < count; i++) {
        const CursorRec *rec = &ec->undo->cursors.data[at + i];
        cs->curs.data[i].pos = BYTEOFF(rec->pos);
        cs->curs.data[i].anchor = BYTEOFF(rec->anchor);
        cs->curs.data[i].goal_col = (GCol){rec->goal};
    }
    sag_cset_normalize(ec->tb, cs);
}

UndoTree *sag_undo_new(const TextBuf *tb)
{
    UndoTree *ut;
    UndoNode root;

    if (tb == NULL)
        SAG_BUG("sag_undo_new: NULL buffer");
    ut = sag_xcalloc(1U, sizeof(*ut));
    bytebuf_init(&ut->blobs);
    ut->root = 1U;
    ut->cur = 1U;
    ut->saved = 1U;
    ut->bytes_max = SAG_UNDO_BYTES_MAX;
    ut->persist_bytes_max = SAG_UNDO_PERSIST_BYTES_MAX;
    ut->min_nodes = SAG_UNDO_MIN_NODES;
    ut->pending_reason = SAG_TXN_REASON_MAX;
    ut->boundary = true;
    ut->mono_clock = default_mono;
    ut->wall_clock = default_wall;
    ut->root_len = sag_textbuf_len(tb);
    ut->root_hash = text_hash(tb);
    (void)memset(&root, 0, sizeof(root));
    root.id = 1U;
    root.flags = SAG_TXN_SAVED;
    root.t_wall = ut->wall_clock(ut->clock_ctx);
    root.t_last_ms = ut->mono_clock(ut->clock_ctx);
    SagUndoNodeVec_push(&ut->nodes, root);
    ut->bytes_live = sizeof(root);
    return ut;
}

void sag_undo_free(UndoTree *ut)
{
    if (ut == NULL)
        return;
    if (ut->depth != 0U || ut->open != 0U)
        SAG_BUG("undo: free with an open transaction");
    SagUndoNodeVec_free(&ut->nodes);
    SagUndoOpVec_free(&ut->ops);
    SagCursorRecVec_free(&ut->cursors);
    SagMarkRepairVec_free(&ut->repairs);
    SagUndoRepairRunVec_free(&ut->repair_runs);
    SagUndoReplaySpanVec_free(&ut->replay_spans);
    bytebuf_free(&ut->blobs);
    free(ut);
}

void sag_undo_set_clock(UndoTree *ut, SagUndoMonoClock mono_clock,
                        SagUndoWallClock wall_clock, void *ctx)
{
    if (ut == NULL)
        SAG_BUG("sag_undo_set_clock: NULL tree");
    ut->mono_clock = mono_clock != NULL ? mono_clock : default_mono;
    ut->wall_clock = wall_clock != NULL ? wall_clock : default_wall;
    ut->clock_ctx = ctx;
}

void sag_undo_set_limits(UndoTree *ut, u64 bytes_max, u32 min_nodes,
                         u64 persist_bytes_max)
{
    if (ut == NULL)
        SAG_BUG("sag_undo_set_limits: NULL tree");
    ut->bytes_max = bytes_max;
    ut->min_nodes = min_nodes;
    ut->persist_bytes_max = persist_bytes_max;
}

static void require_reason(EditCtx *ec, SagTxnReason reason)
{
    if (reason >= SAG_TXN_REASON_MAX)
        SAG_BUG("undo: invalid transaction reason");
    if (reason == SAG_TXN_MULTI)
        SAG_BUG("multi-cursor transactions land in Sprint 17");
    if (reason == SAG_TXN_FILTER)
        SAG_BUG("filter transactions land in Sprint 19");
    if (reason == SAG_TXN_REPLACE)
        SAG_BUG("replace transactions land in Sprint 21");
    if (reason == SAG_TXN_MACRO)
        SAG_BUG("macro transactions land in Sprint 34");
    if (reason == SAG_TXN_LSP)
        SAG_BUG("LSP transactions land in Sprint 47");
    if (ec->cset != NULL)
        sag_cset_require_single_edit(ec->cset);
}

void sag_undo_begin(EditCtx *ec, SagTxnReason why)
{
    UndoTree *ut;
    require_ctx(ec);
    require_reason(ec, why);
    ut = ec->undo;
    if (ut->depth == 0U) {
        ut->pending_reason = why;
        ut->boundary = true;
    } else if (ut->pending_reason != why) {
        SAG_BUG("undo: nested transaction reason mismatch");
    }
    if (ut->depth == UINT32_MAX)
        SAG_BUG("undo: transaction nesting overflow");
    ut->depth++;
}

void sag_undo_boundary(UndoTree *ut)
{
    if (ut == NULL)
        SAG_BUG("sag_undo_boundary: NULL tree");
    ut->boundary = true;
}

static bool reason_mergeable(SagTxnReason reason)
{
    return reason == SAG_TXN_TYPE || reason == SAG_TXN_ERASE;
}

static bool op_contiguous(SagTxnReason reason, const UndoOp *prev,
                          u8 kind, u64 off, u64 len)
{
    if (reason == SAG_TXN_TYPE && kind == SAG_OP_INS &&
        prev->kind == SAG_OP_INS)
        return prev->off <= UINT64_MAX - prev->len &&
               off == prev->off + prev->len;
    if (reason == SAG_TXN_ERASE && kind == SAG_OP_DEL &&
        prev->kind == SAG_OP_DEL)
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

static bool may_merge(UndoTree *ut, SagTxnReason reason, u8 kind,
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
        now - node->t_last_ms >= SAG_UNDO_BURST_MS)
        return false;
    prev = &ut->ops.data[node->ops_at + node->n_ops - 1U];
    return op_contiguous(reason, prev, kind, off, len) &&
           node_payload_bytes(ut, node) < SAG_UNDO_BURST_BYTES;
}

static void link_child(UndoTree *ut, UndoNode *node)
{
    UndoNode *parent = node_mut(ut, node->parent);
    u32 *link;
    if (parent == NULL)
        SAG_BUG("undo: missing parent");
    link = &parent->first_child;
    while (*link != 0U)
        link = &node_mut(ut, *link)->next_sibling;
    *link = node->id;
    parent->redo_child = node->id;
}

static UndoNode *open_node(EditCtx *ec, SagTxnReason reason, u8 kind,
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
        SAG_BUG("undo: invalid current node");
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
        sag_log(SAG_LOG_WARN, "undo: wall clock stepped backwards; clamped");
        wall = ut->nodes.data[ut->nodes.len - 1U].t_wall;
    }
    node.t_wall = wall;
    snapshot_cursors(ut, ec->cset, &node.cur_before, &node.n_before);
    SagUndoNodeVec_push(&ut->nodes, node);
    link_child(ut, &ut->nodes.data[ut->nodes.len - 1U]);
    ut->open = node.id;
    ut->boundary = false;
    ut->bytes_live += sizeof(node) +
                      (u64)node.n_before * sizeof(CursorRec);
    return node_mut(ut, node.id);
}

static SagTxnReason edit_reason(const UndoTree *ut, u8 kind)
{
    if (ut->depth != 0U)
        return ut->pending_reason;
    return kind == SAG_OP_INS ? SAG_TXN_TYPE : SAG_TXN_ERASE;
}

static void capture_delete_bytes(UndoTree *ut, const TextBuf *tb, Span range)
{
    TextIter it;
    u64 left = range.hi - range.lo;
    if (left == 0U)
        return;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        SAG_BUG("undo: cannot capture delete bytes");
    while (left != 0U) {
        const u8 *bytes;
        u64 len;
        u64 take;
        if (!sag_textiter_chunk(&it, tb, &bytes, &len))
            SAG_BUG("undo: delete capture truncated");
        take = len < left ? len : left;
        bytebuf_append(&ut->blobs, bytes, (size_t)take);
        left -= take;
        if (left != 0U && !sag_textiter_advance(&it, tb))
            SAG_BUG("undo: delete capture advance failed");
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
    SagMarkRepairVec_push(&capture->ut->repairs, repair);
    (void)capture->base;
}

void sag_undo_prepare_insert(EditCtx *ec, ByteOff at, u64 len)
{
    UndoTree *ut;
    require_ctx(ec);
    ut = ec->undo;
    (void)open_node(ec, edit_reason(ut, SAG_OP_INS), SAG_OP_INS, at.v, len);
}

void sag_undo_prepare_delete(EditCtx *ec, Span range)
{
    UndoTree *ut;
    UndoNode *node;
    RepairCapture capture;

    require_ctx(ec);
    ut = ec->undo;
    node = open_node(ec, edit_reason(ut, SAG_OP_DEL), SAG_OP_DEL, range.lo,
                     range.hi - range.lo);
    capture_delete_bytes(ut, ec->tb, range);
    node = node_mut(ut, node->id);
    node->blob_hi = ut->blobs.len;
    capture.ut = ut;
    capture.base = range.lo;
    if (ec->marks != NULL)
        sag_marks_observe_collapse(ec->marks, range, capture_repair, &capture);
}

static void finish_record(EditCtx *ec, UndoNode *node)
{
    UndoTree *ut = ec->undo;
    if (node->n_after != 0U &&
        (size_t)node->cur_after + node->n_after == ut->cursors.len) {
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
            sag_journal_sync(ec->jrnl);
    }
}

void sag_undo_record_insert(EditCtx *ec, ByteOff at, u64 len, u64 payload)
{
    UndoTree *ut;
    UndoNode *node;
    UndoOp op;
    UndoRepairRun run = {0U, 0U};
    UndoReplaySpan replay;

    require_ctx(ec);
    ut = ec->undo;
    node = node_mut(ut, ut->open);
    if (node == NULL)
        SAG_BUG("undo: insert recorded without prepare");
    op.kind = SAG_OP_INS;
    op.src = SAG_STORE_ADD;
    op.off = at.v;
    op.len = len;
    op.payload = payload;
    (void)memset(&replay, 0, sizeof(replay));
    SagUndoOpVec_push(&ut->ops, op);
    SagUndoRepairRunVec_push(&ut->repair_runs, run);
    SagUndoReplaySpanVec_push(&ut->replay_spans, replay);
    node = node_mut(ut, ut->open);
    node->n_ops++;
    finish_record(ec, node);
}

void sag_undo_record_delete(EditCtx *ec, Span range)
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
        SAG_BUG("undo: delete recorded without prepare");
    op.kind = SAG_OP_DEL;
    op.src = 0U;
    op.off = range.lo;
    op.len = range.hi - range.lo;
    op.payload = node->blob_hi - op.len;
    run.at = node->rep_at + node->n_rep;
    run.len = (u32)ut->repairs.len - run.at;
    node->n_rep += run.len;
    (void)memset(&replay, 0, sizeof(replay));
    SagUndoOpVec_push(&ut->ops, op);
    SagUndoRepairRunVec_push(&ut->repair_runs, run);
    SagUndoReplaySpanVec_push(&ut->replay_spans, replay);
    node = node_mut(ut, ut->open);
    node->n_ops++;
    ut->bytes_live += op.len + (u64)run.len * sizeof(MarkRepair);
    finish_record(ec, node);
}

static const u8 *store_bytes(const TextBuf *tb, u8 src, Span span)
{
    const TextStore *store;
    store = src == SAG_STORE_ORIG ? &tb->orig : &tb->add;
    if ((src != SAG_STORE_ORIG && src != SAG_STORE_ADD) ||
        span.lo > span.hi || span.hi > store->len)
        SAG_BUG("undo: invalid store span");
    return store->bytes + (size_t)span.lo;
}

static u8 *copy_live_range(const TextBuf *tb, Span range)
{
    TextIter it;
    u64 done = 0U;
    u64 len = range.hi - range.lo;
    u8 *bytes = sag_xmalloc(len == 0U ? 1U : (size_t)len);

    if (len == 0U)
        return bytes;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(range.lo)))
        SAG_BUG("undo replay: invalid delete range");
    while (done < len) {
        const u8 *chunk;
        u64 avail;
        u64 take;
        if (!sag_textiter_chunk(&it, tb, &chunk, &avail))
            SAG_BUG("undo replay: delete iterator failed");
        take = avail < len - done ? avail : len - done;
        (void)memcpy(bytes + (size_t)done, chunk, (size_t)take);
        done += take;
        if (done < len && !sag_textiter_advance(&it, tb))
            SAG_BUG("undo replay: delete iterator truncated");
    }
    return bytes;
}

static void replay_insert_span(EditCtx *ec, ByteOff at, u8 src, Span span)
{
    u64 len = span.hi - span.lo;
    const u8 *bytes = store_bytes(ec->tb, src, span);

    sag_textbuf_insert_span(ec->tb, at, src, span);
    if (ec->marks != NULL)
        sag_marks_adjust(ec->marks, SAG_JOURNAL_INS, at, len);
    if (ec->cset != NULL)
        sag_cset_adjust(ec->cset, SAG_JOURNAL_INS, at, len);
    if (ec->jrnl != NULL)
        sag_journal_record(ec->jrnl, SAG_JOURNAL_INS, at.v, bytes, len);
}

static void replay_insert_blob(EditCtx *ec, u32 op_index, const UndoOp *op)
{
    UndoTree *ut = ec->undo;
    UndoReplaySpan *cache = &ut->replay_spans.data[op_index];

    if (cache->valid) {
        replay_insert_span(ec, BYTEOFF(op->off), cache->src, cache->span);
    } else {
        u64 payload = ec->tb->add.len;
        const u8 *bytes;
        if (op->payload > ut->blobs.len || op->len > ut->blobs.len - op->payload)
            SAG_BUG("undo: corrupt delete payload");
        bytes = ut->blobs.data + (size_t)op->payload;
        sag_textbuf_insert(ec->tb, BYTEOFF(op->off), bytes, op->len);
        if (ec->marks != NULL)
            sag_marks_adjust(ec->marks, SAG_JOURNAL_INS, BYTEOFF(op->off),
                             op->len);
        if (ec->cset != NULL)
            sag_cset_adjust(ec->cset, SAG_JOURNAL_INS, BYTEOFF(op->off),
                            op->len);
        if (ec->jrnl != NULL)
            sag_journal_record(ec->jrnl, SAG_JOURNAL_INS, op->off, bytes,
                               op->len);
        cache->src = SAG_STORE_ADD;
        cache->span = (Span){payload, payload + op->len};
        cache->valid = true;
    }
}

static void replay_delete(EditCtx *ec, const UndoOp *op)
{
    Span range = {op->off, op->off + op->len};
    u8 *bytes;

    if (op->off > sag_textbuf_len(ec->tb) ||
        op->len > sag_textbuf_len(ec->tb) - op->off)
        SAG_BUG("undo replay: delete out of bounds");
    bytes = copy_live_range(ec->tb, range);
    sag_textbuf_delete(ec->tb, range);
    if (ec->marks != NULL)
        sag_marks_adjust(ec->marks, SAG_JOURNAL_DEL, BYTEOFF(op->off), op->len);
    if (ec->cset != NULL)
        sag_cset_adjust(ec->cset, SAG_JOURNAL_DEL, BYTEOFF(op->off), op->len);
    if (ec->jrnl != NULL)
        sag_journal_record(ec->jrnl, SAG_JOURNAL_DEL, op->off, bytes, op->len);
    free(bytes);
}

static void apply_inverse(EditCtx *ec, const UndoNode *node)
{
    UndoTree *ut = ec->undo;
    u32 i = node->n_ops;

    while (i != 0U) {
        u32 index = node->ops_at + --i;
        const UndoOp *op = &ut->ops.data[index];
        if (op->kind == SAG_OP_INS) {
            replay_delete(ec, op);
        } else if (op->kind == SAG_OP_DEL) {
            const UndoRepairRun *run;
            u32 r;
            replay_insert_blob(ec, index, op);
            run = &ut->repair_runs.data[index];
            for (r = 0U; r < run->len; r++) {
                const MarkRepair *repair = &ut->repairs.data[run->at + r];
                if (ec->marks != NULL) {
                    (void)sag_mark_repair(
                        ec->marks,
                        (MarkId){repair->mark_id, repair->mark_gen},
                        BYTEOFF(op->off + repair->rel_off));
                }
            }
        } else {
            SAG_BUG("undo: invalid operation kind");
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
        if (op->kind == SAG_OP_INS) {
            replay_insert_span(ec, BYTEOFF(op->off), op->src,
                               (Span){op->payload, op->payload + op->len});
        } else if (op->kind == SAG_OP_DEL) {
            replay_delete(ec, op);
        } else {
            SAG_BUG("redo: invalid operation kind");
        }
    }
}

static void sync_navigation(EditCtx *ec)
{
    if (ec->jrnl != NULL)
        sag_journal_sync(ec->jrnl);
}

void sag_undo_end(EditCtx *ec)
{
    UndoTree *ut;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth == 0U)
        SAG_BUG("undo: unbalanced end");
    ut->depth--;
    if (ut->depth != 0U)
        return;
    ut->pending_reason = SAG_TXN_REASON_MAX;
    ut->open = 0U;
    ut->boundary = true;
    sync_navigation(ec);
}

static void unlink_child(UndoTree *ut, u32 parent_id, u32 child_id)
{
    UndoNode *parent = node_mut(ut, parent_id);
    u32 *link;
    if (parent == NULL)
        SAG_BUG("undo: abort parent missing");
    link = &parent->first_child;
    while (*link != 0U && *link != child_id)
        link = &node_mut(ut, *link)->next_sibling;
    if (*link == child_id)
        *link = node_mut(ut, child_id)->next_sibling;
    if (parent->redo_child == child_id)
        parent->redo_child = parent->first_child;
}

void sag_undo_abort(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *node;
    u32 parent;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth == 0U)
        SAG_BUG("undo: abort outside transaction");
    if (ut->open == 0U) {
        ut->depth = 0U;
        ut->pending_reason = SAG_TXN_REASON_MAX;
        ut->boundary = true;
        return;
    }
    node = node_mut(ut, ut->open);
    parent = node->parent;
    apply_inverse(ec, node);
    restore_cursors(ec, node->cur_before, node->n_before);
    unlink_child(ut, parent, node->id);
    node->flags |= SAG_TXN_DEAD;
    ut->bytes_dead += node->blob_hi - node->blob_lo;
    ut->cur = parent;
    ut->open = 0U;
    ut->depth = 0U;
    ut->pending_reason = SAG_TXN_REASON_MAX;
    ut->boundary = true;
    sync_navigation(ec);
}

bool sag_undo_reopen(EditCtx *ec, SagTxnReason expect)
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
        node->first_child != 0U)
        return false;
    ut->open = node->id;
    ut->depth = 1U;
    ut->pending_reason = expect;
    ut->boundary = false;
    return true;
}

bool sag_undo(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *node;
    UndoNode *parent;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth != 0U)
        SAG_BUG("undo navigation inside transaction");
    if (ut->cur == ut->root)
        return false;
    node = node_mut(ut, ut->cur);
    parent = node_mut(ut, node->parent);
    if (parent == NULL)
        SAG_BUG("undo: broken parent chain");
    apply_inverse(ec, node);
    restore_cursors(ec, node->cur_before, node->n_before);
    parent->redo_child = node->id;
    ut->cur = parent->id;
    ut->boundary = true;
    sync_navigation(ec);
    return true;
}

bool sag_redo(EditCtx *ec)
{
    UndoTree *ut;
    UndoNode *parent;
    UndoNode *child;
    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth != 0U)
        SAG_BUG("redo navigation inside transaction");
    parent = node_mut(ut, ut->cur);
    if (parent == NULL || !node_live(ut, parent->redo_child))
        return false;
    child = node_mut(ut, parent->redo_child);
    apply_forward(ec, child);
    restore_cursors(ec, child->cur_after, child->n_after);
    parent->redo_child = child->id;
    ut->cur = child->id;
    ut->boundary = true;
    sync_navigation(ec);
    return true;
}

bool sag_undo_to(EditCtx *ec, u32 target_id)
{
    UndoTree *ut;
    u32 from;
    u32 target;
    SagU32Vec path = {0};

    require_ctx(ec);
    ut = ec->undo;
    if (ut->depth != 0U || !node_live(ut, target_id))
        return false;
    from = ut->cur;
    target = target_id;
    while (node_get(ut, from)->depth > node_get(ut, target)->depth) {
        if (!sag_undo(ec))
            SAG_BUG("undo_to: failed ascent");
        from = ut->cur;
    }
    while (node_get(ut, target)->depth > node_get(ut, from)->depth) {
        SagU32Vec_push(&path, target);
        target = node_get(ut, target)->parent;
    }
    while (from != target) {
        SagU32Vec_push(&path, target);
        target = node_get(ut, target)->parent;
        if (!sag_undo(ec))
            SAG_BUG("undo_to: failed LCA ascent");
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
    SagU32Vec_free(&path);
    ut->boundary = true;
    sync_navigation(ec);
    return true;
}

bool sag_undo_state(EditCtx *ec, i64 delta)
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
    return sag_undo_to(ec, id);
}

bool sag_undo_time(EditCtx *ec, i64 wall_secs)
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
    return best != 0U && sag_undo_to(ec, best);
}

u32 sag_undo_branch_cycle(UndoTree *ut, i32 delta)
{
    UndoNode *node;
    SagU32Vec children = {0};
    u32 child;
    size_t index = 0U;
    size_t i;

    if (ut == NULL)
        SAG_BUG("sag_undo_branch_cycle: NULL tree");
    node = node_mut(ut, ut->cur);
    if (node == NULL)
        return 0U;
    for (child = node->first_child; child != 0U;
         child = node_get(ut, child)->next_sibling) {
        if (node_live(ut, child))
            SagU32Vec_push(&children, child);
    }
    if (children.len == 0U) {
        SagU32Vec_free(&children);
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
    SagU32Vec_free(&children);
    return child;
}

u32 sag_undo_current(const UndoTree *ut)
{
    return ut == NULL ? 0U : ut->cur;
}

bool sag_undo_at_save_point(const UndoTree *ut)
{
    return ut != NULL && ut->saved != 0U && ut->cur == ut->saved;
}

void sag_undo_mark_saved(UndoTree *ut)
{
    UndoNode *old;
    UndoNode *node;
    if (ut == NULL)
        SAG_BUG("sag_undo_mark_saved: NULL tree");
    old = node_mut(ut, ut->saved);
    if (old != NULL)
        old->flags &= (u8)~SAG_TXN_SAVED;
    ut->saved = ut->cur;
    node = node_mut(ut, ut->saved);
    if (node != NULL)
        node->flags |= SAG_TXN_SAVED;
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

u32 sag_undo_list(const UndoTree *ut, UndoNodeInfo *out, u32 max)
{
    u32 total = 0U;
    u32 id;
    if (ut == NULL)
        SAG_BUG("sag_undo_list: NULL tree");
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
        info.is_trimmed = (node->flags & SAG_TXN_TRIMMED) != 0U;
        for (child = node->first_child; child != 0U;
             child = node_get(ut, child)->next_sibling) {
            if (node_live(ut, child))
                info.n_children++;
        }
        for (i = 0U; i < node->n_ops; i++) {
            const UndoOp *op = &ut->ops.data[node->ops_at + i];
            if (op->kind == SAG_OP_INS)
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

u32 sag_undo_children(const UndoTree *ut, u32 id, u32 *out, u32 max)
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
    static const char *const names[SAG_TXN_REASON_MAX] = {
        "typed", "erased", "pasted", "cut", "multi", "macro",
        "filtered", "replaced", "lsp", "external"
    };
    return reason < SAG_TXN_REASON_MAX ? names[reason] : "unknown";
}

void sag_undo_describe(const UndoTree *ut, u32 id, i64 now, char *buf,
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
        if (op->kind == SAG_OP_INS)
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

void sag_undo_dump(const UndoTree *ut, FILE *out)
{
    u32 id;
    if (ut == NULL || out == NULL)
        SAG_BUG("sag_undo_dump: NULL argument");
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
