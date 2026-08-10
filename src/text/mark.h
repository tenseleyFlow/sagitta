#ifndef YEW_TEXT_MARK_H
#define YEW_TEXT_MARK_H

#include "text/coords.h"
#include "util/base.h"

typedef struct {
    u32 id;
    u32 gen;
} MarkId;

typedef enum {
    YEW_BIAS_LEFT,
    YEW_BIAS_RIGHT
} MarkBias;

typedef struct Mark {
    ByteOff pos;
    MarkBias bias;
    bool alive;
} Mark;

typedef struct MarkSet MarkSet;

typedef void (*YewMarkCollapseFn)(void *ctx, MarkId id, u64 rel_off);

MarkSet *yew_marks_new(void);
void yew_marks_free(MarkSet *ms);

MarkId yew_mark_add(MarkSet *ms, ByteOff pos, MarkBias bias);
void yew_mark_del(MarkSet *ms, MarkId id);
ByteOff yew_mark_pos(const MarkSet *ms, MarkId id);

/* Visits marks whose original positions an inverse insert cannot infer. */
void yew_marks_observe_collapse(const MarkSet *ms, Span range,
                                YewMarkCollapseFn observe, void *ctx);
/* Returns false when id is dead, stale, or names a reused slab slot. */
bool yew_mark_repair(MarkSet *ms, MarkId id, ByteOff pos);
/*
 * Non-fatal liveness test.  yew_mark_pos treats a dead handle as a bug
 * and aborts, which is right for code that owns its marks; Sprint 21's
 * jumplist holds handles it does NOT own — a position the user visited
 * ten jumps ago may have been deleted since — and has to skip those
 * entries rather than die on them.
 */
bool yew_mark_alive(const MarkSet *ms, MarkId id);

/* op is YEW_JOURNAL_INS or YEW_JOURNAL_DEL; at is a pre-edit offset. */
void yew_marks_adjust(MarkSet *ms, u8 op, ByteOff at, u64 len);

#endif
