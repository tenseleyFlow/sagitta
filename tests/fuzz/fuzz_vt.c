#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "../../tests/pty/vt.h"
#include "term/grid.h"
#include "term/render.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    VT_FUZZ_ROWS = 6,
    VT_FUZZ_COLS = 24,
    VT_FUZZ_MAX_OPS = 128
};

static bool fuzz_fail(char *why, size_t cap, const char *message, size_t at)
{
    (void)snprintf(why, cap, "%s at byte %zu", message, at);
    return false;
}

static const char *tier_256(const char *name)
{
    return strcmp(name, "YEW_COLORS") == 0 ? "256" : NULL;
}

static const u8 *grid_cell_bytes(const Grid *grid, const Cell *cell,
                                 size_t *len)
{
    size_t n;

    if ((cell->flags & CELL_INTERNED) != 0U) {
        const char *text = yew_intern_str(grid->gi, cell->id);

        if (text == NULL) {
            *len = 0U;
            return NULL;
        }
        *len = strlen(text);
        return (const u8 *)text;
    }
    for (n = 0U; n < sizeof(cell->utf8) && cell->utf8[n] != 0U; n++)
        ;
    if (n == 1U && cell->utf8[0] == ' ')
        n = 0U;
    *len = n;
    return cell->utf8;
}

static u16 expected_attrs(u16 attrs, bool undercurl)
{
    if ((attrs & YEW_ATTR_INVALID_BYTE) != 0U)
        attrs |= YEW_ATTR_REVERSE;
    attrs &= (u16)((1u << YEW_CELL_UL_SHIFT) - 1u);
    if ((attrs & YEW_ATTR_UNDERCURL) != 0U) {
        if (undercurl)
            attrs &= (u16)~YEW_ATTR_UNDERLINE;
        else {
            attrs &= (u16)~YEW_ATTR_UNDERCURL;
            attrs |= YEW_ATTR_UNDERLINE;
        }
    }
    return attrs;
}

static bool screens_equal(const Grid *grid, const Render *render,
                          const VtScreen *vt,
                          char *why, size_t why_cap, size_t at)
{
    size_t count = (size_t)grid->rows * grid->cols;
    size_t i;

    if (vt->rows != grid->rows || vt->cols != grid->cols)
        return fuzz_fail(why, why_cap, "VT size differs from grid", at);
    for (i = 0U; i < count; i++) {
        const Cell *want = &grid->back[i];
        const VtCell *got = &vt->cells[i];
        const u8 *want_bytes;
        const u8 *got_bytes;
        size_t want_len;
        size_t got_len;

        want_bytes = grid_cell_bytes(grid, want, &want_len);
        got_bytes = vt_cell_bytes(vt, got, &got_len);
        if ((want_len != 0U && want_bytes == NULL) ||
            want_len != got_len ||
            (want_len != 0U && memcmp(want_bytes, got_bytes, want_len) != 0) ||
            want->w != got->w ||
            expected_attrs(want->attrs, render->undercurl) != got->attrs ||
            memcmp(&want->fg, &got->fg, sizeof(want->fg)) != 0 ||
            memcmp(&want->bg, &got->bg, sizeof(want->bg)) != 0) {
            (void)snprintf(why, why_cap,
                           "VT cell %zu differs at byte %zu "
                           "(len %zu/%zu w %u/%u attrs %u/%u)",
                           i, at, want_len, got_len, (unsigned)want->w,
                           (unsigned)got->w,
                           (unsigned)expected_attrs(want->attrs,
                                                    render->undercurl),
                           (unsigned)got->attrs);
            return false;
        }
    }
    if (vt->cur_r != grid->cur_row || vt->cur_c != grid->cur_col ||
        vt->cur_vis != grid->cur_vis)
        return fuzz_fail(why, why_cap, "VT cursor differs from grid", at);
    return true;
}

static YewColor fuzz_indexed(u8 byte)
{
    YewColor color = {YEW_COLOR_INDEXED, byte, 0U, 0U};

    if ((byte & 3U) == 0U)
        memset(&color, 0, sizeof(color));
    return color;
}

static void mutate_grid(Grid *grid, const u8 *data, size_t len, size_t *pos)
{
    static const u8 ascii[] = "x";
    static const u8 cjk[] = "\xe6\xbc\xa2";
    static const u8 emoji[] = "\xf0\x9f\x98\x80";
    static const u8 family[] =
        "\xf0\x9f\x91\xa8\xe2\x80\x8d"
        "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";
    u8 op = data[(*pos)++];
    u8 a = *pos < len ? data[(*pos)++] : op;
    u8 b = *pos < len ? data[(*pos)++] : (u8)(op * 17U);
    u8 c = *pos < len ? data[(*pos)++] : (u8)(op * 31U);
    u16 row = (u16)(a % grid->rows);
    u16 col = (u16)(b % grid->cols);
    YewColor fg = fuzz_indexed(b);
    YewColor bg = fuzz_indexed(c);
    u16 attrs = (u16)(((u16)a << 8U | b) & YEW_CELL_ATTR_MASK);
    const u8 *text = ascii;
    size_t text_len = sizeof(ascii) - 1U;

    switch (op % 7U) {
    case 0U:
        text = cjk;
        text_len = sizeof(cjk) - 1U;
        break;
    case 1U:
        text = emoji;
        text_len = sizeof(emoji) - 1U;
        break;
    case 2U:
        text = family;
        text_len = sizeof(family) - 1U;
        break;
    case 3U: {
        Cell fill = grid->blank;

        fill.fg = fg;
        fill.bg = bg;
        fill.attrs = attrs;
        fill.utf8[0] = (u8)('a' + c % 26U);
        yew_grid_fill(grid, row, col,
                      (u16)(col + 1U + c % (grid->cols - col)), fill);
        return;
    }
    case 4U:
        yew_grid_cursor(grid, row, col, (c & 1U) != 0U);
        return;
    case 5U:
        yew_grid_clear(grid);
        return;
    default:
        break;
    }
    (void)yew_grid_puts(grid, row, col, text, text_len, fg, bg, attrs);
}

static bool check_random_parser(const u8 *data, size_t len,
                                char *why, size_t why_cap)
{
    VtScreen vt;
    size_t pos = 0U;

    vt_init(&vt, VT_FUZZ_ROWS, VT_FUZZ_COLS);
    while (pos < len) {
        size_t chunk = 1U + data[pos] % 31U;
        u32 errors = vt.nerrors;

        if (chunk > len - pos)
            chunk = len - pos;
        vt_feed(&vt, data + pos, chunk);
        if (vt.nerrors < errors) {
            vt_free(&vt);
            return fuzz_fail(why, why_cap, "VT error count decreased", pos);
        }
        pos += chunk;
    }
    if (vt.errors.len > 65536U ||
        vt.glyphs.len > len * 2U + (size_t)VT_FUZZ_ROWS * VT_FUZZ_COLS * 8U) {
        vt_free(&vt);
        return fuzz_fail(why, why_cap, "VT parser storage is unbounded", pos);
    }
    vt_free(&vt);
    return true;
}

static bool check_render_roundtrip(const u8 *data, size_t len,
                                   char *why, size_t why_cap)
{
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    TtyCaps caps;
    Bytebuf output;
    VtScreen vt;
    size_t pos = 0U;
    size_t ops = 0U;
    bool ok = true;

    arena_init(&arena);
    interner_init(&interner, &arena);
    if (!yew_grid_init(&grid, &interner, VT_FUZZ_ROWS, VT_FUZZ_COLS)) {
        interner_free(&interner);
        arena_free_all(&arena);
        return fuzz_fail(why, why_cap, "grid init failed", 0U);
    }
    memset(&caps, 0, sizeof(caps));
    caps.truecolor = true;
    caps.sync_output = true;
    yew_render_init(&render, &caps, tier_256);
    bytebuf_init(&output);
    vt_init(&vt, VT_FUZZ_ROWS, VT_FUZZ_COLS);
    vt_feed(&vt, (const u8 *)"\033[?1049h", 8U);

    while (pos < len && ops < VT_FUZZ_MAX_OPS) {
        size_t emitted;

        mutate_grid(&grid, data, len, &pos);
        output.len = 0U;
        emitted = yew_render_frame(&render, &grid, &output);
        if (emitted != output.len) {
            ok = fuzz_fail(why, why_cap, "renderer byte count differs", pos);
            break;
        }
        vt_feed(&vt, output.data, output.len);
        if (vt.nerrors != 0U ||
            !screens_equal(&grid, &render, &vt, why, why_cap, pos)) {
            if (vt.nerrors != 0U)
                ok = fuzz_fail(why, why_cap, "renderer left closed VT set", pos);
            else
                ok = false;
            break;
        }
        yew_grid_flip(&grid);
        ops++;
    }

    vt_free(&vt);
    bytebuf_free(&output);
    yew_grid_free(&grid);
    interner_free(&interner);
    arena_free_all(&arena);
    return ok;
}

static bool check_vt(const u8 *data, size_t len,
                     char *why, size_t why_cap)
{
    return check_random_parser(data, len, why, why_cap) &&
           check_render_roundtrip(data, len, why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_vt", NULL, check_vt);
}
