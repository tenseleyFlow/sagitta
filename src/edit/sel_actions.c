#include "edit/sel_actions.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/select.h"
#include "text/register.h"
#include "unicode/case.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/vec.h"

typedef struct SelEdit {
    Span span;
    Bytebuf replacement;
} SelEdit;

VEC_DECL(SelEditVec, SelEdit);

static bool action_context(CmdCtx *cx, Win **win, TextBuf **tb,
                           Cursor **cursor)
{
    size_t index;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U)
        return false;
    index = cx->win->cs.active != SAG_MC_ACTIVE_NONE ?
                cx->win->cs.active : cx->win->cs.primary;
    if (index >= cx->win->cs.curs.len)
        return false;
    *win = cx->win;
    *tb = cx->win->buf->tb;
    *cursor = &cx->win->cs.curs.data[index];
    return true;
}

static u8 text_byte(const TextBuf *tb, u64 off)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (!sag_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !sag_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        SAG_BUG("selection action: cannot read valid byte offset");
    return bytes[0];
}

static void append_span(Bytebuf *out, const TextBuf *tb, Span span)
{
    TextIter it;
    u64 done = 0U;
    u64 total;

    if (span.lo > span.hi || span.hi > sag_textbuf_len(tb))
        SAG_BUG("selection action: invalid source span");
    total = span.hi - span.lo;
    if (total == 0U)
        return;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        SAG_BUG("selection action: cannot start source iterator");
    while (done < total) {
        const u8 *bytes;
        u64 len;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &bytes, &len))
            SAG_BUG("selection action: truncated source iterator");
        take = len < total - done ? len : total - done;
        bytebuf_append(out, bytes, (size_t)take);
        done += take;
        if (done < total && !sag_textiter_advance(&it, tb))
            SAG_BUG("selection action: truncated source iterator advance");
    }
}

static Bytebuf copy_span(const TextBuf *tb, Span span)
{
    Bytebuf out;

    bytebuf_init(&out);
    append_span(&out, tb, span);
    return out;
}

static void edits_free(SelEditVec *edits)
{
    size_t i;

    for (i = 0U; i < edits->len; i++)
        bytebuf_free(&edits->data[i].replacement);
    SelEditVec_free(edits);
}

static SelEdit *edit_push(SelEditVec *edits, Span span)
{
    SelEdit edit;

    edit.span = span;
    bytebuf_init(&edit.replacement);
    SelEditVec_push(edits, edit);
    return &edits->data[edits->len - 1U];
}

static bool apply_edits(CmdCtx *cx, SelEditVec *edits, ByteOff *first)
{
    EditCtx ec = sag_ed_edit_ctx(cx->ed);
    i64 delta = 0;
    size_t i;

    if (ec.tb == NULL || ec.cset == NULL)
        return false;
    if (first != NULL && edits->len != 0U)
        *first = BYTEOFF(edits->data[0].span.lo);
    for (i = 0U; i < edits->len; i++) {
        SelEdit *edit = &edits->data[i];
        u64 removed = edit->span.hi - edit->span.lo;
        Span now;

        if (delta < 0 && edit->span.lo < (u64)(-delta))
            SAG_BUG("selection action: edit offset underflow");
        now.lo = delta < 0 ? edit->span.lo - (u64)(-delta) :
                            edit->span.lo + (u64)delta;
        now.hi = now.lo + removed;
        if (removed != 0U && !sag_edit_delete(&ec, now)) {
            return false;
        }
        if (edit->replacement.len != 0U &&
            !sag_edit_insert(&ec, BYTEOFF(now.lo), edit->replacement.data,
                             edit->replacement.len)) {
            return false;
        }
        delta += (i64)edit->replacement.len - (i64)removed;
    }
    return true;
}

static void collapse_all(Win *win)
{
    size_t i;

    for (i = 0U; i < win->cs.curs.len; i++) {
        Cursor *cursor = &win->cs.curs.data[i];
        ByteOff start = cursor->pos.v < cursor->anchor.v ? cursor->pos :
                                                             cursor->anchor;

        cursor->pos = start;
        cursor->anchor = start;
        cursor->goal_col = (GCol){0U};
    }
    sag_cset_normalize(win->buf->tb, &win->cs);
}

static void cursor_place(Win *win, ByteOff at)
{
    Cursor *cursor;
    Span line;
    size_t index = win->cs.active != SAG_MC_ACTIVE_NONE ?
                       win->cs.active : win->cs.primary;

    if (index >= win->cs.curs.len)
        index = win->cs.primary;
    cursor = &win->cs.curs.data[index];
    cursor->pos = at;
    cursor->anchor = at;
    line = sag_textbuf_line_span(win->buf->tb,
                                 sag_textbuf_line_of(win->buf->tb, at));
    cursor->goal_col = sag_off_to_gcol(win->buf->tb, line, at);
}

static CmdStatus finish_action(CmdCtx *cx, ByteOff at, bool insert)
{
    cursor_place(cx->win, at);
    collapse_all(cx->win);
    if (insert)
        return sag_mode_enter(cx->ed, SAG_MODE_I);
    return sag_mode_enter(cx->ed, SAG_MODE_L);
}

static void selection_spans(const Win *win, const Cursor *cursor,
                            SagSelSpanVec *spans)
{
    if (win->h.kind == SAG_SEL_RECT) {
        sag_sel_rect_spans(win, cursor, spans);
    } else {
        SagSelSpanVec_push(spans, sag_sel_span(win, cursor));
    }
}

static void all_selection_spans(const Win *win, SagSelSpanVec *spans)
{
    size_t i;

    for (i = 0U; i < win->cs.curs.len; i++)
        selection_spans(win, &win->cs.curs.data[i], spans);
}

static RegType selection_reg_type(SelKind kind)
{
    if (kind == SAG_SEL_LINE)
        return SAG_REG_LINEWISE;
    if (kind == SAG_SEL_RECT)
        return SAG_REG_BLOCKWISE;
    return SAG_REG_CHARWISE;
}

static void regval_from_rect(RegVal *value, const Win *win,
                             const Cursor *cursor)
{
    SagSelSpanVec spans = {0};
    CCol c0 = {0U};
    CCol c1 = {0U};
    Span ignored;
    LineNo first;
    size_t i;

    sag_sel_rect_spans(win, cursor, &spans);
    first = sag_textbuf_line_of(win->buf->tb,
                               cursor->pos.v < cursor->anchor.v ?
                                   cursor->pos : cursor->anchor);
    (void)sag_sel_rect_row(win, cursor, first, &ignored, &c0, &c1);
    value->type = SAG_REG_BLOCKWISE;
    value->width = (u32)(c1.v - c0.v);
    value->ragged = false;
    value->bytes.len = 0U;
    value->rows.len = 0U;
    for (i = 0U; i < spans.len; i++) {
        Span row;
        u64 copied;

        row.lo = value->bytes.len;
        append_span(&value->bytes, win->buf->tb, spans.data[i]);
        copied = value->bytes.len - row.lo;
        if (copied == 0U && value->width != 0U)
            value->ragged = true;
        row.hi = value->bytes.len;
        SagRegRowVec_push(&value->rows, row);
    }
    SagSelSpanVec_free(&spans);
}

static void capture_selection(RegVal *value, const Win *win)
{
    RegType type = selection_reg_type(win->h.kind);
    size_t i;

    if (type == SAG_REG_BLOCKWISE) {
        for (i = 0U; i < win->cs.curs.len; i++) {
            RegVal part;
            size_t row;

            sag_regval_init(&part);
            regval_from_rect(&part, win, &win->cs.curs.data[i]);
            if (value->rows.len != 0U && part.width != value->width)
                value->ragged = true;
            if (part.width > value->width)
                value->width = part.width;
            value->type = SAG_REG_BLOCKWISE;
            value->ragged = value->ragged || part.ragged;
            for (row = 0U; row < part.rows.len; row++) {
                Span source = part.rows.data[row];
                Span target = {value->bytes.len, value->bytes.len};

                bytebuf_append(&value->bytes, part.bytes.data + source.lo,
                               (size_t)(source.hi - source.lo));
                target.hi = value->bytes.len;
                SagRegRowVec_push(&value->rows, target);
            }
            sag_regval_free(&part);
        }
    } else {
        for (i = 0U; i < win->cs.curs.len; i++) {
            RegVal part;

            sag_regval_init(&part);
            sag_regval_from_span(&part, win->buf->tb,
                                 sag_sel_span(win, &win->cs.curs.data[i]),
                                 type, &win->buf->meta);
            bytebuf_append(&value->bytes, part.bytes.data, part.bytes.len);
            value->type = (u8)type;
            sag_regval_free(&part);
        }
    }
}

CmdStatus sag_sel_cmd_yank(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    RegVal value;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)tb;
    (void)cursor;
    sag_regval_init(&value);
    capture_selection(&value, win);
    sag_reg_yank(&cx->ed->regs, 0U, &value);
    sag_regval_free(&value);
    return SAG_CMD_OK;
}

static CmdStatus delete_or_change(CmdCtx *cx, bool change)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    RegVal value;
    SagSelSpanVec spans = {0};
    SelEditVec edits = {0};
    ByteOff first;
    size_t i;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    sag_regval_init(&value);
    capture_selection(&value, win);
    all_selection_spans(win, &spans);
    for (i = 0U; i < spans.len; i++)
        (void)edit_push(&edits, spans.data[i]);
    if (!apply_edits(cx, &edits, &first)) {
        edits_free(&edits);
        SagSelSpanVec_free(&spans);
        sag_regval_free(&value);
        return SAG_CMD_ERR_IO;
    }
    sag_reg_delete(&cx->ed->regs, 0U, &value);
    edits_free(&edits);
    if (win->h.kind == SAG_SEL_RECT && change) {
        /* Rectangular change keeps one insertion caret per affected row. */
        Cursor primary = {first, {0U}, first};
        u64 removed = 0U;

        sag_cset_remove_all_but_primary(&win->cs);
        win->cs.curs.data[0] = primary;
        for (i = 1U; i < spans.len; i++) {
            Cursor extra;
            Span original = spans.data[i];

            removed += spans.data[i - 1U].hi - spans.data[i - 1U].lo;
            extra.pos = BYTEOFF(original.lo - removed);
            extra.anchor = extra.pos;
            extra.goal_col = (GCol){0U};
            (void)sag_cset_add(&win->cs, extra);
        }
        sag_cset_normalize(tb, &win->cs);
        SagSelSpanVec_free(&spans);
        sag_regval_free(&value);
        return sag_mode_enter(cx->ed, SAG_MODE_I);
    }
    SagSelSpanVec_free(&spans);
    sag_regval_free(&value);
    return finish_action(cx, first, change);
}

CmdStatus sag_sel_cmd_delete(CmdCtx *cx)
{
    return delete_or_change(cx, false);
}

CmdStatus sag_sel_cmd_change(CmdCtx *cx)
{
    return delete_or_change(cx, true);
}

static CmdStatus change_case(CmdCtx *cx, SagCaseKind kind)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SagSelSpanVec spans = {0};
    SelEditVec edits = {0};
    ByteOff first;
    size_t i;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    all_selection_spans(win, &spans);
    for (i = 0U; i < spans.len; i++) {
        SelEdit *edit = edit_push(&edits, spans.data[i]);
        Bytebuf source = copy_span(tb, spans.data[i]);
        size_t at = 0U;

        while (at < source.len) {
            u32 cp;
            u8 mapped[SAG_CASE_MAX_UTF8];
            size_t used = sag_utf8_decode(source.data + at,
                                          source.len - at, &cp);
            size_t n = sag_case_map_utf8(cp, kind, mapped);

            bytebuf_append(&edit->replacement, mapped, n);
            at += used;
        }
        bytebuf_free(&source);
    }
    if (!apply_edits(cx, &edits, &first)) {
        edits_free(&edits);
        SagSelSpanVec_free(&spans);
        return SAG_CMD_ERR_IO;
    }
    edits_free(&edits);
    SagSelSpanVec_free(&spans);
    return finish_action(cx, first, false);
}

CmdStatus sag_sel_cmd_case_upper(CmdCtx *cx)
{
    return change_case(cx, SAG_CASE_UPPER);
}

CmdStatus sag_sel_cmd_case_lower(CmdCtx *cx)
{
    return change_case(cx, SAG_CASE_LOWER);
}

CmdStatus sag_sel_cmd_case_toggle(CmdCtx *cx)
{
    return change_case(cx, SAG_CASE_TOGGLE);
}

static void covered_lines(const Win *win, const Cursor *cursor,
                          LineNo *first, LineNo *last)
{
    const TextBuf *tb = win->buf->tb;
    u64 lo = cursor->pos.v < cursor->anchor.v ? cursor->pos.v :
                                                   cursor->anchor.v;
    u64 hi = cursor->pos.v > cursor->anchor.v ? cursor->pos.v :
                                                   cursor->anchor.v;

    *first = sag_textbuf_line_of(tb, BYTEOFF(lo));
    *last = sag_textbuf_line_of(tb, BYTEOFF(hi));
}

static CmdStatus indent_action(CmdCtx *cx, bool dedent)
{
    static const u8 spaces[64] = {
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
    };
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SelEditVec edits = {0};
    LineNo first;
    LineNo last;
    bool *covered;
    u64 line_count;
    u32 tabwidth;
    u64 line;
    size_t ci;
    ByteOff collapse;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    line_count = sag_textbuf_line_count(tb);
    covered = sag_xcalloc((size_t)line_count, sizeof(*covered));
    first = LINENO(line_count);
    last = LINENO(0U);
    for (ci = 0U; ci < win->cs.curs.len; ci++) {
        LineNo lo;
        LineNo hi;

        covered_lines(win, &win->cs.curs.data[ci], &lo, &hi);
        if (lo.v < first.v)
            first = lo;
        if (hi.v > last.v)
            last = hi;
        for (line = lo.v; line <= hi.v; line++)
            covered[line] = true;
    }
    tabwidth = win->buf->tabwidth == 0U ? 4U : win->buf->tabwidth;
    for (line = first.v; line <= last.v; line++) {
        Span span = sag_textbuf_line_span(tb, LINENO(line));
        SelEdit *edit;

        if (!covered[line])
            continue;

        if (dedent) {
            u64 end = span.lo;

            if (end < span.hi && text_byte(tb, end) == (u8)'\t') {
                end++;
            } else {
                u32 n = 0U;
                while (end < span.hi && n < tabwidth &&
                       text_byte(tb, end) == (u8)' ') {
                    end++;
                    n++;
                }
            }
            if (end != span.lo)
                (void)edit_push(&edits, (Span){span.lo, end});
        } else {
            edit = edit_push(&edits, (Span){span.lo, span.lo});
            if (span.lo < span.hi && text_byte(tb, span.lo) == (u8)'\t') {
                bytebuf_push_u8(&edit->replacement, (u8)'\t');
            } else {
                u32 left = tabwidth;
                while (left != 0U) {
                    u32 take = left < sizeof(spaces) ? left :
                                                         (u32)sizeof(spaces);
                    bytebuf_append(&edit->replacement, spaces, take);
                    left -= take;
                }
            }
        }
    }
    collapse = sag_textbuf_line_start(tb, first);
    if (!apply_edits(cx, &edits, NULL)) {
        free(covered);
        edits_free(&edits);
        return SAG_CMD_ERR_IO;
    }
    free(covered);
    edits_free(&edits);
    return finish_action(cx, collapse, false);
}

CmdStatus sag_sel_cmd_indent(CmdCtx *cx)
{
    return indent_action(cx, false);
}

CmdStatus sag_sel_cmd_dedent(CmdCtx *cx)
{
    return indent_action(cx, true);
}

static ByteOff line_content_end(const TextBuf *tb, LineNo line)
{
    Span span = sag_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (end.v != span.lo && text_byte(tb, end.v - 1U) == (u8)'\n') {
        end.v--;
        if (end.v != span.lo && text_byte(tb, end.v - 1U) == (u8)'\r')
            end.v--;
    }
    return end;
}

static bool join_punct(u8 byte)
{
    return byte == (u8)')' || byte == (u8)'.' || byte == (u8)',' ||
           byte == (u8)';';
}

CmdStatus sag_sel_cmd_join(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SelEditVec edits = {0};
    ByteOff collapse = BYTEOFF(0U);
    size_t i;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    for (i = 0U; i < win->cs.curs.len; i++) {
        LineNo first;
        LineNo last;
        Span joined;
        Bytebuf source;
        SelEdit *edit;
        size_t at = 0U;
        bool pending_space = false;

        covered_lines(win, &win->cs.curs.data[i], &first, &last);
        if (first.v == last.v)
            continue;
        joined.lo = sag_textbuf_line_start(tb, first).v;
        joined.hi = line_content_end(tb, last).v;
        if (edits.len != 0U && joined.lo < edits.data[edits.len - 1U].span.hi)
            continue;
        source = copy_span(tb, joined);
        edit = edit_push(&edits, joined);
        while (at < source.len) {
            u8 byte = source.data[at++];

            if (byte == (u8)' ' || byte == (u8)'\t' ||
                byte == (u8)'\r' || byte == (u8)'\n') {
                pending_space = edit->replacement.len != 0U;
                continue;
            }
            if (pending_space && !join_punct(byte))
                bytebuf_push_u8(&edit->replacement, (u8)' ');
            pending_space = false;
            bytebuf_push_u8(&edit->replacement, byte);
        }
        bytebuf_free(&source);
        if (edits.len == 1U)
            collapse = BYTEOFF(joined.lo);
    }
    if (edits.len == 0U)
        collapse = win->cs.curs.data[win->cs.primary].pos;
    if (!apply_edits(cx, &edits, NULL)) {
        edits_free(&edits);
        return SAG_CMD_ERR_IO;
    }
    edits_free(&edits);
    return finish_action(cx, collapse, false);
}

CmdStatus sag_sel_cmd_replace_char(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SagSelSpanVec spans = {0};
    SelEditVec edits = {0};
    ByteOff first;
    size_t i;

    if (!action_context(cx, &win, &tb, &cursor) || cx->sarg == NULL ||
        cx->sarg_len == 0U ||
        sag_gb_next_bytes((const u8 *)cx->sarg, cx->sarg_len, 0U) !=
            cx->sarg_len)
        return SAG_CMD_ERR_ARG;
    (void)cursor;
    all_selection_spans(win, &spans);
    for (i = 0U; i < spans.len; i++) {
        SelEdit *edit = edit_push(&edits, spans.data[i]);
        Bytebuf source = copy_span(tb, spans.data[i]);
        size_t at = 0U;

        while (at < source.len) {
            size_t next = sag_gb_next_bytes(source.data, source.len, at);

            bytebuf_append(&edit->replacement, cx->sarg, cx->sarg_len);
            at = next;
        }
        bytebuf_free(&source);
    }
    if (!apply_edits(cx, &edits, &first)) {
        edits_free(&edits);
        SagSelSpanVec_free(&spans);
        return SAG_CMD_ERR_IO;
    }
    edits_free(&edits);
    SagSelSpanVec_free(&spans);
    return finish_action(cx, first, false);
}

static CmdStatus shift_char_or_line(CmdCtx *cx, bool right)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SelEditVec edits = {0};
    ByteOff collapse = BYTEOFF(0U);
    size_t i;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    for (i = 0U; i < win->cs.curs.len; i++) {
        Cursor *item = &win->cs.curs.data[i];
        Span selected = sag_sel_span(win, item);
        Span whole;
        Span neighbor;
        SelEdit *edit;

        if (selected.lo == selected.hi)
            continue;
        if (win->h.kind == SAG_SEL_LINE) {
            LineNo first;
            LineNo last;
            u64 line_count = sag_textbuf_line_count(tb);

            covered_lines(win, item, &first, &last);
            if ((!right && first.v == 0U) ||
                (right && last.v + 1U >= line_count))
                continue;
            neighbor = right ?
                sag_textbuf_line_span(tb, LINENO(last.v + 1U)) :
                sag_textbuf_line_span(tb, LINENO(first.v - 1U));
        } else if (right) {
            if (selected.hi >= sag_textbuf_len(tb))
                continue;
            neighbor = (Span){selected.hi,
                sag_grapheme_next_boundary(tb, BYTEOFF(selected.hi)).v};
        } else {
            if (selected.lo == 0U)
                continue;
            neighbor = (Span){sag_grapheme_prev_boundary(
                                  tb, BYTEOFF(selected.lo)).v,
                              selected.lo};
        }
        whole.lo = right ? selected.lo : neighbor.lo;
        whole.hi = right ? neighbor.hi : selected.hi;
        if (edits.len != 0U && whole.lo < edits.data[edits.len - 1U].span.hi)
            continue;
        edit = edit_push(&edits, whole);
        if (right) {
            append_span(&edit->replacement, tb, neighbor);
            append_span(&edit->replacement, tb, selected);
        } else {
            append_span(&edit->replacement, tb, selected);
            append_span(&edit->replacement, tb, neighbor);
        }
        if (edits.len == 1U)
            collapse = BYTEOFF(right ?
                selected.lo + (neighbor.hi - neighbor.lo) : neighbor.lo);
    }
    if (edits.len == 0U)
        collapse = win->cs.curs.data[win->cs.primary].pos;
    if (!apply_edits(cx, &edits, NULL)) {
        edits_free(&edits);
        return SAG_CMD_ERR_IO;
    }
    edits_free(&edits);
    return finish_action(cx, collapse, false);
}

static CmdStatus shift_rect(CmdCtx *cx, bool right)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SagSelSpanVec spans = {0};
    SelEditVec edits = {0};
    ByteOff first;
    size_t i;

    if (!action_context(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    all_selection_spans(win, &spans);
    for (i = 0U; i < spans.len; i++) {
        Span selected = spans.data[i];
        Span neighbor;
        Span whole;
        SelEdit *edit;
        LineNo line = sag_textbuf_line_of(tb, BYTEOFF(selected.lo));
        ByteOff end = line_content_end(tb, line);

        if (selected.lo == selected.hi)
            continue;
        if (right) {
            if (selected.hi >= end.v)
                continue;
            neighbor = (Span){selected.hi,
                sag_grapheme_next_boundary(tb, BYTEOFF(selected.hi)).v};
            whole = (Span){selected.lo, neighbor.hi};
        } else {
            ByteOff start = sag_textbuf_line_start(tb, line);
            if (selected.lo <= start.v)
                continue;
            neighbor = (Span){sag_grapheme_prev_boundary(
                                  tb, BYTEOFF(selected.lo)).v,
                              selected.lo};
            whole = (Span){neighbor.lo, selected.hi};
        }
        edit = edit_push(&edits, whole);
        if (right) {
            append_span(&edit->replacement, tb, neighbor);
            append_span(&edit->replacement, tb, selected);
        } else {
            append_span(&edit->replacement, tb, selected);
            append_span(&edit->replacement, tb, neighbor);
        }
    }
    first = BYTEOFF(spans.len == 0U ? cursor->pos.v : spans.data[0].lo);
    if (!apply_edits(cx, &edits, NULL)) {
        edits_free(&edits);
        SagSelSpanVec_free(&spans);
        return SAG_CMD_ERR_IO;
    }
    edits_free(&edits);
    SagSelSpanVec_free(&spans);
    return finish_action(cx, first, false);
}

CmdStatus sag_sel_cmd_shift_left(CmdCtx *cx)
{
    if (cx != NULL && cx->win != NULL && cx->win->h.kind == SAG_SEL_RECT)
        return shift_rect(cx, false);
    return shift_char_or_line(cx, false);
}

CmdStatus sag_sel_cmd_shift_right(CmdCtx *cx)
{
    if (cx != NULL && cx->win != NULL && cx->win->h.kind == SAG_SEL_RECT)
        return shift_rect(cx, true);
    return shift_char_or_line(cx, true);
}

static CmdStatus rect_carets(CmdCtx *cx, bool append)
{
    static const u8 spaces[64] = {
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
    };
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    SelEditVec pads = {0};
    SagCursorVec carets = {0};
    u32 tabwidth;
    size_t i;
    size_t ci;
    i64 delta = 0;

    if (!action_context(cx, &win, &tb, &cursor) ||
        win->h.kind != SAG_SEL_RECT)
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    tabwidth = win->buf->tabwidth == 0U ? 4U : win->buf->tabwidth;
    for (ci = 0U; ci < win->cs.curs.len; ci++) {
        Cursor *item = &win->cs.curs.data[ci];
        SagSelSpanVec spans = {0};
        CCol c0 = {0U};
        CCol c1 = {0U};
        CCol target;
        Span ignored;
        LineNo first_line = sag_textbuf_line_of(tb,
            item->pos.v < item->anchor.v ? item->pos : item->anchor);

        sag_sel_rect_spans(win, item, &spans);
        if (!sag_sel_rect_row(win, item, first_line, &ignored, &c0, &c1)) {
            SagSelSpanVec_free(&spans);
            continue;
        }
        target = append ? c1 : c0;
        for (i = 0U; i < spans.len; i++) {
            LineNo line = LINENO(first_line.v + i);
            Span row = sag_textbuf_line_span(tb, line);
            ByteOff end = line_content_end(tb, line);
            CCol end_col = sag_off_to_ccol(tb, row, end, tabwidth);
            u64 pad = target.v > end_col.v ? target.v - end_col.v : 0U;
            Cursor caret;

            if (pad != 0U) {
                SelEdit *edit = edit_push(&pads, (Span){end.v, end.v});
                u64 left = pad;
                while (left != 0U) {
                    u64 take = left < sizeof(spaces) ? left : sizeof(spaces);
                    bytebuf_append(&edit->replacement, spaces, (size_t)take);
                    left -= take;
                }
                caret.pos = BYTEOFF(end.v + (u64)delta + pad);
                delta += (i64)pad;
            } else {
                ByteOff at = append ? BYTEOFF(spans.data[i].hi) :
                                      BYTEOFF(spans.data[i].lo);
                caret.pos = BYTEOFF(at.v + (u64)delta);
            }
            caret.anchor = caret.pos;
            caret.goal_col = (GCol){0U};
            SagCursorVec_push(&carets, caret);
        }
        SagSelSpanVec_free(&spans);
    }
    if (!apply_edits(cx, &pads, NULL)) {
        edits_free(&pads);
        SagCursorVec_free(&carets);
        return SAG_CMD_ERR_IO;
    }
    edits_free(&pads);
    if (carets.len == 0U) {
        SagCursorVec_free(&carets);
        return SAG_CMD_ERR_STATE;
    }
    sag_cset_remove_all_but_primary(&win->cs);
    win->cs.curs.data[0] = carets.data[0];
    for (i = 1U; i < carets.len; i++)
        (void)sag_cset_add(&win->cs, carets.data[i]);
    SagCursorVec_free(&carets);
    sag_cset_normalize(tb, &win->cs);
    return sag_mode_enter(cx->ed, SAG_MODE_I);
}

CmdStatus sag_sel_cmd_rect_insert(CmdCtx *cx)
{
    return rect_carets(cx, false);
}

CmdStatus sag_sel_cmd_rect_append(CmdCtx *cx)
{
    return rect_carets(cx, true);
}
