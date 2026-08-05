#include "text/mark.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "text/journal.h"
#include "util/log.h"

#define SAG_MARK_NO_SLOT UINT32_MAX

typedef struct {
    Mark mark;
    u32 gen;
    u32 next_free;
} MarkSlot;

struct MarkSet {
    MarkSlot *slots;
    u32 slots_len;
    u32 slots_cap;
    u32 free_head;
    u32 *order;
    u32 order_len;
    u32 order_cap;
    u32 *scratch;
    u32 scratch_cap;
};

static void require_set(const MarkSet *ms, const char *where)
{
    if (ms == NULL)
        SAG_BUG("%s: NULL mark set", where);
}

static void reserve_slots(MarkSet *ms, u32 need)
{
    u32 cap;

    if (ms->slots_cap >= need)
        return;
    cap = ms->slots_cap != 0U ? ms->slots_cap : 8U;
    while (cap < need) {
        if (cap > UINT32_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    ms->slots = sag_xreallocarray(ms->slots, cap, sizeof(*ms->slots));
    ms->slots_cap = cap;
}

static void reserve_order(MarkSet *ms, u32 need)
{
    u32 cap;

    if (ms->order_cap >= need)
        return;
    cap = ms->order_cap != 0U ? ms->order_cap : 8U;
    while (cap < need) {
        if (cap > UINT32_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    ms->order = sag_xreallocarray(ms->order, cap, sizeof(*ms->order));
    ms->order_cap = cap;
}

static void reserve_scratch(MarkSet *ms, u32 need)
{
    if (ms->scratch_cap >= need)
        return;
    ms->scratch = sag_xreallocarray(ms->scratch, need, sizeof(*ms->scratch));
    ms->scratch_cap = need;
}

static u32 lower_bound_pos(const MarkSet *ms, u64 pos)
{
    u32 lo = 0U;
    u32 hi = ms->order_len;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        u64 value = ms->slots[ms->order[mid]].mark.pos.v;

        if (value < pos)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static u32 upper_bound_pos(const MarkSet *ms, u64 pos)
{
    u32 lo = 0U;
    u32 hi = ms->order_len;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        u64 value = ms->slots[ms->order[mid]].mark.pos.v;

        if (value <= pos)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static const MarkSlot *require_live_const(const MarkSet *ms, MarkId id,
                                         const char *where)
{
    const MarkSlot *slot;

    require_set(ms, where);
    if (id.id >= ms->slots_len)
        SAG_BUG("%s: dead mark handle %u:%u", where, (unsigned)id.id,
                (unsigned)id.gen);
    slot = &ms->slots[id.id];
    if (!slot->mark.alive || slot->gen != id.gen)
        SAG_BUG("%s: dead mark handle %u:%u", where, (unsigned)id.id,
                (unsigned)id.gen);
    return slot;
}

static MarkSlot *require_live(MarkSet *ms, MarkId id, const char *where)
{
    (void)require_live_const(ms, id, where);
    return &ms->slots[id.id];
}

MarkSet *sag_marks_new(void)
{
    MarkSet *ms = sag_xcalloc(1U, sizeof(*ms));

    ms->free_head = SAG_MARK_NO_SLOT;
    return ms;
}

void sag_marks_free(MarkSet *ms)
{
    if (ms == NULL)
        return;
    free(ms->scratch);
    free(ms->order);
    free(ms->slots);
    free(ms);
}

MarkId sag_mark_add(MarkSet *ms, ByteOff pos, MarkBias bias)
{
    MarkSlot *slot;
    MarkId result;
    u32 at;

    require_set(ms, "sag_mark_add");
    if (bias != SAG_BIAS_LEFT && bias != SAG_BIAS_RIGHT)
        SAG_BUG("sag_mark_add: invalid bias %u", (unsigned)bias);
    if (ms->order_len == UINT32_MAX)
        SAG_BUG("sag_mark_add: mark set is full");
    if (ms->free_head != SAG_MARK_NO_SLOT) {
        result.id = ms->free_head;
        slot = &ms->slots[result.id];
        ms->free_head = slot->next_free;
    } else {
        if (ms->slots_len == UINT32_MAX)
            SAG_BUG("sag_mark_add: mark slab is full");
        reserve_slots(ms, ms->slots_len + 1U);
        result.id = ms->slots_len++;
        slot = &ms->slots[result.id];
        (void)memset(slot, 0, sizeof(*slot));
        slot->gen = 1U;
    }
    slot->mark.pos = pos;
    slot->mark.bias = bias;
    slot->mark.alive = true;
    slot->next_free = SAG_MARK_NO_SLOT;
    result.gen = slot->gen;

    reserve_order(ms, ms->order_len + 1U);
    at = upper_bound_pos(ms, pos.v);
    (void)memmove(&ms->order[at + 1U], &ms->order[at],
                  (size_t)(ms->order_len - at) * sizeof(*ms->order));
    ms->order[at] = result.id;
    ms->order_len++;
    return result;
}

void sag_mark_del(MarkSet *ms, MarkId id)
{
    MarkSlot *slot = require_live(ms, id, "sag_mark_del");
    u32 i;

    for (i = 0U; i < ms->order_len && ms->order[i] != id.id; i++) {
    }
    if (i == ms->order_len)
        SAG_BUG("sag_mark_del: live mark missing from ordered index");
    (void)memmove(&ms->order[i], &ms->order[i + 1U],
                  (size_t)(ms->order_len - i - 1U) * sizeof(*ms->order));
    ms->order_len--;
    slot->mark.alive = false;
    if (slot->gen != UINT32_MAX) {
        slot->gen++;
        slot->next_free = ms->free_head;
        ms->free_head = id.id;
    } else {
        slot->next_free = SAG_MARK_NO_SLOT;
    }
}

ByteOff sag_mark_pos(const MarkSet *ms, MarkId id)
{
    const MarkSlot *slot = require_live_const(ms, id, "sag_mark_pos");

    return slot->mark.pos;
}

void sag_marks_observe_collapse(const MarkSet *ms, Span range,
                                SagMarkCollapseFn observe, void *ctx)
{
    u32 i;

    require_set(ms, "sag_marks_observe_collapse");
    if (range.lo > range.hi)
        SAG_BUG("sag_marks_observe_collapse: invalid range [%llu,%llu)",
                (unsigned long long)range.lo,
                (unsigned long long)range.hi);
    if (observe == NULL)
        SAG_BUG("sag_marks_observe_collapse: NULL observer");
    i = lower_bound_pos(ms, range.lo);
    while (i < ms->order_len) {
        u32 slot_id = ms->order[i];
        const MarkSlot *slot = &ms->slots[slot_id];

        if (slot->mark.pos.v > range.hi)
            break;
        if (slot->mark.pos.v < range.hi ||
            slot->mark.bias == SAG_BIAS_LEFT) {
            observe(ctx, (MarkId){slot_id, slot->gen},
                    slot->mark.pos.v - range.lo);
        }
        i++;
    }
}

bool sag_mark_alive(const MarkSet *ms, MarkId id)
{
    const MarkSlot *slot;

    if (ms == NULL || id.id >= ms->slots_len)
        return false;
    slot = &ms->slots[id.id];
    return slot->mark.alive && slot->gen == id.gen;
}

bool sag_mark_repair(MarkSet *ms, MarkId id, ByteOff pos)
{
    MarkSlot *slot;
    u32 old_at;
    u32 new_at;

    require_set(ms, "sag_mark_repair");
    if (id.id >= ms->slots_len)
        return false;
    slot = &ms->slots[id.id];
    if (!slot->mark.alive || slot->gen != id.gen)
        return false;
    for (old_at = 0U;
         old_at < ms->order_len && ms->order[old_at] != id.id;
         old_at++) {
    }
    if (old_at == ms->order_len)
        SAG_BUG("sag_mark_repair: live mark missing from ordered index");
    (void)memmove(&ms->order[old_at], &ms->order[old_at + 1U],
                  (size_t)(ms->order_len - old_at - 1U) *
                      sizeof(*ms->order));
    ms->order_len--;
    slot->mark.pos = pos;
    new_at = upper_bound_pos(ms, pos.v);
    (void)memmove(&ms->order[new_at + 1U], &ms->order[new_at],
                  (size_t)(ms->order_len - new_at) * sizeof(*ms->order));
    ms->order[new_at] = id.id;
    ms->order_len++;
    return true;
}

static void adjust_insert(MarkSet *ms, u64 at, u64 len)
{
    u32 first = lower_bound_pos(ms, at);
    u32 equal_end = upper_bound_pos(ms, at);
    u32 out = 0U;
    u32 i;

    if (first != equal_end) {
        reserve_scratch(ms, equal_end - first);
        for (i = first; i < equal_end; i++) {
            u32 id = ms->order[i];

            if (ms->slots[id].mark.bias == SAG_BIAS_LEFT)
                ms->scratch[out++] = id;
        }
        for (i = first; i < equal_end; i++) {
            u32 id = ms->order[i];

            if (ms->slots[id].mark.bias == SAG_BIAS_RIGHT)
                ms->scratch[out++] = id;
        }
        (void)memcpy(&ms->order[first], ms->scratch,
                     (size_t)(equal_end - first) * sizeof(*ms->order));
    }
    for (i = first; i < ms->order_len; i++) {
        Mark *mark = &ms->slots[ms->order[i]].mark;
        bool shifts = mark->pos.v > at || mark->bias == SAG_BIAS_RIGHT;

        if (shifts && mark->pos.v > UINT64_MAX - len)
            SAG_BUG("sag_marks_adjust: insertion overflows mark position");
        if (shifts)
            mark->pos.v += len;
    }
}

static void adjust_delete(MarkSet *ms, u64 at, u64 len)
{
    u64 end;
    u32 i;

    if (len > UINT64_MAX - at)
        SAG_BUG("sag_marks_adjust: deletion range overflows");
    end = at + len;
    i = lower_bound_pos(ms, at);
    for (; i < ms->order_len; i++) {
        Mark *mark = &ms->slots[ms->order[i]].mark;

        if (mark->pos.v < end)
            mark->pos.v = at;
        else
            mark->pos.v -= len;
    }
}

void sag_marks_adjust(MarkSet *ms, u8 op, ByteOff at, u64 len)
{
    require_set(ms, "sag_marks_adjust");
    if (op != SAG_JOURNAL_INS && op != SAG_JOURNAL_DEL)
        SAG_BUG("sag_marks_adjust: invalid edit op %u", (unsigned)op);
    if (len == 0U)
        return;
    if (op == SAG_JOURNAL_INS)
        adjust_insert(ms, at.v, len);
    else
        adjust_delete(ms, at.v, len);
}
