#include "ui/statusline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "edit/ed.h"
#include "text/file.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/width.h"
#include "util/log.h"

enum { STATUS_TABWIDTH = 4 };
enum { STATUS_WARN_COLOR = 214 };

typedef struct Segment {
    const char *text;
    u8 priority;
    bool shown;
} Segment;

SagUiStyle sag_statusline_mode_style(Mode mode)
{
    static const SagUiStyle styles[SAG_MODE__N] = {
        [SAG_MODE_L] = {{SAG_COLOR_RGB, 235U, 244U, 255U},
                        {SAG_COLOR_RGB, 38U, 110U, 186U},
                        {SAG_COLOR_RGB, 218U, 229U, 240U},
                        {SAG_COLOR_RGB, 20U, 48U, 76U}, SAG_ATTR_BOLD},
        [SAG_MODE_W] = {{SAG_COLOR_RGB, 10U, 35U, 40U},
                        {SAG_COLOR_RGB, 66U, 190U, 202U},
                        {SAG_COLOR_RGB, 205U, 235U, 238U},
                        {SAG_COLOR_RGB, 19U, 62U, 67U}, SAG_ATTR_BOLD},
        [SAG_MODE_B] = {{SAG_COLOR_RGB, 245U, 238U, 255U},
                        {SAG_COLOR_RGB, 126U, 87U, 194U},
                        {SAG_COLOR_RGB, 228U, 218U, 244U},
                        {SAG_COLOR_RGB, 52U, 37U, 78U}, SAG_ATTR_BOLD},
        [SAG_MODE_H] = {{SAG_COLOR_RGB, 43U, 31U, 8U},
                        {SAG_COLOR_RGB, 225U, 168U, 44U},
                        {SAG_COLOR_RGB, 245U, 229U, 194U},
                        {SAG_COLOR_RGB, 75U, 57U, 20U}, SAG_ATTR_BOLD},
        [SAG_MODE_I] = {{SAG_COLOR_RGB, 225U, 247U, 230U},
                        {SAG_COLOR_RGB, 42U, 145U, 72U},
                        {SAG_COLOR_RGB, 216U, 238U, 222U},
                        {SAG_COLOR_RGB, 18U, 64U, 31U}, SAG_ATTR_BOLD},
        [SAG_MODE_E] = {{SAG_COLOR_RGB, 47U, 24U, 6U},
                        {SAG_COLOR_RGB, 231U, 125U, 36U},
                        {SAG_COLOR_RGB, 247U, 224U, 204U},
                        {SAG_COLOR_RGB, 78U, 42U, 15U}, SAG_ATTR_BOLD},
        [SAG_MODE_F] = {{SAG_COLOR_RGB, 250U, 235U, 248U},
                        {SAG_COLOR_RGB, 177U, 61U, 155U},
                        {SAG_COLOR_RGB, 239U, 215U, 235U},
                        {SAG_COLOR_RGB, 72U, 27U, 64U}, SAG_ATTR_BOLD},
    };

    if (mode < SAG_MODE_L || mode >= SAG_MODE__N)
        SAG_BUG("statusline style: invalid mode");
    return styles[mode];
}

static int cells(const char *text)
{
    int width = sag_str_width((const u8 *)text, strlen(text),
                              STATUS_TABWIDTH);

    return width < 0 ? 0 : width;
}

static SagColor indexed_color(u8 index)
{
    SagColor color = {SAG_COLOR_INDEXED, index, 0U, 0U};

    return color;
}

static const char *base_name(const char *path)
{
    const char *slash;

    if (path == NULL)
        return "[no name]";
    slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool suffix_matches(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > path_len ||
        memcmp(path + path_len - suffix_len, suffix, suffix_len) != 0)
        return false;
    return suffix_len == path_len || path[path_len - suffix_len - 1U] == '/';
}

static const char *unambiguous_path(const Ed *ed, const Buffer *buffer)
{
    const char *path = buffer->path;
    const char *suffix;

    if (path == NULL)
        return "[no name]";
    suffix = base_name(path);
    for (;;) {
        u32 matches = 0U;
        u32 i;

        for (i = 0U; i < ed->ws.nbufs; i++) {
            const char *other = ed->ws.bufs[i]->path;

            if (other != NULL && suffix_matches(other, suffix))
                matches++;
        }
        if (matches <= 1U)
            return suffix;
        if (suffix == path)
            return path;
        suffix--;
        while (suffix > path && suffix[-1] != '/')
            suffix--;
    }
}

static bool read_only(const Buffer *buffer)
{
    mode_t write_bits = S_IWUSR | S_IWGRP | S_IWOTH;

    return buffer->meta.exists && (buffer->meta.mode & write_bits) == 0;
}

static const char *eol_text(SagEol eol)
{
    switch (eol) {
    case SAG_EOL_LF:
        return "lf";
    case SAG_EOL_CRLF:
        return "crlf";
    case SAG_EOL_MIXED:
        return "mixed!";
    }
    SAG_BUG("statusline: invalid EOL kind");
}

static void percent_text(Win *w, char *dst, size_t cap)
{
    u64 lines = sag_textbuf_line_count(w->buf->tb);
    u64 rows = w->vp.rows;
    u64 denominator;
    u64 percent;

    if (lines <= rows) {
        (void)snprintf(dst, cap, "all");
        return;
    }
    if (w->vp.top.v == 0U) {
        (void)snprintf(dst, cap, "top");
        return;
    }
    if (sag_vp_last_visible_line(w).v + 1U >= lines) {
        (void)snprintf(dst, cap, "bot");
        return;
    }
    denominator = lines - rows;
    percent = w->vp.top.v > UINT64_MAX / 100U ? 100U :
              (w->vp.top.v * 100U) / denominator;
    if (percent > 100U)
        percent = 100U;
    (void)snprintf(dst, cap, "%llu%%", (unsigned long long)percent);
}

static const char *highlight_unit(const Win *w)
{
    if (w->h.unit == &sag_unit_line)
        return "L";
    if (w->h.unit == &sag_unit_word)
        return "W";
    if (w->h.unit == &sag_unit_block)
        return "B";
    if (w->h.unit == &sag_unit_char)
        return "C";
    if (w->h.from >= SAG_MODE_L && w->h.from <= SAG_MODE_B)
        return sag_modes[w->h.from].name;
    return "C";
}

static void chip_text(const Ed *ed, const Win *w, char *dst, size_t cap)
{
    if (ed->mode == SAG_MODE_H) {
        (void)snprintf(dst, cap, " H\xC2\xB7%s ", highlight_unit(w));
    } else {
        (void)snprintf(dst, cap, " %s ", sag_modes[ed->mode].name);
    }
}

static int right_width(const Segment *segments, size_t count)
{
    int width = 0;
    size_t i;

    for (i = 0U; i < count; i++) {
        if (!segments[i].shown || segments[i].text[0] == '\0')
            continue;
        if (width != 0)
            width += 2;
        width += cells(segments[i].text);
    }
    return width;
}

static void drop_priority(Segment *segments, size_t count, u8 priority)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        if (segments[i].priority == priority)
            segments[i].shown = false;
    }
}

static void path_clip(const char *path, int max_cells,
                      char *dst, size_t cap)
{
    const char *base = base_name(path);
    const char *suffix = base;
    int base_cells = cells(base);
    static const char ellipsis[] = "\xE2\x80\xA6";

    if (cap == 0U)
        return;
    dst[0] = '\0';
    if (max_cells <= 0)
        return;
    if (cells(path) <= max_cells) {
        (void)snprintf(dst, cap, "%s", path);
        return;
    }
    if (base != path && base_cells + 2 <= max_cells) {
        const char *component = base;

        while (component > path) {
            const char *previous = component - 1U;

            while (previous > path && previous[-1] != '/')
                previous--;
            if (previous == path && path[0] == '/')
                previous++;
            if (previous >= component ||
                cells(previous) + 2 > max_cells)
                break;
            suffix = previous;
            component = previous;
        }
        (void)snprintf(dst, cap, "%s/%s", ellipsis, suffix);
        return;
    }
    if (max_cells == 1) {
        (void)snprintf(dst, cap, "%s", ellipsis);
        return;
    }
    {
        int used = 0;
        size_t keep = sag_str_clip((const u8 *)base, strlen(base),
                                   max_cells - 1, &used);

        if (keep >= cap)
            keep = cap - 1U;
        memcpy(dst, base, keep);
        dst[keep] = '\0';
        if (keep + sizeof(ellipsis) <= cap)
            (void)memcpy(dst + keep, ellipsis, sizeof(ellipsis));
    }
}

static size_t append_text(char *dst, size_t cap, size_t at,
                          const char *text)
{
    size_t len = strlen(text);
    size_t room;

    if (at >= cap)
        return at;
    room = cap - at - 1U;
    if (len > room)
        len = room;
    memcpy(dst + at, text, len);
    dst[at + len] = '\0';
    return at + len;
}

void sag_statusline_build(const Ed *ed, Win *w, u16 cols,
                          StatuslineText *out)
{
    const Cursor *cursor;
    LineNo line;
    LineNo anchor_line;
    Span line_span;
    Span anchor_span;
    GCol gcol;
    GCol anchor_gcol;
    const char *path;
    char encoding[16];
    char position[112];
    char percent[16];
    char cursor_badge[32];
    char *clipped_path;
    size_t path_len;
    Segment segments[8];
    int available;
    int path_cells;
    int dirty_cells;
    int right_cells;
    int min_cells;
    u8 priority;
    size_t at = 0U;
    size_t i;

    if (ed == NULL || w == NULL || out == NULL || w->buf == NULL ||
        w->buf->tb == NULL)
        SAG_BUG("statusline build: missing editor window");
    if (ed->mode < SAG_MODE_L || ed->mode >= SAG_MODE__N)
        SAG_BUG("statusline build: invalid mode");
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len)
        SAG_BUG("statusline build: missing primary cursor");
    memset(out, 0, sizeof(*out));
    path = unambiguous_path(ed, w->buf);
    path_len = strlen(path);
    if (path_len > SIZE_MAX - (size_t)cols - 512U)
        SAG_BUG("statusline build: text capacity overflow");
    out->body_cap = path_len + (size_t)cols + 512U;
    out->body = sag_xmalloc(out->body_cap);
    out->body[0] = '\0';
    chip_text(ed, w, out->chip, sizeof(out->chip));
    out->chip_len = strlen(out->chip);
    out->chip_cells = (u16)cells(out->chip);
    if (cols <= out->chip_cells)
        return;
    available = (int)cols - out->chip_cells;
    cursor = &w->cs.curs.data[w->cs.primary];
    line = sag_textbuf_line_of(w->buf->tb, cursor->pos);
    line_span = sag_textbuf_line_span(w->buf->tb, line);
    gcol = sag_off_to_gcol(w->buf->tb, line_span, cursor->pos);
    if (w->cs.selstacks.data[w->cs.primary].n == 0U ||
        cursor->anchor.v == cursor->pos.v) {
        (void)snprintf(position, sizeof(position), "%llu:%llu",
                       (unsigned long long)(line.v + 1U),
                       (unsigned long long)(gcol.v + 1U));
    } else {
        anchor_line = sag_textbuf_line_of(w->buf->tb, cursor->anchor);
        anchor_span = sag_textbuf_line_span(w->buf->tb, anchor_line);
        anchor_gcol = sag_off_to_gcol(w->buf->tb, anchor_span,
                                     cursor->anchor);
        (void)snprintf(position, sizeof(position), "%llu:%llu@%llu:%llu",
                       (unsigned long long)(line.v + 1U),
                       (unsigned long long)(gcol.v + 1U),
                       (unsigned long long)(anchor_line.v + 1U),
                       (unsigned long long)(anchor_gcol.v + 1U));
    }
    percent_text(w, percent, sizeof(percent));
    (void)snprintf(encoding, sizeof(encoding), "%s",
                   w->buf->meta.binary ? "utf-8 bin" : "utf-8");
    segments[0] = (Segment){"", 4U, read_only(w->buf)};
    if (segments[0].shown)
        segments[0].text = "[ro]";
    segments[1] = (Segment){encoding, 3U, true};
    segments[2] = (Segment){eol_text(w->buf->meta.eol), 3U, true};
    segments[3] = (Segment){"bom", 3U, w->buf->meta.had_bom};
    segments[4] = (Segment){"!utf8", 2U,
                            w->buf->meta.had_invalid_utf8};
    segments[5] = (Segment){position, 1U, true};
    segments[6] = (Segment){percent, 5U, true};
    if (w->cs.curs.len > 1U) {
        (void)snprintf(cursor_badge, sizeof(cursor_badge),
                       "\xC3\x97%llu",
                       (unsigned long long)w->cs.curs.len);
    } else {
        cursor_badge[0] = '\0';
    }
    segments[7] = (Segment){cursor_badge, 1U, w->cs.curs.len > 1U};
    path_cells = cells(path);
    dirty_cells = sag_buf_dirty(w->buf) ? 2 : 0;
    right_cells = right_width(segments, SAG_ARRAY_LEN(segments));
    min_cells = 1 + path_cells + dirty_cells +
                (right_cells == 0 ? 0 : 2 + right_cells);
    for (priority = 5U; priority >= 1U && min_cells > available;
         priority--) {
        drop_priority(segments, SAG_ARRAY_LEN(segments), priority);
        right_cells = right_width(segments, SAG_ARRAY_LEN(segments));
        min_cells = 1 + path_cells + dirty_cells +
                    (right_cells == 0 ? 0 : 2 + right_cells);
    }
    right_cells = right_width(segments, SAG_ARRAY_LEN(segments));
    {
        int reserved = 1 + dirty_cells +
                       (right_cells == 0 ? 0 : 2 + right_cells);
        int path_budget = available - reserved;

        if (path_budget < 0)
            path_budget = 0;
        clipped_path = sag_xmalloc(path_len + 4U);
        path_clip(path, path_budget, clipped_path, path_len + 4U);
    }
    path_cells = cells(clipped_path);
    at = append_text(out->body, out->body_cap, at, " ");
    at = append_text(out->body, out->body_cap, at, clipped_path);
    if (sag_buf_dirty(w->buf))
        at = append_text(out->body, out->body_cap, at, " *");
    if (right_cells != 0) {
        int used = 1 + path_cells + dirty_cells;
        int gap = available - used - right_cells;

        if (gap < 2)
            gap = 2;
        while (gap-- > 0)
            at = append_text(out->body, out->body_cap, at, " ");
        for (i = 0U; i < SAG_ARRAY_LEN(segments); i++) {
            if (!segments[i].shown || segments[i].text[0] == '\0')
                continue;
            if (i != 0U) {
                size_t j;
                bool prior = false;

                for (j = 0U; j < i; j++)
                    prior = prior || (segments[j].shown &&
                                      segments[j].text[0] != '\0');
                if (prior)
                    at = append_text(out->body, out->body_cap, at, "  ");
            }
            if (i == 2U && w->buf->meta.eol == SAG_EOL_MIXED)
                out->warn_at = at;
            at = append_text(out->body, out->body_cap, at,
                             segments[i].text);
            if (i == 2U && w->buf->meta.eol == SAG_EOL_MIXED)
                out->warn_len = at - out->warn_at;
        }
    }
    out->body_len = at;
    out->body_cells = (u16)cells(out->body);
    free(clipped_path);
}

void sag_statusline_text_free(StatuslineText *text)
{
    if (text == NULL)
        return;
    free(text->body);
    memset(text, 0, sizeof(*text));
}

void sag_statusline_draw(Ed *ed, Win *w)
{
    StatuslineText text;
    SagUiStyle style;
    Grid *grid;
    Cell blank;
    u16 row;
    u16 col;

    if (ed == NULL || w == NULL)
        SAG_BUG("statusline draw: missing editor window");
    grid = &ed->grid;
    if (ed->footer_rect.h == 0U || ed->footer_rect.y >= grid->rows ||
        grid->cols == 0U)
        return;
    row = ed->footer_rect.y;
    style = sag_statusline_mode_style(ed->mode);
    sag_statusline_build(ed, w, ed->footer_rect.w, &text);
    blank = grid->blank;
    blank.fg = style.row_fg;
    blank.bg = style.row_bg;
    blank.attrs = 0U;
    sag_grid_fill(grid, row, ed->footer_rect.x,
                  (u16)(ed->footer_rect.x + ed->footer_rect.w), blank);
    col = sag_grid_puts(grid, row, ed->footer_rect.x,
                        (const u8 *)text.chip, text.chip_len,
                        style.chip_fg, style.chip_bg, style.attrs);
    if (col < grid->cols && text.body_len != 0U) {
        if (text.warn_len == 0U) {
            (void)sag_grid_puts(grid, row, col, (const u8 *)text.body,
                                text.body_len, style.row_fg, style.row_bg,
                                0U);
        } else {
            size_t tail = text.warn_at + text.warn_len;

            col = sag_grid_puts(grid, row, col, (const u8 *)text.body,
                                text.warn_at, style.row_fg, style.row_bg,
                                0U);
            col = sag_grid_puts(grid, row, col,
                                (const u8 *)text.body + text.warn_at,
                                text.warn_len,
                                indexed_color(STATUS_WARN_COLOR),
                                style.row_bg, SAG_ATTR_BOLD);
            if (tail < text.body_len)
                (void)sag_grid_puts(grid, row, col,
                                    (const u8 *)text.body + tail,
                                    text.body_len - tail, style.row_fg,
                                    style.row_bg, 0U);
        }
    }
    sag_statusline_text_free(&text);
}
