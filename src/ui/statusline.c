#include "search/searchui.h"
#include "ui/statusline.h"

#include "edit/job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "syn/theme.h"
#include "text/file.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/width.h"
#include "util/log.h"
#if YEW_WITH_LSP
#include "mod/lsp/diag.h"
#include "mod/lsp/lsp.h"
#endif

enum { STATUS_TABWIDTH = 4 };
enum { STATUS_WARN_COLOR = 214 };

typedef struct Segment {
    const char *text;
    u8 priority;
    bool shown;
} Segment;

YewUiStyle yew_statusline_mode_style(Mode mode)
{
    static const YewUiStyle styles[YEW_MODE__N] = {
        [YEW_MODE_L] = {{YEW_COLOR_RGB, 235U, 244U, 255U},
                        {YEW_COLOR_RGB, 38U, 110U, 186U},
                        {YEW_COLOR_RGB, 218U, 229U, 240U},
                        {YEW_COLOR_RGB, 20U, 48U, 76U}, YEW_ATTR_BOLD},
        [YEW_MODE_W] = {{YEW_COLOR_RGB, 10U, 35U, 40U},
                        {YEW_COLOR_RGB, 66U, 190U, 202U},
                        {YEW_COLOR_RGB, 205U, 235U, 238U},
                        {YEW_COLOR_RGB, 19U, 62U, 67U}, YEW_ATTR_BOLD},
        [YEW_MODE_B] = {{YEW_COLOR_RGB, 245U, 238U, 255U},
                        {YEW_COLOR_RGB, 126U, 87U, 194U},
                        {YEW_COLOR_RGB, 228U, 218U, 244U},
                        {YEW_COLOR_RGB, 52U, 37U, 78U}, YEW_ATTR_BOLD},
        [YEW_MODE_H] = {{YEW_COLOR_RGB, 43U, 31U, 8U},
                        {YEW_COLOR_RGB, 225U, 168U, 44U},
                        {YEW_COLOR_RGB, 245U, 229U, 194U},
                        {YEW_COLOR_RGB, 75U, 57U, 20U}, YEW_ATTR_BOLD},
        [YEW_MODE_I] = {{YEW_COLOR_RGB, 225U, 247U, 230U},
                        {YEW_COLOR_RGB, 42U, 145U, 72U},
                        {YEW_COLOR_RGB, 216U, 238U, 222U},
                        {YEW_COLOR_RGB, 18U, 64U, 31U}, YEW_ATTR_BOLD},
        [YEW_MODE_E] = {{YEW_COLOR_RGB, 47U, 24U, 6U},
                        {YEW_COLOR_RGB, 231U, 125U, 36U},
                        {YEW_COLOR_RGB, 247U, 224U, 204U},
                        {YEW_COLOR_RGB, 78U, 42U, 15U}, YEW_ATTR_BOLD},
        [YEW_MODE_F] = {{YEW_COLOR_RGB, 250U, 235U, 248U},
                        {YEW_COLOR_RGB, 177U, 61U, 155U},
                        {YEW_COLOR_RGB, 239U, 215U, 235U},
                        {YEW_COLOR_RGB, 72U, 27U, 64U}, YEW_ATTR_BOLD},
    };

    if (mode < YEW_MODE_L || mode >= YEW_MODE__N)
        YEW_BUG("statusline style: invalid mode");
    return styles[mode];
}

static int cells(const char *text)
{
    int width = yew_str_width((const u8 *)text, strlen(text),
                              STATUS_TABWIDTH);

    return width < 0 ? 0 : width;
}

static const char *mode_theme_role(Mode mode)
{
    static const char *const roles[YEW_MODE__N] = {
        [YEW_MODE_L] = "mode.line",
        [YEW_MODE_W] = "mode.word",
        [YEW_MODE_B] = "mode.block",
        [YEW_MODE_H] = "mode.highlight",
        [YEW_MODE_I] = "mode.insert",
        [YEW_MODE_E] = "mode.explore",
        [YEW_MODE_F] = "mode.fuss",
    };

    return mode >= YEW_MODE_L && mode < YEW_MODE__N ? roles[mode] : NULL;
}

static void statusline_apply_theme(const Ed *ed, Mode mode,
                                   YewUiStyle *style)
{
    const ThemeEnt *chip;
    const ThemeEnt *fg;
    const ThemeEnt *bg;

    if (style == NULL)
        return;
    chip = yew_theme_ui_tab(ed, mode_theme_role(mode));
    fg = yew_theme_ui_tab(ed, "fg");
    bg = yew_theme_ui_tab(ed, "bg");
    if (chip != NULL) {
        if (chip->fg.tag != YEW_COLOR_DEFAULT)
            style->chip_fg = chip->fg;
        if (chip->bg.tag != YEW_COLOR_DEFAULT)
            style->chip_bg = chip->bg;
        style->attrs = chip->attrs;
    }
    if (fg != NULL && fg->fg.tag != YEW_COLOR_DEFAULT)
        style->row_fg = fg->fg;
    if (bg != NULL && bg->bg.tag != YEW_COLOR_DEFAULT)
        style->row_bg = bg->bg;
}

static YewColor indexed_color(u8 index)
{
    YewColor color = {YEW_COLOR_INDEXED, index, 0U, 0U};

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

    /* A scratch buffer carries a display name instead of a path — showing
     * "[no name]" while the user looks at *job:3 make* tells them nothing
     * about what they are reading. */
    if (buffer->name != NULL)
        return buffer->name;
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

static const char *eol_text(YewEol eol)
{
    switch (eol) {
    case YEW_EOL_LF:
        return "lf";
    case YEW_EOL_CRLF:
        return "crlf";
    case YEW_EOL_MIXED:
        return "mixed!";
    }
    YEW_BUG("statusline: invalid EOL kind");
}

static void percent_text(Win *w, char *dst, size_t cap)
{
    u64 lines = yew_textbuf_line_count(w->buf->tb);
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
    if (yew_vp_last_visible_line(w).v + 1U >= lines) {
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
    if (w->h.unit == &yew_unit_line)
        return "L";
    if (w->h.unit == &yew_unit_word)
        return "W";
    if (w->h.unit == &yew_unit_block)
        return "B";
    if (w->h.unit == &yew_unit_char)
        return "C";
    if (w->h.from >= YEW_MODE_L && w->h.from <= YEW_MODE_B)
        return yew_modes[w->h.from].name;
    return "C";
}

static void chip_text(const Ed *ed, const Win *w, char *dst, size_t cap)
{
    if (ed->mode == YEW_MODE_H) {
        (void)snprintf(dst, cap, " H\xC2\xB7%s ", highlight_unit(w));
    } else {
        (void)snprintf(dst, cap, " %s ", yew_modes[ed->mode].name);
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
        size_t keep = yew_str_clip((const u8 *)base, strlen(base),
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

void yew_statusline_build(const Ed *ed, Win *w, u16 cols,
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
    char job_badge[32];
    char *clipped_path;
    size_t path_len;
    char search_badge[40];
    char wrap_badge[8];
    char syn_badge[8];
    char diag_badge[48];
    size_t diag_error_off = 0U;
    size_t diag_error_len = 0U;
    size_t diag_warn_off = 0U;
    size_t diag_warn_len = 0U;
    char recording[32];
    RecStatus rec_status;
    Segment segments[14];
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
        YEW_BUG("statusline build: missing editor window");
    if (ed->mode < YEW_MODE_L || ed->mode >= YEW_MODE__N)
        YEW_BUG("statusline build: invalid mode");
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len)
        YEW_BUG("statusline build: missing primary cursor");
    memset(out, 0, sizeof(*out));
    path = unambiguous_path(ed, w->buf);
    path_len = strlen(path);
    if (path_len > SIZE_MAX - (size_t)cols - 512U)
        YEW_BUG("statusline build: text capacity overflow");
    out->body_cap = path_len + (size_t)cols + 512U;
    out->body = yew_xmalloc(out->body_cap);
    out->body[0] = '\0';
    chip_text(ed, w, out->chip, sizeof(out->chip));
    out->chip_len = strlen(out->chip);
    out->chip_cells = (u16)cells(out->chip);
    (void)memset(&rec_status, 0, sizeof(rec_status));
    (void)yew_record_status(ed, &rec_status);
    if (rec_status.active) {
        if (rec_status.nevents >= 10U)
            (void)snprintf(recording, sizeof(recording),
                           "\xE2\x97\x8FREC %c %u", (int)rec_status.reg,
                           (unsigned)rec_status.nevents);
        else
            (void)snprintf(recording, sizeof(recording),
                           "\xE2\x97\x8FREC %c", (int)rec_status.reg);
        if ((int)cols - (int)out->chip_cells < cells(recording))
            (void)snprintf(recording, sizeof(recording),
                           "\xE2\x97\x8F%c", (int)rec_status.reg);
        (void)snprintf(out->recording, sizeof(out->recording), "%s",
                       recording);
        out->recording_len = strlen(out->recording);
        out->recording_cells = (u16)cells(out->recording);
    }
    if (cols <= (u16)(out->chip_cells + out->recording_cells))
        return;
    available = (int)cols - out->chip_cells - out->recording_cells;
    cursor = &w->cs.curs.data[w->cs.primary];
    line = yew_textbuf_line_of(w->buf->tb, cursor->pos);
    line_span = yew_textbuf_line_span(w->buf->tb, line);
    gcol = yew_off_to_gcol(w->buf->tb, line_span, cursor->pos);
    if (w->cs.selstacks.data[w->cs.primary].n == 0U ||
        cursor->anchor.v == cursor->pos.v) {
        (void)snprintf(position, sizeof(position), "%llu:%llu",
                       (unsigned long long)(line.v + 1U),
                       (unsigned long long)(gcol.v + 1U));
    } else {
        anchor_line = yew_textbuf_line_of(w->buf->tb, cursor->anchor);
        anchor_span = yew_textbuf_line_span(w->buf->tb, anchor_line);
        anchor_gcol = yew_off_to_gcol(w->buf->tb, anchor_span,
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
    {
        /* Sprint 19 §8: the job badge is present iff something is
         * running, and disappears at zero — a badge that lingers at
         * "0 jobs" trains people to ignore it. */
        u32 running = yew_job_running_count(ed);

        if (running != 0U)
            (void)snprintf(job_badge, sizeof(job_badge),
                           "\xE2\x9F\xA8%llu\xE2\x9F\xA9",
                           (unsigned long long)running);
        else
            job_badge[0] = '\0';
        segments[8] = (Segment){job_badge, 2U, running != 0U};
    }
    {
        /*
         * Sprint 21 §3: `[3/17]`, and `[3/10000+]` past the cap.  The
         * `+` is not decoration — it says the editor stopped counting
         * rather than that there are exactly ten thousand, which is the
         * difference between a bounded feature and a wrong number.
         */
        const MatchOverlay *ov = &w->overlay;
        bool show = ov->count_total != 0U;

        if (show) {
            if (ov->cur_index >= 0)
                (void)snprintf(search_badge, sizeof(search_badge),
                               "[%llu/%llu%s]",
                               (unsigned long long)ov->cur_index + 1ULL,
                               (unsigned long long)ov->count_total,
                               ov->count_capped ? "+" : "");
            else
                (void)snprintf(search_badge, sizeof(search_badge),
                               "[%llu%s]",
                               (unsigned long long)ov->count_total,
                               ov->count_capped ? "+" : "");
        } else {
            search_badge[0] = '\0';
        }
        segments[9] = (Segment){search_badge, 2U, show};
    }
    {
        /* The wrap indicator, for the two seconds after a search came
         * round the other end.  It is a glyph rather than a message so
         * it does not displace whatever the message line is saying. */
        bool show = yew_search_wrap_until(ed) > ed->now_ms;

        if (show)
            (void)snprintf(wrap_badge, sizeof(wrap_badge), "\xE2\x86\xBB");
        else
            wrap_badge[0] = '\0';
        segments[10] = (Segment){wrap_badge, 2U, show};
    }
    {
        bool show = yew_syn_status_visible(&w->buf->syn);

        if (show)
            (void)snprintf(syn_badge, sizeof(syn_badge), "%s",
                           w->buf->syn.degraded ? "syn!" :
                           "syn\xE2\x80\xA6");
        else
            syn_badge[0] = '\0';
        segments[11] = (Segment){syn_badge, 4U, show};
    }
    diag_badge[0] = '\0';
#if YEW_WITH_LSP
    (void)yew_lsp_status_badge(ed, w->buf, diag_badge,
                               sizeof(diag_badge));
    if (w->buf->diag != NULL &&
        (w->buf->diag->n[YEW_DIAG_ERROR] != 0U ||
         w->buf->diag->n[YEW_DIAG_WARN] != 0U)) {
        size_t used = strlen(diag_badge);

        if (w->buf->diag->n[YEW_DIAG_ERROR] != 0U) {
            if (used != 0U && used + 1U < sizeof(diag_badge))
                diag_badge[used++] = ' ';
            diag_error_off = used;
            (void)snprintf(diag_badge + used, sizeof(diag_badge) - used,
                           "E:%u",
                           (unsigned)w->buf->diag->n[YEW_DIAG_ERROR]);
            used = strlen(diag_badge);
            diag_error_len = used - diag_error_off;
        }
        if (w->buf->diag->n[YEW_DIAG_WARN] != 0U) {
            if (used != 0U && used + 1U < sizeof(diag_badge))
                diag_badge[used++] = ' ';
            diag_warn_off = used;
            (void)snprintf(diag_badge + used, sizeof(diag_badge) - used,
                           "W:%u",
                           (unsigned)w->buf->diag->n[YEW_DIAG_WARN]);
            used = strlen(diag_badge);
            diag_warn_len = used - diag_warn_off;
        }
    }
#endif
    segments[12] = (Segment){diag_badge, 5U, diag_badge[0] != '\0'};
    segments[13] = (Segment){w->buf->lang, 3U,
                             w->buf->lang != NULL &&
                             strncmp(w->buf->lang, "fortran", 7U) == 0};
    path_cells = cells(path);
    dirty_cells = yew_buf_dirty(w->buf) ? 2 : 0;
    right_cells = right_width(segments, YEW_ARRAY_LEN(segments));
    min_cells = 1 + path_cells + dirty_cells +
                (right_cells == 0 ? 0 : 2 + right_cells);
    for (priority = 5U; priority >= 1U && min_cells > available;
         priority--) {
        drop_priority(segments, YEW_ARRAY_LEN(segments), priority);
        right_cells = right_width(segments, YEW_ARRAY_LEN(segments));
        min_cells = 1 + path_cells + dirty_cells +
                    (right_cells == 0 ? 0 : 2 + right_cells);
    }
    right_cells = right_width(segments, YEW_ARRAY_LEN(segments));
    {
        int reserved = 1 + dirty_cells +
                       (right_cells == 0 ? 0 : 2 + right_cells);
        int path_budget = available - reserved;

        if (path_budget < 0)
            path_budget = 0;
        clipped_path = yew_xmalloc(path_len + 4U);
        path_clip(path, path_budget, clipped_path, path_len + 4U);
    }
    path_cells = cells(clipped_path);
    at = append_text(out->body, out->body_cap, at, " ");
    at = append_text(out->body, out->body_cap, at, clipped_path);
    if (yew_buf_dirty(w->buf))
        at = append_text(out->body, out->body_cap, at, " *");
    if (right_cells != 0) {
        int used = 1 + path_cells + dirty_cells;
        int gap = available - used - right_cells;

        if (gap < 2)
            gap = 2;
        while (gap-- > 0)
            at = append_text(out->body, out->body_cap, at, " ");
        for (i = 0U; i < YEW_ARRAY_LEN(segments); i++) {
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
            if (i == 2U && w->buf->meta.eol == YEW_EOL_MIXED)
                out->warn_at = at;
            if (i == 12U) {
                if (diag_error_len != 0U) {
                    out->diag_error_at = at + diag_error_off;
                    out->diag_error_len = diag_error_len;
                }
                if (diag_warn_len != 0U) {
                    out->diag_warn_at = at + diag_warn_off;
                    out->diag_warn_len = diag_warn_len;
                }
            }
            at = append_text(out->body, out->body_cap, at,
                             segments[i].text);
            if (i == 2U && w->buf->meta.eol == YEW_EOL_MIXED)
                out->warn_len = at - out->warn_at;
        }
    }
    out->body_len = at;
    out->body_cells = (u16)cells(out->body);
    free(clipped_path);
}

void yew_statusline_text_free(StatuslineText *text)
{
    if (text == NULL)
        return;
    free(text->body);
    memset(text, 0, sizeof(*text));
}

typedef struct StatusRoleSpan {
    size_t at;
    size_t len;
    const char *role;
    bool metadata_warn;
} StatusRoleSpan;

static u16 statusline_draw_role(Ed *ed, Grid *grid, u16 row, u16 col,
                                const u8 *text, size_t len,
                                const YewUiStyle *base,
                                const StatusRoleSpan *span)
{
    YewColor fg = base->row_fg;
    YewColor bg = base->row_bg;
    u16 attrs = 0U;

    if (span->metadata_warn) {
        fg = indexed_color(STATUS_WARN_COLOR);
        attrs = YEW_ATTR_BOLD;
    } else {
        const ThemeEnt *theme = yew_theme_ui_tab(ed, span->role);

        if (theme != NULL) {
            if (theme->fg.tag != YEW_COLOR_DEFAULT)
                fg = theme->fg;
            if (theme->bg.tag != YEW_COLOR_DEFAULT)
                bg = theme->bg;
            attrs = theme->attrs;
        }
    }
    return yew_grid_puts(grid, row, col, text, len, fg, bg, attrs);
}

static void statusline_draw_body(Ed *ed, Grid *grid, u16 row, u16 col,
                                 const StatuslineText *text,
                                 const YewUiStyle *style)
{
    StatusRoleSpan spans[3];
    size_t n = 0U;
    size_t pos = 0U;
    size_t i;

    if (text->warn_len != 0U)
        spans[n++] = (StatusRoleSpan){text->warn_at, text->warn_len,
                                     NULL, true};
    if (text->diag_error_len != 0U)
        spans[n++] = (StatusRoleSpan){text->diag_error_at,
                                     text->diag_error_len,
                                     "diag.error", false};
    if (text->diag_warn_len != 0U)
        spans[n++] = (StatusRoleSpan){text->diag_warn_at,
                                     text->diag_warn_len,
                                     "diag.warn", false};
    for (i = 0U; i < n; i++) {
        if (spans[i].at > pos)
            col = yew_grid_puts(grid, row, col,
                                (const u8 *)text->body + pos,
                                spans[i].at - pos, style->row_fg,
                                style->row_bg, 0U);
        col = statusline_draw_role(ed, grid, row, col,
                                   (const u8 *)text->body + spans[i].at,
                                   spans[i].len, style, &spans[i]);
        pos = spans[i].at + spans[i].len;
    }
    if (pos < text->body_len)
        (void)yew_grid_puts(grid, row, col, (const u8 *)text->body + pos,
                            text->body_len - pos, style->row_fg,
                            style->row_bg, 0U);
}

void yew_statusline_draw(Ed *ed, Win *w)
{
    StatuslineText text;
    YewUiStyle style;
    YewUiStyle recording_style;
    Grid *grid;
    Cell blank;
    u16 row;
    u16 col;

    if (ed == NULL || w == NULL)
        YEW_BUG("statusline draw: missing editor window");
    grid = &ed->grid;
    if (ed->footer_rect.h == 0U || ed->footer_rect.y >= grid->rows ||
        grid->cols == 0U)
        return;
    row = ed->footer_rect.y;
    style = yew_statusline_mode_style(ed->mode);
    statusline_apply_theme(ed, ed->mode, &style);
    recording_style = yew_statusline_mode_style(YEW_MODE_H);
    statusline_apply_theme(ed, YEW_MODE_H, &recording_style);
    yew_statusline_build(ed, w, ed->footer_rect.w, &text);
    blank = grid->blank;
    blank.fg = style.row_fg;
    blank.bg = style.row_bg;
    blank.attrs = 0U;
    yew_grid_fill(grid, row, ed->footer_rect.x,
                  (u16)(ed->footer_rect.x + ed->footer_rect.w), blank);
    col = yew_grid_puts(grid, row, ed->footer_rect.x,
                        (const u8 *)text.chip, text.chip_len,
                        style.chip_fg, style.chip_bg, style.attrs);
    if (col < grid->cols && text.recording_len != 0U)
        col = yew_grid_puts(grid, row, col,
                            (const u8 *)text.recording,
                            text.recording_len,
                            recording_style.chip_fg,
                            recording_style.chip_bg,
                            YEW_ATTR_BOLD);
    if (col < grid->cols && text.body_len != 0U) {
        statusline_draw_body(ed, grid, row, col, &text, &style);
    }
    yew_statusline_text_free(&text);
}
