#include "ui/message.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "ui/statusline.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"

enum {
    YEW_MESSAGE_INFO_MS = 4000,
    YEW_MESSAGE_WARN_MS = 8000,
    YEW_MESSAGE_OVERLAY_ROWS = 5,
    YEW_MESSAGE_WARN_COLOR = 214,
    YEW_MESSAGE_ERROR_COLOR = 196,
    YEW_MESSAGE_PROMPT_COLOR = 81
};

static const char ellipsis[] = "\xe2\x80\xa6";

static const Msg *visible_message(const Ed *ed)
{
    if (ed->msg.active)
        return &ed->msg;
    return ed->msg_hint.active ? &ed->msg_hint : NULL;
}

static YewColor indexed_color(u8 index)
{
    YewColor color = {YEW_COLOR_INDEXED, index, 0U, 0U};

    return color;
}

static void message_damage(Ed *ed)
{
    ed->footer_dirty = true;
    /* Expanded overlays and prompt messages borrow document rows. */
    if (ed->msg.expanded || ed->cmdline.active)
        ed->full_damage = true;
}

static void message_expire(Ed *ed, void *ctx)
{
    bool expanded;

    (void)ctx;
    if (ed == NULL)
        return;
    expanded = ed->msg.expanded;
    free(ed->msg.full);
    (void)memset(&ed->msg, 0, sizeof(ed->msg));
    ed->footer_dirty = true;
    if (expanded || ed->cmdline.active)
        ed->full_damage = true;
}

void yew_msg_clear(Ed *ed)
{
    bool expanded;

    if (ed == NULL)
        return;
    expanded = ed->msg.expanded;
    if (ed->msg.expiry != YEW_TIMER_NONE)
        (void)yew_timer_cancel(&ed->timers, ed->msg.expiry);
    free(ed->msg.full);
    (void)memset(&ed->msg, 0, sizeof(ed->msg));
    ed->footer_dirty = true;
    if (expanded || ed->cmdline.active)
        ed->full_damage = true;
}

void yew_msg_hint_clear(Ed *ed)
{
    if (ed == NULL)
        return;
    free(ed->msg_hint.full);
    (void)memset(&ed->msg_hint, 0, sizeof(ed->msg_hint));
    ed->footer_dirty = true;
}

static void hint_vset(Ed *ed, MsgSev sev, const char *fmt, va_list ap)
{
    va_list copy;
    int needed;

    if (ed == NULL)
        return;
    if (fmt == NULL) {
        yew_msg_hint_clear(ed);
        return;
    }
    yew_msg_hint_clear(ed);
    va_copy(copy, ap);
    needed = vsnprintf(ed->msg_hint.text, sizeof(ed->msg_hint.text), fmt,
                       copy);
    va_end(copy);
    if (needed < 0) {
        ed->msg_hint.text[0] = '\0';
        needed = 0;
    } else if ((size_t)needed >= sizeof(ed->msg_hint.text)) {
        ed->msg_hint.full = yew_xmalloc((size_t)needed + 1U);
        va_copy(copy, ap);
        (void)vsnprintf(ed->msg_hint.full, (size_t)needed + 1U, fmt, copy);
        va_end(copy);
    }
    ed->msg_hint.len = (size_t)needed;
    ed->msg_hint.sev = sev;
    ed->msg_hint.active = true;
    ed->msg_hint.expiry = YEW_TIMER_NONE;
    ed->footer_dirty = true;
}

void yew_msg_hint(Ed *ed, MsgSev sev, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    hint_vset(ed, sev, fmt, ap);
    va_end(ap);
}

bool yew_msg_visible(const Ed *ed)
{
    return ed != NULL && visible_message(ed) != NULL;
}

static void message_vset(Ed *ed, MsgSev sev, i64 now_ms,
                         const char *fmt, va_list ap)
{
    i64 duration;
    bool prompt;
    va_list copy;
    int needed;

    if (ed == NULL || fmt == NULL)
        return;
    prompt = ed->prompt != YEW_PROMPT_NONE;
    if (ed->msg.active && ed->msg.prompt && !prompt)
        return;
    if (ed->msg.active && ed->msg.sev == YEW_MSG_ERROR &&
        sev == YEW_MSG_INFO && !prompt)
        return;

    yew_msg_clear(ed);
    va_copy(copy, ap);
    needed = vsnprintf(ed->msg.text, sizeof(ed->msg.text), fmt, copy);
    va_end(copy);
    if (needed < 0) {
        ed->msg.text[0] = '\0';
        needed = 0;
    } else if ((size_t)needed >= sizeof(ed->msg.text)) {
        ed->msg.full = yew_xmalloc((size_t)needed + 1U);
        va_copy(copy, ap);
        (void)vsnprintf(ed->msg.full, (size_t)needed + 1U, fmt, copy);
        va_end(copy);
    }
    ed->msg.len = (size_t)needed;
    ed->msg.sev = sev;
    ed->msg.active = true;
    ed->msg.prompt = prompt;
    if (sev == YEW_MSG_ERROR && ed->errorbells && ed->tty_ready)
        (void)!write(ed->tty.wfd, "\a", 1U);
    duration = prompt || sev == YEW_MSG_ERROR
                   ? -1
                   : sev == YEW_MSG_INFO ? YEW_MESSAGE_INFO_MS
                                         : YEW_MESSAGE_WARN_MS;
    if (duration >= 0) {
        if (now_ms > INT64_MAX - duration)
            now_ms = INT64_MAX - duration;
        ed->msg.expiry = yew_timer_add(&ed->timers, now_ms + duration,
                                       message_expire, NULL);
    }
    message_damage(ed);
}

static const char *message_text(const Msg *msg)
{
    return msg->full == NULL ? msg->text : msg->full;
}

void yew_msg_at(Ed *ed, MsgSev sev, i64 now_ms, const char *fmt, ...)
{
    va_list ap;

    if (ed == NULL || fmt == NULL)
        return;
    va_start(ap, fmt);
    message_vset(ed, sev, now_ms, fmt, ap);
    va_end(ap);
}

void yew_msg(Ed *ed, MsgSev sev, const char *fmt, ...)
{
    va_list ap;

    if (ed == NULL || fmt == NULL)
        return;
    va_start(ap, fmt);
    message_vset(ed, sev, yew_now_ms(), fmt, ap);
    va_end(ap);
}

bool yew_msg_expand(Ed *ed)
{
    if (ed == NULL || !ed->msg.active || !ed->msg.truncated ||
        ed->msg.expanded)
        return false;
    ed->msg.expanded = true;
    message_damage(ed);
    return true;
}

bool yew_msg_dismiss_overlay(Ed *ed)
{
    if (ed == NULL || !ed->msg.expanded)
        return false;
    ed->msg.expanded = false;
    ed->full_damage = true;
    return true;
}

size_t yew_message_clip(const char *text, size_t len, u16 max_cells,
                        char *out, size_t out_cap, bool *truncated)
{
    const u8 *bytes = (const u8 *)text;
    size_t take;
    size_t written = 0U;
    int cells;
    bool cut;

    if (truncated != NULL)
        *truncated = false;
    if (out != NULL && out_cap != 0U)
        out[0] = '\0';
    if (text == NULL)
        return 0U;

    take = yew_str_clip(bytes, len, (int)max_cells, &cells);
    cut = take < len;
    if (cut && max_cells != 0U)
        take = yew_str_clip(bytes, len, (int)max_cells - 1, &cells);

    if (out != NULL && out_cap != 0U) {
        size_t room = out_cap - 1U;
        size_t next;

        if (cut && max_cells != 0U && room >= sizeof(ellipsis) - 1U)
            room -= sizeof(ellipsis) - 1U;
        if (take > room) {
            written = 0U;
            while (written < take) {
                next = yew_gb_next_bytes(bytes, take, written);
                if (next <= written || next > room)
                    break;
                written = next;
            }
        } else {
            written = take;
        }
        if (written != 0U)
            memcpy(out, text, written);
        if (cut && max_cells != 0U && out_cap - 1U - written >= 3U) {
            memcpy(out + written, ellipsis, 3U);
            written += 3U;
        }
        out[written] = '\0';
    } else {
        written = take + (cut && max_cells != 0U ? 3U : 0U);
    }
    if (truncated != NULL)
        *truncated = cut;
    return written;
}

static Cell styled_blank(YewColor fg, YewColor bg, u16 attrs)
{
    Cell cell;

    (void)memset(&cell, 0, sizeof(cell));
    cell.fg = fg;
    cell.bg = bg;
    cell.attrs = attrs;
    cell.w = 1U;
    return cell;
}

YewUiStyle yew_message_style(const Ed *ed)
{
    YewUiStyle style = yew_statusline_mode_style(ed->mode);
    const Msg *msg = visible_message(ed);

    /* Style remains queryable for a prepared, not-yet-active message. */
    if (msg == NULL)
        msg = &ed->msg;
    if (msg->prompt) {
        style.row_fg = indexed_color(YEW_MESSAGE_PROMPT_COLOR);
        style.attrs = YEW_ATTR_BOLD;
    } else if (msg->sev == YEW_MSG_WARN) {
        style.row_fg = indexed_color(YEW_MESSAGE_WARN_COLOR);
        style.attrs = YEW_ATTR_BOLD;
    } else if (msg->sev == YEW_MSG_ERROR) {
        style.row_fg = indexed_color(YEW_MESSAGE_ERROR_COLOR);
        style.attrs = YEW_ATTR_BOLD;
    } else {
        style.attrs = 0U;
    }
    return style;
}

static u16 draw_text_row(Grid *grid, u16 row, u16 col, u16 right,
                         const char *text, size_t len, YewUiStyle style,
                         bool ellipsize, bool *was_cut)
{
    char *clipped;
    size_t clipped_len;
    bool cut = false;

    clipped = yew_xmalloc(len + sizeof(ellipsis));
    clipped_len = yew_message_clip(text, len, (u16)(right - col), clipped,
                                   len + sizeof(ellipsis),
                                   ellipsize ? &cut : NULL);
    col = yew_grid_puts(grid, row, col, (const u8 *)clipped, clipped_len,
                        style.row_fg, style.row_bg, style.attrs);
    free(clipped);
    if (col < right)
        yew_grid_fill(grid, row, col, right,
                      styled_blank(style.row_fg, style.row_bg, style.attrs));
    if (was_cut != NULL)
        *was_cut = cut;
    return col;
}

static size_t row_take(const char *text, size_t len, u16 cells)
{
    return yew_str_clip((const u8 *)text, len, (int)cells, NULL);
}

void yew_message_draw(Ed *ed, Win *w)
{
    StatuslineText status;
    YewUiStyle style;
    YewUiStyle mode_style;
    YewUiStyle recording_style;
    Grid *grid;
    size_t text_len;
    size_t pos = 0U;
    u16 footer;
    u16 chip_cells;
    u16 prefix_cells;
    u16 available;
    u16 rows = 1U;
    u16 first;
    u16 i;
    const char *text;
    Msg *msg;

    if (ed == NULL || w == NULL || !yew_msg_visible(ed))
        return;
    msg = ed->msg.active ? &ed->msg : &ed->msg_hint;
    grid = &ed->grid;
    if (grid->rows == 0U || grid->cols == 0U)
        return;
    footer = ed->footer_rect.h != 0U && ed->footer_rect.y < grid->rows
                 ? ed->footer_rect.y
                 : (u16)(grid->rows - 1U);
    yew_statusline_build(ed, w, grid->cols, &status);
    mode_style = yew_statusline_mode_style(ed->mode);
    recording_style = yew_statusline_mode_style(YEW_MODE_H);
    style = yew_message_style(ed);
    chip_cells = (u16)yew_str_width((const u8 *)status.chip,
                                    status.chip_len, 1U);
    if (chip_cells > grid->cols)
        chip_cells = grid->cols;
    prefix_cells = chip_cells;
    if (status.recording_cells > (u16)(grid->cols - prefix_cells))
        prefix_cells = grid->cols;
    else
        prefix_cells = (u16)(prefix_cells + status.recording_cells);
    available = (u16)(grid->cols - prefix_cells);
    text = message_text(msg);
    text_len = msg->len;
    msg->truncated = row_take(text, text_len, available) < text_len;

    if (msg->expanded && msg->truncated && available != 0U) {
        size_t scan = 0U;

        rows = 0U;
        while (scan < text_len && rows < YEW_MESSAGE_OVERLAY_ROWS) {
            size_t take = row_take(text + scan, text_len - scan,
                                   available);

            if (take == 0U)
                break;
            scan += take;
            rows++;
        }
        if (rows == 0U)
            rows = 1U;
        if (rows > footer + 1U)
            rows = (u16)(footer + 1U);
    }
    first = (u16)(footer + 1U - rows);
    for (i = 0U; i < rows; i++) {
        u16 row = (u16)(first + i);
        u16 col = prefix_cells;
        size_t take;
        bool last = i + 1U == rows;
        bool remainder;

        yew_grid_fill(grid, row, 0U, grid->cols,
                      styled_blank(style.row_fg, style.row_bg, style.attrs));
        take = row_take(text + pos, text_len - pos, available);
        remainder = pos + take < text_len;
        if (last && remainder) {
            (void)draw_text_row(grid, row, col, grid->cols,
                                text + pos, text_len - pos, style,
                                true, NULL);
            pos = text_len;
        } else {
            col = yew_grid_puts(grid, row, col,
                                (const u8 *)text + pos, take,
                                style.row_fg, style.row_bg, style.attrs);
            if (col < grid->cols)
                yew_grid_fill(grid, row, col, grid->cols,
                              styled_blank(style.row_fg, style.row_bg,
                                           style.attrs));
            pos += take;
        }
    }

    /* The modal anchor remains on the footer even while continuation rows
     * occupy the document area above it. */
    {
        u16 col = yew_grid_puts(grid, footer, 0U,
                                (const u8 *)status.chip,
                                status.chip_len, mode_style.chip_fg,
                                mode_style.chip_bg, mode_style.attrs);

        if (col < grid->cols && status.recording_len != 0U)
            (void)yew_grid_puts(grid, footer, col,
                                (const u8 *)status.recording,
                                status.recording_len,
                                recording_style.chip_fg,
                                recording_style.chip_bg,
                                YEW_ATTR_BOLD);
    }
    yew_statusline_text_free(&status);
}
