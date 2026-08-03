#include "term/render.h"

#include <string.h>

#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/intern.h"
#include "util/log.h"

enum {
    ATTR_BOLD = 1u << 0,
    ATTR_DIM = 1u << 1,
    ATTR_ITALIC = 1u << 2,
    ATTR_UNDERLINE = 1u << 3,
    ATTR_UNDERCURL = 1u << 4,
    ATTR_BLINK = 1u << 5,
    ATTR_REVERSE = 1u << 6,
    ATTR_CONCEAL = 1u << 7,
    ATTR_STRIKE = 1u << 8,
    ATTR_OVERLINE = 1u << 9,
    ATTR_TERMINAL = (1u << 10) - 1u
};

static Bytebuf sag_oob;
static bool sag_oob_ready;

void sag_term_oob_queue(const u8 *seq, u64 n)
{
    if (n == 0u)
        return;
    if (seq == NULL)
        SAG_BUG("terminal OOB queue received NULL bytes");
    if (n > SIZE_MAX)
        SAG_BUG("terminal OOB sequence exceeds addressable memory");
    if (!sag_oob_ready) {
        bytebuf_init(&sag_oob);
        sag_oob_ready = true;
    }
    bytebuf_append(&sag_oob, seq, (size_t)n);
}

u64 sag_term_oob_pending(void)
{
    return sag_oob_ready ? (u64)sag_oob.len : 0u;
}

size_t sag_term_oob_flush(Bytebuf *out)
{
    size_t n;

    if (out == NULL)
        SAG_BUG("terminal OOB flush received NULL output");
    if (!sag_oob_ready || sag_oob.len == 0u)
        return 0u;
    n = sag_oob.len;
    bytebuf_append(out, sag_oob.data, n);
    sag_oob.len = 0u;
    return n;
}

void sag_term_oob_clear(void)
{
    if (sag_oob_ready)
        bytebuf_free(&sag_oob);
    sag_oob_ready = false;
}

static size_t render_finish(Render *r, Bytebuf *out, size_t start,
                            size_t frame_end)
{
    if (frame_end < start || frame_end > out->len)
        SAG_BUG("renderer produced invalid frame bounds");
    r->bytes += (u64)(frame_end - start);
    (void)sag_term_oob_flush(out);
    return out->len - start;
}

static u16 terminal_attrs(u16 attrs, bool undercurl)
{
    u16 result = (u16)(attrs & ATTR_TERMINAL);

    if ((attrs & SAG_ATTR_INVALID_BYTE) != 0u)
        result |= ATTR_REVERSE;
    if ((result & ATTR_UNDERCURL) != 0u) {
        if (undercurl)
            result &= (u16)~ATTR_UNDERLINE;
        else {
            result &= (u16)~ATTR_UNDERCURL;
            result |= ATTR_UNDERLINE;
        }
    }
    return result;
}

static const u8 cube_level[6] = { 0, 95, 135, 175, 215, 255 };

static const u8 ansi_rgb[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0x80, 0x00, 0x00 },
    { 0x00, 0x80, 0x00 }, { 0x80, 0x80, 0x00 },
    { 0x00, 0x00, 0x80 }, { 0x80, 0x00, 0x80 },
    { 0x00, 0x80, 0x80 }, { 0xc0, 0xc0, 0xc0 },
    { 0x80, 0x80, 0x80 }, { 0xff, 0x00, 0x00 },
    { 0x00, 0xff, 0x00 }, { 0xff, 0xff, 0x00 },
    { 0x00, 0x00, 0xff }, { 0xff, 0x00, 0xff },
    { 0x00, 0xff, 0xff }, { 0xff, 0xff, 0xff }
};

static void bytes(Bytebuf *out, const char *s)
{
    bytebuf_append(out, s, strlen(s));
}

static void decimal(Bytebuf *out, u32 value)
{
    u8 text[10];
    size_t n = 0;

    do {
        text[n++] = (u8)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (n != 0u)
        bytebuf_push_u8(out, text[--n]);
}

static u8 cube_index(u8 value)
{
    if (value < 48u)
        return 0;
    if (value < 115u)
        return 1;
    return (u8)((value - 35u) / 40u);
}

static u32 color_distance(u8 r1, u8 g1, u8 b1, u8 r2, u8 g2, u8 b2)
{
    int dr = (int)r1 - (int)r2;
    int dg = (int)g1 - (int)g2;
    int db = (int)b1 - (int)b2;

    return (u32)(2 * dr * dr + 4 * dg * dg + 3 * db * db);
}

u8 sag_rgb_to_256(u8 r, u8 g, u8 b)
{
    u8 ri = cube_index(r);
    u8 gi = cube_index(g);
    u8 bi = cube_index(b);
    u8 cube = (u8)(16u + 36u * ri + 6u * gi + bi);
    u32 cube_dist = color_distance(r, g, b, cube_level[ri],
                                   cube_level[gi], cube_level[bi]);
    int average = ((int)r + (int)g + (int)b) / 3;
    int gray_index = (average - 8 + 5) / 10;
    u8 gray;

    if (gray_index < 0)
        gray_index = 0;
    if (gray_index > 23)
        gray_index = 23;
    gray = (u8)(8 + 10 * gray_index);
    if (color_distance(r, g, b, gray, gray, gray) < cube_dist)
        return (u8)(232 + gray_index);
    return cube;
}

u8 sag_rgb_to_16(u8 r, u8 g, u8 b)
{
    u8 best = 0;
    u32 best_dist = color_distance(r, g, b, ansi_rgb[0][0],
                                   ansi_rgb[0][1], ansi_rgb[0][2]);
    u8 i;

    for (i = 1; i < 16u; i++) {
        u32 candidate = color_distance(r, g, b, ansi_rgb[i][0],
                                       ansi_rgb[i][1], ansi_rgb[i][2]);

        if (candidate < best_dist) {
            best = i;
            best_dist = candidate;
        }
    }
    return best;
}

static void indexed_rgb(u8 index, u8 *r, u8 *g, u8 *b)
{
    if (index < 16u) {
        *r = ansi_rgb[index][0];
        *g = ansi_rgb[index][1];
        *b = ansi_rgb[index][2];
    } else if (index < 232u) {
        unsigned value = (unsigned)index - 16u;

        *r = cube_level[value / 36u];
        *g = cube_level[(value / 6u) % 6u];
        *b = cube_level[value % 6u];
    } else {
        *r = (u8)(8u + 10u * ((unsigned)index - 232u));
        *g = *r;
        *b = *r;
    }
}

u8 sag_render_tier(const TtyCaps *caps,
                   const char *(*getv)(const char *))
{
    const char *forced = getv != NULL ? getv("SAG_COLORS") : NULL;
    const char *term;

    if (forced != NULL) {
        if (strcmp(forced, "truecolor") == 0)
            return SAG_RENDER_TIER_TRUECOLOR;
        if (strcmp(forced, "256") == 0)
            return SAG_RENDER_TIER_256;
        if (strcmp(forced, "16") == 0)
            return SAG_RENDER_TIER_16;
    }
    if (caps != NULL && caps->truecolor)
        return SAG_RENDER_TIER_TRUECOLOR;
    term = getv != NULL ? getv("TERM") : NULL;
    if (term != NULL &&
        (strstr(term, "256color") != NULL || strstr(term, "direct") != NULL))
        return SAG_RENDER_TIER_256;
    return SAG_RENDER_TIER_16;
}

void sag_render_init(Render *r, const TtyCaps *caps,
                     const char *(*getv)(const char *))
{
    memset(r, 0, sizeof(*r));
    r->tier = sag_render_tier(caps, getv);
    r->sync = caps != NULL && caps->sync_output;
    r->undercurl = caps != NULL && caps->truecolor;
}

static bool color_equal(SagColor a, SagColor b)
{
    if (a.tag != b.tag)
        return false;
    if (a.tag == 0u)
        return true;
    if (a.tag == 1u)
        return a.r == b.r;
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void param_sep(Bytebuf *out, bool *first)
{
    if (!*first)
        bytebuf_push_u8(out, (u8)';');
    *first = false;
}

static void param_num(Bytebuf *out, bool *first, u32 value)
{
    param_sep(out, first);
    decimal(out, value);
}

static void param_text(Bytebuf *out, bool *first, const char *value)
{
    param_sep(out, first);
    bytes(out, value);
}

static u8 color_ansi(const SagColor *color)
{
    u8 r;
    u8 g;
    u8 b;

    if (color->tag == 1u) {
        if (color->r < 16u)
            return color->r;
        indexed_rgb(color->r, &r, &g, &b);
        return sag_rgb_to_16(r, g, b);
    }
    return sag_rgb_to_16(color->r, color->g, color->b);
}

static void color_param(Bytebuf *out, bool *first, const SagColor *color,
                        bool foreground, u8 tier)
{
    if (color->tag == 0u) {
        param_num(out, first, foreground ? 39u : 49u);
        return;
    }
    if (color->tag == 1u && color->r < 16u) {
        u8 index = color->r;
        u32 code;

        if (index < 8u)
            code = (foreground ? 30u : 40u) + index;
        else
            code = (foreground ? 90u : 100u) + (u32)(index - 8u);
        param_num(out, first, code);
        return;
    }
    if (tier == SAG_RENDER_TIER_TRUECOLOR && color->tag == 2u) {
        param_num(out, first, foreground ? 38u : 48u);
        param_num(out, first, 2u);
        param_num(out, first, color->r);
        param_num(out, first, color->g);
        param_num(out, first, color->b);
        return;
    }
    if ((tier == SAG_RENDER_TIER_TRUECOLOR && color->tag == 1u) ||
        tier == SAG_RENDER_TIER_256) {
        u8 index = color->tag == 1u
                       ? color->r
                       : sag_rgb_to_256(color->r, color->g, color->b);

        param_num(out, first, foreground ? 38u : 48u);
        param_num(out, first, 5u);
        param_num(out, first, index);
        return;
    }
    {
        u8 index = color_ansi(color);
        u32 code;

        if (index < 8u)
            code = (foreground ? 30u : 40u) + index;
        else
            code = (foreground ? 90u : 100u) + (u32)(index - 8u);
        param_num(out, first, code);
    }
}

static void attrs_full(Bytebuf *out, bool *first, u16 attrs,
                       bool undercurl)
{
    if ((attrs & ATTR_BOLD) != 0u)
        param_num(out, first, 1u);
    if ((attrs & ATTR_DIM) != 0u)
        param_num(out, first, 2u);
    if ((attrs & ATTR_ITALIC) != 0u)
        param_num(out, first, 3u);
    if ((attrs & ATTR_UNDERLINE) != 0u)
        param_num(out, first, 4u);
    if ((attrs & ATTR_UNDERCURL) != 0u) {
        if (undercurl)
            param_text(out, first, "4:3");
        else if ((attrs & ATTR_UNDERLINE) == 0u)
            param_num(out, first, 4u);
    }
    if ((attrs & ATTR_BLINK) != 0u)
        param_num(out, first, 5u);
    if ((attrs & ATTR_REVERSE) != 0u)
        param_num(out, first, 7u);
    if ((attrs & ATTR_CONCEAL) != 0u)
        param_num(out, first, 8u);
    if ((attrs & ATTR_STRIKE) != 0u)
        param_num(out, first, 9u);
    if ((attrs & ATTR_OVERLINE) != 0u)
        param_num(out, first, 53u);
}

static unsigned attr_reset_count(u16 from, u16 to)
{
    unsigned count = 0;

    if ((from & (ATTR_BOLD | ATTR_DIM) & ~to) != 0u)
        count++;
    if ((from & ATTR_ITALIC & ~to) != 0u)
        count++;
    if ((from & (ATTR_UNDERLINE | ATTR_UNDERCURL) & ~to) != 0u)
        count++;
    if ((from & ATTR_BLINK & ~to) != 0u)
        count++;
    if ((from & ATTR_REVERSE & ~to) != 0u)
        count++;
    if ((from & ATTR_CONCEAL & ~to) != 0u)
        count++;
    if ((from & ATTR_STRIKE & ~to) != 0u)
        count++;
    if ((from & ATTR_OVERLINE & ~to) != 0u)
        count++;
    return count;
}

static void attrs_delta(Bytebuf *out, bool *first, u16 from, u16 to,
                        bool undercurl)
{
    u16 shared = ATTR_BOLD | ATTR_DIM;
    u16 line = ATTR_UNDERLINE | ATTR_UNDERCURL;
    static const u16 bits[] = {
        ATTR_ITALIC, ATTR_BLINK, ATTR_REVERSE, ATTR_CONCEAL,
        ATTR_STRIKE, ATTR_OVERLINE
    };
    static const u8 sets[] = { 3, 5, 7, 8, 9, 53 };
    static const u8 resets[] = { 23, 25, 27, 28, 29, 55 };
    size_t i;

    if ((from & shared & ~to) != 0u) {
        param_num(out, first, 22u);
        if ((to & ATTR_BOLD) != 0u)
            param_num(out, first, 1u);
        if ((to & ATTR_DIM) != 0u)
            param_num(out, first, 2u);
    } else {
        if ((to & ATTR_BOLD & ~from) != 0u)
            param_num(out, first, 1u);
        if ((to & ATTR_DIM & ~from) != 0u)
            param_num(out, first, 2u);
    }

    if ((from & line & ~to) != 0u) {
        param_num(out, first, 24u);
        if ((to & ATTR_UNDERLINE) != 0u)
            param_num(out, first, 4u);
        if ((to & ATTR_UNDERCURL) != 0u) {
            if (undercurl)
                param_text(out, first, "4:3");
            else if ((to & ATTR_UNDERLINE) == 0u)
                param_num(out, first, 4u);
        }
    } else {
        if ((to & ATTR_UNDERLINE & ~from) != 0u)
            param_num(out, first, 4u);
        if ((to & ATTR_UNDERCURL & ~from) != 0u) {
            if (undercurl)
                param_text(out, first, "4:3");
            else if ((to & ATTR_UNDERLINE) == 0u)
                param_num(out, first, 4u);
        }
    }

    for (i = 0; i < SAG_ARRAY_LEN(bits); i++) {
        if ((from & bits[i]) != 0u && (to & bits[i]) == 0u)
            param_num(out, first, resets[i]);
        else if ((from & bits[i]) == 0u && (to & bits[i]) != 0u)
            param_num(out, first, sets[i]);
    }
}

static void set_style(Render *r, Bytebuf *out, const Cell *cell)
{
    u16 attrs = terminal_attrs(cell->attrs, r->undercurl);
    u16 old_attrs = terminal_attrs(r->attrs, r->undercurl);
    bool fg_changed;
    bool bg_changed;
    bool full;
    bool first = true;
    unsigned resets;

    if (r->pen_known && old_attrs == attrs &&
        color_equal(r->fg, cell->fg) && color_equal(r->bg, cell->bg)) {
        r->attrs = cell->attrs;
        return;
    }

    fg_changed = !r->pen_known || !color_equal(r->fg, cell->fg);
    bg_changed = !r->pen_known || !color_equal(r->bg, cell->bg);
    resets = r->pen_known ? attr_reset_count(old_attrs, attrs) : 3u;
    if (r->pen_known && fg_changed && cell->fg.tag == 0u)
        resets++;
    if (r->pen_known && bg_changed && cell->bg.tag == 0u)
        resets++;
    full = !r->pen_known || resets >= 3u;

    bytes(out, "\033[");
    if (full) {
        param_num(out, &first, 0u);
        attrs_full(out, &first, attrs, r->undercurl);
        if (cell->fg.tag != 0u)
            color_param(out, &first, &cell->fg, true, r->tier);
        if (cell->bg.tag != 0u)
            color_param(out, &first, &cell->bg, false, r->tier);
    } else {
        attrs_delta(out, &first, old_attrs, attrs, r->undercurl);
        if (fg_changed)
            color_param(out, &first, &cell->fg, true, r->tier);
        if (bg_changed)
            color_param(out, &first, &cell->bg, false, r->tier);
    }
    bytebuf_push_u8(out, (u8)'m');
    r->fg = cell->fg;
    r->bg = cell->bg;
    r->attrs = cell->attrs;
    r->pen_known = true;
}

static void cell_bytes(const Grid *g, const Cell *cell, u16 row, u16 col,
                       const u8 **data, size_t *len)
{
    size_t n;

    if ((cell->flags & CELL_INTERNED) != 0u) {
        const char *text = sag_intern_str(g->gi, cell->id);

        if (text == NULL)
            SAG_BUG("render cell %u,%u has invalid grapheme intern id %u",
                    row, col, cell->id);
        *data = (const u8 *)text;
        *len = strlen(text);
        return;
    }
    if (cell->utf8[0] == 0u) {
        *data = (const u8 *)" ";
        *len = 1u;
        return;
    }
    for (n = 0; n < sizeof(cell->utf8) && cell->utf8[n] != 0u; n++)
        ;
    *data = cell->utf8;
    *len = n;
}

static void emit_cell(Render *r, const Grid *g, Bytebuf *out,
                      const Cell *cell)
{
    const u8 *data;
    size_t len;
    size_t i;

    set_style(r, out, cell);
    cell_bytes(g, cell, r->row, r->col, &data, &len);
    if (sag_gb_next_bytes(data, len, 0u) != len)
        SAG_BUG("render cell %u,%u contains multiple grapheme clusters",
                r->row, r->col);
    for (i = 0; i < len; i++) {
        if (data[i] < 0x20u || data[i] == 0x7fu)
            SAG_BUG("render cell %u,%u contains raw control byte 0x%02x",
                    r->row, r->col, data[i]);
    }
    if (sag_cluster_width(data, len) != (int)cell->w)
        SAG_BUG("render cell %u,%u has inconsistent grapheme width",
                r->row, r->col);
    bytebuf_append(out, data, len);
    if (r->pos_known) {
        u32 next = (u32)r->col + (cell->w == 2u ? 2u : 1u);

        if (next > UINT16_MAX)
            r->pos_known = false;
        else
            r->col = (u16)next;
    }
}

static void cup(Render *r, Bytebuf *out, u16 row, u16 col)
{
    if (row == 0u && col == 0u) {
        bytes(out, "\033[H");
    } else {
        bytes(out, "\033[");
        decimal(out, (u32)row + 1u);
        bytebuf_push_u8(out, (u8)';');
        decimal(out, (u32)col + 1u);
        bytebuf_push_u8(out, (u8)'H');
    }
    r->row = row;
    r->col = col;
    r->pos_known = true;
}

static bool can_reemit_gap(const Render *r, const Grid *g, u16 row,
                           u16 target)
{
    size_t motion_cost = 4u;
    size_t reemit_cost = 0u;
    u16 col;

    if (!r->pos_known || r->row != row || target <= r->col ||
        (u16)(target - r->col) > 3u || !r->pen_known)
        return false;
    for (col = r->col; col < target; col++) {
        const Cell *cell = &g->back[(size_t)row * g->cols + col];
        const u8 *data;
        size_t len;
        u16 cell_attrs = terminal_attrs(cell->attrs, r->undercurl);
        u16 pen_attrs = terminal_attrs(r->attrs, r->undercurl);

        if (cell->w != 1u || cell_attrs != pen_attrs ||
            !color_equal(cell->fg, r->fg) || !color_equal(cell->bg, r->bg))
            return false;
        cell_bytes(g, cell, row, col, &data, &len);
        (void)data;
        if (len >= motion_cost - reemit_cost)
            return false;
        reemit_cost += len;
    }
    return reemit_cost < motion_cost;
}

static void move_to(Render *r, const Grid *g, Bytebuf *out, u16 row, u16 col)
{
    if (r->pos_known && r->row == row && r->col == col)
        return;
    if (can_reemit_gap(r, g, row, col)) {
        while (r->col < col) {
            const Cell *cell = &g->back[(size_t)row * g->cols + r->col];

            emit_cell(r, g, out, cell);
        }
        return;
    }
    if (r->pos_known && r->row == row && col > r->col) {
        u16 gap = (u16)(col - r->col);

        bytes(out, "\033[");
        decimal(out, gap);
        bytebuf_push_u8(out, (u8)'C');
        r->col = col;
        return;
    }
    cup(r, out, row, col);
}

static bool blank_tail(const Grid *g, u16 row, u16 col)
{
    u16 i;

    if ((u16)(g->cols - col) < 6u)
        return false;
    for (i = col; i < g->cols; i++) {
        const Cell *cell = &g->back[(size_t)row * g->cols + i];

        if ((cell->flags & CELL_INTERNED) != 0u || cell->utf8[0] != 0u ||
            cell->w != 1u || cell->bg.tag != 0u ||
            cell->attrs != 0u)
            return false;
    }
    return true;
}

static bool has_changes(const Grid *g)
{
    u16 row;
    u16 lo = g->dmg_lo < g->rows ? g->dmg_lo : g->rows;
    u16 hi = g->dmg_hi < g->rows ? g->dmg_hi : g->rows;

    for (row = lo; row < hi; row++) {
        u16 col;
        u16 c0 = g->dmg[row].lo < g->cols ? g->dmg[row].lo : g->cols;
        u16 c1 = g->dmg[row].hi < g->cols ? g->dmg[row].hi : g->cols;

        for (col = c0; col < c1; col++) {
            size_t at = (size_t)row * g->cols + col;

            if (!sag_cell_eq(&g->front[at], &g->back[at]))
                return true;
        }
    }
    return false;
}

static void cursor_shape(Render *r, const Grid *g, Bytebuf *out)
{
    if (g->cur_shape == SAG_CURSOR_BLOCK)
        bytes(out, "\033[2 q");
    else if (g->cur_shape == SAG_CURSOR_BAR)
        bytes(out, "\033[6 q");
    else
        SAG_BUG("renderer received invalid cursor shape");
    r->cursor_shape = g->cur_shape;
    r->cursor_generation = g->cursor_generation;
}

size_t sag_render_frame(Render *r, Grid *g, Bytebuf *out)
{
    size_t start = out->len;
    bool cells_changed;
    bool cursor_changed;
    bool shape_changed;
    u16 row;
    u16 lo;
    u16 hi;

    r->frames++;
    if (g->rows == 0u || g->cols == 0u)
        return render_finish(r, out, start, out->len);
    cells_changed = has_changes(g);
    cursor_changed = r->cursor_known &&
                     (r->row != g->cur_row || r->col != g->cur_col ||
                      r->cursor_visible != g->cur_vis);
    shape_changed = r->cursor_generation != g->cursor_generation ||
                    r->cursor_shape != g->cur_shape;
    if (!cells_changed && !cursor_changed &&
        (!shape_changed || !r->cursor_known))
        return render_finish(r, out, start, out->len);

    if (!cells_changed) {
        if (r->sync)
            bytes(out, "\033[?2026h");
        if (shape_changed)
            cursor_shape(r, g, out);
        cup(r, out, g->cur_row, g->cur_col);
        if (r->cursor_visible != g->cur_vis)
            bytes(out, g->cur_vis ? "\033[?25h" : "\033[?25l");
        if (r->sync)
            bytes(out, "\033[?2026l");
        r->cursor_known = true;
        r->cursor_visible = g->cur_vis;
        return render_finish(r, out, start, out->len);
    }

    /* Cell-bearing frames are byte-identical for the same grid state and do
     * not depend on whether a prior frame reached the terminal. Cursor-only
     * frames may still use the final CUP state recorded below. */
    r->pos_known = false;
    r->pen_known = false;
    if (r->sync)
        bytes(out, "\033[?2026h");
    bytes(out, "\033[?25l");
    if (shape_changed)
        cursor_shape(r, g, out);

    lo = g->dmg_lo < g->rows ? g->dmg_lo : g->rows;
    hi = g->dmg_hi < g->rows ? g->dmg_hi : g->rows;
    for (row = lo; row < hi; row++) {
        u16 col = g->dmg[row].lo < g->cols ? g->dmg[row].lo : g->cols;
        u16 end = g->dmg[row].hi < g->cols ? g->dmg[row].hi : g->cols;

        while (col < end) {
            size_t at = (size_t)row * g->cols + col;
            const Cell *cell = &g->back[at];

            if (sag_cell_eq(&g->front[at], cell) || cell->w == 0u) {
                col++;
                continue;
            }
            move_to(r, g, out, row, col);
            if (blank_tail(g, row, col)) {
                set_style(r, out, cell);
                bytes(out, "\033[K");
                break;
            }
            emit_cell(r, g, out, cell);
            col = (u16)(col + (cell->w == 2u ? 2u : 1u));
        }
    }

    /* The frame envelope always ends with an absolute position. Besides
     * making frames deterministic in isolation, CUP clears any terminal's
     * pending-wrap state after output in the last column. */
    cup(r, out, g->cur_row, g->cur_col);
    if (g->cur_vis)
        bytes(out, "\033[?25h");
    if (r->sync)
        bytes(out, "\033[?2026l");
    r->cursor_known = true;
    r->cursor_visible = g->cur_vis;
    return render_finish(r, out, start, out->len);
}
