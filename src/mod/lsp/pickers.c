#define _POSIX_C_SOURCE 200809L

#include "mod/lsp/pickers.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "edit/pane_cmds.h"
#include "mod/lsp/sync.h"
#include "term/grid.h"
#include "text/piece.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "ui/tabs.h"
#include "ui/win.h"
#include "unicode/width.h"

enum {
    LSP_LOC_DETAIL_MAX = 512,
    LSP_LOC_PREVIEW_BYTES = 64U * 1024U,
    LSP_LOC_PREVIEW_LINES = 40
};

typedef struct LocSrc {
    Vec_LspLoc locs;
    PickItem *rows;
    char **labels;
    char **details;
    u32 nrows;
    u8 pos_enc;
} LocSrc;

static LocSrc source;

static void source_free(void)
{
    u32 i;

    for (i = 0U; i < source.nrows; i++) {
        free(source.labels[i]);
        free(source.details[i]);
    }
    free(source.rows);
    free(source.labels);
    free(source.details);
    yew_lsp_locations_free(&source.locs);
    (void)memset(&source, 0, sizeof(source));
}

void yew_lsp_pickers_free(void)
{
    source_free();
}

static Buffer *buffer_for_path(Ed *ed, const char *path)
{
    u32 i;

    if (ed == NULL || path == NULL)
        return NULL;
    for (i = 0U; i < ed->ws.nbufs; i++) {
        Buffer *b = ed->ws.bufs[i];

        if (b != NULL && b->path != NULL && strcmp(b->path, path) == 0)
            return b;
    }
    return NULL;
}

static char *copy_line(const TextBuf *tb, u32 line)
{
    Span span;
    TextIter it;
    char *out;
    u64 at = 0U;
    u64 len;

    if (tb == NULL || line >= yew_textbuf_line_count(tb))
        return NULL;
    span = yew_textbuf_line_span(tb, LINENO(line));
    while (span.hi > span.lo) {
        TextIter tail;
        const u8 *chunk;
        u64 avail;

        if (!yew_textiter_begin(&tail, tb, BYTEOFF(span.hi - 1U)) ||
            !yew_textiter_chunk(&tail, tb, &chunk, &avail) || avail == 0U ||
            (chunk[0] != (u8)'\n' && chunk[0] != (u8)'\r'))
            break;
        span.hi--;
    }
    while (span.lo < span.hi) {
        const u8 *chunk;
        u64 avail;

        if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)) ||
            !yew_textiter_chunk(&it, tb, &chunk, &avail) || avail == 0U ||
            (chunk[0] != (u8)' ' && chunk[0] != (u8)'\t'))
            break;
        span.lo++;
    }
    len = span.hi - span.lo;
    if (len > LSP_LOC_DETAIL_MAX)
        len = LSP_LOC_DETAIL_MAX;
    out = yew_xmalloc((size_t)len + 1U);
    if (len != 0U && !yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
        free(out);
        return NULL;
    }
    while (at < len) {
        const u8 *chunk;
        u64 avail;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &chunk, &avail) || avail == 0U) {
            free(out);
            return NULL;
        }
        take = avail < len - at ? avail : len - at;
        (void)memcpy(out + at, chunk, (size_t)take);
        at += take;
        if (at < len && !yew_textiter_advance(&it, tb)) {
            free(out);
            return NULL;
        }
    }
    out[len] = '\0';
    return out;
}

static char *read_line_file(const char *path, u32 line)
{
    FILE *file;
    char *text = NULL;
    size_t cap = 0U;
    ssize_t got;
    u32 at = 0U;
    char *copy = NULL;

    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    while ((got = getline(&text, &cap, file)) >= 0) {
        if (at == line) {
            size_t lo = 0U;
            size_t hi = (size_t)got;
            size_t len;

            while (lo < hi && (text[lo] == ' ' || text[lo] == '\t'))
                lo++;
            while (hi > lo && (text[hi - 1U] == '\n' ||
                               text[hi - 1U] == '\r'))
                hi--;
            len = hi - lo;
            if (len > LSP_LOC_DETAIL_MAX)
                len = LSP_LOC_DETAIL_MAX;
            copy = yew_xmalloc(len + 1U);
            if (len != 0U)
                (void)memcpy(copy, text + lo, len);
            copy[len] = '\0';
            break;
        }
        at++;
    }
    free(text);
    (void)fclose(file);
    return copy;
}

static char *location_detail(Ed *ed, const LspLoc *loc)
{
    Buffer *b = buffer_for_path(ed, loc->path);
    char *detail;

    detail = b != NULL && b->tb != NULL ? copy_line(b->tb, loc->line) :
                                          read_line_file(loc->path,
                                                         loc->line);
    if (detail == NULL) {
        detail = yew_xmalloc(1U);
        detail[0] = '\0';
    }
    return detail;
}

static const char *display_path(Ed *ed, const char *path)
{
    const char *root = yew_ws_root(ed);
    size_t root_len = strlen(root);

    if (root_len != 0U && strncmp(path, root, root_len) == 0 &&
        path[root_len] == '/')
        return path + root_len + 1U;
    return path;
}

static char *location_label(Ed *ed, const LspLoc *loc)
{
    const char *path = display_path(ed, loc->path);
    unsigned long long line = (unsigned long long)loc->line + 1ULL;
    unsigned long long chr = (unsigned long long)loc->chr + 1ULL;
    int n = snprintf(NULL, 0, "%s:%llu:%llu", path, line, chr);
    char *label;

    if (n < 0) {
        label = yew_xmalloc(sizeof("(location)"));
        (void)memcpy(label, "(location)", sizeof("(location)"));
        return label;
    }
    label = yew_xmalloc((size_t)n + 1U);
    (void)snprintf(label, (size_t)n + 1U, "%s:%llu:%llu", path, line,
                   chr);
    return label;
}

static const PickItem *location_items(void *ctx, u32 *n)
{
    LocSrc *src = ctx;

    *n = src == NULL ? 0U : src->nrows;
    return src == NULL ? NULL : src->rows;
}

static bool option_is(Ed *ed, const char *want)
{
    OptVal value;

    return yew_opt_get(ed, yew_ed_doc(ed), ed == NULL ? NULL : ed->win,
                       "lsp.open_in", 11U, &value) &&
           (value.type == (u8)YEW_OPT_STR ||
            value.type == (u8)YEW_OPT_ENUM) &&
           value.as.str.len == strlen(want) &&
           memcmp(value.as.str.s, want, value.as.str.len) == 0;
}

static bool jump_in_focused(Ed *ed, Win *w, Buffer *target, ByteOff pos)
{
    Cursor *cursor;

    if (ed == NULL || w == NULL || target == NULL || ed->win != w)
        return false;
    cursor = yew_ed_cursor(ed);
    if (cursor == NULL)
        return false;
    yew_jump_push(w, cursor->pos, ed->now_ms);
    if (!yew_ed_show_buffer(ed, target))
        return false;
    cursor = yew_ed_cursor(ed);
    if (cursor == NULL)
        return false;
    cursor->pos = pos;
    cursor->anchor = pos;
    cursor->goal_col = (GCol){0U};
    yew_win_follow_cursor(ed->win);
    yew_ed_damage_document(ed);
    return true;
}

bool yew_lsp_location_jump(Ed *ed, Win *w, const LspLoc *loc, u8 pos_enc)
{
    Buffer *target;
    ByteOff pos;

    if (ed == NULL || w == NULL || ed->win != w || loc == NULL ||
        loc->path == NULL)
        return false;
    target = yew_ws_file_buf(ed, loc->path);
    if (target == NULL || yew_buf_hydrate(ed, target) != 0)
        return false;
    pos = yew_lsp_off_of_pos(pos_enc, target->tb, LINENO(loc->line),
                             loc->chr);
    if (option_is(ed, "tab")) {
        Cursor *cursor = yew_ed_cursor(ed);
        int tab;

        if (cursor == NULL)
            return false;
        tab = yew_tab_find_by_path(ed, loc->path);
        if (tab < 0)
            tab = yew_tab_open(ed, loc->path);
        if (tab < 0)
            return false;
        yew_jump_push(w, cursor->pos, ed->now_ms);
        yew_tab_switch(ed, tab);
        if (ed->win == NULL || ed->win->buf == NULL ||
            ed->win->buf->tb == NULL)
            return false;
        pos = yew_lsp_off_of_pos(pos_enc, ed->win->buf->tb,
                                 LINENO(loc->line), loc->chr);
        cursor = yew_ed_cursor(ed);
        if (cursor == NULL)
            return false;
        cursor->pos = pos;
        cursor->anchor = pos;
        cursor->goal_col = (GCol){0U};
        yew_win_follow_cursor(ed->win);
        yew_ed_damage_document(ed);
        return true;
    }
    if (option_is(ed, "split")) {
        CmdCtx split = {0};

        split.ed = ed;
        split.win = w;
        if (yew_pane_cmd_split_v(&split) != YEW_CMD_OK)
            return false;
        w = ed->win;
    }
    return jump_in_focused(ed, w, target, pos);
}

static bool location_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    LocSrc *src = ctx;

    (void)how;
    if (ed == NULL || src == NULL || payload < 0 ||
        (u32)payload >= src->nrows || ed->win == NULL)
        return false;
    /* The picker already performed any requested split.  Its focused view
     * still shows the origin, so accept directly into that view instead of
     * consulting lsp.open_in a second time. */
    {
        Buffer *target = yew_ws_file_buf(ed, src->locs.data[payload].path);
        ByteOff pos;

        if (target == NULL || yew_buf_hydrate(ed, target) != 0)
            return false;
        pos = yew_lsp_off_of_pos(src->pos_enc, target->tb,
                                 LINENO(src->locs.data[payload].line),
                                 src->locs.data[payload].chr);
        return jump_in_focused(ed, ed->win, target, pos);
    }
}

static void location_preview(Ed *ed, void *ctx, i32 payload, Rect r)
{
    static u8 bytes[LSP_LOC_PREVIEW_BYTES];
    YewColor dim = {YEW_COLOR_RGB, 140U, 140U, 140U};
    YewColor bg = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    LocSrc *src = ctx;
    ssize_t got;
    int fd;
    u16 row = 0U;
    u64 at = 0U;
    u64 i;

    if (ed == NULL || src == NULL || payload < 0 ||
        (u32)payload >= src->nrows || r.w == 0U || r.h == 0U)
        return;
    fd = open(src->locs.data[payload].path, O_RDONLY);
    if (fd < 0)
        return;
    got = pread(fd, bytes, sizeof(bytes), 0);
    (void)close(fd);
    if (got <= 0)
        return;
    /* Use the editor's ordinary first-block NUL heuristic.  Feeding a
     * binary target through the text preview would otherwise turn control
     * bytes into terminal-facing grid content. */
    for (i = 0U; i < (u64)got && i < 8192U; i++) {
        if (bytes[i] == 0U) {
            static const u8 binary[] = "binary file";

            (void)yew_grid_puts(&ed->grid, r.y, r.x, binary,
                                sizeof(binary) - 1U, dim, bg,
                                YEW_ATTR_DIM);
            return;
        }
    }
    while (at < (u64)got && row < r.h &&
           row < (u16)LSP_LOC_PREVIEW_LINES) {
        u64 eol = at;
        size_t fit;
        int cells = 0;

        while (eol < (u64)got && bytes[eol] != (u8)'\n')
            eol++;
        fit = yew_str_clip(bytes + at, (size_t)(eol - at), (int)r.w,
                           &cells);
        (void)yew_grid_puts(&ed->grid, (u16)(r.y + row), r.x, bytes + at,
                            fit, dim, bg, YEW_ATTR_DIM);
        row++;
        at = eol + 1U;
    }
}

void yew_lsp_location_picker_open(Ed *ed, Win *w, Vec_LspLoc *locs,
                                  u8 pos_enc, const char *title)
{
    PickerSpec spec = {0};
    u32 i;

    if (ed == NULL || w == NULL || locs == NULL || locs->len == 0U)
        return;
    if (locs->len > INT32_MAX)
        return;
    source_free();
    source.locs = *locs;
    (void)memset(locs, 0, sizeof(*locs));
    source.nrows = (u32)source.locs.len;
    source.pos_enc = pos_enc;
    source.rows = yew_xcalloc(source.nrows, sizeof(*source.rows));
    source.labels = yew_xcalloc(source.nrows, sizeof(*source.labels));
    source.details = yew_xcalloc(source.nrows, sizeof(*source.details));
    for (i = 0U; i < source.nrows; i++) {
        source.labels[i] = location_label(ed, &source.locs.data[i]);
        source.details[i] = location_detail(ed, &source.locs.data[i]);
        source.rows[i].label = source.labels[i];
        source.rows[i].detail = source.details[i];
        source.rows[i].payload = (i32)i;
    }
    spec.title = title == NULL ? "locations" : title;
    spec.items = location_items;
    spec.preview = location_preview;
    spec.accept = location_accept;
    spec.path_mode = true;
    spec.ctx = &source;
    yew_picker_open(ed, &spec);
}
