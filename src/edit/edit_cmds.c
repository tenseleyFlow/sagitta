#include "edit/edit_cmds.h"

#include <stdlib.h>
#include <string.h>

#include "edit/block.h"
#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/motion.h"
#include "edit/sel_actions.h"
#include "edit/word.h"
#include "ui/message.h"
#include "ui/cmdparse.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "util/log.h"

static CmdStatus delete_span(CmdCtx *cx, Span span);

static bool edit_window(CmdCtx *cx, Win **win, TextBuf **tb, Cursor **cursor)
{
    u32 cursor_index;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U ||
        (size_t)cx->win->cs.primary >= cx->win->cs.curs.len)
        return false;
    cursor_index = cx->cursor_given ? cx->cursor_index : cx->win->cs.primary;
    if (cx->cursor_given) {
        if ((size_t)cursor_index >= cx->win->cs.curs.len)
            return false;
    } else if (cx->win->cs.active != YEW_MC_ACTIVE_NONE) {
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
    if (yew_ed_damage_batch_active(ed)) {
        yew_ed_damage_document(ed);
        return;
    }
    win = ed->win;
    tb = win->buf->tb;
    lo = yew_textbuf_line_of(tb, first.v < second.v ? first : second);
    hi = yew_textbuf_line_of(tb, first.v > second.v ? first : second);
    if (win->vp.wrap) {
        yew_ed_damage_document(ed);
        return;
    }
    top = yew_win_view_top(win);
    bottom = yew_vp_last_visible_line(win);
    if (hi.v < top.v || lo.v > bottom.v)
        return;
    if (lo.v < top.v)
        lo = top;
    if (hi.v > bottom.v)
        hi = bottom;
    if (!yew_win_view_row(win, lo, &row_lo) ||
        !yew_win_view_row(win, hi, &row_hi))
        return;
    yew_ed_damage_rows(ed, row_lo, (u16)(row_hi + 1U));
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
    line = yew_textbuf_line_span(tb, yew_textbuf_line_of(tb, pos));
    cursor->goal_col = yew_off_to_gcol(tb, line, pos);
}

static void finish_direct_motion(CmdCtx *cx, Cursor *cursor,
                                 ByteOff anchor, ByteOff old_pos)
{
    if (cx->ed->mode != YEW_MODE_H)
        return;
    cursor->anchor = anchor;
    damage_offsets(cx->ed, anchor,
                   old_pos.v < cursor->pos.v ? old_pos : cursor->pos);
    damage_offsets(cx->ed, anchor,
                   old_pos.v > cursor->pos.v ? old_pos : cursor->pos);
}

static ByteOff line_content_end(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < yew_textbuf_line_count(tb))
        end = yew_grapheme_prev_boundary(tb, end);
    return end;
}

static u8 byte_at(const TextBuf *tb, ByteOff at)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (!yew_textiter_begin(&it, tb, at) ||
        !yew_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        YEW_BUG("editor command: cannot inspect valid byte offset");
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
        return YEW_CMD_ERR_STATE;
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
        (void)yew_vp_move_display(win, amount);
    } else {
        for (i = 0U; i < rows; i++) {
            if (down)
                yew_cursor_down(tb, cursor);
            else
                yew_cursor_up(tb, cursor);
        }
    }
    if (cx->ed->mode == YEW_MODE_H) {
        cursor->anchor = anchor;
        damage_offsets(cx->ed, anchor,
                       old_pos.v < cursor->pos.v ? old_pos : cursor->pos);
        damage_offsets(cx->ed, anchor,
                       old_pos.v > cursor->pos.v ? old_pos : cursor->pos);
    }
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_buf_home(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    yew_cursor_buf_home(tb, cursor);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_buf_end(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    if (cx->count_given) {
        u64 lines = yew_textbuf_line_count(tb);
        u64 requested = cx->count == 0U ? 0U : (u64)cx->count - 1U;
        LineNo line = LINENO(requested < lines ? requested : lines - 1U);

        cursor_place(tb, cursor, yew_textbuf_line_start(tb, line));
    } else {
        yew_cursor_buf_end(tb, cursor);
    }
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_line_home(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff anchor;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    yew_cursor_line_home(tb, cursor);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_line_end(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff anchor;
    ByteOff old_pos;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    yew_cursor_line_end(tb, cursor);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_line_up(CmdCtx *cx)
{
    return move_vertical(cx, false, 1U);
}

CmdStatus yew_edit_cmd_move_line_down(CmdCtx *cx)
{
    return move_vertical(cx, true, 1U);
}

CmdStatus yew_edit_cmd_move_line_first_nonblank(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    ByteOff pos;
    ByteOff end;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    line = yew_textbuf_line_of(tb, cursor->pos);
    pos = yew_textbuf_line_start(tb, line);
    end = line_content_end(tb, line);
    while (pos.v < end.v) {
        ByteOff next = yew_grapheme_next_boundary(tb, pos);

        if (!ascii_blank_cluster(tb, pos, next))
            break;
        pos = next;
    }
    cursor_place(tb, cursor, pos);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_line_last_nonblank(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    ByteOff pos;
    ByteOff end;
    ByteOff last;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    line = yew_textbuf_line_of(tb, cursor->pos);
    pos = yew_textbuf_line_start(tb, line);
    end = line_content_end(tb, line);
    last = pos;
    while (pos.v < end.v) {
        ByteOff next = yew_grapheme_next_boundary(tb, pos);

        if (!ascii_blank_cluster(tb, pos, next))
            last = pos;
        pos = next;
    }
    cursor_place(tb, cursor, last);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_line_half_page_up(CmdCtx *cx)
{
    u64 rows;

    if (cx == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    rows = (u64)cx->win->vp.rows / 2U;
    return move_vertical(cx, false, rows == 0U ? 1U : rows);
}

CmdStatus yew_edit_cmd_move_line_half_page_down(CmdCtx *cx)
{
    u64 rows;

    if (cx == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    rows = (u64)cx->win->vp.rows / 2U;
    return move_vertical(cx, true, rows == 0U ? 1U : rows);
}

CmdStatus yew_edit_cmd_view_page_up(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_vp_page(cx->win, -1);
    yew_vp_push_cursor(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_page_down(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_vp_page(cx->win, 1);
    yew_vp_push_cursor(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

static CmdStatus view_scroll(CmdCtx *cx, i32 rows)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_vp_scroll(cx->win, rows);
    yew_vp_push_cursor(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_scroll_up(CmdCtx *cx)
{
    return view_scroll(cx, -1);
}

CmdStatus yew_edit_cmd_view_scroll_down(CmdCtx *cx)
{
    return view_scroll(cx, 1);
}

CmdStatus yew_edit_cmd_view_half_page_up(CmdCtx *cx)
{
    i32 rows;

    if (cx == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    rows = (i32)(cx->win->vp.rows / 2U);
    return view_scroll(cx, rows == 0 ? -1 : -rows);
}

CmdStatus yew_edit_cmd_view_half_page_down(CmdCtx *cx)
{
    i32 rows;

    if (cx == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    rows = (i32)(cx->win->vp.rows / 2U);
    return view_scroll(cx, rows == 0 ? 1 : rows);
}

CmdStatus yew_edit_cmd_view_center(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_vp_center(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_top(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_vp_top(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_bottom(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_vp_bottom(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_goto_line(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    u64 line_count;
    u64 requested;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    line_count = yew_textbuf_line_count(tb);
    requested = cx->count == 0U ? 0U : (u64)cx->count - 1U;
    if (requested >= line_count)
        requested = line_count - 1U;
    cursor_place(tb, cursor, yew_textbuf_line_start(tb, LINENO(requested)));
    win->wrap_goal_valid = false;
    yew_vp_center(win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_toggle_wrap(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    cx->win->vp.wrap = !cx->win->vp.wrap;
    cx->win->vp.left = (CCol){0U};
    cx->win->vp.top_sub = 0U;
    cx->win->wrap_goal_valid = false;
    yew_vp_invalidate(cx->win);
    yew_vp_clamp(cx->win);
    yew_vp_follow(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_view_number_style(CmdCtx *cx)
{
    NumStyle style;

    if (cx == NULL || cx->win == NULL || cx->ed == NULL ||
        cx->sarg == NULL)
        return YEW_CMD_ERR_ARG;
    if (cx->sarg_len == 4U && memcmp(cx->sarg, "none", 4U) == 0)
        style = YEW_NUM_NONE;
    else if (cx->sarg_len == 3U && memcmp(cx->sarg, "abs", 3U) == 0)
        style = YEW_NUM_ABS;
    else if (cx->sarg_len == 3U && memcmp(cx->sarg, "rel", 3U) == 0)
        style = YEW_NUM_REL;
    else if (cx->sarg_len == 6U && memcmp(cx->sarg, "hybrid", 6U) == 0)
        style = YEW_NUM_HYBRID;
    else
        return YEW_CMD_ERR_ARG;
    cx->win->number_style = style;
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_message_expand(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    if (!yew_msg_expand(cx->ed))
        return YEW_CMD_ERR_STATE;
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_ui_cancel(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    (void)yew_msg_dismiss_overlay(cx->ed);
    if (cx->ed->prompt != YEW_PROMPT_NONE)
        yew_ed_prompt(cx->ed, YEW_PROMPT_NONE);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_char_prev(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    yew_cursor_left(tb, cursor);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_char_next(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    yew_cursor_right(tb, cursor);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
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
        return YEW_CMD_ERR_STATE;
    ops = cx->ed->mode == YEW_MODE_H ? win->h.unit :
          cx->ed->prev_unit == YEW_MODE_I ? &yew_unit_char :
          yew_unit_of_mode(cx->ed->mode);
    if (ops == NULL)
        return YEW_CMD_ERR_STATE;
    line_vertical = ops == &yew_unit_line &&
                    (motion == UNIT_NEXT || motion == UNIT_PREV);
    vertical_goal = cursor->goal_col;
    old_pos = cursor->pos;
    unselected = cursor->anchor.v == cursor->pos.v;
    if (line_vertical && win->vp.wrap && !win->wrap_goal_valid) {
        win->wrap_goal = yew_vp_display_col(win, cursor->pos);
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
        return YEW_CMD_ERR_ARG;
    }
    if (cx->ed->mode == YEW_MODE_H) {
        cursor->pos = pos;
        if (line_vertical)
            cursor->goal_col = vertical_goal;
        else
            cursor->goal_col = yew_off_to_gcol(
                tb, yew_textbuf_line_span(tb, yew_textbuf_line_of(tb, pos)),
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
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_unit_next(CmdCtx *cx)
{
    return move_unit(cx, UNIT_NEXT, false);
}

CmdStatus yew_edit_cmd_move_unit_prev(CmdCtx *cx)
{
    return move_unit(cx, UNIT_PREV, false);
}

CmdStatus yew_edit_cmd_move_unit_home(CmdCtx *cx)
{
    return move_unit(cx, UNIT_HOME, false);
}

CmdStatus yew_edit_cmd_move_unit_end(CmdCtx *cx)
{
    return move_unit(cx, UNIT_END, false);
}

CmdStatus yew_edit_cmd_move_unit_next_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_NEXT, true);
}

CmdStatus yew_edit_cmd_move_unit_prev_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_PREV, true);
}

CmdStatus yew_edit_cmd_move_unit_home_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_HOME, true);
}

CmdStatus yew_edit_cmd_move_unit_end_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_END, true);
}

CmdStatus yew_edit_cmd_move_unit_up(CmdCtx *cx)
{
    return yew_edit_cmd_move_line_up(cx);
}

CmdStatus yew_edit_cmd_move_unit_down(CmdCtx *cx)
{
    return yew_edit_cmd_move_line_down(cx);
}

CmdStatus yew_edit_cmd_move_unit_up_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_PREV, true);
}

CmdStatus yew_edit_cmd_move_unit_down_alt(CmdCtx *cx)
{
    return move_unit(cx, UNIT_NEXT, true);
}

CmdStatus yew_edit_cmd_delete_unit(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    const UnitOps *ops;
    UnitCtx u;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    if (cx->ed->mode == YEW_MODE_H)
        return yew_sel_cmd_delete(cx);
    if (cx->ed->prev_unit == YEW_MODE_I)
        ops = &yew_unit_char;
    else
        ops = yew_unit_of_mode(cx->ed->mode);
    if (ops == NULL)
        return YEW_CMD_ERR_STATE;
    if (ops == &yew_unit_line)
        return yew_edit_cmd_delete_line(cx);
    u = (UnitCtx){tb, win->buf, win};
    return delete_span(cx, ops->span(&u, cursor->pos, false));
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
        return YEW_CMD_ERR_STATE;
    u = (UnitCtx){tb, win->buf, win};
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    if (!yew_block_match(&u, cursor->pos, next, &pos)) {
        yew_msg(cx->ed, YEW_MSG_INFO, "no enclosing delimiter");
        return YEW_CMD_OK;
    }
    cursor_place(tb, cursor, pos);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_block_match_prev(CmdCtx *cx)
{
    return move_block_match(cx, false);
}

CmdStatus yew_edit_cmd_move_block_match_next(CmdCtx *cx)
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
        return YEW_CMD_ERR_STATE;
    u = (UnitCtx){tb, win->buf, win};
    anchor = cursor->anchor;
    old_pos = cursor->pos;
    pos = next ? yew_word_sub_next(&u, cursor->pos)
               : yew_word_sub_prev(&u, cursor->pos);
    cursor_place(tb, cursor, pos);
    finish_direct_motion(cx, cursor, anchor, old_pos);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_move_word_sub_prev(CmdCtx *cx)
{
    return move_word_sub(cx, false);
}

CmdStatus yew_edit_cmd_move_word_sub_next(CmdCtx *cx)
{
    return move_word_sub(cx, true);
}

static void select_span(const TextBuf *tb, Cursor *cursor, Span span)
{
    cursor->anchor = BYTEOFF(span.lo);
    cursor->pos = BYTEOFF(span.hi);
    cursor->goal_col = yew_off_to_gcol(
        tb, yew_textbuf_line_span(tb, yew_textbuf_line_of(tb, cursor->pos)),
        cursor->pos);
}

CmdStatus yew_edit_cmd_sel_unit_expand(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    UnitCtx u;
    bool changed = false;
    size_t i;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    (void)cursor;
    u = (UnitCtx){tb, win->buf, win};
    for (i = 0U; i < win->cs.curs.len; i++) {
        Cursor *item = &win->cs.curs.data[i];
        SelStack *stack = &win->cs.selstacks.data[i];
        Span span;
        ByteOff seed = stack->n == 0U ? item->pos :
                                            BYTEOFF(stack->s[0].lo);

        if (stack->n == YEW_SEL_DEPTH ||
            !yew_block_level(&u, seed, stack->n, &span) ||
            (stack->n != 0U &&
             span.lo == stack->s[stack->n - 1U].lo &&
             span.hi == stack->s[stack->n - 1U].hi))
            continue;
        stack->s[stack->n++] = span;
        select_span(tb, item, span);
        changed = true;
    }
    if (!changed) {
        yew_msg(cx->ed, YEW_MSG_INFO, "selection is already at buffer level");
        return YEW_CMD_OK;
    }
    yew_cset_normalize(tb, &win->cs);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_sel_unit_contract(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    bool changed = false;
    size_t i;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
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
        yew_msg(cx->ed, YEW_MSG_INFO, "selection stack is empty");
        return YEW_CMD_OK;
    }
    yew_cset_normalize(tb, &win->cs);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_sel_kind(CmdCtx *cx)
{
    SelKind kind;
    size_t i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->ed->mode != YEW_MODE_H || cx->sarg == NULL ||
        cx->sarg_len != 1U)
        return YEW_CMD_ERR_ARG;
    switch (cx->sarg[0]) {
    case 'c':
    case 'C':
        kind = YEW_SEL_CHAR;
        break;
    case 'l':
    case 'L':
        kind = YEW_SEL_LINE;
        break;
    case 'r':
    case 'R':
        kind = YEW_SEL_RECT;
        break;
    default:
        return YEW_CMD_ERR_ARG;
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
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_sel_swap_ends(CmdCtx *cx)
{
    size_t i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->ed->mode != YEW_MODE_H)
        return YEW_CMD_ERR_STATE;
    for (i = 0U; i < cx->win->cs.curs.len; i++) {
        Cursor *cursor = &cx->win->cs.curs.data[i];
        ByteOff old_pos = cursor->pos;

        cursor->pos = cursor->anchor;
        cursor->anchor = old_pos;
        cursor->goal_col = yew_off_to_gcol(
            cx->win->buf->tb,
            yew_textbuf_line_span(
                cx->win->buf->tb,
                yew_textbuf_line_of(cx->win->buf->tb, cursor->pos)),
            cursor->pos);
    }
    yew_cset_normalize(cx->win->buf->tb, &cx->win->cs);
    yew_win_follow_cursor(cx->win);
    {
        ByteOff lo;
        ByteOff hi;

        cursor_set_bounds(&cx->win->cs, &lo, &hi);
        damage_offsets(cx->ed, lo, hi);
    }
    return YEW_CMD_OK;
}

static void replace_cursors(Win *win, CursorSet *replacement)
{
    yew_cset_free(&win->cs);
    win->cs = *replacement;
    (void)memset(replacement, 0, sizeof(*replacement));
}

static Cursor cursor_at(const TextBuf *tb, ByteOff pos)
{
    Cursor cursor;
    Span line = yew_textbuf_line_span(tb, yew_textbuf_line_of(tb, pos));

    cursor.pos = pos;
    cursor.anchor = pos;
    cursor.goal_col = yew_off_to_gcol(tb, line, pos);
    return cursor;
}

static CmdStatus finish_lift(CmdCtx *cx, CursorSet *replacement)
{
    ByteOff old_lo;
    ByteOff old_hi;
    ByteOff new_lo;
    ByteOff new_hi;

    if (replacement->curs.len == 0U) {
        yew_cset_free(replacement);
        yew_msg(cx->ed, YEW_MSG_INFO, "selection contains no lift targets");
        return YEW_CMD_OK;
    }
    cursor_set_bounds(&cx->win->cs, &old_lo, &old_hi);
    cursor_set_bounds(replacement, &new_lo, &new_hi);
    replace_cursors(cx->win, replacement);
    yew_selstack_clear(cx->win);
    damage_offsets(cx->ed,
                   old_lo.v < new_lo.v ? old_lo : new_lo,
                   old_hi.v > new_hi.v ? old_hi : new_hi);
    yew_win_follow_cursor(cx->win);
    return yew_mode_enter(cx->ed, YEW_MODE_L);
}

CmdStatus yew_edit_cmd_cursor_lift_lines(CmdCtx *cx)
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
        cx->win->cs.curs.len == 0U || cx->ed->mode != YEW_MODE_H)
        return YEW_CMD_ERR_STATE;
    tb = cx->win->buf->tb;
    selected = &cx->win->cs.curs.data[cx->win->cs.primary];
    first = yew_textbuf_line_of(
        tb, selected->pos.v < selected->anchor.v ? selected->pos :
                                                     selected->anchor);
    col = yew_off_to_gcol(tb, yew_textbuf_line_span(tb, first),
                          selected->pos.v < selected->anchor.v ?
                              selected->pos : selected->anchor);
    rows = yew_sel_rows(cx->win, selected);
    if (rows > YEW_MC_MAX) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "selection exceeds 10000 cursors");
        return YEW_CMD_ERR_STATE;
    }
    yew_cset_init(&lifted,
                  cursor_at(tb, yew_gcol_to_off(
                                    tb, yew_textbuf_line_span(tb, first), col)));
    for (i = 1U; i < rows; i++) {
        LineNo line = LINENO(first.v + i);
        Cursor cursor = cursor_at(
            tb, yew_gcol_to_off(tb, yew_textbuf_line_span(tb, line), col));

        (void)yew_cset_add(&lifted, cursor);
    }
    return finish_lift(cx, &lifted);
}

static u8 *copy_span_bytes(const TextBuf *tb, Span span)
{
    u64 total = span.hi - span.lo;
    u64 copied = 0U;
    u8 *bytes = yew_xmalloc(total == 0U ? 1U : (size_t)total);
    TextIter it;

    if (total == 0U)
        return bytes;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        YEW_BUG("selection lift: cannot begin valid span");
    while (copied < total) {
        const u8 *chunk;
        u64 avail;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &chunk, &avail) || avail == 0U)
            YEW_BUG("selection lift: invalid text iterator");
        take = avail < total - copied ? avail : total - copied;
        (void)memcpy(bytes + copied, chunk, (size_t)take);
        copied += take;
        if (copied < total && !yew_textiter_advance(&it, tb))
            YEW_BUG("selection lift: truncated valid span");
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
        if (!yew_is_grapheme_boundary(tb, at))
            continue;
        if (!*have_match) {
            yew_cset_init(matches, cursor_at(tb, at));
            *have_match = true;
        } else if (!yew_cset_add(matches, cursor_at(tb, at)) &&
                   matches->curs.len >= YEW_MC_MAX) {
            free(bytes);
            return false;
        }
    }
    free(bytes);
    return true;
}

CmdStatus yew_edit_cmd_cursor_lift_matches(CmdCtx *cx)
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
        cx->win->cs.curs.len == 0U || cx->ed->mode != YEW_MODE_H)
        return YEW_CMD_ERR_STATE;
    yew_mc_require_literal_lift(false);
    tb = cx->win->buf->tb;
    selected = &cx->win->cs.curs.data[cx->win->cs.primary];
    if (cx->sarg != NULL && cx->sarg_len != 0U) {
        pat = (const u8 *)cx->sarg;
        pat_len = cx->sarg_len;
    } else {
        UnitCtx unit = {tb, cx->win->buf, cx->win};
        Span word = yew_unit_word.span(&unit, selected->pos, false);

        if (word.lo == word.hi)
            return YEW_CMD_ERR_ARG;
        if (word.hi - word.lo > UINT32_MAX)
            return YEW_CMD_ERR_ARG;
        pat_len = (u32)(word.hi - word.lo);
        owned_pat = copy_span_bytes(tb, word);
        pat = owned_pat;
    }
    if (cx->win->h.kind == YEW_SEL_RECT) {
        YewSelSpanVec spans = {0};
        size_t i;

        yew_sel_rect_spans(cx->win, selected, &spans);
        for (i = 0U; i < spans.len && ok; i++)
            ok = add_matches_in_span(tb, spans.data[i], pat, pat_len,
                                     &matches, &have_match);
        YewSelSpanVec_free(&spans);
    } else {
        ok = add_matches_in_span(tb, yew_sel_span(cx->win, selected), pat,
                                 pat_len, &matches, &have_match);
    }
    free(owned_pat);
    if (!ok) {
        yew_cset_free(&matches);
        yew_msg(cx->ed, YEW_MSG_ERROR, "match lift exceeds 10000 cursors");
        return YEW_CMD_ERR_STATE;
    }
    return finish_lift(cx, &matches);
}

CmdStatus yew_edit_cmd_cursor_lift_ends(CmdCtx *cx)
{
    const TextBuf *tb;
    CursorSet lifted = {0};
    const Cursor *primary;
    size_t i;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U || cx->ed->mode != YEW_MODE_H)
        return YEW_CMD_ERR_STATE;
    if (cx->win->cs.curs.len > YEW_MC_MAX / 2U) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "selection ends exceed 10000 cursors");
        return YEW_CMD_ERR_STATE;
    }
    tb = cx->win->buf->tb;
    primary = &cx->win->cs.curs.data[cx->win->cs.primary];
    yew_cset_init(&lifted, cursor_at(tb, primary->pos));
    for (i = 0U; i < cx->win->cs.curs.len; i++) {
        const Cursor *selected = &cx->win->cs.curs.data[i];
        Cursor ends[2] = {cursor_at(tb, selected->anchor),
                          cursor_at(tb, selected->pos)};
        size_t j;

        for (j = 0U; j < YEW_ARRAY_LEN(ends); j++) {
            (void)yew_cset_add(&lifted, ends[j]);
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
        return YEW_CMD_ERR_STATE;
    tb = cx->win->buf->tb;
    cursor_set_bounds(&cx->win->cs, &lo, &hi);
    cursor = cx->win->cs.curs.data[cx->win->cs.primary];
    cursor.anchor = cursor.pos;
    before = cursor.pos;
    if (below)
        yew_cursor_down(tb, &cursor);
    else
        yew_cursor_up(tb, &cursor);
    if (cursor.pos.v != before.v && !yew_cset_add(&cx->win->cs, cursor) &&
        cx->win->cs.curs.len >= YEW_MC_MAX) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "cursor limit is 10000");
        return YEW_CMD_ERR_STATE;
    }
    if (cursor.pos.v < lo.v)
        lo = cursor.pos;
    if (cursor.pos.v > hi.v)
        hi = cursor.pos;
    damage_offsets(cx->ed, lo, hi);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_cursor_add_above(CmdCtx *cx)
{
    return cursor_add_vertical(cx, false);
}

CmdStatus yew_edit_cmd_cursor_add_below(CmdCtx *cx)
{
    return cursor_add_vertical(cx, true);
}

CmdStatus yew_edit_cmd_cursor_drop(CmdCtx *cx)
{
    ByteOff old_lo;
    ByteOff old_hi;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    cursor_set_bounds(&cx->win->cs, &old_lo, &old_hi);
    (void)yew_cset_drop_latest(&cx->win->cs);
    damage_offsets(cx->ed, old_lo, old_hi);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_cursor_collapse(CmdCtx *cx)
{
    ByteOff old_lo;
    ByteOff old_hi;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->cs.curs.len == 0U)
        return YEW_CMD_ERR_STATE;
    cursor_set_bounds(&cx->win->cs, &old_lo, &old_hi);
    yew_cset_remove_all_but_primary(&cx->win->cs);
    cx->win->cs.curs.data[0].anchor = cx->win->cs.curs.data[0].pos;
    yew_selstack_clear(cx->win);
    damage_offsets(cx->ed, old_lo, old_hi);
    return YEW_CMD_OK;
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
        return YEW_CMD_ERR_STATE;
    if (bytes == NULL && len != 0U)
        return YEW_CMD_ERR_ARG;
    line = yew_textbuf_line_of(tb, cursor->pos);
    old_line_count = yew_textbuf_line_count(tb);
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_insert(&ec, cursor->pos, bytes, len)) {
        yew_ed_finish_edit(cx->ed, &ec);
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    yew_ed_finish_edit(cx->ed, &ec);
    win->wrap_goal_valid = false;
    /* Typeahead is drained as one event-loop batch.  Following here would
     * rescan the growing line for every byte; render follows once instead. */
    cx->ed->cursor_follow_pending = true;
    yew_ed_damage_line(cx->ed, line,
                       old_line_count != yew_textbuf_line_count(tb));
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_insert_text(CmdCtx *cx)
{
    if (cx == NULL || cx->sarg == NULL)
        return YEW_CMD_ERR_ARG;
    return insert_bytes(cx, (const u8 *)cx->sarg, cx->sarg_len);
}

CmdStatus yew_edit_cmd_insert_at(CmdCtx *cx)
{
    EditCtx ec;
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->sarg == NULL || cx->iarg < 0 ||
        (u64)cx->iarg > yew_textbuf_len(cx->win->buf->tb))
        return YEW_CMD_ERR_ARG;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_insert(&ec, BYTEOFF((u64)cx->iarg), (const u8 *)cx->sarg,
                         cx->sarg_len)) {
        yew_ed_finish_edit(cx->ed, &ec);
        return YEW_CMD_ERR_IO;
    }
    yew_ed_finish_edit(cx->ed, &ec);
    yew_ed_damage_document(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_delete_span(CmdCtx *cx)
{
    EditCtx ec;
    Span s;
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL)
        return YEW_CMD_ERR_STATE;
    s = cx->range.given ? cx->range.tok :
                          (Span){(u64)cx->iarg, (u64)cx->iarg};
    if (s.lo > s.hi || s.hi > yew_textbuf_len(cx->win->buf->tb))
        return YEW_CMD_ERR_ARG;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_delete(&ec, s)) { yew_ed_finish_edit(cx->ed, &ec); return YEW_CMD_ERR_IO; }
    yew_ed_finish_edit(cx->ed, &ec);
    yew_ed_damage_document(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_replace_span(CmdCtx *cx)
{
    EditCtx ec;
    Span s;
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL || cx->sarg == NULL)
        return YEW_CMD_ERR_ARG;
    s = cx->range.given ? cx->range.tok :
                          (Span){(u64)cx->iarg, (u64)cx->iarg};
    if (s.lo > s.hi || s.hi > yew_textbuf_len(cx->win->buf->tb))
        return YEW_CMD_ERR_ARG;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_delete(&ec, s) ||
        !yew_edit_insert(&ec, BYTEOFF(s.lo), (const u8 *)cx->sarg,
                         cx->sarg_len)) {
        yew_ed_finish_edit(cx->ed, &ec);
        return YEW_CMD_ERR_IO;
    }
    yew_ed_finish_edit(cx->ed, &ec);
    yew_ed_damage_document(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_cursor_set(CmdCtx *cx)
{
    Cursor *c;
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U ||
        (cx->cursor_given
             ? (size_t)cx->cursor_index >= cx->win->cs.curs.len
             : (size_t)cx->win->cs.primary >= cx->win->cs.curs.len) ||
        cx->iarg < 0 ||
        (u64)cx->iarg > yew_textbuf_len(cx->win->buf->tb))
        return YEW_CMD_ERR_ARG;
    c = &cx->win->cs.curs.data[cx->cursor_given ? cx->cursor_index :
                                                   cx->win->cs.primary];
    cursor_place(cx->win->buf->tb, c, BYTEOFF((u64)cx->iarg));
    cx->win->wrap_goal_valid = false;
    yew_win_follow_cursor(cx->win);
    yew_ed_damage_document(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_insert_newline(CmdCtx *cx)
{
    const u8 *bytes;
    size_t len;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL)
        return YEW_CMD_ERR_STATE;
    yew_filemeta_eol_bytes(&cx->win->buf->meta, &bytes, &len);
    return insert_bytes(cx, bytes, (u64)len);
}

CmdStatus yew_edit_cmd_insert_tab(CmdCtx *cx)
{
    static const u8 tab = (u8)'\t';

    return insert_bytes(cx, &tab, 1U);
}

CmdStatus yew_edit_cmd_insert_after(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    ByteOff end;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    line = yew_textbuf_line_of(tb, cursor->pos);
    end = line_content_end(tb, line);
    if (cursor->pos.v < end.v)
        yew_cursor_right(tb, cursor);
    return yew_mode_enter(cx->ed, YEW_MODE_I);
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
        return YEW_CMD_ERR_STATE;
    line = yew_textbuf_line_of(tb, cursor->pos);
    span = yew_textbuf_line_span(tb, line);
    lines = yew_textbuf_line_count(tb);
    yew_filemeta_eol_bytes(&win->buf->meta, &eol, &eol_len);
    at = below ? BYTEOFF(span.hi) : BYTEOFF(span.lo);
    placed = at;
    if (below && line.v + 1U == lines)
        placed = BYTEOFF(at.v + (u64)eol_len);
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_insert(&ec, at, eol, (u64)eol_len)) {
        yew_ed_finish_edit(cx->ed, &ec);
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    yew_ed_finish_edit(cx->ed, &ec);
    cursor = &win->cs.curs.data[win->cs.primary];
    cursor_place(tb, cursor, placed);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    yew_ed_damage_line(cx->ed, line, true);
    return yew_mode_enter(cx->ed, YEW_MODE_I);
}

CmdStatus yew_edit_cmd_open_below(CmdCtx *cx)
{
    return open_line(cx, true);
}

CmdStatus yew_edit_cmd_open_above(CmdCtx *cx)
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
        return YEW_CMD_ERR_STATE;
    if (span.lo > span.hi || span.hi > yew_textbuf_len(tb))
        return YEW_CMD_ERR_ARG;
    line = yew_textbuf_line_of(tb, BYTEOFF(span.lo));
    old_line_count = yew_textbuf_line_count(tb);
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_delete(&ec, span)) {
        yew_ed_finish_edit(cx->ed, &ec);
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    yew_ed_finish_edit(cx->ed, &ec);
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    yew_ed_damage_line(cx->ed, line,
                       old_line_count != yew_textbuf_line_count(tb));
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_delete_grapheme_left(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff prev;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    prev = yew_grapheme_prev_boundary(tb, cursor->pos);
    return delete_span(cx, (Span){prev.v, cursor->pos.v});
}

CmdStatus yew_edit_cmd_delete_grapheme(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    ByteOff next;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    next = yew_grapheme_next_boundary(tb, cursor->pos);
    return delete_span(cx, (Span){cursor->pos.v, next.v});
}

CmdStatus yew_edit_cmd_delete_line(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    LineNo line;
    Span span;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    if (cx->range.kind != YEW_RANGE_NONE)
        return delete_span(cx, yew_range_span(tb, &cx->range));
    line = yew_textbuf_line_of(tb, cursor->pos);
    span = yew_textbuf_line_span(tb, line);
    if (line.v + 1U == yew_textbuf_line_count(tb) && line.v != 0U)
        span.lo = yew_grapheme_prev_boundary(tb, BYTEOFF(span.lo)).v;
    return delete_span(cx, span);
}

CmdStatus yew_edit_cmd_undo(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;
    bool batch;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    (void)tb;
    (void)cursor;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if ((ec.jrnl != NULL && !yew_journal_ok(ec.jrnl)) ||
        (yew_undo_current(ec.undo) != ec.undo->root &&
         !yew_edit_ensure_journal(&ec))) {
        yew_ed_finish_edit(cx->ed, &ec);
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    batch = win->cs.curs.len > 1U;
    if (batch)
        yew_ed_damage_batch_begin(cx->ed, win);
    (void)yew_undo(&ec);
    yew_ed_finish_edit(cx->ed, &ec);
    if (batch)
        yew_ed_damage_batch_end(cx->ed);
    if (ec.jrnl != NULL && !yew_journal_ok(ec.jrnl)) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    yew_ed_damage_document(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_redo(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;
    bool batch;

    if (!edit_window(cx, &win, &tb, &cursor))
        return YEW_CMD_ERR_STATE;
    (void)tb;
    (void)cursor;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if ((ec.jrnl != NULL && !yew_journal_ok(ec.jrnl)) ||
        (ec.undo->cur != 0U && ec.undo->cur <= ec.undo->nodes.len &&
         ec.undo->nodes.data[ec.undo->cur - 1U].redo_child != 0U &&
         !yew_edit_ensure_journal(&ec))) {
        yew_ed_finish_edit(cx->ed, &ec);
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    batch = win->cs.curs.len > 1U;
    if (batch)
        yew_ed_damage_batch_begin(cx->ed, win);
    (void)yew_redo(&ec);
    yew_ed_finish_edit(cx->ed, &ec);
    if (batch)
        yew_ed_damage_batch_end(cx->ed);
    if (ec.jrnl != NULL && !yew_journal_ok(ec.jrnl)) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot persist edit to crash journal");
        return YEW_CMD_ERR_IO;
    }
    win->wrap_goal_valid = false;
    yew_win_follow_cursor(win);
    yew_ed_damage_document(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_undo_barrier(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_ed_insert_barrier(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_edit_cmd_mode_enter(CmdCtx *cx)
{
    Mode mode;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL)
        return YEW_CMD_ERR_ARG;
    if (cx->sarg_len == 3U && cx->sarg[0] == 'H' &&
        cx->sarg[1] == ' ') {
        Mode unit;

        switch (cx->sarg[2]) {
        case 'L':
            unit = YEW_MODE_L;
            break;
        case 'W':
            unit = YEW_MODE_W;
            break;
        case 'B':
            unit = YEW_MODE_B;
            break;
        case 'C':
            unit = YEW_MODE_I;
            break;
        default:
            return YEW_CMD_ERR_ARG;
        }
        return yew_mode_enter_highlight(cx->ed, unit, true);
    }
    if (cx->sarg_len != 1U)
        return YEW_CMD_ERR_ARG;
    if (cx->sarg[0] == 'C') {
        cx->ed->prev_unit = YEW_MODE_I;
        if (cx->ed->mode == YEW_MODE_H)
            return yew_mode_enter_highlight(cx->ed, YEW_MODE_I,
                                            cx->ed->win->h.sticky);
        cx->ed->footer_dirty = true;
        return YEW_CMD_OK;
    }
    for (mode = YEW_MODE_L; mode < YEW_MODE__N; mode++) {
        if (yew_modes[mode].name[0] == cx->sarg[0] &&
            yew_modes[mode].name[1] == '\0')
            return yew_mode_enter(cx->ed, mode);
    }
    return YEW_CMD_ERR_ARG;
}

CmdStatus yew_edit_cmd_mode_escape(CmdCtx *cx)
{
    if (cx == NULL)
        return YEW_CMD_ERR_ARG;
    return yew_mode_escape(cx->ed);
}
