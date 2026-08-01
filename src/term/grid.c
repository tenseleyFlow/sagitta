#include "term/grid.h"

#include <stdlib.h>
#include <string.h>

#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "unicode/width.h"
#include "util/log.h"

static void damage_reset(Grid *g)
{
    u16 row;

    for (row = 0; row < g->rows; row++) {
        g->dmg[row].lo = g->cols;
        g->dmg[row].hi = 0;
    }
    g->dmg_lo = g->rows;
    g->dmg_hi = 0;
}

static void damage_add(Grid *g, u16 row, u16 lo, u16 hi)
{
    Damage *d;

    if (row >= g->rows || lo >= hi || lo >= g->cols)
        return;
    if (hi > g->cols)
        hi = g->cols;
    d = &g->dmg[row];
    if (d->lo >= d->hi) {
        d->lo = lo;
        d->hi = hi;
    } else {
        if (lo < d->lo)
            d->lo = lo;
        if (hi > d->hi)
            d->hi = hi;
    }
    if (row < g->dmg_lo)
        g->dmg_lo = row;
    if ((u16)(row + 1u) > g->dmg_hi)
        g->dmg_hi = (u16)(row + 1u);
}

static void break_pair(Grid *g, u16 row, u16 col, u16 *lo, u16 *hi)
{
    Cell *cells = &g->back[(size_t)row * g->cols];

    if (cells[col].w == 0u && col > 0u) {
        cells[col - 1u] = g->blank;
        if ((u16)(col - 1u) < *lo)
            *lo = (u16)(col - 1u);
    }
    if (cells[col].w == 2u && (u16)(col + 1u) < g->cols)
        cells[col + 1u] = g->blank;
    if (col < *lo)
        *lo = col;
    if ((size_t)col + 2u < g->cols) {
        if ((u16)(col + 2u) > *hi)
            *hi = (u16)(col + 2u);
    } else if (g->cols > *hi) {
        *hi = g->cols;
    }
}

static SagColor color_normalize(SagColor color)
{
    if (color.tag == SAG_COLOR_DEFAULT || color.tag > SAG_COLOR_RGB) {
        memset(&color, 0, sizeof(color));
    } else if (color.tag == SAG_COLOR_INDEXED) {
        color.g = 0u;
        color.b = 0u;
    }
    return color;
}

static Cell cell_make(const u8 *bytes, size_t n, SagColor fg, SagColor bg,
                      u16 attrs, u8 width, Interner *gi)
{
    Cell c;

    memset(&c, 0, sizeof(c));
    c.fg = color_normalize(fg);
    c.bg = color_normalize(bg);
    c.attrs = (u16)(attrs & ((SAG_ATTR_INVALID_BYTE << 1) - 1u));
    c.w = width;
    if (n <= sizeof(c.utf8)) {
        if (n != 0u)
            memcpy(c.utf8, bytes, n);
    } else {
        c.id = sag_intern(gi, (const char *)bytes, n);
        c.flags = CELL_INTERNED;
    }
    return c;
}

static size_t inline_len(const Cell *c)
{
    size_t n = 0;

    while (n < sizeof(c->utf8) && c->utf8[n] != 0u)
        n++;
    return n;
}

static void append_zero_width(Grid *g, u16 row, u16 col,
                              const u8 *cluster, size_t n)
{
    Cell *cells;
    Cell *base;
    const u8 *old;
    size_t old_n;
    size_t total;
    u8 *joined;
    u16 base_col;

    if (col == 0u || row >= g->rows || col > g->cols) {
        sag_log(SAG_LOG_WARN, "dropping zero-width cluster without a base");
        return;
    }
    cells = &g->back[(size_t)row * g->cols];
    base_col = (u16)(col - 1u);
    if (cells[base_col].w == 0u) {
        if (base_col == 0u || cells[base_col - 1u].w != 2u) {
            sag_log(SAG_LOG_WARN,
                    "dropping zero-width cluster after orphan continuation");
            return;
        }
        base_col--;
    }
    base = &cells[base_col];
    if (base->w != 1u && base->w != 2u) {
        sag_log(SAG_LOG_WARN, "dropping zero-width cluster without a base");
        return;
    }
    if ((base->flags & CELL_INTERNED) == 0u && base->utf8[0] == 0u) {
        sag_log(SAG_LOG_WARN, "dropping zero-width cluster after a blank");
        return;
    }
    if ((base->flags & CELL_INTERNED) != 0u) {
        const char *interned = sag_intern_str(g->gi, base->id);

        if (interned == NULL)
            SAG_BUG("cell contains invalid grapheme intern id %u", base->id);
        old = (const u8 *)interned;
        old_n = strlen(interned);
    } else {
        old = base->utf8;
        old_n = inline_len(base);
    }
    if (n > SIZE_MAX - old_n)
        SAG_BUG("grapheme cluster length overflow");
    total = old_n + n;
    joined = sag_xmalloc(total);
    if (old_n != 0u)
        memcpy(joined, old, old_n);
    if (n != 0u)
        memcpy(joined + old_n, cluster, n);
    if (total <= sizeof(base->utf8)) {
        memset(base->utf8, 0, sizeof(base->utf8));
        memcpy(base->utf8, joined, total);
        base->flags = 0u;
    } else {
        u32 id = sag_intern(g->gi, (const char *)joined, total);

        memset(base->utf8, 0, sizeof(base->utf8));
        base->id = id;
        base->flags = CELL_INTERNED;
    }
    free(joined);
    damage_add(g, row, base_col, (u16)(base_col + 1u));
}

static u16 put_ascii_expansion(Grid *g, u16 row, u16 col,
                               const u8 *bytes, size_t n, SagColor fg,
                               SagColor bg, u16 attrs)
{
    size_t i;

    for (i = 0; i < n && col < g->cols; i++)
        col = sag_grid_put(g, row, col, &bytes[i], 1u, fg, bg, attrs);
    return col;
}

static bool control_expansion(const u8 *cluster, size_t n, u8 out[4],
                              size_t *out_n, bool *invalid)
{
    static const u8 hex[] = "0123456789ABCDEF";
    u32 cp;

    (void)sag_utf8_decode(cluster, n, &cp);
    *invalid = sag_utf8_is_escape(cp);
    if (*invalid || (cp >= 0x80u && cp <= 0x9fu)) {
        u8 byte = *invalid ? sag_utf8_escape_byte(cp) : (u8)cp;

        out[0] = '<';
        out[1] = hex[byte >> 4];
        out[2] = hex[byte & 0x0fu];
        out[3] = '>';
        *out_n = 4u;
        return true;
    }
    if (cp <= 0x1fu || cp == 0x7fu) {
        out[0] = '^';
        out[1] = cp == 0x7fu ? '?' : (u8)(cp + 0x40u);
        *out_n = 2u;
        return true;
    }
    return false;
}

bool sag_grid_init(Grid *g, Interner *gi, u16 rows, u16 cols)
{
    if (g == NULL || gi == NULL)
        return false;
    memset(g, 0, sizeof(*g));
    g->gi = gi;
    g->blank.w = 1u;
    return sag_grid_resize(g, rows, cols);
}

void sag_grid_free(Grid *g)
{
    if (g == NULL)
        return;
    free(g->front);
    free(g->back);
    free(g->dmg);
    memset(g, 0, sizeof(*g));
}

bool sag_grid_resize(Grid *g, u16 rows, u16 cols)
{
    Cell *front;
    Cell *back;
    Damage *dmg;
    size_t count;
    size_t i;

    if (g == NULL || g->gi == NULL)
        return false;
    count = (size_t)rows * cols;
    front = sag_xcalloc(count, sizeof(*front));
    back = sag_xcalloc(count, sizeof(*back));
    dmg = sag_xcalloc(rows, sizeof(*dmg));
    for (i = 0; i < count; i++) {
        front[i].w = 0xffu;
        back[i] = g->blank;
    }
    free(g->front);
    free(g->back);
    free(g->dmg);
    g->front = front;
    g->back = back;
    g->dmg = dmg;
    g->rows = rows;
    g->cols = cols;
    if (rows == 0u)
        g->cur_row = 0u;
    else if (g->cur_row >= rows)
        g->cur_row = (u16)(rows - 1u);
    if (cols == 0u)
        g->cur_col = 0u;
    else if (g->cur_col >= cols)
        g->cur_col = (u16)(cols - 1u);
    damage_reset(g);
    sag_grid_mark_all(g);
    return true;
}

void sag_grid_clear(Grid *g)
{
    size_t count;
    size_t i;

    if (g == NULL)
        return;
    count = (size_t)g->rows * g->cols;
    for (i = 0; i < count; i++)
        g->back[i] = g->blank;
    sag_grid_mark_all(g);
}

u16 sag_grid_put(Grid *g, u16 row, u16 col, const u8 *cluster, size_t n,
                 SagColor fg, SagColor bg, u16 attrs)
{
    Cell *cells;
    Cell head;
    int width;
    u16 lo;
    u16 hi;
    u8 expanded[4];
    size_t expanded_n;
    bool invalid;

    if (g == NULL || row >= g->rows || col >= g->cols || cluster == NULL ||
        n == 0u)
        return g != NULL && col > g->cols ? g->cols : col;
    if (n == 1u && cluster[0] == '\t') {
        sag_log(SAG_LOG_WARN, "dropping unresolved tab passed to grid");
        return col;
    }
    width = sag_cluster_width(cluster, n);
    if (control_expansion(cluster, n, expanded, &expanded_n, &invalid)) {
        if (invalid)
            attrs |= SAG_ATTR_INVALID_BYTE;
        return put_ascii_expansion(g, row, col, expanded, expanded_n, fg, bg,
                                   attrs);
    }
    if (width == 0) {
        append_zero_width(g, row, col, cluster, n);
        return col;
    }
    if (width != 1 && width != 2) {
        sag_log(SAG_LOG_WARN, "dropping cluster with unsupported width %d",
                width);
        return col;
    }

    cells = &g->back[(size_t)row * g->cols];
    lo = col;
    hi = (u16)(col + 1u);
    break_pair(g, row, col, &lo, &hi);
    if (width == 2 && (u16)(col + 1u) >= g->cols) {
        static const u8 space_byte = ' ';
        Cell space = cell_make(&space_byte, 1u, fg, bg, attrs, 1u, g->gi);

        cells[col] = space;
        damage_add(g, row, lo, hi);
        return g->cols;
    }
    if (width == 2)
        break_pair(g, row, (u16)(col + 1u), &lo, &hi);
    head = cell_make(cluster, n, fg, bg, attrs, (u8)width, g->gi);
    cells[col] = head;
    if (width == 2) {
        Cell tail;

        memset(&tail, 0, sizeof(tail));
        tail.fg = head.fg;
        tail.bg = head.bg;
        tail.attrs = head.attrs;
        tail.w = 0u;
        cells[col + 1u] = tail;
        if (g->cur_row == row && g->cur_col == (u16)(col + 1u))
            g->cur_col = col;
    }
    damage_add(g, row, lo, hi);
    return (u16)(col + (u16)width);
}

u16 sag_grid_puts(Grid *g, u16 row, u16 col, const u8 *s, size_t n,
                  SagColor fg, SagColor bg, u16 attrs)
{
    size_t pos = 0u;

    if (g == NULL || s == NULL || row >= g->rows)
        return col;
    while (pos < n && col < g->cols) {
        size_t next = sag_gb_next_bytes(s, n, pos);

        if (next <= pos || next > n)
            SAG_BUG("grapheme iterator returned invalid grid boundary");
        col = sag_grid_put(g, row, col, s + pos, next - pos, fg, bg, attrs);
        pos = next;
    }
    return col;
}

void sag_grid_fill(Grid *g, u16 row, u16 c0, u16 c1, Cell c)
{
    Cell *cells;
    u16 lo;
    u16 hi;
    u16 col;

    if (g == NULL || row >= g->rows || c0 >= g->cols || c0 >= c1)
        return;
    if (c1 > g->cols)
        c1 = g->cols;
    lo = c0;
    hi = c1;
    cells = &g->back[(size_t)row * g->cols];
    break_pair(g, row, c0, &lo, &hi);
    if ((u16)(c1 - 1u) != c0)
        break_pair(g, row, (u16)(c1 - 1u), &lo, &hi);
    if (c.w != 1u) {
        SagColor fg = c.fg;
        SagColor bg = c.bg;
        u16 attrs = c.attrs;

        c = g->blank;
        c.fg = color_normalize(fg);
        c.bg = color_normalize(bg);
        c.attrs = (u16)(attrs & ((SAG_ATTR_INVALID_BYTE << 1) - 1u));
    } else {
        const u8 *data;
        size_t n;

        c.fg = color_normalize(c.fg);
        c.bg = color_normalize(c.bg);
        c.attrs = (u16)(c.attrs & ((SAG_ATTR_INVALID_BYTE << 1) - 1u));
        c.flags &= CELL_INTERNED;
        if ((c.flags & CELL_INTERNED) != 0u) {
            const char *interned = sag_intern_str(g->gi, c.id);

            if (interned == NULL)
                SAG_BUG("grid fill contains invalid grapheme intern id %u",
                        c.id);
            data = (const u8 *)interned;
            n = strlen(interned);
        } else {
            n = inline_len(&c);
            data = c.utf8;
            if (n < sizeof(c.utf8))
                memset(c.utf8 + n, 0, sizeof(c.utf8) - n);
        }
        if (n != 0u &&
            (sag_gb_next_bytes(data, n, 0u) != n ||
             sag_cluster_width(data, n) != 1))
            SAG_BUG("grid fill requires one printable width-1 grapheme");
    }
    for (col = c0; col < c1; col++)
        cells[col] = c;
    damage_add(g, row, lo, hi);
}

void sag_grid_cursor(Grid *g, u16 row, u16 col, bool visible)
{
    if (g == NULL)
        return;
    if (g->rows == 0u || g->cols == 0u) {
        g->cur_row = 0u;
        g->cur_col = 0u;
        g->cur_vis = false;
        return;
    }
    if (row >= g->rows)
        row = (u16)(g->rows - 1u);
    if (col >= g->cols)
        col = (u16)(g->cols - 1u);
    if (g->back[(size_t)row * g->cols + col].w == 0u && col > 0u)
        col--;
    g->cur_row = row;
    g->cur_col = col;
    g->cur_vis = visible;
}

void sag_grid_mark_all(Grid *g)
{
    u16 row;

    if (g == NULL || g->rows == 0u || g->cols == 0u)
        return;
    for (row = 0; row < g->rows; row++) {
        g->dmg[row].lo = 0u;
        g->dmg[row].hi = g->cols;
    }
    g->dmg_lo = 0u;
    g->dmg_hi = g->rows;
}

void sag_grid_flip(Grid *g)
{
    u16 row;

    if (g == NULL)
        return;
    for (row = g->dmg_lo; row < g->dmg_hi; row++) {
        Damage *d = &g->dmg[row];

        if (d->lo < d->hi) {
            size_t off = (size_t)row * g->cols + d->lo;
            size_t count = (size_t)(d->hi - d->lo);

            memcpy(g->front + off, g->back + off,
                   count * sizeof(*g->front));
        }
    }
    damage_reset(g);
}

bool sag_cell_eq(const Cell *a, const Cell *b)
{
    return a != NULL && b != NULL && memcmp(a, b, sizeof(*a)) == 0;
}
