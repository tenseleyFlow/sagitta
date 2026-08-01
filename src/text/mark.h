#ifndef SAG_TEXT_MARK_H
#define SAG_TEXT_MARK_H

#include "text/coords.h"
#include "util/base.h"

typedef struct {
    u32 id;
    u32 gen;
} MarkId;

typedef enum {
    SAG_BIAS_LEFT,
    SAG_BIAS_RIGHT
} MarkBias;

typedef struct Mark {
    ByteOff pos;
    MarkBias bias;
    bool alive;
} Mark;

typedef struct MarkSet MarkSet;

typedef void (*SagMarkCollapseFn)(void *ctx, MarkId id, u64 rel_off);

MarkSet *sag_marks_new(void);
void sag_marks_free(MarkSet *ms);

MarkId sag_mark_add(MarkSet *ms, ByteOff pos, MarkBias bias);
void sag_mark_del(MarkSet *ms, MarkId id);
ByteOff sag_mark_pos(const MarkSet *ms, MarkId id);

/* Visits marks whose original positions an inverse insert cannot infer. */
void sag_marks_observe_collapse(const MarkSet *ms, Span range,
                                SagMarkCollapseFn observe, void *ctx);
/* Returns false when id is dead, stale, or names a reused slab slot. */
bool sag_mark_repair(MarkSet *ms, MarkId id, ByteOff pos);

/* op is SAG_JOURNAL_INS or SAG_JOURNAL_DEL; at is a pre-edit offset. */
void sag_marks_adjust(MarkSet *ms, u8 op, ByteOff at, u64 len);

#endif
