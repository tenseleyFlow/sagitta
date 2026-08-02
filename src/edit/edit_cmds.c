#include "edit/edit_cmds.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "unicode/coords.h"
#include "util/log.h"

static bool edit_window(CmdCtx *cx, Win **win, TextBuf **tb, Cursor **cursor)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        cx->win->cs.curs.len == 0U ||
        (size_t)cx->win->cs.primary >= cx->win->cs.curs.len)
        return false;
    *win = cx->win;
    *tb = cx->win->buf->tb;
    *cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    return true;
}

static void cursor_place(const TextBuf *tb, Cursor *cursor, ByteOff pos)
{
    Span line;

    cursor->pos = pos;
    cursor->anchor = pos;
    line = sag_textbuf_line_span(tb, sag_textbuf_line_of(tb, pos));
    cursor->goal_col = sag_off_to_gcol(tb, line, pos);
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
    u64 i;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    for (i = 0U; i < rows; i++) {
        if (down)
            sag_cursor_down(tb, cursor);
        else
            sag_cursor_up(tb, cursor);
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
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_home(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    sag_cursor_line_home(tb, cursor);
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

CmdStatus sag_edit_cmd_move_line_end(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    sag_cursor_line_end(tb, cursor);
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
    u64 rows;

    if (cx == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    rows = cx->win->vp.rows;
    return move_vertical(cx, false, rows == 0U ? 1U : rows);
}

CmdStatus sag_edit_cmd_view_page_down(CmdCtx *cx)
{
    u64 rows;

    if (cx == NULL || cx->win == NULL)
        return SAG_CMD_ERR_STATE;
    rows = cx->win->vp.rows;
    return move_vertical(cx, true, rows == 0U ? 1U : rows);
}

CmdStatus sag_edit_cmd_move_char_prev(CmdCtx *cx)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    sag_cursor_left(tb, cursor);
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
    sag_win_follow_cursor(win);
    return SAG_CMD_OK;
}

static CmdStatus insert_bytes(CmdCtx *cx, const u8 *bytes, u64 len)
{
    Win *win;
    TextBuf *tb;
    Cursor *cursor;
    EditCtx ec;

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    if (bytes == NULL && len != 0U)
        return SAG_CMD_ERR_ARG;
    ec = sag_ed_edit_ctx(cx->ed);
    sag_edit_insert(&ec, cursor->pos, bytes, len);
    sag_ed_finish_edit(cx->ed, &ec);
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
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
    sag_edit_insert(&ec, at, eol, (u64)eol_len);
    sag_ed_finish_edit(cx->ed, &ec);
    cursor = &win->cs.curs.data[win->cs.primary];
    cursor_place(tb, cursor, placed);
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
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

    if (!edit_window(cx, &win, &tb, &cursor))
        return SAG_CMD_ERR_STATE;
    if (span.lo > span.hi || span.hi > sag_textbuf_len(tb))
        return SAG_CMD_ERR_ARG;
    ec = sag_ed_edit_ctx(cx->ed);
    sag_edit_delete(&ec, span);
    sag_ed_finish_edit(cx->ed, &ec);
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
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
    (void)sag_undo(&ec);
    sag_ed_finish_edit(cx->ed, &ec);
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
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
    (void)sag_redo(&ec);
    sag_ed_finish_edit(cx->ed, &ec);
    sag_win_follow_cursor(win);
    cx->ed->full_damage = true;
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

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->sarg_len != 1U)
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
