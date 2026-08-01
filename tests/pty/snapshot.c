#include "snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "term/grid.h"
#include "unicode/width.h"

typedef struct SnapStyle {
    SagColor fg;
    SagColor bg;
    u16 attrs;
} SnapStyle;

typedef struct SnapLines {
    const u8 **line;
    size_t *len;
    size_t count;
} SnapLines;

static void put_text(Bytebuf *out, const char *text)
{
    bytebuf_append(out, text, strlen(text));
}

static bool color_equal(SagColor a, SagColor b)
{
    if (a.tag != b.tag)
        return false;
    if (a.tag == SAG_COLOR_DEFAULT)
        return true;
    if (a.tag == SAG_COLOR_INDEXED)
        return a.r == b.r;
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool style_equal(SnapStyle a, SnapStyle b)
{
    return color_equal(a.fg, b.fg) && color_equal(a.bg, b.bg) &&
           a.attrs == b.attrs;
}

static bool trim_blank(const VtScreen *v, const VtCell *cell)
{
    size_t glyph_len;

    (void)vt_cell_bytes(v, cell, &glyph_len);
    return glyph_len == 0u && cell->w == 1u &&
           cell->fg.tag == SAG_COLOR_DEFAULT &&
           cell->bg.tag == SAG_COLOR_DEFAULT && cell->attrs == 0u;
}

static size_t used_cells(const VtScreen *v, int row)
{
    const VtCell *cells = v->cells + (size_t)row * (size_t)v->cols;
    size_t used = (size_t)v->cols;

    while (used != 0u && trim_blank(v, &cells[used - 1u]))
        used--;
    return used;
}

static void token_write(Bytebuf *out, size_t index)
{
    if (index < 26u)
        bytebuf_push_u8(out, (u8)('A' + index));
    else if (index < 52u)
        bytebuf_push_u8(out, (u8)('a' + index - 26u));
    else {
        bytebuf_push_u8(out, '#');
        bytebuf_printf(out, "%zu", index - 52u);
    }
}

static size_t style_index(SnapStyle **styles, size_t *len, size_t *cap,
                          const VtCell *cell)
{
    SnapStyle wanted = {cell->fg, cell->bg, cell->attrs};
    size_t i;

    for (i = 0u; i < *len; i++) {
        if (style_equal((*styles)[i], wanted))
            return i;
    }
    if (*len == *cap) {
        size_t new_cap = *cap == 0u ? 16u : *cap * 2u;
        SnapStyle *new_styles = realloc(*styles,
                                        new_cap * sizeof(*new_styles));

        if (new_styles == NULL)
            abort();
        *styles = new_styles;
        *cap = new_cap;
    }
    (*styles)[*len] = wanted;
    return (*len)++;
}

static void color_write(Bytebuf *out, SagColor color)
{
    if (color.tag == SAG_COLOR_DEFAULT)
        put_text(out, "default");
    else if (color.tag == SAG_COLOR_INDEXED)
        bytebuf_printf(out, "idx:%u", (unsigned)color.r);
    else
        bytebuf_printf(out, "#%02x%02x%02x", (unsigned)color.r,
                       (unsigned)color.g, (unsigned)color.b);
}

static void attrs_write(Bytebuf *out, u16 attrs)
{
    static const u16 bits[10] = {
        SAG_ATTR_BOLD, SAG_ATTR_DIM, SAG_ATTR_ITALIC, SAG_ATTR_UNDERLINE,
        SAG_ATTR_UNDERCURL, SAG_ATTR_BLINK, SAG_ATTR_REVERSE,
        SAG_ATTR_CONCEAL, SAG_ATTR_STRIKE, SAG_ATTR_OVERLINE
    };
    static const u8 marks[10] = {'b', 'd', 'i', 'u', 'c',
                                 'k', 'r', 'h', 's', 'o'};
    size_t i;

    for (i = 0u; i < 10u; i++)
        bytebuf_push_u8(out, (attrs & bits[i]) != 0u ? marks[i] : (u8)'-');
}

static void modes_write(const VtScreen *v, Bytebuf *out)
{
    static const u32 bits[4] = {
        VT_MODE_BRACKETED_PASTE, VT_MODE_BUTTON_MOUSE,
        VT_MODE_SGR_MOUSE, VT_MODE_FOCUS
    };
    static const unsigned names[4] = {2004u, 1002u, 1006u, 1004u};
    size_t i;
    bool any = false;

    put_text(out, "modes ");
    for (i = 0u; i < 4u; i++) {
        if ((v->modes & bits[i]) == 0u)
            continue;
        if (any)
            bytebuf_push_u8(out, ',');
        bytebuf_printf(out, "%u", names[i]);
        any = true;
    }
    if (!any)
        bytebuf_push_u8(out, '-');
    bytebuf_printf(out, " kitty=%u sync_pairs=%u\n",
                   v->ksp > 0 ? (unsigned)v->kitty[v->ksp - 1] : 0u,
                   (unsigned)v->nsync_pairs);
}

void snapshot_write(const VtScreen *v, Bytebuf *out)
{
    static const u8 orphan[] = {0xc2u, 0xbfu};
    SnapStyle *styles = NULL;
    size_t nstyles = 0u;
    size_t cap = 0u;
    int row;
    int col;

    put_text(out, "# sagitta pty golden v1\n");
    bytebuf_printf(out, "size %dx%d alt=%d cursor=%d,%d vis=%d\n",
                   v->cols, v->rows, v->alt ? 1 : 0, v->cur_r, v->cur_c,
                   v->cur_vis ? 1 : 0);
    modes_write(v, out);
    for (row = 0; row < v->rows; row++) {
        for (col = 0; col < v->cols; col++) {
            (void)style_index(&styles, &nstyles, &cap,
                              &v->cells[(size_t)row * v->cols + col]);
        }
    }

    put_text(out, "--- text\n");
    for (row = 0; row < v->rows; row++) {
        const VtCell *cells = v->cells + (size_t)row * v->cols;
        size_t used = used_cells(v, row);

        for (col = 0; (size_t)col < used; col++) {
            const VtCell *cell = &cells[col];

            if (cell->w == 0u) {
                if (col == 0 || cells[col - 1].w != 2u)
                    bytebuf_append(out, orphan, sizeof(orphan));
            } else {
                size_t glyph_len;
                const u8 *glyph = vt_cell_bytes(v, cell, &glyph_len);

                if (glyph_len == 0u)
                    bytebuf_push_u8(out, ' ');
                else
                    bytebuf_append(out, glyph, glyph_len);
            }
        }
        bytebuf_push_u8(out, '\n');
    }

    put_text(out, "--- style\n");
    for (row = 0; row < v->rows; row++) {
        const VtCell *cells = v->cells + (size_t)row * v->cols;
        size_t used = used_cells(v, row);

        for (col = 0; (size_t)col < used; col++) {
            SnapStyle wanted = {cells[col].fg, cells[col].bg,
                                cells[col].attrs};
            size_t i;

            for (i = 0u; i < nstyles; i++) {
                if (style_equal(styles[i], wanted))
                    break;
            }
            token_write(out, i);
        }
        bytebuf_push_u8(out, '\n');
    }

    put_text(out, "--- legend\n");
    for (size_t i = 0u; i < nstyles; i++) {
        token_write(out, i);
        put_text(out, " fg=");
        color_write(out, styles[i].fg);
        put_text(out, " bg=");
        color_write(out, styles[i].bg);
        put_text(out, " attrs=");
        attrs_write(out, styles[i].attrs);
        bytebuf_push_u8(out, '\n');
    }
    free(styles);
}

static bool lines_make(const Bytebuf *buf, SnapLines *lines)
{
    size_t count = 0u;
    size_t start = 0u;
    size_t i;

    memset(lines, 0, sizeof(*lines));
    for (i = 0u; i < buf->len; i++) {
        if (buf->data[i] == '\n')
            count++;
    }
    if (buf->len != 0u && buf->data[buf->len - 1u] != '\n')
        count++;
    lines->line = calloc(count, sizeof(*lines->line));
    lines->len = calloc(count, sizeof(*lines->len));
    if (count != 0u && (lines->line == NULL || lines->len == NULL)) {
        free(lines->line);
        free(lines->len);
        return false;
    }
    for (i = 0u; i <= buf->len; i++) {
        if (i == buf->len || buf->data[i] == '\n') {
            if (i != start || i < buf->len) {
                lines->line[lines->count] = buf->data + start;
                lines->len[lines->count++] = i - start;
            }
            start = i + 1u;
        }
    }
    return true;
}

static void lines_free(SnapLines *lines)
{
    free(lines->line);
    free(lines->len);
}

static bool line_is(const SnapLines *lines, size_t i, const char *text)
{
    size_t len = strlen(text);

    return i < lines->count && lines->len[i] == len &&
           memcmp(lines->line[i], text, len) == 0;
}

static size_t first_diff(const u8 *a, size_t na, const u8 *b, size_t nb)
{
    size_t i;
    size_t n = na < nb ? na : nb;

    for (i = 0u; i < n; i++) {
        if (a[i] != b[i])
            return i;
    }
    return n;
}

static size_t display_prefix(const u8 *line, size_t len, size_t byte_col)
{
    size_t pos = 0u;
    size_t cells = 0u;

    while (pos < len && pos < byte_col) {
        size_t next = sag_gb_next_bytes(line, len, pos);
        int width;

        if (next <= pos || next > len || next > byte_col)
            break;
        width = sag_cluster_width(line + pos, next - pos);
        cells += width > 0 ? (size_t)width : 0u;
        pos = next;
    }
    return cells;
}

static void line_append(Bytebuf *out, const u8 *line, size_t len)
{
    bytebuf_append(out, line, len);
    bytebuf_push_u8(out, '\n');
}

static size_t legend_start(const SnapLines *lines)
{
    size_t i;

    for (i = 0u; i < lines->count; i++) {
        if (line_is(lines, i, "--- legend"))
            return i + 1u;
    }
    return lines->count;
}

bool snapshot_compare(const Bytebuf *got, const Bytebuf *want, Bytebuf *msg)
{
    SnapLines glines;
    SnapLines wlines;
    const char *full;
    size_t line = 0u;
    size_t col;
    size_t display_col;

    msg->len = 0u;
    if (got->len == want->len &&
        (got->len == 0u || memcmp(got->data, want->data, got->len) == 0))
        return true;
    full = getenv("SAG_PTY_DIFF");
    if (full != NULL && strcmp(full, "full") == 0) {
        put_text(msg, "snapshot differs\n--- want\n");
        bytebuf_append(msg, want->data, want->len);
        if (want->len == 0u || want->data[want->len - 1u] != '\n')
            bytebuf_push_u8(msg, '\n');
        put_text(msg, "--- got\n");
        bytebuf_append(msg, got->data, got->len);
        if (got->len == 0u || got->data[got->len - 1u] != '\n')
            bytebuf_push_u8(msg, '\n');
        return false;
    }
    if (!lines_make(got, &glines) || !lines_make(want, &wlines)) {
        put_text(msg, "snapshot differs (diff allocation failed)\n");
        return false;
    }
    while (line < glines.count && line < wlines.count &&
           glines.len[line] == wlines.len[line] &&
           memcmp(glines.line[line], wlines.line[line],
                  glines.len[line]) == 0)
        line++;
    col = first_diff(line < glines.count ? glines.line[line] : NULL,
                     line < glines.count ? glines.len[line] : 0u,
                     line < wlines.count ? wlines.line[line] : NULL,
                     line < wlines.count ? wlines.len[line] : 0u);
    display_col = line < wlines.count
                      ? display_prefix(wlines.line[line], wlines.len[line], col)
                      : col;
    bytebuf_printf(msg, "snapshot differs at line %zu, column %zu\n",
                   line + 1u, display_col + 1u);
    put_text(msg, "want: ");
    if (line < wlines.count)
        line_append(msg, wlines.line[line], wlines.len[line]);
    else
        put_text(msg, "<missing>\n");
    put_text(msg, " got: ");
    if (line < glines.count)
        line_append(msg, glines.line[line], glines.len[line]);
    else
        put_text(msg, "<missing>\n");
    put_text(msg, "      ");
    {
        for (size_t i = 0u; i < display_col; i++)
            bytebuf_push_u8(msg, ' ');
    }
    put_text(msg, "^\n");

    {
        size_t gi = legend_start(&glines);
        size_t wi = legend_start(&wlines);

        while (gi < glines.count || wi < wlines.count) {
            bool same = gi < glines.count && wi < wlines.count &&
                        glines.len[gi] == wlines.len[wi] &&
                        memcmp(glines.line[gi], wlines.line[wi],
                               glines.len[gi]) == 0;

            if (!same) {
                if (wi < wlines.count) {
                    put_text(msg, "legend want: ");
                    line_append(msg, wlines.line[wi], wlines.len[wi]);
                }
                if (gi < glines.count) {
                    put_text(msg, "legend got:  ");
                    line_append(msg, glines.line[gi], glines.len[gi]);
                }
            }
            if (gi < glines.count) gi++;
            if (wi < wlines.count) wi++;
        }
    }
    lines_free(&glines);
    lines_free(&wlines);
    return false;
}

static bool uint_read(const u8 **p, const u8 *end, unsigned *value)
{
    const u8 *start = *p;
    unsigned result = 0u;

    while (*p < end && **p >= '0' && **p <= '9') {
        result = result * 10u + (unsigned)(**p - '0');
        (*p)++;
    }
    if (*p == start)
        return false;
    *value = result;
    return true;
}

static bool literal_read(const u8 **p, const u8 *end, const char *literal)
{
    size_t len = strlen(literal);

    if ((size_t)(end - *p) < len || memcmp(*p, literal, len) != 0)
        return false;
    *p += len;
    return true;
}

static int hex_value(u8 byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static bool color_read(const u8 **p, const u8 *end, SagColor *color)
{
    unsigned index;

    *color = (SagColor){SAG_COLOR_DEFAULT, 0u, 0u, 0u};
    if (literal_read(p, end, "default"))
        return true;
    if (literal_read(p, end, "idx:")) {
        if (!uint_read(p, end, &index) || index > 255u)
            return false;
        color->tag = SAG_COLOR_INDEXED;
        color->r = (u8)index;
        return true;
    }
    if (*p < end && **p == '#' && (size_t)(end - *p) >= 7u) {
        int digits[6];
        size_t i;

        (*p)++;
        for (i = 0u; i < 6u; i++) {
            digits[i] = hex_value((*p)[i]);
            if (digits[i] < 0)
                return false;
        }
        *p += 6u;
        color->tag = SAG_COLOR_RGB;
        color->r = (u8)((digits[0] << 4) | digits[1]);
        color->g = (u8)((digits[2] << 4) | digits[3]);
        color->b = (u8)((digits[4] << 4) | digits[5]);
        return true;
    }
    return false;
}

static bool attrs_read(const u8 **p, const u8 *end, u16 *attrs)
{
    static const u16 bits[10] = {
        SAG_ATTR_BOLD, SAG_ATTR_DIM, SAG_ATTR_ITALIC, SAG_ATTR_UNDERLINE,
        SAG_ATTR_UNDERCURL, SAG_ATTR_BLINK, SAG_ATTR_REVERSE,
        SAG_ATTR_CONCEAL, SAG_ATTR_STRIKE, SAG_ATTR_OVERLINE
    };
    static const u8 marks[10] = {'b', 'd', 'i', 'u', 'c',
                                 'k', 'r', 'h', 's', 'o'};
    size_t i;

    if ((size_t)(end - *p) < 10u)
        return false;
    *attrs = 0u;
    for (i = 0u; i < 10u; i++) {
        if ((*p)[i] == marks[i])
            *attrs |= bits[i];
        else if ((*p)[i] != '-')
            return false;
    }
    *p += 10u;
    return true;
}

static bool token_read(const u8 **p, const u8 *end, size_t *index)
{
    unsigned value;

    if (*p >= end)
        return false;
    if (**p >= 'A' && **p <= 'Z') {
        *index = (size_t)(**p - 'A');
        (*p)++;
        return true;
    }
    if (**p >= 'a' && **p <= 'z') {
        *index = 26u + (size_t)(**p - 'a');
        (*p)++;
        return true;
    }
    if (**p != '#')
        return false;
    (*p)++;
    if (!uint_read(p, end, &value))
        return false;
    *index = 52u + value;
    return true;
}

static bool size_line_read(const SnapLines *lines, VtScreen *out)
{
    const u8 *p = lines->line[1];
    const u8 *end = p + lines->len[1];
    unsigned cols, rows, alt, cur_r, cur_c, vis;

    if (!literal_read(&p, end, "size ") || !uint_read(&p, end, &cols) ||
        !literal_read(&p, end, "x") || !uint_read(&p, end, &rows) ||
        !literal_read(&p, end, " alt=") || !uint_read(&p, end, &alt) ||
        !literal_read(&p, end, " cursor=") ||
        !uint_read(&p, end, &cur_r) || !literal_read(&p, end, ",") ||
        !uint_read(&p, end, &cur_c) || !literal_read(&p, end, " vis=") ||
        !uint_read(&p, end, &vis) || p != end || cols > 65535u ||
        rows > 65535u)
        return false;
    vt_init(out, (int)rows, (int)cols);
    out->alt = alt != 0u;
    out->cur_r = (int)cur_r;
    out->cur_c = (int)cur_c;
    out->cur_vis = vis != 0u;
    return true;
}

static bool modes_line_read(const SnapLines *lines, VtScreen *out)
{
    const u8 *p = lines->line[2];
    const u8 *end = p + lines->len[2];
    unsigned value;

    if (!literal_read(&p, end, "modes "))
        return false;
    if (p < end && *p == '-') {
        p++;
    } else {
        for (;;) {
            if (!uint_read(&p, end, &value)) return false;
            if (value == 2004u) out->modes |= VT_MODE_BRACKETED_PASTE;
            else if (value == 1002u) out->modes |= VT_MODE_BUTTON_MOUSE;
            else if (value == 1006u) out->modes |= VT_MODE_SGR_MOUSE;
            else if (value == 1004u) out->modes |= VT_MODE_FOCUS;
            else return false;
            if (p >= end || *p != ',') break;
            p++;
        }
    }
    if (!literal_read(&p, end, " kitty=") ||
        !uint_read(&p, end, &value)) return false;
    if (value != 0u) {
        out->kitty[0] = value;
        out->ksp = 1;
    }
    if (!literal_read(&p, end, " sync_pairs=") ||
        !uint_read(&p, end, &value) || p != end) return false;
    out->nsync_pairs = value;
    return true;
}

static bool legend_read(const SnapLines *lines, size_t start,
                        SnapStyle **styles, size_t *nstyles)
{
    size_t count = lines->count - start;
    size_t i;

    *styles = calloc(count, sizeof(**styles));
    if (count != 0u && *styles == NULL)
        return false;
    for (i = 0u; i < count; i++) {
        const u8 *p = lines->line[start + i];
        const u8 *end = p + lines->len[start + i];
        size_t index;

        if (!token_read(&p, end, &index) || index != i ||
            !literal_read(&p, end, " fg=") ||
            !color_read(&p, end, &(*styles)[i].fg) ||
            !literal_read(&p, end, " bg=") ||
            !color_read(&p, end, &(*styles)[i].bg) ||
            !literal_read(&p, end, " attrs=") ||
            !attrs_read(&p, end, &(*styles)[i].attrs) || p != end) {
            free(*styles);
            *styles = NULL;
            return false;
        }
    }
    *nstyles = count;
    return true;
}

static bool style_row_read(const u8 *line, size_t len, size_t *indices,
                           size_t cap, size_t *count, size_t nstyles)
{
    const u8 *p = line;
    const u8 *end = p + len;

    *count = 0u;
    while (p < end) {
        size_t index;

        if (*count == cap || !token_read(&p, end, &index) ||
            index >= nstyles)
            return false;
        indices[(*count)++] = index;
    }
    return true;
}

static bool screen_row_read(VtScreen *out, int row, const u8 *text,
                            size_t text_len, const size_t *style_ids,
                            size_t nids, const SnapStyle *styles)
{
    static const u8 orphan[] = {0xc2u, 0xbfu};
    size_t pos = 0u;
    size_t col = 0u;

    while (pos < text_len) {
        size_t next;
        int width;
        const u8 *bytes;
        size_t len;
        SnapStyle style;

        if (col >= nids)
            return false;
        style = styles[style_ids[col]];
        if (pos + sizeof(orphan) <= text_len &&
            memcmp(text + pos, orphan, sizeof(orphan)) == 0) {
            VtCell *cell = &out->cells[(size_t)row * out->cols + col];

            memset(cell, 0, sizeof(*cell));
            cell->fg = style.fg;
            cell->bg = style.bg;
            cell->attrs = style.attrs;
            pos += sizeof(orphan);
            col++;
            continue;
        }
        next = sag_gb_next_bytes(text, text_len, pos);
        if (next <= pos || next > text_len)
            return false;
        bytes = text + pos;
        len = next - pos;
        width = sag_cluster_width(bytes, len);
        if (width != 1 && width != 2)
            return false;
        if (len == 1u && bytes[0] == ' ')
            len = 0u;
        if (col + (size_t)width > nids ||
            !vt_set_cell(out, row, (int)col, bytes, len, style.fg, style.bg,
                         style.attrs, (u8)width))
            return false;
        if (width == 2) {
            SnapStyle tail_style = styles[style_ids[col + 1u]];

            if (!style_equal(style, tail_style) ||
                !vt_set_cell(out, row, (int)col + 1, NULL, 0u,
                             tail_style.fg, tail_style.bg, tail_style.attrs,
                             0u))
                return false;
        }
        col += (size_t)width;
        pos = next;
    }
    return col == nids;
}

bool snapshot_read(const Bytebuf *in, VtScreen *out, Bytebuf *msg)
{
    SnapLines lines;
    SnapStyle *styles = NULL;
    size_t nstyles = 0u;
    size_t text_mark = 0u;
    size_t style_mark = 0u;
    size_t legend_mark = 0u;
    size_t *style_ids = NULL;
    size_t i;
    bool ok = false;

    msg->len = 0u;
    if (!lines_make(in, &lines)) {
        put_text(msg, "snapshot: allocation failed\n");
        return false;
    }
    for (i = 0u; i < lines.count; i++) {
        if (line_is(&lines, i, "--- text")) text_mark = i;
        else if (line_is(&lines, i, "--- style")) style_mark = i;
        else if (line_is(&lines, i, "--- legend")) legend_mark = i;
    }
    if (lines.count < 6u ||
        !line_is(&lines, 0u, "# sagitta pty golden v1") ||
        text_mark != 3u ||
        legend_mark <= style_mark) {
        put_text(msg, "snapshot: malformed golden v1 blocks\n");
        goto done;
    }
    if (!size_line_read(&lines, out)) {
        put_text(msg, "snapshot: malformed size line\n");
        goto done;
    }
    if (style_mark != text_mark + 1u + (size_t)out->rows ||
        legend_mark != style_mark + 1u + (size_t)out->rows ||
        !modes_line_read(&lines, out) ||
        !legend_read(&lines, legend_mark + 1u, &styles, &nstyles)) {
        put_text(msg, "snapshot: malformed metadata or legend\n");
        vt_free(out);
        goto done;
    }
    style_ids = calloc((size_t)out->cols, sizeof(*style_ids));
    if (out->cols != 0 && style_ids == NULL) {
        put_text(msg, "snapshot: allocation failed\n");
        vt_free(out);
        goto done;
    }
    for (i = 0u; i < (size_t)out->rows; i++) {
        size_t nids;

        if (!style_row_read(lines.line[style_mark + 1u + i],
                            lines.len[style_mark + 1u + i], style_ids,
                            (size_t)out->cols, &nids, nstyles) ||
            !screen_row_read(out, (int)i, lines.line[text_mark + 1u + i],
                             lines.len[text_mark + 1u + i], style_ids,
                             nids, styles)) {
            put_text(msg, "snapshot: malformed text/style row\n");
            vt_free(out);
            goto done;
        }
    }
    ok = true;
done:
    free(style_ids);
    free(styles);
    lines_free(&lines);
    return ok;
}
