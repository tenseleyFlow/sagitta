#include "edit/multicursor.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#include "text/journal.h"
#include "util/base.h"
#include "util/log.h"
#include "util/sort.h"

typedef struct CursorItem {
    Cursor cursor;
    size_t order;
    bool primary;
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
    merged.motion_col_valid = 0U;
    return merged;
}

static void normalize_unclamped(CursorSet *cs)
{
    CursorItem *items;
    CursorItem *groups;
    size_t item_count;
    size_t group_count = 0U;
    size_t i;

    if (cs == NULL)
        SAG_BUG("cursor set: NULL set");
    if (cs->curs.len == 0U)
        SAG_BUG("cursor set: empty set");
    if ((size_t)cs->primary >= cs->curs.len)
        SAG_BUG("cursor set: primary index out of range");

    item_count = cs->curs.len;
    items = sag_xreallocarray(NULL, item_count, sizeof(*items));
    groups = sag_xreallocarray(NULL, item_count, sizeof(*groups));
    for (i = 0U; i < item_count; i++) {
        items[i].cursor = cs->curs.data[i];
        items[i].order = i;
        items[i].primary = i == (size_t)cs->primary;
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
            j++;
        }
        if (!selected)
            governing = point_survivor;
        groups[group_count].cursor = merged_cursor(&governing, lo, hi);
        groups[group_count].order = governing.order;
        groups[group_count].primary = primary;
        group_count++;
        i = j;
    }

    sag_sort_stable(groups, group_count, sizeof(*groups), cmp_pos, NULL);
    cs->primary = 0U;
    for (i = 0U; i < group_count; i++) {
        cs->curs.data[i] = groups[i].cursor;
        if (groups[i].primary)
            cs->primary = (u32)i;
    }
    cs->curs.len = group_count;
    free(groups);
    free(items);
    sag_cset_check(cs);
}

void sag_cset_init(CursorSet *cs, Cursor primary)
{
    if (cs == NULL)
        SAG_BUG("sag_cset_init: NULL set");
    cs->curs.data = NULL;
    cs->curs.len = 0U;
    cs->curs.cap = 0U;
    cs->primary = 0U;
    SagCursorVec_push(&cs->curs, primary);
}

void sag_cset_free(CursorSet *cs)
{
    if (cs == NULL)
        return;
    SagCursorVec_free(&cs->curs);
    cs->primary = 0U;
}

bool sag_cset_add(CursorSet *cs, Cursor cursor)
{
    size_t old_len;

    if (cs == NULL)
        SAG_BUG("sag_cset_add: NULL set");
    old_len = cs->curs.len;
    SagCursorVec_push(&cs->curs, cursor);
    normalize_unclamped(cs);
    return cs->curs.len > old_len;
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
    cs->curs.len = 1U;
    cs->primary = 0U;
}

void sag_cset_normalize(const TextBuf *tb, CursorSet *cs)
{
    size_t i;

    if (tb == NULL || cs == NULL)
        SAG_BUG("sag_cset_normalize: NULL argument");
    for (i = 0U; i < cs->curs.len; i++)
        sag_cursor_clamp(tb, &cs->curs.data[i]);
    normalize_unclamped(cs);
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

void sag_cset_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len)
{
    size_t i;

    if (cs == NULL)
        SAG_BUG("sag_cset_adjust: NULL set");
    if (op != SAG_JOURNAL_INS && op != SAG_JOURNAL_DEL)
        SAG_BUG("sag_cset_adjust: unknown edit operation %u", (unsigned)op);
    for (i = 0U; i < cs->curs.len; i++) {
        cs->curs.data[i].pos =
            adjust_off(cs->curs.data[i].pos, true, op, at, len);
        cs->curs.data[i].anchor =
            adjust_off(cs->curs.data[i].anchor, false, op, at, len);
        cs->curs.data[i].motion_col_valid = 0U;
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
        (size_t)cs->primary >= cs->curs.len) {
        SAG_BUG("cursor set invariant: invalid primary or empty set");
    }
    for (i = 1U; i < cs->curs.len; i++) {
        if (cs->curs.data[i - 1U].pos.v > cs->curs.data[i].pos.v)
            SAG_BUG("cursor set invariant: cursors are not sorted");
    }

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

void sag_cset_require_single_edit(const CursorSet *cs)
{
    sag_cset_check(cs);
    if (cs->curs.len > 1U) {
        SAG_BUG("multi-cursor editing lands in Sprint 17");
    }
}
