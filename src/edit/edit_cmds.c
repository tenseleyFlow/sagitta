#include "edit/edit_cmds.h"

#include <stdlib.h>
#include <string.h>

#include "edit/block.h"
#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/motion.h"
#include "edit/word.h"
#include "ui/message.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "util/log.h"

static bool edit_window(CmdCtx *cx, Win **win, TextBuf **tb, Cursor **cursor)
{
    u32 cursor_index;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U ||
        (size_t)cx->win->cs.primary >= cx->win->cs.curs.len)
        return false;
    cursor_index = cx->win->cs.primary;
    if (cx->win->cs.active != SAG_MC_ACTIVE_NONE) {
        if ((size_t)cx->cursor_index >= cx->win->cs.curs.len ||
            cx->cursor_index != cx->win->cs.active)
            return false;
        cursor_index = cx->cursor_index;
    }
    *win = cx->win;
    *tb = cx->win->buf->tb;
    *cursor = &cx->win->cs.curs.data[cursor_index];
    return true;
}

static void damage_offsets(Ed *ed, ByteOff first, ByteOff second)
{
    Win *win;
    TextBuf *tb;
    LineNo lo;
    LineNo hi;
    LineNo top;
    LineNo bottom;
    u16 row_lo;
    u16 row_hi;

    if (ed == NULL || ed->win == NULL || ed->win->buf == NULL ||
        ed->win->buf->tb == NULL)
        return;
    win = ed->win;
    tb = win->buf->tb;
    lo = sag_textbuf_line_of(tb, first.v < second.v ? first : second);
    hi = sag_textbuf_line_of(tb, first.v > second.v ? first : second);
    if (win->vp.wrap) {
        sag_ed_damage_document(ed);
        return;
    }
    top = sag_win_view_top(win);
    bottom = sag_vp_last_visible_line(win);
    if (hi.v < top.v || lo.v > bottom.v)
        return;
    if (lo.v < top.v)
        lo = top;
    if (hi.v > bottom.v)
        hi = bottom;
    if (!sag_win_view_row(win, lo, &row_lo) ||
        !sag_win_view_row(win, hi, &row_hi))
        return;
    sag_ed_damage_rows(ed, row_lo, (u16)(row_hi + 1U));
}

static void cursor_set_bounds(const CursorSet *cs, ByteOff *lo, ByteOff *hi)
{
    size_t i;

    *lo = cs->curs.data[0].pos;
    *hi = cs->curs.data[0].pos;
    for (i = 0U; i < cs->curs.len; i++) {
        const Cursor *cursor = &cs->curs.data[i];

        if (cursor->pos.v < lo->v)
            *lo = cursor->pos;
        if (cursor->anchor.v < lo->v)
            *lo = cursor->anchor;
        if (cursor->pos.v > hi->v)
            *hi = cursor->pos;
        if (cursor->anchor.v > hi->v)
            *hi = cursor->anchor;
    }
}

static void cursor_place(const TextBuf *tb, Cursor *cursor, ByteOff pos)
{
    Span line;

    cursor->pos = pos;
    cursor->anchor = pos;
    line = sag_textbuf_line_span(tb, sag_textbuf_line_of(tb, pos));
    cursor->goal_col = sag_off_to_gcol(tb, line, pos);
}

static void finish_direct_motion(CmdCtx *cx, Cursor *cursor,
                                 ByteOff anchor, ByteOff old_pos)
{
    if (cx->ed->mode != SAG_MODE_H)
        return;
    cursor->anchor = anchor;
    damage_offsets(cx->ed, anchor,
                   old_pos.v < cursor->pos.v ? old_pos : cursor->pos);
    damage_offsets(cx->ed, anchor,
                   old_pos.v > cursor->pos.v ? old_pos : cursor->pos);
}

static ByteOff line_content_end(const TextBuf *tb, LineNo line)
{
    Span span = sag_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < sag_textbuf_line_count(tb))
        end = sag_grapheme_prev_boundary(tb, end);
    return end;
}

static u8 byte_at(const TextBuf *tb, ByteOff at)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (!sag_textiter_begin(&it, tb, at) ||
        !sag_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        SAG_BUG("editor command: cannot inspect valid byte offset");
    return bytes[0];
}

static bool ascii_blank_cluster(const TextBuf *tb, ByteOff at, ByteOff next)
{
    u8 byte;

    if (next.v != at.v + 1U)
        return false;
    byte = byte_at(tb, at);
    return byte == (u8)' ' || byte == (u8)'\t';
}

static CmdStatus move_vertical(CmdCtx *cx, bool down, u64 rows)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff anchor;
    ByteOff old_pos;
    u64 i;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    if (win->vp.wrap) {
        i32 amount;

        if (rows > (u64)INT32_MAX)
            amount = INT32_MAX;
        else
            amount = (i32)rows;
        if (!down)
            amount = -amount;
        (void)sag_vp_move_display(win, amount);
    } else {
        for (i = 0U; i < rows; i++) {
            if (down)
                sag_cursor_down(tb, cursor);
            else
                sag_cursor_up(tb, cursor);
        }
    }
    if (cx->ed->mode == SAG_MODE_H) {
        cursor->anchor = anchor;
        damage_offsets(cx->ed, anchor,
                       old_pos.v < cursor->pos.v ? old_pos : cursor->pos);
        damage_offsets(cx->ed, anchor,
                       old_pos.v > cursor->pos.v ? old_pos : cursor->pos);
    }
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_buf_home(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    sag_cursor_buf_home(tb, cursor);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_buf_end(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    if (cx->count_given) {
        u64 lines = sag_textbuf_line_count(tb);
        u64 requested = cx->count == 0U ? 0U : (u64)cx->count - 1U;
        LineNo line = LINENO(requested < lines ? requested : lines - 1U);

        cursor_place(tb, cursor, sag_textbuf_line_start(tb, line));
    } else {
        sag_cursor_buf_end(tb, cursor);
    }
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_home(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff anchor;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    sag_cursor_line_home(tb, cursor);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_end(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff anchor;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    sag_cursor_line_end(tb, cursor);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_up(CmdCtx *cx)
{
    return move_vertical(cx, false, 1U);
}

CmdStatus sag_edit_cmd_move_line_down(CmdCtx *cx)
{
    return move_vertical(cx, true, 1U);
}

CmdStatus sag_edit_cmd_move_line_first_nonblank(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    ByteOff pos;
    ByteOff end;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    line = sag_textbuf_line_of(tb, cursor->pos);
    pos = sag_textbuf_line_start(tb, line);
    end = line_content_end(tb, line);
    while (pos.v < end.v) {
        ByteOff next = sag_grapheme_next_boundary(tb, pos);

        if (!ascii_blank_cluster(tb, pos, next))
            break;
        pos = next;
    }
    cursor_place(tb, cursor, pos);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_last_nonblank(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    ByteOff pos;
    ByteOff end;
    ByteOff last;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    line = sag_textbuf_line_of(tb, cursor->pos);
    pos = sag_textbuf_line_start(tb, line);
    end = line_content_end(tb, line);
    last = pos;
    while (pos.v < end.v) {
        ByteOff next = sag_grapheme_next_boundary(tb, pos);

        if (!ascii_blank_cluster(tb, pos, next))
            last = pos;
        pos = next;
    }
    cursor_place(tb, cursor, last);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_half_page_up(CmdCtx *cx)
{
    u64 rows;

    if (cx == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    rows = (u64)cx->win->vp.rows / 2U;
    return move_vertical(cx, false, rows == 0U ? 1U : rows);
}

CmdStatus sag_edit_cmd_move_line_half_page_down(CmdCtx *cx)
{
    u64 rows;

    if (cx == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    rows = (u64)cx->win->vp.rows / 2U;
    return move_vertical(cx, true, rows == 0U ? 1U : rows);
}

CmdStatus sag_edit_cmd_view_page_up(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_vp_page(cx->win, -1);
    sag_vp_push_cursor(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_page_down(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_vp_page(cx->win, 1);
    sag_vp_push_cursor(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

static CmdStatus view_scroll(CmdCtx *cx, i32 rows)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_vp_scroll(cx->win, rows);
    sag_vp_push_cursor(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_scroll_up(CmdCtx *cx)
{
    return view_scroll(cx, -1);
}

CmdStatus sag_edit_cmd_view_scroll_down(CmdCtx *cx)
{
    return view_scroll(cx, 1);
}

CmdStatus sag_edit_cmd_view_half_page_up(CmdCtx *cx)
{
    i32 rows;

    if (cx == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    rows = (i32)(cx->win->vp.rows / 2U);
    return view_scroll(cx, rows == 0 ? -1 : -rows);
}

CmdStatus sag_edit_cmd_view_half_page_down(CmdCtx *cx)
{
    i32 rows;

    if (cx == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    rows = (i32)(cx->win->vp.rows / 2U);
    return view_scroll(cx, rows == 0 ? 1 : rows);
}

CmdStatus sag_edit_cmd_view_center(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_vp_center(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_top(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_vp_top(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_bottom(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_vp_bottom(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_goto_line(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    u64 line_count;
    u64 requested;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    line_count = sag_textbuf_line_count(tb);
    requested = cx->count == 0U ? 0U : (u64)cx->count - 1U;
    if (requested >= line_count)
        requested = line_count - 1U;
    cursor_place(tb, cursor, sag_textbuf_line_start(tb, LINENO(requested)));
    win->wrap_goal_valid = false;
    sag_vp_center(win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_toggle_wrap(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    cx->win->vp.wrap = !cx->win->vp.wrap;
    cx->win->vp.left = (CCol){0U};
    cx->win->vp.top_sub = 0U;
    cx->win->wrap_goal_valid = false;
    sag_vp_invalidate(cx->win);
    sag_vp_clamp(cx->win);
    sag_vp_follow(cx->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_view_number_style(CmdCtx *cx)
{
    NumStyle style;

    if (cx == NULL || cx->win == NULL || cx->ed == NULL ||
        cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->sarg_len == 4U && memcmp(cx->sarg, "none", 4U) == 0)
        style = SAG_NUM_NONE;
    else if (cx->sarg_len == 3U && memcmp(cx->sarg, "abs", 3U) == 0)
        style = SAG_NUM_ABS;
    else if (cx->sarg_len == 3U && memcmp(cx->sarg, "rel", 3U) == 0)
        style = SAG_NUM_REL;
    else if (cx->sarg_len == 6U && memcmp(cx->sarg, "hybrid", 6U) == 0)
        style = SAG_NUM_HYBRID;
    else
        return SAG_CMD_ERR_ARG;
    cx->win->number_style = style;
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_message_expand(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (!sag_msg_expand(cx->ed))
        return SAG_CMD_ERR_STATE;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_ui_cancel(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    (void)sag_msg_dismiss_overlay(cx->ed);
    if (cx->ed->prompt != SAG_PROMPT_NONE)
        sag_ed_prompt(cx->ed, SAG_PROMPT_NONE);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_char_prev(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    sag_cursor_left(tb, cursor);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_char_next(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    sag_cursor_right(tb, cursor);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

typedef enum {
    UNIT_NEXT,
    UNIT_PREV,
    UNIT_HOME,
    UNIT_END
} UnitMotion;

static CmdStatus move_unit(CmdCtx *cx, UnitMotion motion, bool alt)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    const UnitOps *ops;
    UnitCtx u;
    ByteOff pos;
    GCol vertical_goal;
    bool line_vertical;
    bool unselected;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    ops = cx->ed->mode == SAG_MODE_H ? win->h.unit :
          sag_unit_of_mode(cx->ed->mode);
    if (ops == NULL)
        return SAG_CMD_ERR_STATE;
    line_vertical = ops == &sag_unit_line &&
                    (motion == UNIT_NEXT || motion == UNIT_PREV);
    vertical_goal = cursor->goal_col;
    old_pos = cursor->pos;
    unselected = cursor->anchor.v == cursor->pos.v;
    if (line_vertical && win->vp.wrap && !win->wrap_goal_valid) {
        win->wrap_goal = sag_vp_display_col(win, cursor->pos);
        win->wrap_goal_valid = true;
    }
    u = (UnitCtx){tb, win->buf, win};
    switch (motion) {
    case UNIT_NEXT:
        pos = ops->next(&u, cursor->pos, alt);
        break;
    case UNIT_PREV:
        pos = ops->prev(&u, cursor->pos, alt);
        break;
    case UNIT_HOME:
        pos = ops->home(&u, cursor->pos, alt);
        break;
    case UNIT_END:
        pos = ops->end(&u, cursor->pos, alt);
        break;
    default:
        return SAG_CMD_ERR_ARG;
    }
    if (cx->ed->mode == SAG_MODE_H) {
        cursor->pos = pos;
        if (line_vertical)
            cursor->goal_col = vertical_goal;
        else
            cursor->goal_col = sag_off_to_gcol(
                tb, sag_textbuf_line_span(tb, sag_textbuf_line_of(tb, pos)),
                pos);
        win->wrap_goal_valid = false;
        damage_offsets(cx->ed, cursor->anchor,
                       old_pos.v < cursor->pos.v ? old_pos : cursor->pos);
        damage_offsets(cx->ed, cursor->anchor,
                       old_pos.v > cursor->pos.v ? old_pos : cursor->pos);
    } else if (line_vertical) {
        cursor->pos = pos;
        if (unselected)
            cursor->anchor = pos;
        cursor->goal_col = vertical_goal;
        if (!win->vp.wrap)
            win->wrap_goal_valid = false;
    } else {
        cursor_place(tb, cursor, pos);
        win->wrap_goal_valid = false;
    }
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_unit_next(CmdCtx *cx)
{
    return move_unit(cx, UNIT_NEXT, false);
}

CmdStatus sag_edit_cmd_move_unit_prev(CmdCtx *cx)
{
    return move_unit(cx, UNIT_PREV, false);
}

CmdStatus sag_edit_cmd_move_unit_home(CmdCtx *cx)
{
    return move_unit(cx, UNIT_HOME, false);
}

CmdStatus sag_edit_cmd_move_unit_end(CmdCtx *cx)
{
    return move_unit(cx, UNIT_END, false);
}

CmdStatus sag_edit_cmd_move_unit_next_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_NEXT, true);
}

CmdStatus sag_edit_cmd_move_unit_prev_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_PREV, true);
}

CmdStatus sag_edit_cmd_move_unit_home_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_HOME, true);
}

CmdStatus sag_edit_cmd_move_unit_end_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_END, true);
}

static CmdStatus move_block_match(CmdCtx *cx, bool next)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    UnitCtx u;
    ByteOff pos;
    ByteOff anchor;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    u = (UnitCtx){tb, win->buf, win};
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    if (!sag_block_match(&u, cursor->pos, next, &pos)) {
        sag_msg(cx->ed, SAG_MSG_INFO, "no enclosing delimiter");
        return SAG_CMD_OK;
    }
    cursor_place(tb, cursor, pos);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_block_match_prev(CmdCtx *cx)
{
    return move_block_match(cx, false);
}

CmdStatus sag_edit_cmd_move_block_match_next(CmdCtx *cx)
{
    return move_block_match(cx, true);
}

static CmdStatus move_word_sub(CmdCtx *cx, bool next)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    UnitCtx u;
    ByteOff pos;
    ByteOff anchor;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    u = (UnitCtx){tb, win->buf, win};
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    pos = next ? sag_word_sub_next(&u, cursor->pos)
               : sag_word_sub_prev(&u, cursor->pos);
    cursor_place(tb, cursor, pos);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_word_sub_prev(CmdCtx *cx)
{
    return move_word_sub(cx, false);
}

CmdStatus sag_edit_cmd_move_word_sub_next(CmdCtx *cx)
{
    return move_word_sub(cx, true);
}

static void select_span(const TextBuf *tb, Cursor *cursor, Span span)
{
    cursor->anchor = BYTEOFF(span.lo);
    cursor->pos = BYTEOFF(span.hi);
    cursor->goal_col = sag_off_to_gcol(
        tb, sag_textbuf_line_span(tb, sag_textbuf_line_of(tb, cursor->pos)),
        cursor->pos);
}

CmdStatus sag_edit_cmd_sel_unit_expand(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    UnitCtx u;
    bool changed = false;
    size_t i;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    u = (UnitCtx){tb, win->buf, win};
    for (i = 0U; i < win->cs.curs.len; i++) {
        Cursor *item = &win->cs.curs.data[i];
        SelStack *stack = &win->cs.selstacks.data[i];
        Span span;
        ByteOff seed = stack->n == 0U ? item->pos :
                                            BYTEOFF(stack->s[0].lo);

        if (stack->n == SAG_SEL_DEPTH ||
            !sag_block_level(&u, seed, stack->n, &span) ||
            (stack->n != 0U &&
             span.lo == stack->s[stack->n - 1U].lo &&
             span.hi == stack->s[stack->n - 1U].hi))
            continue;
        stack->s[stack->n++] = span;
        select_span(tb, item, span);
        changed = true;
    }
    if (!changed) {
        sag_msg(cx->ed, SAG_MSG_INFO, "selection is already at buffer level");
        return SAG_CMD_OK;
    }
    sag_cset_normalize(tb, &win->cs);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_sel_unit_contract(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    bool changed = false;
    size_t i;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)cursor;
    for (i = 0U; i < win->cs.curs.len; i++) {
        Cursor *item = &win->cs.curs.data[i];
        SelStack *stack = &win->cs.selstacks.data[i];

        if (stack->n == 0U)
            continue;
        stack->n--;
        if (stack->n == 0U)
            item->anchor = item->pos;
        else
            select_span(tb, item, stack->s[stack->n - 1U]);
        changed = true;
    }
    if (!changed) {
        sag_msg(cx->ed, SAG_MSG_INFO, "selection stack is empty");
        return SAG_CMD_OK;
    }
    sag_cset_normalize(tb, &win->cs);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_sel_kind(CmdCtx *cx)
{
    SelKind kind;
    size_t i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->ed->mode != SAG_MODE_H || cx->sarg == NULL ||
        cx->sarg_len != 1U)
        return SAG_CMD_ERR_ARG;
    switch (cx->sarg[0]) {
    case 'c':
    case 'C':
        kind = SAG_SEL_CHAR;
        break;
    case 'l':
    case 'L':
        kind = SAG_SEL_LINE;
        break;
    case 'r':
    case 'R':
        kind = SAG_SEL_RECT;
        break;
    default:
        return SAG_CMD_ERR_ARG;
    }
    for (i = 0U; i < cx->win->cs.curs.len; i++) {
        const Cursor *cursor = &cx->win->cs.curs.data[i];

        damage_offsets(cx->ed, cursor->anchor, cursor->pos);
    }
    cx->win->h.kind = kind;
    for (i = 0U; i < cx->win->cs.curs.len; i++) {
        const Cursor *cursor = &cx->win->cs.curs.data[i];

        damage_offsets(cx->ed, cursor->anchor, cursor->pos);
    }
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_sel_swap_ends(CmdCtx *cx)
{
    size_t i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->ed->mode != SAG_MODE_H)
        return SAG_CMD_ERR_STATE;
    for (i = 0U; i < cx->win->cs.curs.len; i++) {
        Cursor *cursor = &cx->win->cs.curs.data[i];
        ByteOff old_pos = cursor->pos;

        cursor->pos = cursor->anchor;
        cursor->anchor = old_pos;
        cursor->goal_col = sag_off_to_gcol(
            cx->win->buf->tb,
            sag_textbuf_line_span(
                cx->win->buf->tb,
                sag_textbuf_line_of(cx->win->buf->tb, cursor->pos)),
            cursor->pos);
    }
    sag_cset_normalize(cx->win->buf->tb, &cx->win->cs);
    sag_win_follow_cursor(cx->win);
    {
        ByteOff lo;
        ByteOff hi;

        cursor_set_bounds(&cx->win->cs, &lo, &hi);
        damage_offsets(cx->ed, lo, hi);
    }
    return SAG_CMD_OK;
}

static void replace_cursors(Win *win, CursorSet *replacement)
{
    sag_cset_free(&win->cs);
    win->cs = *replacement;
    (void)memset(replacement, 0, sizeof(*replacement));
}

static Cursor cursor_at(const TextBuf *tb, ByteOff pos)
{
    Cursor cursor;
    Span line = sag_textbuf_line_span(tb, sag_textbuf_line_of(tb, pos));

    cursor.pos = pos;
    cursor.anchor = pos;
    cursor.goal_col = sag_off_to_gcol(tb, line, pos);
    return cursor;
}

static CmdStatus finish_lift(CmdCtx *cx, CursorSet *replacement)
{
    ByteOff old_lo;
    ByteOff old_hi;
    ByteOff new_lo;
    ByteOff new_hi;

    if (replacement->curs.len == 0U) {
        sag_cset_free(replacement);
        sag_msg(cx->ed, SAG_MSG_INFO, "selection contains no lift targets");
        return SAG_CMD_OK;
    }
    cursor_set_bounds(&cx->win->cs, &old_lo, &old_hi);
    cursor_set_bounds(replacement, &new_lo, &new_hi);
    replace_cursors(cx->win, replacement);
    sag_selstack_clear(cx->win);
    damage_offsets(cx->ed,
                   old_lo.v < new_lo.v ? old_lo : new_lo,
                   old_hi.v > new_hi.v ? old_hi : new_hi);
    sag_win_follow_cursor(cx->win);
    return sag_mode_enter(cx->ed, SAG_MODE_L);
}

CmdStatus sag_edit_cmd_cursor_lift_lines(CmdCtx *cx)
{
    const TextBuf *tb;
    const Cursor *selected;
    CursorSet lifted;
    LineNo first;
    GCol col;
    u32 rows;
    u32 i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U || cx->ed->mode != SAG_MODE_H)
        return SAG_CMD_ERR_STATE;
    tb = cx->win->buf->tb;
    selected = &cx->win->cs.curs.data[cx->win->cs.primary];
    first = sag_textbuf_line_of(
        tb, selected->pos.v < selected->anchor.v ? selected->pos :
                                                     selected->anchor);
    col = sag_off_to_gcol(tb, sag_textbuf_line_span(tb, first),
                          selected->pos.v < selected->anchor.v ?
                              selected->pos : selected->anchor);
    rows = sag_sel_rows(cx->win, selected);
    if (rows > SAG_MC_MAX) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "selection exceeds 10000 cursors");
        return SAG_CMD_ERR_STATE;
    }
    sag_cset_init(&lifted,
                  cursor_at(tb, sag_gcol_to_off(
                                    tb, sag_textbuf_line_span(tb, first), col)));
    for (i = 1U; i < rows; i++) {
        LineNo line = LINENO(first.v + i);
        Cursor cursor = cursor_at(
            tb, sag_gcol_to_off(tb, sag_textbuf_line_span(tb, line), col));

        (void)sag_cset_add(&lifted, cursor);
    }
    return finish_lift(cx, &lifted);
}

static u8 *copy_span_bytes(const TextBuf *tb, Span span)
{
    u64 total = span.hi - span.lo;
    u64 copied = 0U;
    u8 *bytes = sag_xmalloc(total == 0U ? 1U : (size_t)total);
    TextIter it;

    if (total == 0U)
        return bytes;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        SAG_BUG("selection lift: cannot begin valid span");
    while (copied < total) {
        const u8 *chunk;
        u64 avail;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &chunk, &avail) || avail == 0U)
            SAG_BUG("selection lift: invalid text iterator");
        take = avail < total - copied ? avail : total - copied;
        (void)memcpy(bytes + copied, chunk, (size_t)take);
        copied += take;
        if (copied < total && !sag_textiter_advance(&it, tb))
            SAG_BUG("selection lift: truncated valid span");
    }
    return bytes;
}

static bool add_matches_in_span(const TextBuf *tb, Span haystack,
                                const u8 *pat, u32 pat_len,
                                CursorSet *matches, bool *have_match)
{
    u64 hay_len = haystack.hi - haystack.lo;
    u8 *bytes;
    u64 i;

    if (hay_len < pat_len)
        return true;
    bytes = copy_span_bytes(tb, haystack);
    for (i = 0U; i <= hay_len - pat_len; i++) {
        ByteOff at;

        if (memcmp(bytes + i, pat, pat_len) != 0U)
            continue;
        at = BYTEOFF(haystack.lo + i);
        if (!sag_is_grapheme_boundary(tb, at))
            continue;
        if (!*have_match) {
            sag_cset_init(matches, cursor_at(tb, at));
            *have_match = true;
        } else if (!sag_cset_add(matches, cursor_at(tb, at)) &&
                   matches->curs.len >= SAG_MC_MAX) {
            free(bytes);
            return false;
        }
    }
    free(bytes);
    return true;
}

CmdStatus sag_edit_cmd_cursor_lift_matches(CmdCtx *cx)
{
    const TextBuf *tb;
    const Cursor *selected;
    const u8 *pat;
    u32 pat_len;
    u8 *owned_pat = NULL;
    CursorSet matches = {0};
    bool have_match = false;
    bool ok = true;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U || cx->ed->mode != SAG_MODE_H)
        return SAG_CMD_ERR_STATE;
    sag_mc_require_literal_lift(false);
    tb = cx->win->buf->tb;
    selected = &cx->win->cs.curs.data[cx->win->cs.primary];
    if (cx->sarg != NULL && cx->sarg_len != 0U) {
        pat = (const u8 *)cx->sarg;
        pat_len = cx->sarg_len;
    } else {
        UnitCtx unit = {tb, cx->win->buf, cx->win};
        Span word = sag_unit_word.span(&unit, selected->pos, false);

        if (word.lo == word.hi)
            return SAG_CMD_ERR_ARG;
        if (word.hi - word.lo > UINT32_MAX)
            return SAG_CMD_ERR_ARG;
        pat_len = (u32)(word.hi - word.lo);
        owned_pat = copy_span_bytes(tb, word);
        pat = owned_pat;
    }
    if (cx->win->h.kind == SAG_SEL_RECT) {
        SagSelSpanVec spans = {0};
        size_t i;

        sag_sel_rect_spans(cx->win, selected, &spans);
        for (i = 0U; i < spans.len && ok; i++)
            ok = add_matches_in_span(tb, spans.data[i], pat, pat_len,
                                     &matches, &have_match);
        SagSelSpanVec_free(&spans);
    } else {
        ok = add_matches_in_span(tb, sag_sel_span(cx->win, selected), pat,
                                 pat_len, &matches, &have_match);
    }
    free(owned_pat);
    if (!ok) {
        sag_cset_free(&matches);
        sag_msg(cx->ed, SAG_MSG_ERROR, "match lift exceeds 10000 cursors");
        return SAG_CMD_ERR_STATE;
    }
    return finish_lift(cx, &matches);
}

CmdStatus sag_edit_cmd_cursor_lift_ends(CmdCtx *cx)
{
    const TextBuf *tb;
    CursorSet lifted = {0};
    const Cursor *primary;
    size_t i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U || cx->ed->mode != SAG_MODE_H)
        return SAG_CMD_ERR_STATE;
    if (cx->win->cs.curs.len > SAG_MC_MAX / 2U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "selection ends exceed 10000 cursors");
        return SAG_CMD_ERR_STATE;
    }
    tb = cx->win->buf->tb;
    primary = &cx->win->cs.curs.data[cx->win->cs.primary];
    sag_cset_init(&lifted, cursor_at(tb, primary->pos));
    for (i = 0U; i < cx->win->cs.curs.len; i++) {
        const Cursor *selected = &cx->win->cs.curs.data[i];
        Cursor ends[2] = {cursor_at(tb, selected->anchor),
                          cursor_at(tb, selected->pos)};
        size_t j;

        for (j = 0U; j < SAG_ARRAY_LEN(ends); j++) {
            (void)sag_cset_add(&lifted, ends[j]);
        }
    }
    return finish_lift(cx, &lifted);
}

static CmdStatus cursor_add_vertical(CmdCtx *cx, bool below)
{
    TextBuf *tb;
    Cursor cursor;
    ByteOff before;
    ByteOff lo;
    ByteOff hi;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U)
        return SAG_CMD_ERR_STATE;
    tb = cx->win->buf->tb;
    cursor_set_bounds(&cx->win->cs, &lo, &hi);
    cursor = cx->win->cs.curs.data[cx->win->cs.primary];
    cursor.anchor = cursor.pos;
    before = cursor.pos;
    if (below)
        sag_cursor_down(tb, &cursor);
    else
        sag_cursor_up(tb, &cursor);
    if (cursor.pos.v != before.v && !sag_cset_add(&cx->win->cs, cursor) &&
        cx->win->cs.curs.len >= SAG_MC_MAX) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "cursor limit is 10000");
        return SAG_CMD_ERR_STATE;
    }
    if (cursor.pos.v < lo.v)
        lo = cursor.pos;
    if (cursor.pos.v > hi.v)
        hi = cursor.pos;
    damage_offsets(cx->ed, lo, hi);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_cursor_add_above(CmdCtx *cx)
{
    return cursor_add_vertical(cx, false);
}

CmdStatus sag_edit_cmd_cursor_add_below(CmdCtx *cx)
{
    return cursor_add_vertical(cx, true);
}

CmdStatus sag_edit_cmd_cursor_drop(CmdCtx *cx)
{
    ByteOff old_lo;
    ByteOff old_hi;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    cursor_set_bounds(&cx->win->cs, &old_lo, &old_hi);
    (void)sag_cset_drop_latest(&cx->win->cs);
    damage_offsets(cx->ed, old_lo, old_hi);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_cursor_collapse(CmdCtx *cx)
{
    ByteOff old_lo;
    ByteOff old_hi;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->cs.curs.len == 0U)
        return SAG_CMD_ERR_STATE;
    cursor_set_bounds(&cx->win->cs, &old_lo, &old_hi);
    sag_cset_remove_all_but_primary(&cx->win->cs);
    cx->win->cs.curs.data[0].anchor = cx->win->cs.curs.data[0].pos;
    sag_selstack_clear(cx->win);
    damage_offsets(cx->ed, old_lo, old_hi);
    return SAG_CMD_OK;
}

static CmdStatus insert_bytes(CmdCtx *cx, const u8 *bytes, u64 len)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;
    LineNo line;
    u64 old_line_count;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    if (bytes == NULL && len != 0U)
        return SAG_CMD_ERR_ARG;
    line = sag_textbuf_line_of(tb, cursor->pos);
    old_line_count = sag_textbuf_line_count(tb);
    ec = sag_ed_edit_ctx(cx->ed);
    if (!sag_edit_insert(&ec, cursor->pos, bytes, len)) {
        sag_ed_finish_edit(cx->ed, &ec);
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    sag_ed_finish_edit(cx->ed, &ec);
    win->wrap_goal_valid = false;
    /* Typeahead is drained as one event-loop batch.  Following here would
     * rescan the growing line for every byte; render follows once instead. */
    cx->ed->cursor_follow_pending = true;
    sag_ed_damage_line(cx->ed, line,
                       old_line_count != sag_textbuf_line_count(tb));
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_insert_text(CmdCtx *cx)
{
    if (cx == NULL || cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    return insert_bytes(cx, (const u8 *)cx->sarg, cx->sarg_len);
}

CmdStatus sag_edit_cmd_insert_newline(CmdCtx *cx)
{
    const u8 *bytes;
    size_t len;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL)
        return SAG_CMD_ERR_STATE;
    sag_filemeta_eol_bytes(&cx->win->buf->meta, &bytes, &len);
    return insert_bytes(cx, bytes, (u64)len);
}

CmdStatus sag_edit_cmd_insert_tab(CmdCtx *cx)
{
    static const u8 tab = (u8)'\t';

    return insert_bytes(cx, &tab, 1U);
}

CmdStatus sag_edit_cmd_insert_after(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    ByteOff end;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    line = sag_textbuf_line_of(tb, cursor->pos);
    end = line_content_end(tb, line);
    if (cursor->pos.v < end.v)
        sag_cursor_right(tb, cursor);
    return sag_mode_enter(cx->ed, SAG_MODE_I);
}

static CmdStatus open_line(CmdCtx *cx, bool below)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;
    LineNo line;
    Span span;
    ByteOff at;
    ByteOff placed;
    const u8 *eol;
    size_t eol_len;
    u64 lines;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    line = sag_textbuf_line_of(tb, cursor->pos);
    span = sag_textbuf_line_span(tb, line);
    lines = sag_textbuf_line_count(tb);
    sag_filemeta_eol_bytes(&win->buf->meta, &eol, &eol_len);
    at = below ? BYTEOFF(span.hi) : BYTEOFF(span.lo);
    placed = at;
    if (below && line.v + 1U == lines)
        placed = BYTEOFF(at.v + (u64)eol_len);
    ec = sag_ed_edit_ctx(cx->ed);
    if (!sag_edit_insert(&ec, at, eol, (u64)eol_len)) {
        sag_ed_finish_edit(cx->ed, &ec);
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    sag_ed_finish_edit(cx->ed, &ec);
    cursor = &win->cs.curs.data[win->cs.primary];
    cursor_place(tb, cursor, placed);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    sag_ed_damage_line(cx->ed, line, true);
    return sag_mode_enter(cx->ed, SAG_MODE_I);
}

CmdStatus sag_edit_cmd_open_below(CmdCtx *cx)
{
    return open_line(cx, true);
}

CmdStatus sag_edit_cmd_open_above(CmdCtx *cx)
{
    return open_line(cx, false);
}

static CmdStatus delete_span(CmdCtx *cx, Span span)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;
    LineNo line;
    u64 old_line_count;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    if (span.lo > span.hi || span.hi > sag_textbuf_len(tb))
        return SAG_CMD_ERR_ARG;
    line = sag_textbuf_line_of(tb, BYTEOFF(span.lo));
    old_line_count = sag_textbuf_line_count(tb);
    ec = sag_ed_edit_ctx(cx->ed);
    if (!sag_edit_delete(&ec, span)) {
        sag_ed_finish_edit(cx->ed, &ec);
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    sag_ed_finish_edit(cx->ed, &ec);
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    sag_ed_damage_line(cx->ed, line,
                       old_line_count != sag_textbuf_line_count(tb));
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_delete_grapheme_left(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff prev;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    prev = sag_grapheme_prev_boundary(tb, cursor->pos);
    return delete_span(cx, (Span){prev.v, cursor->pos.v});
}

CmdStatus sag_edit_cmd_delete_grapheme(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff next;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    next = sag_grapheme_next_boundary(tb, cursor->pos);
    return delete_span(cx, (Span){cursor->pos.v, next.v});
}

CmdStatus sag_edit_cmd_delete_line(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    Span span;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    line = sag_textbuf_line_of(tb, cursor->pos);
    span = sag_textbuf_line_span(tb, line);
    if (line.v + 1U == sag_textbuf_line_count(tb) && line.v != 0U)
        span.lo = sag_grapheme_prev_boundary(tb, BYTEOFF(span.lo)).v;
    return delete_span(cx, span);
}

CmdStatus sag_edit_cmd_undo(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)tb;
    (void)cursor;
    ec = sag_ed_edit_ctx(cx->ed);
    if ((ec.jrnl != NULL && !sag_journal_ok(ec.jrnl)) ||
        (sag_undo_current(ec.undo) != ec.undo->root &&
         !sag_edit_ensure_journal(&ec))) {
        sag_ed_finish_edit(cx->ed, &ec);
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    (void)sag_undo(&ec);
    sag_ed_finish_edit(cx->ed, &ec);
    if (ec.jrnl != NULL && !sag_journal_ok(ec.jrnl)) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    sag_ed_damage_document(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_redo(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    (void)tb;
    (void)cursor;
    ec = sag_ed_edit_ctx(cx->ed);
    if ((ec.jrnl != NULL && !sag_journal_ok(ec.jrnl)) ||
        (ec.undo->cur != 0U && ec.undo->cur <= ec.undo->nodes.len &&
         ec.undo->nodes.data[ec.undo->cur - 1U].redo_child != 0U &&
         !sag_edit_ensure_journal(&ec))) {
        sag_ed_finish_edit(cx->ed, &ec);
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    (void)sag_redo(&ec);
    sag_ed_finish_edit(cx->ed, &ec);
    if (ec.jrnl != NULL && !sag_journal_ok(ec.jrnl)) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "cannot persist edit to crash journal");
        return SAG_CMD_ERR_IO;
    }
    win->wrap_goal_valid = false;
    sag_win_follow_cursor(win);
    sag_ed_damage_document(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_undo_barrier(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_ed_insert_barrier(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_mode_enter(CmdCtx *cx)
{
    Mode mode;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->sarg_len == 3U && cx->sarg[0] == 'H' &&
        cx->sarg[1] == ' ') {
        Mode unit;

        switch (cx->sarg[2]) {
        case 'L':
            unit = SAG_MODE_L;
            break;
        case 'W':
            unit = SAG_MODE_W;
            break;
        case 'B':
            unit = SAG_MODE_B;
            break;
        case 'C':
            unit = SAG_MODE_I;
            break;
        default:
            return SAG_CMD_ERR_ARG;
        }
        return sag_mode_enter_highlight(cx->ed, unit, true);
    }
    if (cx->sarg_len != 1U)
        return SAG_CMD_ERR_ARG;
    for (mode = SAG_MODE_L; mode < SAG_MODE__N; mode++) {
        if (sag_modes[mode].name[0] == cx->sarg[0] &&
            sag_modes[mode].name[1] == '\0')
            return sag_mode_enter(cx->ed, mode);
    }
    return SAG_CMD_ERR_ARG;
}

CmdStatus sag_edit_cmd_mode_escape(CmdCtx *cx)
{
    if (cx == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_mode_escape(cx->ed);
}
