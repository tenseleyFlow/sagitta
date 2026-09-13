#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.16: random insert-mode typing with autoindent and autopair on.
 *
 * The invariants are the sprint's own: the buffer is never corrupted, the
 * pair stack never lies (every live entry names a byte that really is its
 * closer, and the stack never exceeds its bound), and undoing everything
 * returns the original bytes exactly.
 */

#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "edit/pairs.h"
#include "text/edit.h"
#include "ui/message.h"
#include "util/buf.h"

enum {
    INSERT_FUZZ_OPS = 96,
    INSERT_FUZZ_SEED_LINES = 6
};

typedef enum {
    INSERT_OP_TEXT = 0,
    INSERT_OP_OPENER,
    INSERT_OP_CLOSER,
    INSERT_OP_QUOTE,
    INSERT_OP_NEWLINE,
    INSERT_OP_TAB,
    INSERT_OP_BACKSPACE,
    INSERT_OP_MOVE,
    INSERT_OP_COUNT
} InsertOp;

static u64 op_counts[INSERT_OP_COUNT];
static u64 cases_checked;
static u64 undo_rounds;

static u8 input_byte(const u8 *data, size_t len, size_t at)
{
    static const u8 fallback[] = {0x3BU, 0x91U, 0x07U, 0xC4U, 0x5EU};

    return len == 0U ? fallback[at % YEW_ARRAY_LEN(fallback)]
                     : data[at % len];
}

static void model_init(Ed *ed, Win *win, const u8 *bytes, size_t len)
{
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};

    (void)memset(ed, 0, sizeof(*ed));
    (void)memset(win, 0, sizeof(*win));
    ed->buffer.tb = yew_textbuf_from_bytes(bytes, (u64)len);
    ed->buffer.tabwidth = 4U;
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
    yew_syn_detach(&ed->buffer.syn);
    yew_pairs_clear(&ed->buffer);
    yew_marks_free(ed->buffer.marks);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
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

/*
 * Every live entry must name a byte inside the buffer, and no entry may
 * survive past the stack bound.  A remembered closer whose byte no longer
 * matches is allowed to exist only until it is consulted; what must never
 * happen is an entry pointing outside the text.
 */
static bool stack_sound(const Ed *ed, char *why, size_t why_cap)
{
    const PairState *st = &ed->buffer.pairs;
    u64 len = yew_textbuf_len(ed->buffer.tb);
    u32 i;

    if ((u32)st->n > (u32)YEW_PAIR_STACK_MAX) {
        (void)snprintf(why, why_cap, "pair stack overflowed to %u",
                       (unsigned)st->n);
        return false;
    }
    for (i = 0U; i < (u32)st->n; i++) {
        if (!yew_mark_alive(ed->buffer.marks, st->v[i].close))
            continue;
        if (yew_mark_pos(ed->buffer.marks, st->v[i].close).v > len) {
            (void)snprintf(why, why_cap,
                           "pair mark %u past end (%llu > %llu)",
                           (unsigned)i,
                           (unsigned long long)
                               yew_mark_pos(ed->buffer.marks,
                                            st->v[i].close).v,
                           (unsigned long long)len);
            return false;
        }
    }
    return true;
}

static bool run_op(Ed *ed, Win *win, InsertOp op, u8 pick)
{
    static const char openers[] = "([{";
    static const char closers[] = ")]}";
    static const char quotes[] = "\"'`";
    /* Whole clusters only: the input layer never hands a command half a
     * codepoint, and a lone lead byte would break the cursor's grapheme
     * invariant before any of this sprint's code was reached. */
    static const char *const letters[] = {
        "a", "b", "c", " ", "\xc3\xa9", "\xe6\xbc\xa2"
    };
    CmdCtx cx = {0};
    const char *name;
    char one[2] = {0, 0};
    CmdId id;

    cx.win = win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    switch (op) {
    case INSERT_OP_TEXT:
        cx.sarg = letters[pick % (u8)YEW_ARRAY_LEN(letters)];
        cx.sarg_len = (u32)strlen(cx.sarg);
        name = "ed.edit.insert.text";
        break;
    case INSERT_OP_OPENER:
        one[0] = openers[pick % 3U];
        cx.sarg = one;
        cx.sarg_len = 1U;
        name = "ed.edit.insert.text";
        break;
    case INSERT_OP_CLOSER:
        one[0] = closers[pick % 3U];
        cx.sarg = one;
        cx.sarg_len = 1U;
        name = "ed.edit.insert.text";
        break;
    case INSERT_OP_QUOTE:
        one[0] = quotes[pick % 3U];
        cx.sarg = one;
        cx.sarg_len = 1U;
        name = "ed.edit.insert.text";
        break;
    case INSERT_OP_NEWLINE:
        name = "ed.edit.insert.newline";
        break;
    case INSERT_OP_TAB:
        name = "ed.edit.insert.tab";
        break;
    case INSERT_OP_BACKSPACE:
        name = "ed.edit.delete.grapheme_left";
        break;
    case INSERT_OP_MOVE:
    default:
        name = (pick & 1U) != 0U ? "ed.move.char.prev"
                                 : "ed.move.char.next";
        break;
    }
    id = yew_cmd_lookup(name, (u32)strlen(name));
    if (id.v == 0U)
        return false;
    (void)yew_ed_invoke(ed, id, &cx);
    return true;
}

static bool check_insert(const u8 *data, size_t len, char *why,
                         size_t why_cap)
{
    Bytebuf initial;
    Bytebuf actual;
    Ed ed;
    Win win;
    EditCtx ec;
    size_t i;
    bool ok = false;

    bytebuf_init(&initial);
    bytebuf_init(&actual);
    for (i = 0U; i < (size_t)INSERT_FUZZ_SEED_LINES; i++) {
        u8 pick = input_byte(data, len, i);

        bytebuf_append(&initial, (pick & 1U) != 0U ?
                       (const u8 *)"    alpha\n" : (const u8 *)"beta\n",
                       (pick & 1U) != 0U ? 10U : 5U);
    }
    model_init(&ed, &win, initial.data, initial.len);
    if (yew_mode_enter(&ed, YEW_MODE_I) != YEW_CMD_OK) {
        (void)snprintf(why, why_cap, "could not enter insert mode");
        goto done;
    }
    for (i = 0U; i < (size_t)INSERT_FUZZ_OPS; i++) {
        u8 choice = input_byte(data, len, i * 2U);
        u8 pick = input_byte(data, len, i * 2U + 1U);
        InsertOp op = (InsertOp)(choice % (u8)INSERT_OP_COUNT);

        if (!run_op(&ed, &win, op, pick)) {
            (void)snprintf(why, why_cap, "op %u not dispatchable",
                           (unsigned)op);
            goto done;
        }
        op_counts[op]++;
        yew_textbuf_check(ed.buffer.tb);
        yew_cset_check_text(ed.buffer.tb, &win.cs);
        if (!stack_sound(&ed, why, why_cap))
            goto done;
        if (!materialize(ed.buffer.tb, &actual)) {
            (void)snprintf(why, why_cap, "buffer unreadable after op %zu",
                           i);
            goto done;
        }
    }
    /* Close the open insert run, then rewind the whole session. */
    yew_ed_insert_barrier(&ed);
    ec = yew_ed_edit_ctx(&ed);
    while (yew_undo_current(ed.buffer.undo) != ed.buffer.undo->root) {
        if (!yew_undo(&ec)) {
            (void)snprintf(why, why_cap, "undo refused before the root");
            yew_ed_finish_edit(&ed, &ec);
            goto done;
        }
        undo_rounds++;
        ec = yew_ed_edit_ctx(&ed);
    }
    yew_ed_finish_edit(&ed, &ec);
    if (!materialize(ed.buffer.tb, &actual) ||
        actual.len != initial.len ||
        memcmp(actual.data, initial.data, initial.len) != 0) {
        (void)snprintf(why, why_cap,
                       "undo did not restore the original bytes "
                       "(actual=%zu initial=%zu)", actual.len,
                       initial.len);
        goto done;
    }
    cases_checked++;
    ok = true;

done:
    model_free(&ed, &win);
    bytebuf_free(&actual);
    bytebuf_free(&initial);
    return ok;
}

int main(int argc, char **argv)
{
    int status = yew_fuzz_main(argc, argv, "fuzz_insert", NULL,
                               check_insert);

    if (status == 0)
        (void)printf("fuzz_insert: cases=%llu undo_rounds=%llu text=%llu "
                     "open=%llu close=%llu quote=%llu newline=%llu "
                     "tab=%llu backspace=%llu move=%llu ok\n",
                     (unsigned long long)cases_checked,
                     (unsigned long long)undo_rounds,
                     (unsigned long long)op_counts[INSERT_OP_TEXT],
                     (unsigned long long)op_counts[INSERT_OP_OPENER],
                     (unsigned long long)op_counts[INSERT_OP_CLOSER],
                     (unsigned long long)op_counts[INSERT_OP_QUOTE],
                     (unsigned long long)op_counts[INSERT_OP_NEWLINE],
                     (unsigned long long)op_counts[INSERT_OP_TAB],
                     (unsigned long long)op_counts[INSERT_OP_BACKSPACE],
                     (unsigned long long)op_counts[INSERT_OP_MOVE]);
    return status;
}
