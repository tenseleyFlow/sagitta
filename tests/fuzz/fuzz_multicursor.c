#define _POSIX_C_SOURCE 200809L

#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "text/edit.h"
#include "ui/message.h"
#include "util/buf.h"

enum {
    MC_FUZZ_MAX_CURSORS = 500,
    MC_FUZZ_MAX_DELETE_CURSORS = 32,
    MC_FUZZ_MIN_GRAPHEMES = 512,
    MC_FUZZ_OPS_PER_CASE = 128
};

typedef enum {
    MC_ACTION_INSERT,
    MC_ACTION_DELETE_NEXT,
    MC_ACTION_DELETE_PREV,
    MC_ACTION_COUNT
} McAction;

typedef struct OracleRange {
    Span span;
    bool accepted;
} OracleRange;

static u64 cursor_operations;
static u64 action_operations[MC_ACTION_COUNT];
static u64 selected_sets;
static u64 overlap_skips;
static u64 counted_actions;
static u64 cases_checked;
static size_t min_cursors = MC_FUZZ_MAX_CURSORS;
static size_t max_cursors;

static u8 input_byte(const u8 *data, size_t len, size_t at)
{
    static const u8 fallback[] = {0x17U, 0xA5U, 0x4DU, 0xE3U};

    return len == 0U ? fallback[at % YEW_ARRAY_LEN(fallback)]
                     : data[at % len];
}

static u32 input_u16(const u8 *data, size_t len, size_t at)
{
    return (u32)input_byte(data, len, at) |
           ((u32)input_byte(data, len, at + 1U) << 8U);
}

static void make_random_buffer(const u8 *data, size_t len, Bytebuf *out)
{
    static const u8 *const tokens[] = {
        (const u8 *)"a", (const u8 *)"\n", (const u8 *)"\xE6\xBC\xA2",
        (const u8 *)"e\xCC\x81", (const u8 *)"\xF0\x9F\x91\x8D",
        (const u8 *)"\x80", (const u8 *)"\t", (const u8 *)"z",
    };
    static const u8 sizes[] = {1U, 1U, 3U, 3U, 4U, 1U, 1U, 1U};
    size_t graphemes = MC_FUZZ_MIN_GRAPHEMES +
                       (input_u16(data, len, 0U) & 127U);
    size_t i;

    bytebuf_init(out);
    for (i = 0U; i < graphemes; i++) {
        size_t token = input_byte(data, len, i + 2U) %
                       YEW_ARRAY_LEN(tokens);

        bytebuf_append(out, tokens[token], sizes[token]);
    }
}

static bool materialize(const TextBuf *tb, Bytebuf *out)
{
    TextIter iter;

    out->len = 0U;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return yew_textbuf_len(tb) == 0U;
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
            return false;
        bytebuf_append(out, bytes, (size_t)len);
    } while (yew_textiter_advance(&iter, tb));
    return out->len == yew_textbuf_len(tb);
}

static void oracle_insert(Bytebuf *bytes, size_t at,
                          const u8 *payload, size_t payload_len)
{
    bytebuf_reserve(bytes, bytes->len + payload_len);
    (void)memmove(bytes->data + at + payload_len, bytes->data + at,
                  bytes->len - at);
    (void)memcpy(bytes->data + at, payload, payload_len);
    bytes->len += payload_len;
}

static void oracle_delete(Bytebuf *bytes, size_t at, size_t len)
{
    (void)memmove(bytes->data + at, bytes->data + at + len,
                  bytes->len - at - len);
    bytes->len -= len;
}

static bool fail(char *why, size_t why_cap, const char *message)
{
    (void)snprintf(why, why_cap, "%s", message);
    return false;
}

static void model_init(Ed *ed, Win *win, const u8 *bytes, size_t len)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    (void)memset(ed, 0, sizeof(*ed));
    (void)memset(win, 0, sizeof(*win));
    ed->buffer.tb = yew_textbuf_from_bytes(bytes, (u64)len);
    ed->buffer.undo = yew_undo_new(ed->buffer.tb);
    ed->buffer.marks = yew_marks_new();
    yew_timers_init(&ed->timers);
    win->buf = &ed->buffer;
    yew_cset_init(&win->cs, cursor);
    ed->win = win;
    ed->model_ready = true;
}

static void model_free(Ed *ed, Win *win)
{
    yew_msg_clear(ed);
    yew_timers_free(&ed->timers);
    yew_cset_free(&win->cs);
    yew_marks_free(ed->buffer.marks);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
}

static bool collect_boundaries(const TextBuf *tb, ByteOff *boundaries,
                               size_t capacity, size_t *count)
{
    size_t n = 1U;

    boundaries[0] = BYTEOFF(0U);
    while (boundaries[n - 1U].v < yew_textbuf_len(tb)) {
        if (n == capacity)
            return false;
        boundaries[n] = yew_grapheme_next_boundary(tb, boundaries[n - 1U]);
        if (boundaries[n].v <= boundaries[n - 1U].v)
            return false;
        n++;
    }
    *count = n;
    return true;
}

static size_t install_random_cursors(CursorSet *set,
                                     const ByteOff *boundaries,
                                     size_t boundary_count,
                                     const u8 *data, size_t len,
                                     size_t salt, size_t requested)
{
    Cursor cursors[MC_FUZZ_MAX_CURSORS];
    size_t usable = boundary_count - 2U;
    size_t max_spacing = usable / requested;
    size_t spacing;
    size_t window;
    size_t start;
    bool selections;
    size_t i;

    if (max_spacing > 4U)
        max_spacing = 4U;
    spacing = 1U + input_byte(data, len, salt + 1U) % max_spacing;
    window = (requested - 1U) * spacing + 1U;
    start = 1U + input_u16(data, len, salt + 2U) %
                         (usable - window + 1U);
    selections = requested < MC_FUZZ_MAX_CURSORS &&
                 (input_byte(data, len, salt + 4U) & 1U) != 0U;
    for (i = 0U; i < requested; i++) {
        size_t slot = start + i * spacing;
        size_t anchor_slot = slot;

        if (selections && (input_byte(data, len, salt + 5U + i) & 7U) == 0U)
            anchor_slot = slot + 1U;
        cursors[i].pos = boundaries[slot];
        cursors[i].anchor = boundaries[anchor_slot];
        cursors[i].goal_col = (GCol){input_byte(data, len,
                                                salt + 9U + i)};
    }
    yew_cset_free(set);
    yew_cset_init(set, cursors[0]);
    if (requested > 1U &&
        !yew_cset_add_many(set, cursors + 1U, (u32)requested - 1U))
        return 0U;
    if (selections)
        selected_sets++;
    return set->curs.len;
}

static Span action_range(const TextBuf *tb, const Cursor *cursor,
                         McAction action, u32 count)
{
    ByteOff at = cursor->pos;
    Span span = {at.v, at.v};
    u32 i;

    if (action == MC_ACTION_DELETE_NEXT) {
        for (i = 0U; i < count; i++)
            at = yew_grapheme_next_boundary(tb, at);
        span.hi = at.v;
    } else if (action == MC_ACTION_DELETE_PREV) {
        for (i = 0U; i < count; i++)
            at = yew_grapheme_prev_boundary(tb, at);
        span.lo = at.v;
    }
    return span;
}

static void oracle_apply(Bytebuf *oracle, const TextBuf *tb,
                         const Cursor *cursors, size_t cursor_count,
                         McAction action, u32 count, const u8 *payload,
                         size_t payload_len)
{
    OracleRange ranges[MC_FUZZ_MAX_CURSORS];
    Span accepted = {0U, 0U};
    bool have_accepted = false;
    i64 delta = 0;
    size_t i;

    if (action == MC_ACTION_INSERT) {
        for (i = 0U; i < cursor_count; i++) {
            size_t at = (size_t)((i64)cursors[i].pos.v + delta);

            oracle_insert(oracle, at, payload, payload_len);
            delta += (i64)payload_len;
        }
        return;
    }
    for (i = 0U; i < cursor_count; i++) {
        ranges[i].span = action_range(tb, &cursors[i], action, count);
        ranges[i].accepted = ranges[i].span.lo != ranges[i].span.hi;
        if (!ranges[i].accepted)
            continue;
        if (have_accepted && ranges[i].span.lo < accepted.hi &&
            accepted.lo < ranges[i].span.hi) {
            ranges[i].accepted = false;
            overlap_skips++;
            continue;
        }
        accepted = ranges[i].span;
        have_accepted = true;
    }
    for (i = 0U; i < cursor_count; i++) {
        size_t at;
        size_t span_len;

        if (!ranges[i].accepted)
            continue;
        at = (size_t)((i64)ranges[i].span.lo + delta);
        span_len = (size_t)(ranges[i].span.hi - ranges[i].span.lo);
        oracle_delete(oracle, at, span_len);
        delta -= (i64)span_len;
    }
}

static bool cursors_equal(const CursorSet *set, const Cursor *before,
                          size_t count, u32 primary)
{
    size_t i;

    if (set->curs.len != count || set->primary != primary)
        return false;
    for (i = 0U; i < count; i++) {
        if (set->curs.data[i].pos.v != before[i].pos.v ||
            set->curs.data[i].anchor.v != before[i].anchor.v ||
            set->curs.data[i].goal_col.v != before[i].goal_col.v)
            return false;
    }
    return true;
}

static bool cursor_set_has_selection(const CursorSet *set)
{
    size_t i;

    for (i = 0U; i < set->curs.len; i++) {
        if (set->curs.data[i].pos.v != set->curs.data[i].anchor.v)
            return true;
    }
    return false;
}

static bool check_multicursor(const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    static const u8 *const payloads[] = {
        (const u8 *)"x", (const u8 *)"\xE6\xBC\xA2",
        (const u8 *)"e\xCC\x81", (const u8 *)"\xF0\x9F\x91\x8D",
    };
    static const u8 payload_lens[] = {1U, 3U, 3U, 4U};
    Bytebuf initial;
    Bytebuf oracle;
    Bytebuf actual;
    ByteOff *boundaries = NULL;
    Cursor before[MC_FUZZ_MAX_CURSORS];
    size_t boundary_count = 0U;
    size_t case_ops = 0U;
    size_t round = 0U;
    Ed ed;
    Win win;
    CmdCtx cx = {0};
    CmdId commands[MC_ACTION_COUNT];
    u64 case_index = cases_checked++;
    bool ok = false;

    make_random_buffer(data, len, &initial);
    model_init(&ed, &win, initial.data, initial.len);
    boundaries = malloc((initial.len + 1U) * sizeof(*boundaries));
    bytebuf_init(&oracle);
    bytebuf_init(&actual);
    if (boundaries == NULL ||
        !collect_boundaries(ed.buffer.tb, boundaries, initial.len + 1U,
                            &boundary_count))
        goto done;
    commands[MC_ACTION_INSERT] =
        yew_cmd_lookup("ed.edit.insert.text", 19U);
    commands[MC_ACTION_DELETE_NEXT] =
        yew_cmd_lookup("ed.edit.delete.grapheme", 23U);
    commands[MC_ACTION_DELETE_PREV] =
        yew_cmd_lookup("ed.edit.delete.grapheme_left", 28U);
    if (commands[0].v == 0U || commands[1].v == 0U ||
        commands[2].v == 0U)
        goto done;
    cx.ed = &ed;
    cx.win = &win;
    cx.source = YEW_SRC_TEST;
    while (case_ops < MC_FUZZ_OPS_PER_CASE) {
        size_t salt = 11U + round * 37U;
        size_t remaining = MC_FUZZ_OPS_PER_CASE - case_ops;
        size_t requested = 1U + input_u16(data, len, salt) % remaining;
        size_t cursor_count;
        size_t payload_index;
        McAction action;
        u32 count;
        u32 primary;
        CmdStatus status;
        EditCtx ec;

        if (round == 0U && case_index == 0U)
            requested = MC_FUZZ_MAX_CURSORS;
        else if (round == 0U && case_index == 1U)
            requested = 1U;
        else if (round == 0U && case_index % 128U == 0U)
            requested = 129U + input_u16(data, len, salt + 10U) % 372U;
        if (requested > MC_FUZZ_MAX_CURSORS)
            requested = MC_FUZZ_MAX_CURSORS;
        cursor_count = install_random_cursors(
            &win.cs, boundaries, boundary_count, data, len, salt,
            requested);
        if (cursor_count == 0U)
            goto done;
        yew_cset_check_text(ed.buffer.tb, &win.cs);
        if (cursor_count < min_cursors)
            min_cursors = cursor_count;
        if (cursor_count > max_cursors)
            max_cursors = cursor_count;
        (void)memcpy(before, win.cs.curs.data,
                     cursor_count * sizeof(*before));
        primary = win.cs.primary;
        action = (McAction)(input_byte(data, len, salt + 6U) %
                            MC_ACTION_COUNT);
        /* Raw grapheme deletes ignore selections; H actions cover those. */
        if (cursor_count > MC_FUZZ_MAX_DELETE_CURSORS ||
            cursor_set_has_selection(&win.cs))
            action = MC_ACTION_INSERT;
        count = 1U + input_byte(data, len, salt + 7U) % 4U;
        if (action != MC_ACTION_INSERT && count > 1U)
            counted_actions++;
        payload_index = input_byte(data, len, salt + 8U) %
                        YEW_ARRAY_LEN(payloads);
        oracle.len = 0U;
        bytebuf_append(&oracle, initial.data, initial.len);
        oracle_apply(&oracle, ed.buffer.tb, before, cursor_count,
                     action, count, payloads[payload_index],
                     payload_lens[payload_index]);
        cx.count = count;
        cx.count_given = count != 1U;
        cx.sarg = action == MC_ACTION_INSERT
                      ? (const char *)payloads[payload_index]
                      : NULL;
        cx.sarg_len = action == MC_ACTION_INSERT
                          ? payload_lens[payload_index]
                          : 0U;
        status = yew_ed_invoke(&ed, commands[action], &cx);
        if (status != YEW_CMD_OK) {
            (void)snprintf(why, why_cap,
                           "action %u count %u rejected %zu cursors",
                           (unsigned)action, count, cursor_count);
            goto done;
        }
        cursor_operations += cursor_count;
        action_operations[action] += cursor_count;
        case_ops += cursor_count;
        yew_cset_check_text(ed.buffer.tb, &win.cs);
        if (!materialize(ed.buffer.tb, &actual) ||
            actual.len != oracle.len ||
            memcmp(actual.data, oracle.data, oracle.len) != 0) {
            size_t common = actual.len < oracle.len ? actual.len : oracle.len;
            size_t diff = 0U;

            while (diff < common && actual.data[diff] == oracle.data[diff])
                diff++;
            (void)snprintf(why, why_cap,
                           "oracle mismatch action %u count %u with %zu "
                           "cursors at %zu (actual=%zu oracle=%zu)",
                           (unsigned)action, count, cursor_count, diff,
                           actual.len, oracle.len);
            goto done;
        }
        ec = yew_ed_edit_ctx(&ed);
        if (!yew_undo(&ec)) {
            (void)snprintf(why, why_cap,
                           "undo rejected action %u in round %zu",
                           (unsigned)action, round);
            goto done;
        }
        yew_ed_finish_edit(&ed, &ec);
        if (!materialize(ed.buffer.tb, &actual) ||
            actual.len != initial.len ||
            memcmp(actual.data, initial.data, initial.len) != 0) {
            (void)snprintf(why, why_cap,
                           "undo changed initial bytes in round %zu", round);
            goto done;
        }
        yew_cset_check_text(ed.buffer.tb, &win.cs);
        if (!cursors_equal(&win.cs, before, cursor_count, primary)) {
            (void)snprintf(why, why_cap,
                           "undo changed cursor set in round %zu", round);
            goto done;
        }
        round++;
    }
    ok = true;

done:
    if (!ok && why[0] == '\0')
        (void)fail(why, why_cap, "setup, invariant, or undo failure");
    bytebuf_free(&actual);
    bytebuf_free(&oracle);
    free(boundaries);
    model_free(&ed, &win);
    bytebuf_free(&initial);
    return ok;
}

int main(int argc, char **argv)
{
    int status = yew_fuzz_main(argc, argv, "fuzz_multicursor", NULL,
                               check_multicursor);

    if (status == 0)
        (void)printf("fuzz_multicursor: cursor_ops=%llu insert=%llu "
                     "delete_next=%llu delete_prev=%llu cursors=%zu..%zu "
                     "selected_sets=%llu overlap_skips=%llu counts=%llu "
                     "ok\n",
                     (unsigned long long)cursor_operations,
                     (unsigned long long)action_operations[MC_ACTION_INSERT],
                     (unsigned long long)
                         action_operations[MC_ACTION_DELETE_NEXT],
                     (unsigned long long)
                         action_operations[MC_ACTION_DELETE_PREV],
                     min_cursors, max_cursors,
                     (unsigned long long)selected_sets,
                     (unsigned long long)overlap_skips,
                     (unsigned long long)counted_actions);
    return status;
}
