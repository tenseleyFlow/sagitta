#include "edit/readline_cmds.h"

#include "edit/ed.h"
#include "edit/motion.h"
#include "edit/multicursor.h"
#include "edit/sel_actions.h"
#include "text/yankstack.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "unicode/case.h"
#include "unicode/coords.h"
#include "unicode/utf8.h"
#include "unicode/wordbreak.h"
#include "util/buf.h"
#include "util/vec.h"

/*
 * The span one cursor contributes.  `false` means this cursor has nothing
 * to do, which is NOT an error: see rl_finish below.
 */
typedef bool (*RlSpanFn)(UnitCtx *u, ByteOff pos, Span *out);

static bool rl_context(CmdCtx *cx, Win **win, TextBuf **tb, UnitCtx *u)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U)
        return false;
    *win = cx->win;
    *tb = cx->win->buf->tb;
    u->tb = *tb;
    u->buf = cx->win->buf;
    u->win = cx->win;
    return true;
}

/*
 * A no-op is YEW_CMD_OK, never an error status.
 *
 * In Insert mode yew_ed_invoke keeps ONE transaction open across the
 * typing run, and a CHANGES_BUFFER command that returns anything but OK
 * makes it call yew_undo_abort -- which applies the inverse of the whole
 * open transaction.  Backspace at offset 0 already returns OK for exactly
 * this reason.  C-u pressed at column 0 after typing a line must not
 * un-type the line.
 */
static CmdStatus rl_nothing_to_do(void)
{
    return YEW_CMD_OK;
}

/*
 * A multi-cursor aggregate edit needs a MULTI-family transaction reason or
 * text/edit.c refuses it.  yew_ed_invoke opens YEW_TXN_MULTI whenever more
 * than one cursor is live, so this only matters for the ordering where a
 * single-cursor Insert transaction was already open when the cursor set
 * grew; promoting is cheap and keeps the command honest about what it is.
 */
static void rl_promote_multi(CmdCtx *cx, EditCtx *ec)
{
    if (ec->undo == NULL || ec->cset == NULL || ec->cset->curs.len < 2U ||
        ec->undo->depth == 0U)
        return;
    if (ec->undo->pending_reason != YEW_TXN_TYPE &&
        ec->undo->pending_reason != YEW_TXN_ERASE)
        return;
    (void)cx;
    yew_undo_promote_multi(ec);
}

static bool rl_cluster_white(const TextBuf *tb, Span span, ByteOff at)
{
    YewTextCluster cluster;

    if (!yew_text_cluster_next(tb, span, at, &cluster))
        return false;
    return yew_unicode_is_white_space(cluster.base_cp);
}

/* The whole buffer as a span: word runs cross line ends, so a line span
 * is the wrong window to decode a cluster through. */
static Span rl_all(const TextBuf *tb)
{
    return (Span){0U, yew_textbuf_len(tb)};
}

static bool rl_span_is_white(const TextBuf *tb, Span span)
{
    return span.lo >= span.hi ||
           rl_cluster_white(tb, rl_all(tb), BYTEOFF(span.lo));
}

static ByteOff rl_line_content_end(const TextBuf *tb, ByteOff pos)
{
    LineNo line = yew_textbuf_line_of(tb, pos);
    Span span = yew_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    /* line_span.hi is past the line terminator; one grapheme back steps
     * over the whole terminator, CRLF included, because CR x LF is a
     * single cluster (UAX #29 GB3). */
    if (line.v + 1U < yew_textbuf_line_count(tb))
        end = yew_grapheme_prev_boundary(tb, end);
    return end;
}

/*
 * The kill plan.
 *
 * Spans arrive in ascending order because the cursor set is normalized,
 * and every span_of here is monotonic in its cursor.  Two cursors on one
 * line can still ask for OVERLAPPING spans -- C-u from two carets on the
 * same line both reach back to its start -- so an overlap is MERGED into
 * the previous span rather than dropped.  The union is what "each cursor
 * kills to its line start" means when the cursors share a line; dropping
 * the second would leave text one of the carets asked to remove.
 */
static void rl_plan_kill(Win *win, UnitCtx *u, RlSpanFn span_of,
                         SelEditVec *edits)
{
    size_t i;

    for (i = 0U; i < win->cs.curs.len; i++) {
        Span span;

        if (!span_of(u, win->cs.curs.data[i].pos, &span) ||
            span.lo >= span.hi)
            continue;
        if (edits->len != 0U) {
            SelEdit *last = &edits->data[edits->len - 1U];

            if (span.lo <= last->span.hi) {
                if (span.hi > last->span.hi)
                    last->span.hi = span.hi;
                continue;
            }
        }
        (void)yew_sel_edit_push(edits, span);
    }
}

/*
 * Apply a plan and hand what it removed to the yank stack.
 *
 * The entry is the concatenation of every killed span in document order,
 * one entry for the whole fan-out.  A kill over several cursors is
 * YEW_KILL_ALONE: it never joins the previous kill and the next never
 * joins it.  Nothing here touches a register (text/yankstack.h).
 */
static CmdStatus rl_kill_apply(CmdCtx *cx, Win *win, TextBuf *tb,
                               SelEditVec *edits, YewKillDir dir)
{
    Bytebuf killed;
    EditCtx ec;
    size_t i;

    if (edits->len == 0U) {
        yew_sel_edits_free(edits);
        return rl_nothing_to_do();
    }
    if (win->cs.curs.len > 1U)
        dir = YEW_KILL_ALONE;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    rl_promote_multi(cx, &ec);
    bytebuf_init(&killed);
    for (i = 0U; i < edits->len; i++) {
        Bytebuf part = yew_sel_copy_span(tb, edits->data[i].span);

        bytebuf_append(&killed, part.data, part.len);
        bytebuf_free(&part);
    }
    if (!yew_sel_apply_edits(cx, edits, NULL)) {
        yew_sel_edits_free(edits);
        bytebuf_free(&killed);
        return YEW_CMD_ERR_IO;
    }
    yew_yank_kill(&cx->ed->yank, killed.data, killed.len, dir,
                  cx->ed->cmd_seq, win);
    bytebuf_free(&killed);
    yew_sel_edits_free(edits);
    yew_cset_normalize(tb, &win->cs);
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

static CmdStatus rl_kill(CmdCtx *cx, RlSpanFn span_of, YewKillDir dir)
{
    Win *win;
    TextBuf *tb;
    UnitCtx u;
    SelEditVec edits = {0};

    if (!rl_context(cx, &win, &tb, &u))
        return YEW_CMD_ERR_STATE;
    rl_plan_kill(win, &u, span_of, &edits);
    return rl_kill_apply(cx, win, tb, &edits, dir);
}

static bool rl_span_word_prev(UnitCtx *u, ByteOff pos, Span *out)
{
    ByteOff start = yew_unit_word.prev(u, pos, false);

    out->lo = start.v;
    out->hi = pos.v;
    return start.v < pos.v;
}

static bool rl_is_blank(u8 c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

static bool rl_byte_before(const TextBuf *tb, u64 at, u8 *out)
{
    Bytebuf one = yew_sel_copy_span(tb, (Span){at - 1U, at});
    bool ok = one.len == 1U;

    if (ok)
        *out = one.data[0];
    bytebuf_free(&one);
    return ok;
}

/*
 * bash's unix-word-rubout: back over blanks, then back to the previous
 * blank.  Whitespace is the ONLY delimiter, so `a/b/c` and `--x=y` go
 * whole -- which is why the prompt's C-w is this and not the word-char
 * kill A-<bs> keeps.  The blanks are ASCII: every byte of a multi-byte
 * sequence is >= 0x80, so stepping bytes never splits a grapheme at a
 * stop, and the span always ends on a blank boundary or the line start.
 */
static bool rl_span_ws_word_prev(UnitCtx *u, ByteOff pos, Span *out)
{
    u64 at = pos.v;
    u64 home = yew_textbuf_line_span(u->tb,
                                     yew_textbuf_line_of(u->tb, pos)).lo;
    u8 c = 0U;

    while (at > home && rl_byte_before(u->tb, at, &c) && rl_is_blank(c))
        at--;
    while (at > home && rl_byte_before(u->tb, at, &c) && !rl_is_blank(c))
        at--;
    out->lo = at;
    out->hi = pos.v;
    return out->lo < out->hi;
}

/*
 * Forward to where A-f would land, which is the START of the next word.
 * That takes the word AND the run of blanks after it, where readline's
 * M-d stops at the word's end.  The pair is deliberate: kill-backward
 * removes exactly what A-b moves over, kill-forward removes exactly what
 * A-f moves over, and both are the one yew_unit_word definition.
 */
static bool rl_span_word_next(UnitCtx *u, ByteOff pos, Span *out)
{
    ByteOff end = yew_unit_word.next(u, pos, false);

    out->lo = pos.v;
    out->hi = end.v;
    return end.v > pos.v;
}

static bool rl_span_to_home(UnitCtx *u, ByteOff pos, Span *out)
{
    LineNo line = yew_textbuf_line_of(u->tb, pos);

    out->lo = yew_textbuf_line_span(u->tb, line).lo;
    out->hi = pos.v;
    return out->lo < out->hi;
}

/*
 * To the end of the line's content -- and, when the caret is already
 * there, the line terminator itself, so C-k on an empty line removes the
 * line instead of doing nothing.  That is readline's kill-line and the
 * only reading under which the key is not dead at end of line.  The last
 * line of the buffer has no terminator, so there it really is a no-op.
 */
static bool rl_span_to_end(UnitCtx *u, ByteOff pos, Span *out)
{
    ByteOff end = rl_line_content_end(u->tb, pos);

    out->lo = pos.v;
    out->hi = end.v;
    if (out->lo < out->hi)
        return true;
    out->hi = yew_textbuf_line_span(u->tb,
                                    yew_textbuf_line_of(u->tb, pos)).hi;
    return out->lo < out->hi;
}

CmdStatus yew_rl_cmd_kill_word_prev(CmdCtx *cx)
{
    return rl_kill(cx, rl_span_word_prev, YEW_KILL_BACKWARD);
}

CmdStatus yew_rl_cmd_kill_word_next(CmdCtx *cx)
{
    return rl_kill(cx, rl_span_word_next, YEW_KILL_FORWARD);
}

CmdStatus yew_rl_cmd_kill_to_home(CmdCtx *cx)
{
    return rl_kill(cx, rl_span_to_home, YEW_KILL_BACKWARD);
}

CmdStatus yew_rl_cmd_kill_to_end(CmdCtx *cx)
{
    return rl_kill(cx, rl_span_to_end, YEW_KILL_FORWARD);
}

CmdStatus yew_rl_cmd_kill_ws_word_prev(CmdCtx *cx)
{
    return rl_kill(cx, rl_span_ws_word_prev, YEW_KILL_BACKWARD);
}

/*
 * Put `entry` at every caret, replacing the `replace` bytes before each
 * one (0 for a yank, the previous yank's length for a yank-pop).  The
 * carets sit at the END of what the last yank inserted -- yew_cset_adjust
 * biases a caret at an insertion point past it -- so the spans a pop
 * replaces are known without having been stored: every caret yanked the
 * same text.
 */
static CmdStatus rl_yank_put(CmdCtx *cx, const Bytebuf *entry, u64 replace,
                             u32 k)
{
    Win *win;
    TextBuf *tb;
    UnitCtx u;
    SelEditVec edits = {0};
    EditCtx ec;
    Bytebuf text;
    YewYankStack *y = &cx->ed->yank;
    size_t i;

    if (!rl_context(cx, &win, &tb, &u))
        return YEW_CMD_ERR_STATE;
    bytebuf_init(&text);
    if (!yew_cmdline_clean(cx->ed, win, entry->data, entry->len, &text)) {
        bytebuf_free(&text);
        return YEW_CMD_ERR_ARG;
    }
    for (i = 0U; i < win->cs.curs.len; i++) {
        ByteOff at = win->cs.curs.data[i].pos;
        SelEdit *edit;

        if (at.v < replace) {
            bytebuf_free(&text);
            yew_sel_edits_free(&edits);
            return YEW_CMD_ERR_STATE;
        }
        edit = yew_sel_edit_push(&edits, (Span){at.v - replace, at.v});
        bytebuf_append(&edit->replacement, text.data, text.len);
    }
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    rl_promote_multi(cx, &ec);
    if (!yew_sel_apply_edits(cx, &edits, NULL)) {
        yew_sel_edits_free(&edits);
        bytebuf_free(&text);
        return YEW_CMD_ERR_IO;
    }
    yew_sel_edits_free(&edits);
    y->yank_seq = cx->ed->cmd_seq;
    y->yank_owner = win;
    y->yank_len = (u64)text.len;
    y->yank_k = k;
    bytebuf_free(&text);
    yew_cset_normalize(tb, &win->cs);
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

/*
 * Yank the newest yank-stack entry at every caret.
 *
 * The bytes go in verbatim and charwise -- into the prompt with each
 * newline run folded to a blank, as any paste there is.  ed.clip.paste
 * reads the SYSTEM clipboard and is a different key (C-v); `p` reads the
 * registers.  Only the readline kills feed this.
 */
CmdStatus yew_rl_cmd_kill_yank(CmdCtx *cx)
{
    const Bytebuf *entry;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    entry = yew_yank_at(&cx->ed->yank, 0U);
    if (entry == NULL || entry->len == 0U)
        return rl_nothing_to_do();
    return rl_yank_put(cx, entry, 0U, 0U);
}

/*
 * A-y: straight after a yank or a yank-pop in the same Win, replace what
 * it inserted with the next older entry, wrapping past the oldest back to
 * the newest.  Anything else in between -- a motion, a typed character --
 * and the inserted text is no longer "what the yank put there", so the
 * key says why it did nothing.  That is OK, not an error: an error would
 * abort Insert mode's open typing transaction.
 */
CmdStatus yew_rl_cmd_kill_yank_pop(CmdCtx *cx)
{
    YewYankStack *y;
    u32 k;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    y = &cx->ed->yank;
    if (y->len == 0U || y->yank_owner != cx->win ||
        cx->ed->cmd_seq != y->yank_seq + 1U) {
        yew_msg(cx->ed, YEW_MSG_WARN, "A-y follows a yank");
        return rl_nothing_to_do();
    }
    k = (y->yank_k + 1U) % y->len;
    return rl_yank_put(cx, yew_yank_at(y, k), y->yank_len, k);
}

/*
 * The replacing commands plan the same way the kills do, but an overlap
 * is DROPPED rather than merged: two carets inside one word both want to
 * rewrite it, and applying the second rewrite to a span the first already
 * replaced would double the edit.  The cursor that reached it first wins.
 */
typedef bool (*RlEditFn)(UnitCtx *u, ByteOff pos, Span *span,
                         Bytebuf *out);

static CmdStatus rl_rewrite(CmdCtx *cx, RlEditFn plan)
{
    Win *win;
    TextBuf *tb;
    UnitCtx u;
    SelEditVec edits = {0};
    EditCtx ec;
    size_t i;

    if (!rl_context(cx, &win, &tb, &u))
        return YEW_CMD_ERR_STATE;
    for (i = 0U; i < win->cs.curs.len; i++) {
        Span span;
        Bytebuf out;
        SelEdit *edit;

        bytebuf_init(&out);
        if (!plan(&u, win->cs.curs.data[i].pos, &span, &out)) {
            bytebuf_free(&out);
            continue;
        }
        if (edits.len != 0U &&
            span.lo < edits.data[edits.len - 1U].span.hi) {
            bytebuf_free(&out);
            continue;
        }
        edit = yew_sel_edit_push(&edits, span);
        bytebuf_append(&edit->replacement, out.data, out.len);
        bytebuf_free(&out);
    }
    if (edits.len == 0U) {
        yew_sel_edits_free(&edits);
        return rl_nothing_to_do();
    }
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    rl_promote_multi(cx, &ec);
    if (!yew_sel_apply_edits(cx, &edits, NULL)) {
        yew_sel_edits_free(&edits);
        return YEW_CMD_ERR_IO;
    }
    yew_sel_edits_free(&edits);
    yew_cset_normalize(tb, &win->cs);
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

static void rl_append_span(Bytebuf *out, const TextBuf *tb, Span span)
{
    Bytebuf part = yew_sel_copy_span(tb, span);

    bytebuf_append(out, part.data, part.len);
    bytebuf_free(&part);
}

/*
 * Transpose the two graphemes around the caret, and step past the pair.
 *
 * At the end of a line there is no grapheme to the right, so the two
 * BEFORE the caret swap and the caret stays -- readline's transpose-char,
 * and the reason the key is usable at all while typing.  The pair never
 * crosses a line end: both graphemes must lie inside the caret's line, so
 * a newline is never one of the two things swapped.
 */
static bool rl_plan_transpose_chars(UnitCtx *u, ByteOff pos, Span *span,
                                    Bytebuf *out)
{
    LineNo line = yew_textbuf_line_of(u->tb, pos);
    Span content = {yew_textbuf_line_span(u->tb, line).lo,
                    rl_line_content_end(u->tb, pos).v};
    ByteOff left;
    ByteOff right;

    if (pos.v <= content.lo || content.hi <= content.lo)
        return false;
    if (pos.v >= content.hi) {
        right = BYTEOFF(content.hi);
        left = yew_grapheme_prev_boundary(u->tb, right);
    } else {
        right = yew_grapheme_next_boundary(u->tb, pos);
        left = pos;
    }
    span->hi = right.v;
    span->lo = yew_grapheme_prev_boundary(u->tb, left).v;
    if (span->lo >= left.v)
        return false;
    rl_append_span(out, u->tb, (Span){left.v, right.v});
    rl_append_span(out, u->tb, (Span){span->lo, left.v});
    return true;
}

/*
 * The nearest whole word at or after `pos`, blanks skipped.  NULL span
 * when the caret is past the last word.
 */
static bool rl_word_forward(UnitCtx *u, ByteOff pos, Span *out)
{
    u64 len = yew_textbuf_len(u->tb);
    Span span;

    if (pos.v >= len)
        return false;
    span = yew_unit_word.span(u, pos, false);
    if (span.hi > pos.v && !rl_span_is_white(u->tb, span)) {
        *out = span;
        return true;
    }
    pos = yew_unit_word.next(u, pos, false);
    if (pos.v >= len)
        return false;
    span = yew_unit_word.span(u, pos, false);
    if (rl_span_is_white(u->tb, span))
        return false;
    *out = span;
    return true;
}

/* The nearest whole word ending at or before `limit`. */
static bool rl_word_backward(UnitCtx *u, ByteOff limit, Span *out)
{
    Span span;
    ByteOff at;

    if (limit.v == 0U)
        return false;
    at = yew_unit_word.prev(u, limit, false);
    span = yew_unit_word.span(u, at, false);
    if (rl_span_is_white(u->tb, span) || span.hi > limit.v)
        return false;
    *out = span;
    return true;
}

/*
 * Drag the word before the caret past the word after it, keeping whatever
 * separates them where it is, and leave the caret after the pair.  The
 * replaced run is exactly [first.lo, second.hi), so the caret lands at
 * second.hi -- the same offset, because a transposition changes no
 * lengths in total.
 */
static bool rl_plan_transpose_words(UnitCtx *u, ByteOff pos, Span *span,
                                    Bytebuf *out)
{
    Span second;
    Span first;

    if (!rl_word_forward(u, pos, &second) ||
        !rl_word_backward(u, BYTEOFF(second.lo), &first))
        return false;
    span->lo = first.lo;
    span->hi = second.hi;
    rl_append_span(out, u->tb, second);
    rl_append_span(out, u->tb, (Span){first.hi, second.lo});
    rl_append_span(out, u->tb, first);
    return true;
}

/*
 * Case the rest of the word under or after the caret.
 *
 * From the CARET, not from the word's start, which is emacs M-u/M-l/M-c:
 * the half of a word already typed keeps the case it was typed with.
 *
 * The planned span STARTS at the caret even when blanks separate it from
 * the word, and those blanks are copied through byte for byte.  That is
 * what carries the caret: yew_cset_adjust biases a cursor sitting at an
 * insertion point to the far side of it, so a caret inside the replaced
 * run lands past the word with no second cursor-placement pass to keep in
 * step with the running edit delta.  A caret left BEFORE the run would
 * simply not move, and the key would case the word without advancing.
 */
static bool rl_plan_case(UnitCtx *u, ByteOff pos, Span *span, Bytebuf *out,
                         YewCaseKind kind, bool capitalize)
{
    Span word;
    Bytebuf source;
    size_t at;
    size_t head;
    bool first = true;

    if (!rl_word_forward(u, pos, &word))
        return false;
    span->lo = pos.v;
    span->hi = word.hi;
    if (span->lo >= span->hi)
        return false;
    head = (size_t)((word.lo > span->lo ? word.lo : span->lo) - span->lo);
    source = yew_sel_copy_span(u->tb, *span);
    bytebuf_append(out, source.data, head);
    at = head;
    while (at < source.len) {
        u32 cp;
        u8 mapped[YEW_CASE_MAX_UTF8];
        size_t used = yew_utf8_decode(source.data + at, source.len - at,
                                      &cp);
        YewCaseKind use = kind;
        size_t n;

        if (capitalize)
            use = first ? YEW_CASE_UPPER : YEW_CASE_LOWER;
        n = yew_case_map_utf8(cp, use, mapped);
        bytebuf_append(out, mapped, n);
        at += used;
        first = false;
    }
    bytebuf_free(&source);
    return true;
}

static bool rl_plan_upper(UnitCtx *u, ByteOff pos, Span *span, Bytebuf *out)
{
    return rl_plan_case(u, pos, span, out, YEW_CASE_UPPER, false);
}

static bool rl_plan_lower(UnitCtx *u, ByteOff pos, Span *span, Bytebuf *out)
{
    return rl_plan_case(u, pos, span, out, YEW_CASE_LOWER, false);
}

static bool rl_plan_cap(UnitCtx *u, ByteOff pos, Span *span, Bytebuf *out)
{
    return rl_plan_case(u, pos, span, out, YEW_CASE_LOWER, true);
}

CmdStatus yew_rl_cmd_transpose_chars(CmdCtx *cx)
{
    return rl_rewrite(cx, rl_plan_transpose_chars);
}

CmdStatus yew_rl_cmd_transpose_words(CmdCtx *cx)
{
    return rl_rewrite(cx, rl_plan_transpose_words);
}

CmdStatus yew_rl_cmd_case_upper_word(CmdCtx *cx)
{
    return rl_rewrite(cx, rl_plan_upper);
}

CmdStatus yew_rl_cmd_case_lower_word(CmdCtx *cx)
{
    return rl_rewrite(cx, rl_plan_lower);
}

CmdStatus yew_rl_cmd_case_cap_word(CmdCtx *cx)
{
    return rl_rewrite(cx, rl_plan_cap);
}
