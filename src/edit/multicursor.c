#include "edit/multicursor.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "edit/ed.h"
#include "text/journal.h"
#include "ui/message.h"
#include "unicode/grapheme.h"
#include "util/base.h"
#include "util/log.h"
#include "util/sort.h"

typedef struct CursorItem {
    Cursor cursor;
    u64 stamp;
    size_t order;
    size_t stack_index;
    bool primary;
    bool active;
} CursorItem;

static u64 cursor_lo(const Cursor *cursor)
{
    return cursor->pos.v < cursor->anchor.v ? cursor->pos.v : cursor->anchor.v;
}

static u64 cursor_hi(const Cursor *cursor)
{
    return cursor->pos.v > cursor->anchor.v ? cursor->pos.v : cursor->anchor.v;
}

static bool cursor_selected(const Cursor *cursor)
{
    return cursor->pos.v != cursor->anchor.v;
}

static int cmp_pos(const void *left, const void *right, void *context)
{
    const CursorItem *a = left;
    const CursorItem *b = right;

    (void)context;
    if (a->cursor.pos.v < b->cursor.pos.v)
        return -1;
    if (a->cursor.pos.v > b->cursor.pos.v)
        return 1;
    return 0;
}

static int cmp_span_start(const void *left, const void *right, void *context)
{
    const CursorItem *a = left;
    const CursorItem *b = right;
    u64 a_lo = cursor_lo(&a->cursor);
    u64 b_lo = cursor_lo(&b->cursor);

    (void)context;
    if (a_lo < b_lo)
        return -1;
    if (a_lo > b_lo)
        return 1;
    return 0;
}

static bool spans_merge(u64 lo, u64 hi, bool selected,
                        const Cursor *cursor)
{
    u64 other_lo = cursor_lo(cursor);
    u64 other_hi = cursor_hi(cursor);

    if (other_lo > hi || other_hi < lo)
        return false;
    if (selected || cursor_selected(cursor))
        return true;
    return lo == other_lo;
}

static Cursor merged_cursor(const CursorItem *governing, u64 lo, u64 hi)
{
    Cursor merged = governing->cursor;

    if (governing->cursor.pos.v < governing->cursor.anchor.v) {
        merged.pos = BYTEOFF(lo);
        merged.anchor = BYTEOFF(hi);
    } else {
        merged.pos = BYTEOFF(hi);
        merged.anchor = BYTEOFF(lo);
    }
    return merged;
}

static bool collapsed_strictly_sorted(const CursorSet *cs)
{
    size_t i;

    if (cs->curs.data[0].pos.v != cs->curs.data[0].anchor.v)
        return false;
    for (i = 1U; i < cs->curs.len; i++) {
        if (cs->curs.data[i].pos.v != cs->curs.data[i].anchor.v ||
            cs->curs.data[i - 1U].pos.v >= cs->curs.data[i].pos.v)
            return false;
    }
    return true;
}

static void normalize_unclamped(CursorSet *cs)
{
    CursorItem *items;
    CursorItem *groups;
    SelStack *group_stacks;
    size_t item_count;
    size_t group_count = 0U;
    size_t i;

    if (cs == NULL)
        SAG_BUG("cursor set: NULL set");
    if (cs->curs.len == 0U)
        SAG_BUG("cursor set: empty set");
    if ((size_t)cs->primary >= cs->curs.len)
        SAG_BUG("cursor set: primary index out of range");
    if (cs->stamps.len != cs->curs.len)
        SAG_BUG("cursor set: tracking length mismatch");
    if (cs->selstacks.len != cs->curs.len)
        SAG_BUG("cursor set: selection-stack length mismatch");
    if (cs->active != SAG_MC_ACTIVE_NONE &&
        (size_t)cs->active >= cs->curs.len)
        SAG_BUG("cursor set: active index out of range");

    /* The simultaneous-insert hot path preserves strict position order.
     * A collapsed, strictly ordered set has neither intervals nor duplicate
     * points to merge, so the stable-sort normalization would be a no-op. */
    if (collapsed_strictly_sorted(cs)) {
        sag_cset_check(cs);
        return;
    }

    item_count = cs->curs.len;
    items = sag_xreallocarray(NULL, item_count, sizeof(*items));
    groups = sag_xreallocarray(NULL, item_count, sizeof(*groups));
    group_stacks = sag_xreallocarray(NULL, item_count,
                                     sizeof(*group_stacks));
    for (i = 0U; i < item_count; i++) {
        items[i].cursor = cs->curs.data[i];
        items[i].stamp = cs->stamps.data[i];
        items[i].order = i;
        items[i].stack_index = i;
        items[i].primary = i == (size_t)cs->primary;
        items[i].active = i == (size_t)cs->active;
    }

    /* Position order defines "earlier cursor" and exact-point survivors. */
    sag_sort_stable(items, item_count, sizeof(*items), cmp_pos, NULL);
    for (i = 0U; i < item_count; i++)
        items[i].order = i;

    /* Interval order makes chained/nested overlap merging a linear sweep. */
    sag_sort_stable(items, item_count, sizeof(*items), cmp_span_start, NULL);
    i = 0U;
    while (i < item_count) {
        CursorItem governing = items[i];
        CursorItem point_survivor = items[i];
        u64 lo = cursor_lo(&items[i].cursor);
        u64 hi = cursor_hi(&items[i].cursor);
        bool selected = cursor_selected(&items[i].cursor);
        bool primary = items[i].primary;
        bool active = items[i].active;
        size_t j = i + 1U;

        while (j < item_count &&
               spans_merge(lo, hi, selected, &items[j].cursor)) {
            u64 next_lo = cursor_lo(&items[j].cursor);
            u64 next_hi = cursor_hi(&items[j].cursor);

            if (next_lo < lo)
                lo = next_lo;
            if (next_hi > hi)
                hi = next_hi;
            selected = selected || cursor_selected(&items[j].cursor);
            if (items[j].order < point_survivor.order)
                point_survivor = items[j];
            if (items[j].primary ||
                (!primary && items[j].order < governing.order)) {
                governing = items[j];
            }
            primary = primary || items[j].primary;
            active = active || items[j].active;
            j++;
        }
        if (!selected && !primary)
            governing = point_survivor;
        groups[group_count].cursor = merged_cursor(&governing, lo, hi);
        group_stacks[group_count] =
            cs->selstacks.data[governing.stack_index];
        groups[group_count].stamp = governing.stamp;
        groups[group_count].order = governing.order;
        groups[group_count].stack_index = group_count;
        groups[group_count].primary = primary;
        groups[group_count].active = active;
        group_count++;
        i = j;
    }

    sag_sort_stable(groups, group_count, sizeof(*groups), cmp_pos, NULL);
    cs->primary = 0U;
    cs->active = SAG_MC_ACTIVE_NONE;
    for (i = 0U; i < group_count; i++) {
        cs->curs.data[i] = groups[i].cursor;
        cs->stamps.data[i] = groups[i].stamp;
        cs->selstacks.data[i] = group_stacks[groups[i].stack_index];
        if (groups[i].primary)
            cs->primary = (u32)i;
        if (groups[i].active)
            cs->active = (u32)i;
    }
    cs->curs.len = group_count;
    cs->stamps.len = group_count;
    cs->selstacks.len = group_count;
    free(group_stacks);
    free(groups);
    free(items);
    sag_cset_check(cs);
}

void sag_cset_init(CursorSet *cs, Cursor primary)
{
    SelStack empty = {0};

    if (cs == NULL)
        SAG_BUG("sag_cset_init: NULL set");
    cs->curs.data = NULL;
    cs->curs.len = 0U;
    cs->curs.cap = 0U;
    cs->stamps.data = NULL;
    cs->stamps.len = 0U;
    cs->stamps.cap = 0U;
    cs->selstacks.data = NULL;
    cs->selstacks.len = 0U;
    cs->selstacks.cap = 0U;
    cs->primary = 0U;
    cs->active = SAG_MC_ACTIVE_NONE;
    cs->next_stamp = 2U;
    cs->batch_delta = 0;
    cs->batch_next = 0U;
    cs->batching = false;
    SagCursorVec_push(&cs->curs, primary);
    SagCursorStampVec_push(&cs->stamps, 1U);
    SagSelStackVec_push(&cs->selstacks, empty);
}

void sag_cset_free(CursorSet *cs)
{
    if (cs == NULL)
        return;
    SagCursorVec_free(&cs->curs);
    SagCursorStampVec_free(&cs->stamps);
    SagSelStackVec_free(&cs->selstacks);
    cs->primary = 0U;
    cs->active = SAG_MC_ACTIVE_NONE;
    cs->next_stamp = 0U;
    cs->batch_delta = 0;
    cs->batch_next = 0U;
    cs->batching = false;
}

bool sag_cset_add(CursorSet *cs, Cursor cursor)
{
    size_t old_len;

    if (cs == NULL)
        SAG_BUG("sag_cset_add: NULL set");
    old_len = cs->curs.len;
    if (!sag_cset_add_many(cs, &cursor, 1U))
        return false;
    return cs->curs.len > old_len;
}

bool sag_cset_add_many(CursorSet *cs, const Cursor *cursors, u32 count)
{
    SelStack empty = {0};
    size_t final_len;
    u32 i;

    if (cs == NULL || (cursors == NULL && count != 0U))
        SAG_BUG("sag_cset_add_many: invalid argument");
    if ((u64)cs->curs.len + count > SAG_MC_MAX)
        return false;
    if (count == 0U)
        return true;
    if (cs->next_stamp == 0U ||
        cs->next_stamp > UINT64_MAX - (u64)count)
        sag_cset_reseed(cs);
    final_len = cs->curs.len + count;
    SagCursorVec_reserve(&cs->curs, final_len);
    SagCursorStampVec_reserve(&cs->stamps, final_len);
    SagSelStackVec_reserve(&cs->selstacks, final_len);
    for (i = 0U; i < count; i++) {
        SagCursorVec_push(&cs->curs, cursors[i]);
        SagCursorStampVec_push(&cs->stamps, cs->next_stamp++);
        SagSelStackVec_push(&cs->selstacks, empty);
    }
    normalize_unclamped(cs);
    return true;
}

bool sag_cset_drop_latest(CursorSet *cs)
{
    size_t latest;
    size_t i;

    if (cs == NULL || cs->curs.len == 0U ||
        (size_t)cs->primary >= cs->curs.len)
        SAG_BUG("sag_cset_drop_latest: invalid set");
    if (cs->curs.len == 1U)
        return false;
    latest = (size_t)cs->primary == 0U ? 1U : 0U;
    for (i = 0U; i < cs->curs.len; i++) {
        if (i != (size_t)cs->primary &&
            cs->stamps.data[i] > cs->stamps.data[latest])
            latest = i;
    }
    if (latest + 1U < cs->curs.len) {
        (void)memmove(cs->curs.data + latest, cs->curs.data + latest + 1U,
                      (cs->curs.len - latest - 1U) * sizeof(*cs->curs.data));
        (void)memmove(cs->stamps.data + latest,
                      cs->stamps.data + latest + 1U,
                      (cs->stamps.len - latest - 1U) *
                          sizeof(*cs->stamps.data));
        (void)memmove(cs->selstacks.data + latest,
                      cs->selstacks.data + latest + 1U,
                      (cs->selstacks.len - latest - 1U) *
                          sizeof(*cs->selstacks.data));
    }
    cs->curs.len--;
    cs->stamps.len--;
    cs->selstacks.len--;
    if ((size_t)cs->primary > latest)
        cs->primary--;
    if (cs->active != SAG_MC_ACTIVE_NONE) {
        if ((size_t)cs->active > latest)
            cs->active--;
        else if ((size_t)cs->active == latest)
            cs->active = SAG_MC_ACTIVE_NONE;
    }
    sag_cset_check(cs);
    return true;
}

void sag_cset_remove_all_but_primary(CursorSet *cs)
{
    Cursor primary;

    if (cs == NULL || cs->curs.len == 0U ||
        (size_t)cs->primary >= cs->curs.len) {
        SAG_BUG("sag_cset_remove_all_but_primary: invalid set");
    }
    primary = cs->curs.data[cs->primary];
    cs->curs.data[0] = primary;
    cs->stamps.data[0] = cs->stamps.data[cs->primary];
    cs->selstacks.data[0] = cs->selstacks.data[cs->primary];
    cs->curs.len = 1U;
    cs->stamps.len = 1U;
    cs->selstacks.len = 1U;
    cs->primary = 0U;
    cs->active = SAG_MC_ACTIVE_NONE;
}

void sag_cset_reseed(CursorSet *cs)
{
    SelStack empty = {0};
    size_t i;

    if (cs == NULL || cs->curs.len == 0U || cs->curs.len > SAG_MC_MAX)
        SAG_BUG("sag_cset_reseed: invalid set");
    SagCursorStampVec_reserve(&cs->stamps, cs->curs.len);
    cs->stamps.len = cs->curs.len;
    SagSelStackVec_reserve(&cs->selstacks, cs->curs.len);
    while (cs->selstacks.len < cs->curs.len)
        SagSelStackVec_push(&cs->selstacks, empty);
    if (cs->selstacks.len > cs->curs.len)
        cs->selstacks.len = cs->curs.len;
    for (i = 0U; i < cs->curs.len; i++)
        cs->stamps.data[i] = (u64)i + 1U;
    cs->next_stamp = (u64)cs->curs.len + 1U;
    cs->active = SAG_MC_ACTIVE_NONE;
    cs->batch_delta = 0;
    cs->batch_next = 0U;
    cs->batching = false;
}

void sag_cset_normalize(const TextBuf *tb, CursorSet *cs)
{
    size_t i;

    if (tb == NULL || cs == NULL)
        SAG_BUG("sag_cset_normalize: NULL argument");
    for (i = 0U; i < cs->curs.len; i++)
        sag_cursor_clamp(tb, &cs->curs.data[i]);
    normalize_unclamped(cs);
    sag_cset_check_text(tb, cs);
}

static ByteOff adjust_off(ByteOff position, bool right_bias, u8 op,
                          ByteOff at, u64 len)
{
    u64 end;

    if (op == SAG_JOURNAL_INS) {
        if (position.v > at.v || (position.v == at.v && right_bias)) {
            if (UINT64_MAX - position.v < len)
                SAG_BUG("cursor adjustment overflow");
            position.v += len;
        }
        return position;
    }
    if (op != SAG_JOURNAL_DEL)
        SAG_BUG("sag_cset_adjust: unknown edit operation %u", (unsigned)op);
    if (UINT64_MAX - at.v < len)
        SAG_BUG("cursor deletion range overflow");
    end = at.v + len;
    if (end <= position.v)
        position.v -= len;
    else if (at.v <= position.v)
        position = at;
    return position;
}

static ByteOff add_delta(ByteOff position, i64 delta)
{
    u64 amount;

    if (delta >= 0) {
        amount = (u64)delta;
        if (UINT64_MAX - position.v < amount)
            SAG_BUG("cursor batch adjustment overflow");
        return BYTEOFF(position.v + amount);
    }
    amount = (u64)(-(delta + 1)) + 1U;
    if (position.v < amount)
        SAG_BUG("cursor batch adjustment underflow");
    return BYTEOFF(position.v - amount);
}

static void materialize_from(CursorSet *cs, size_t first)
{
    size_t i;

    if (!cs->batching)
        return;
    for (i = first; i < cs->curs.len; i++) {
        cs->curs.data[i].pos = add_delta(cs->curs.data[i].pos,
                                         cs->batch_delta);
        cs->curs.data[i].anchor = add_delta(cs->curs.data[i].anchor,
                                            cs->batch_delta);
    }
    cs->batch_delta = 0;
    cs->batch_next = (u32)cs->curs.len;
}

static void batch_materialize_active(CursorSet *cs)
{
    Cursor *cursor;

    if (!cs->batching || cs->active == SAG_MC_ACTIVE_NONE)
        return;
    if (cs->active < cs->batch_next)
        return;
    if (cs->active != cs->batch_next)
        SAG_BUG("cursor batch visited out of order");
    cursor = &cs->curs.data[cs->active];
    cursor->pos = add_delta(cursor->pos, cs->batch_delta);
    cursor->anchor = add_delta(cursor->anchor, cs->batch_delta);
    cs->batch_next++;
}

static bool batch_add_delta(CursorSet *cs, i64 delta)
{
    if ((delta > 0 && cs->batch_delta > INT64_MAX - delta) ||
        (delta < 0 && cs->batch_delta < INT64_MIN - delta))
        return false;
    cs->batch_delta += delta;
    return true;
}

static bool batch_delta_fits(const CursorSet *cs, i64 delta)
{
    return !((delta > 0 && cs->batch_delta > INT64_MAX - delta) ||
             (delta < 0 && cs->batch_delta < INT64_MIN - delta));
}

static void batch_remove_future(CursorSet *cs, size_t first, size_t count)
{
    size_t after;

    if (count == 0U)
        return;
    after = first + count;
    if ((size_t)cs->primary >= first && (size_t)cs->primary < after) {
        Cursor *active = &cs->curs.data[cs->active];
        const Cursor *primary = &cs->curs.data[cs->primary];

        active->goal_col = primary->goal_col;
        cs->selstacks.data[cs->active] =
            cs->selstacks.data[cs->primary];
        cs->primary = cs->active;
    } else if ((size_t)cs->primary >= after) {
        cs->primary -= (u32)count;
    }
    if (after < cs->curs.len) {
        (void)memmove(cs->curs.data + first, cs->curs.data + after,
                      (cs->curs.len - after) * sizeof(*cs->curs.data));
        (void)memmove(cs->stamps.data + first, cs->stamps.data + after,
                      (cs->stamps.len - after) * sizeof(*cs->stamps.data));
        (void)memmove(cs->selstacks.data + first,
                      cs->selstacks.data + after,
                      (cs->selstacks.len - after) *
                          sizeof(*cs->selstacks.data));
    }
    cs->curs.len -= count;
    cs->stamps.len -= count;
    cs->selstacks.len -= count;
    cs->batch_next = (u32)first;
}

static bool batch_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len)
{
    Cursor *active;
    bool collapsed;
    size_t first;
    size_t last;

    if (!cs->batching || cs->active == SAG_MC_ACTIVE_NONE ||
        cs->active >= cs->batch_next || len > (u64)INT64_MAX)
        return false;
    first = cs->batch_next;
    last = first;
    if (op == SAG_JOURNAL_INS) {
        if (!batch_delta_fits(cs, (i64)len))
            return false;
    } else if (op == SAG_JOURNAL_DEL) {
        u64 end;

        if (UINT64_MAX - at.v < len)
            SAG_BUG("cursor deletion range overflow");
        end = at.v + len;
        while (last < cs->curs.len) {
            ByteOff pos = add_delta(cs->curs.data[last].pos,
                                    cs->batch_delta);
            ByteOff anchor = add_delta(cs->curs.data[last].anchor,
                                       cs->batch_delta);

            if (pos.v != anchor.v || pos.v >= end)
                break;
            if (pos.v < at.v)
                return false;
            last++;
        }
        if (!batch_delta_fits(cs, -(i64)len))
            return false;
    } else {
        return false;
    }
    active = &cs->curs.data[cs->active];
    collapsed = active->pos.v == active->anchor.v;
    active->pos = adjust_off(active->pos, true, op, at, len);
    active->anchor = collapsed ? active->pos :
                                adjust_off(active->anchor, false, op, at,
                                           len);
    if (op == SAG_JOURNAL_INS)
        return batch_add_delta(cs, (i64)len);
    if (op == SAG_JOURNAL_DEL) {
        batch_remove_future(cs, first, last - first);
        return batch_add_delta(cs, -(i64)len);
    }
    return false;
}

typedef struct McSkip {
    u64 stamp;
    u64 owner_stamp;
} McSkip;

static bool command_original_range(const CmdDesc *desc, const TextBuf *tb,
                                   const Cursor *cursor, u32 repeats,
                                   Span *range)
{
    ByteOff at;
    u32 i;

    if (strcmp(desc->name, "ed.edit.delete.grapheme") == 0 ||
        strcmp(desc->name, "ed.edit.delete.next") == 0) {
        at = cursor->pos;
        range->lo = at.v;
        for (i = 0U; i < repeats; i++)
            at = sag_grapheme_next_boundary(tb, at);
        range->hi = at.v;
        return range->lo != range->hi;
    }
    if (strcmp(desc->name, "ed.edit.delete.grapheme_left") == 0 ||
        strcmp(desc->name, "ed.edit.delete.prev") == 0) {
        at = cursor->pos;
        range->hi = at.v;
        for (i = 0U; i < repeats; i++)
            at = sag_grapheme_prev_boundary(tb, at);
        range->lo = at.v;
        return range->lo != range->hi;
    }
    if (strcmp(desc->name, "ed.edit.line.delete") == 0) {
        LineNo line = sag_textbuf_line_of(tb, cursor->pos);
        u64 lines = sag_textbuf_line_count(tb);
        u64 forward = lines - line.v;
        u64 count = repeats;

        if (count < forward) {
            range->lo = sag_textbuf_line_start(tb, line).v;
            range->hi = sag_textbuf_line_start(
                tb, LINENO(line.v + count)).v;
        } else {
            u64 extra = count - forward;

            range->hi = sag_textbuf_len(tb);
            if (line.v == 0U) {
                range->lo = 0U;
            } else if (extra == 0U) {
                range->lo = sag_grapheme_prev_boundary(
                    tb, sag_textbuf_line_start(tb, line)).v;
            } else {
                u64 first = extra >= line.v ? 0U : line.v - extra;

                range->lo = sag_textbuf_line_start(tb, LINENO(first)).v;
            }
        }
        return range->lo != range->hi;
    }
    return false;
}

static McSkip *build_skip_plan(const CmdDesc *desc, const TextBuf *tb,
                               const CursorSet *cs, u32 repeats,
                               size_t *skip_count)
{
    McSkip *skips;
    Span accepted = {0U, 0U};
    u64 owner_stamp = 0U;
    bool have_accepted = false;
    size_t count = 0U;
    size_t i;

    *skip_count = 0U;
    skips = sag_xreallocarray(NULL, cs->curs.len, sizeof(*skips));
    for (i = 0U; i < cs->curs.len; i++) {
        Span range;

        if (!command_original_range(desc, tb, &cs->curs.data[i], repeats,
                                    &range))
            continue;
        if (have_accepted && range.lo < accepted.hi &&
            accepted.lo < range.hi) {
            skips[count].stamp = cs->stamps.data[i];
            skips[count].owner_stamp = owner_stamp;
            count++;
            continue;
        }
        accepted = range;
        owner_stamp = cs->stamps.data[i];
        have_accepted = true;
    }
    if (count == 0U) {
        free(skips);
        return NULL;
    }
    *skip_count = count;
    return skips;
}

static bool skip_owner(const McSkip *skips, size_t count, u64 stamp,
                       u64 *owner_stamp)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        if (skips[i].stamp == stamp) {
            *owner_stamp = skips[i].owner_stamp;
            return true;
        }
    }
    return false;
}

static void batch_remove_active(CursorSet *cs, u64 owner_stamp)
{
    size_t active = cs->active;
    size_t owner;

    if (!cs->batching || cs->active == SAG_MC_ACTIVE_NONE ||
        active >= cs->curs.len || cs->batch_next != active + 1U)
        SAG_BUG("cursor batch: cannot remove inactive cursor");
    for (owner = 0U; owner < cs->stamps.len; owner++) {
        if (cs->stamps.data[owner] == owner_stamp)
            break;
    }
    if (owner == cs->stamps.len) {
        SAG_BUG("cursor batch: overlap owner stamp %llu disappeared at %zu",
                (unsigned long long)owner_stamp, active);
    }
    if (owner >= active) {
        SAG_BUG("cursor batch: overlap owner %zu was not before active %zu",
                owner, active);
    }
    if ((size_t)cs->primary == active) {
        cs->curs.data[owner].goal_col = cs->curs.data[active].goal_col;
        cs->selstacks.data[owner] = cs->selstacks.data[active];
        cs->primary = (u32)owner;
    } else if ((size_t)cs->primary > active) {
        cs->primary--;
    }
    if (active + 1U < cs->curs.len) {
        (void)memmove(cs->curs.data + active, cs->curs.data + active + 1U,
                      (cs->curs.len - active - 1U) *
                          sizeof(*cs->curs.data));
        (void)memmove(cs->stamps.data + active,
                      cs->stamps.data + active + 1U,
                      (cs->stamps.len - active - 1U) *
                          sizeof(*cs->stamps.data));
        (void)memmove(cs->selstacks.data + active,
                      cs->selstacks.data + active + 1U,
                      (cs->selstacks.len - active - 1U) *
                          sizeof(*cs->selstacks.data));
    }
    cs->curs.len--;
    cs->stamps.len--;
    cs->selstacks.len--;
    cs->batch_next--;
    if (active >= cs->curs.len)
        cs->active = SAG_MC_ACTIVE_NONE;
}

void sag_cset_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len)
{
    size_t i;

    if (cs == NULL)
        SAG_BUG("sag_cset_adjust: NULL set");
    if (op != SAG_JOURNAL_INS && op != SAG_JOURNAL_DEL)
        SAG_BUG("sag_cset_adjust: unknown edit operation %u", (unsigned)op);
    if (batch_adjust(cs, op, at, len))
        return;
    if (cs->batching) {
        materialize_from(cs, cs->batch_next);
        cs->batching = false;
        cs->batch_delta = 0;
        cs->batch_next = 0U;
    }
    for (i = 0U; i < cs->curs.len; i++) {
        bool collapsed = cs->curs.data[i].pos.v ==
                         cs->curs.data[i].anchor.v;

        cs->curs.data[i].pos =
            adjust_off(cs->curs.data[i].pos, true, op, at, len);
        cs->curs.data[i].anchor = collapsed ? cs->curs.data[i].pos :
            adjust_off(cs->curs.data[i].anchor, false, op, at, len);
    }
    normalize_unclamped(cs);
}

void sag_cset_check(const CursorSet *cs)
{
    CursorItem *items;
    u64 lo;
    u64 hi;
    bool selected;
    size_t i;

    if (cs == NULL || cs->curs.len == 0U ||
        cs->curs.len > SAG_MC_MAX ||
        (size_t)cs->primary >= cs->curs.len ||
        cs->stamps.len != cs->curs.len ||
        cs->selstacks.len != cs->curs.len ||
        (cs->active != SAG_MC_ACTIVE_NONE &&
         (size_t)cs->active >= cs->curs.len)) {
        SAG_BUG("cursor set invariant: invalid primary or empty set");
    }
    for (i = 0U; i < cs->stamps.len; i++) {
        if (cs->stamps.data[i] == 0U)
            SAG_BUG("cursor set invariant: invalid tracking stamp");
    }
    for (i = 1U; i < cs->curs.len; i++) {
        if (cs->curs.data[i - 1U].pos.v > cs->curs.data[i].pos.v)
            SAG_BUG("cursor set invariant: cursors are not sorted");
    }
    if (collapsed_strictly_sorted(cs))
        return;

    items = sag_xreallocarray(NULL, cs->curs.len, sizeof(*items));
    for (i = 0U; i < cs->curs.len; i++) {
        items[i].cursor = cs->curs.data[i];
        items[i].order = i;
        items[i].primary = i == (size_t)cs->primary;
    }
    sag_sort_stable(items, cs->curs.len, sizeof(*items), cmp_span_start, NULL);
    lo = cursor_lo(&items[0].cursor);
    hi = cursor_hi(&items[0].cursor);
    selected = cursor_selected(&items[0].cursor);
    for (i = 1U; i < cs->curs.len; i++) {
        if (spans_merge(lo, hi, selected, &items[i].cursor)) {
            SAG_BUG("cursor set invariant: mergeable cursors remain");
        }
        lo = cursor_lo(&items[i].cursor);
        hi = cursor_hi(&items[i].cursor);
        selected = cursor_selected(&items[i].cursor);
    }
    free(items);
}

void sag_cset_check_text(const TextBuf *tb, const CursorSet *cs)
{
    size_t i;
    u64 len;

    if (tb == NULL)
        SAG_BUG("cursor set invariant: NULL text buffer");
    sag_cset_check(cs);
    len = sag_textbuf_len(tb);
    for (i = 0U; i < cs->curs.len; i++) {
        const Cursor *cursor = &cs->curs.data[i];

        if (cursor->pos.v > len || cursor->anchor.v > len)
            SAG_BUG("cursor set invariant: cursor outside text buffer");
        if (!sag_is_grapheme_boundary(tb, cursor->pos) ||
            !sag_is_grapheme_boundary(tb, cursor->anchor))
            SAG_BUG("cursor set invariant: cursor splits grapheme");
    }
}

void sag_cset_require_single_edit(const CursorSet *cs)
{
    sag_cset_check(cs);
    if (cs->curs.len > 1U) {
        SAG_BUG("multi-cursor edits require a MULTI transaction");
    }
}

void sag_mc_require_literal_lift(bool regex)
{
    if (regex)
        SAG_BUG("regex cursor lift lands in Sprint 21");
}

void sag_mc_require_single_completion(const CursorSet *cs)
{
    sag_cset_check(cs);
    if (cs->curs.len > 1U)
        SAG_BUG("per-cursor completion lands in Sprint 43");
}

void sag_mc_require_single_lsp_edit(const CursorSet *cs)
{
    sag_cset_check(cs);
    if (cs->curs.len > 1U)
        SAG_BUG("per-cursor LSP edits land in Sprint 47");
}

CmdStatus sag_mc_run(Win *w, CmdId cmd, CmdCtx *cx)
{
    const CmdDesc *desc;
    EditCtx ec;
    CmdStatus status = SAG_CMD_OK;
    size_t before_count;
    McSkip *skips;
    size_t skip_count;
    u32 repeats;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL || cx == NULL ||
        cx->ed == NULL || cx->win != w || w->cs.curs.len < 2U)
        return SAG_CMD_ERR_STATE;
    status = sag_cmd_prepare(cmd, cx, &desc);
    if (status != SAG_CMD_OK)
        return status;
    if (desc == NULL || desc->fn == NULL ||
        (desc->flags & SAG_CMD_CHANGES_BUFFER) == 0U)
        return SAG_CMD_ERR_ARG;
    sag_cset_check_text(w->buf->tb, &w->cs);
    /*
     * READ-ONLY, hence no sag_ed_finish_edit: this context exists to
     * check that the caller already opened a multi transaction, and is
     * never handed to sag_edit_*.  A context that IS edited through owns
     * the journal handle those calls open into it and must finish —
     * apply_edits in sel_actions.c says why.
     */
    ec = sag_ed_edit_ctx_for(cx->ed, cx->win);
    if (ec.tb != w->buf->tb || ec.cset != &w->cs || ec.undo == NULL ||
        ec.undo->depth == 0U || ec.undo->pending_reason != SAG_TXN_MULTI)
        return SAG_CMD_ERR_STATE;

    before_count = w->cs.curs.len;
    repeats = (desc->flags & SAG_CMD_REPEATABLE) != 0U ? cx->count : 1U;
    skips = build_skip_plan(desc, w->buf->tb, &w->cs, repeats,
                            &skip_count);
    w->cs.active = 0U;
    w->cs.batching = true;
    w->cs.batch_delta = 0;
    w->cs.batch_next = 1U;
    while (w->cs.active != SAG_MC_ACTIVE_NONE &&
           (size_t)w->cs.active < w->cs.curs.len &&
           status == SAG_CMD_OK) {
        u32 r;
        u64 owner_stamp;

        batch_materialize_active(&w->cs);
        if (skip_owner(skips, skip_count,
                       w->cs.stamps.data[w->cs.active], &owner_stamp)) {
            batch_remove_active(&w->cs, owner_stamp);
            continue;
        }
        for (r = 0U; r < repeats && status == SAG_CMD_OK; r++) {
            if (w->cs.active == SAG_MC_ACTIVE_NONE ||
                (size_t)w->cs.active >= w->cs.curs.len)
                break;
            cx->cursor_index = w->cs.active;
            status = desc->fn(cx);
        }
        if (status == SAG_CMD_OK)
            w->cs.active++;
    }

    materialize_from(&w->cs, w->cs.batch_next);
    w->cs.batching = false;
    w->cs.batch_delta = 0;
    w->cs.batch_next = 0U;
    w->cs.active = SAG_MC_ACTIVE_NONE;
    free(skips);
    if (status == SAG_CMD_OK) {
        size_t merged;

        sag_cset_normalize(w->buf->tb, &w->cs);
        merged = before_count - w->cs.curs.len;
        if (merged != 0U) {
            sag_msg(cx->ed, SAG_MSG_INFO, "%zu cursor%s merged", merged,
                    merged == 1U ? "" : "s");
        }
    }
    return status;
}
