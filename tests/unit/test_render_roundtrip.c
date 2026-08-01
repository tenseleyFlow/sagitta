#include "harness.h"

#include "term/grid.h"
#include "term/render.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

#include <stdlib.h>
#include <string.h>

typedef struct VtRef {
    u16 rows, cols, row, col;
    Cell *cells;
    Cell blank;
    SagColor fg, bg;
    u16 attrs;
    size_t bsu, esu;
} VtRef;

static const char *roundtrip_env(const char *name)
{
    return strcmp(name, "SAG_COLORS") == 0 ? "256" : NULL;
}

static void vt_init(VtRef *vt, u16 rows, u16 cols)
{
    memset(vt, 0, sizeof(*vt));
    vt->rows = rows;
    vt->cols = cols;
    vt->blank.w = 1u;
    vt->cells = calloc((size_t)rows * cols, sizeof(*vt->cells));
    SAG_ASSERT_NOT_NULL(vt->cells);
    for (size_t i = 0u; i < (size_t)rows * cols; i++)
        vt->cells[i] = vt->blank;
}

static void vt_sgr(VtRef *vt, const unsigned *p, size_t n)
{
    size_t i = 0u;

    if (n == 0u) {
        vt->fg = vt->blank.fg;
        vt->bg = vt->blank.bg;
        vt->attrs = 0u;
        return;
    }
    while (i < n) {
        unsigned v = p[i++];

        if (v == 0u) {
            vt->fg = vt->blank.fg;
            vt->bg = vt->blank.bg;
            vt->attrs = 0u;
        } else if (v == 1u) vt->attrs |= SAG_ATTR_BOLD;
        else if (v == 2u) vt->attrs |= SAG_ATTR_DIM;
        else if (v == 3u) vt->attrs |= SAG_ATTR_ITALIC;
        else if (v == 4u) {
            vt->attrs &= (u16)~SAG_ATTR_UNDERCURL;
            vt->attrs |= SAG_ATTR_UNDERLINE;
        } else if (v == 43u) {
            vt->attrs &= (u16)~SAG_ATTR_UNDERLINE;
            vt->attrs |= SAG_ATTR_UNDERCURL;
        }
        else if (v == 5u) vt->attrs |= SAG_ATTR_BLINK;
        else if (v == 7u) vt->attrs |= SAG_ATTR_REVERSE;
        else if (v == 8u) vt->attrs |= SAG_ATTR_CONCEAL;
        else if (v == 9u) vt->attrs |= SAG_ATTR_STRIKE;
        else if (v == 22u) vt->attrs &= (u16)~(SAG_ATTR_BOLD | SAG_ATTR_DIM);
        else if (v == 23u) vt->attrs &= (u16)~SAG_ATTR_ITALIC;
        else if (v == 24u) vt->attrs &= (u16)~(SAG_ATTR_UNDERLINE |
                                               SAG_ATTR_UNDERCURL);
        else if (v == 25u) vt->attrs &= (u16)~SAG_ATTR_BLINK;
        else if (v == 27u) vt->attrs &= (u16)~SAG_ATTR_REVERSE;
        else if (v == 28u) vt->attrs &= (u16)~SAG_ATTR_CONCEAL;
        else if (v == 29u) vt->attrs &= (u16)~SAG_ATTR_STRIKE;
        else if (v == 53u) vt->attrs |= SAG_ATTR_OVERLINE;
        else if (v == 55u) vt->attrs &= (u16)~SAG_ATTR_OVERLINE;
        else if (v >= 30u && v <= 37u) {
            vt->fg = (SagColor){SAG_COLOR_INDEXED, (u8)(v - 30u), 0u, 0u};
        } else if (v >= 90u && v <= 97u) {
            vt->fg = (SagColor){SAG_COLOR_INDEXED, (u8)(v - 82u), 0u, 0u};
        } else if (v >= 40u && v <= 47u) {
            vt->bg = (SagColor){SAG_COLOR_INDEXED, (u8)(v - 40u), 0u, 0u};
        } else if (v >= 100u && v <= 107u) {
            vt->bg = (SagColor){SAG_COLOR_INDEXED, (u8)(v - 92u), 0u, 0u};
        } else if (v == 39u) vt->fg = vt->blank.fg;
        else if (v == 49u) vt->bg = vt->blank.bg;
        else if ((v == 38u || v == 48u) && i < n) {
            SagColor *color = v == 38u ? &vt->fg : &vt->bg;

            if (p[i] == 5u && i + 1u < n) {
                *color = (SagColor){SAG_COLOR_INDEXED, (u8)p[i + 1u], 0u, 0u};
                i += 2u;
            } else if (p[i] == 2u && i + 3u < n) {
                *color = (SagColor){SAG_COLOR_RGB, (u8)p[i + 1u],
                                    (u8)p[i + 2u], (u8)p[i + 3u]};
                i += 4u;
            } else {
                SAG_ASSERT(false);
            }
        } else {
            SAG_ASSERT(false);
        }
    }
}

static size_t vt_csi(VtRef *vt, const u8 *s, size_t n, size_t pos)
{
    unsigned p[16] = {0u};
    size_t np = 0u;
    bool private_mode = false;
    u8 final;

    if (pos < n && s[pos] == '?') {
        private_mode = true;
        pos++;
    }
    while (pos < n && (s[pos] < 0x40u || s[pos] > 0x7eu)) {
        if (s[pos] >= '0' && s[pos] <= '9') {
            if (np == 0u)
                np = 1u;
            p[np - 1u] = p[np - 1u] * 10u + (unsigned)(s[pos] - '0');
        } else if (s[pos] == ';') {
            SAG_ASSERT(np < SAG_ARRAY_LEN(p));
            np++;
        } else if (s[pos] == ':') {
            SAG_ASSERT(np != 0u && p[np - 1u] == 4u);
            p[np - 1u] = 43u;
            pos++;
            SAG_ASSERT(pos < n && s[pos] == '3');
            pos++;
            continue;
        } else {
            SAG_ASSERT(false);
        }
        pos++;
    }
    SAG_ASSERT(pos < n);
    final = s[pos++];
    if (private_mode) {
        SAG_ASSERT(final == 'h' || final == 'l');
        SAG_ASSERT(np == 1u && (p[0] == 25u || p[0] == 2026u));
        if (p[0] == 2026u && final == 'h') vt->bsu++;
        if (p[0] == 2026u && final == 'l') vt->esu++;
    } else if (final == 'H') {
        vt->row = np >= 1u && p[0] != 0u ? (u16)(p[0] - 1u) : 0u;
        vt->col = np >= 2u && p[1] != 0u ? (u16)(p[1] - 1u) : 0u;
        SAG_ASSERT(vt->row < vt->rows && vt->col < vt->cols);
    } else if (final == 'C') {
        vt->col = (u16)(vt->col + (np != 0u && p[0] != 0u ? p[0] : 1u));
        SAG_ASSERT(vt->col <= vt->cols);
    } else if (final == 'K') {
        u16 col;

        SAG_ASSERT(np == 0u || (np == 1u && p[0] == 0u));
        for (col = vt->col; col < vt->cols; col++)
            vt->cells[(size_t)vt->row * vt->cols + col] = vt->blank;
    } else if (final == 'm') {
        vt_sgr(vt, p, np);
    } else {
        SAG_ASSERT(false);
    }
    return pos;
}

static void vt_text(VtRef *vt, const u8 *s, size_t n)
{
    Cell head;
    int width = sag_cluster_width(s, n);
    size_t off;

    SAG_ASSERT(width == 1 || width == 2);
    SAG_ASSERT(vt->row < vt->rows && vt->col < vt->cols);
    off = (size_t)vt->row * vt->cols + vt->col;
    memset(&head, 0, sizeof(head));
    head.fg = vt->fg;
    head.bg = vt->bg;
    head.attrs = vt->attrs;
    head.w = (u8)width;
    if (n == 1u && s[0] == ' ') {
        head.utf8[0] = 0u;
    } else {
        SAG_ASSERT(n <= sizeof(head.utf8));
        memcpy(head.utf8, s, n);
    }
    vt->cells[off] = head;
    if (width == 2) {
        Cell tail;

        SAG_ASSERT(vt->col + 1u < vt->cols);
        memset(&tail, 0, sizeof(tail));
        tail.fg = vt->fg;
        tail.bg = vt->bg;
        tail.attrs = vt->attrs;
        vt->cells[off + 1u] = tail;
    }
    vt->col = (u16)(vt->col + (u16)width);
}

static void vt_replay(VtRef *vt, const u8 *s, size_t n)
{
    size_t pos = 0u;

    while (pos < n) {
        if (s[pos] == 0x1bu) {
            SAG_ASSERT(pos + 1u < n && s[pos + 1u] == '[');
            pos = vt_csi(vt, s, n, pos + 2u);
        } else {
            size_t next = sag_gb_next_bytes(s, n, pos);

            SAG_ASSERT(next > pos && next <= n);
            vt_text(vt, s + pos, next - pos);
            pos = next;
        }
    }
}

static u32 roundtrip_rand(u32 *state)
{
    u32 x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void test_render_roundtrip_reference_vt(void)
{
    static const u8 cjk[] = {0xe6u, 0xbcu, 0xa2u};
    u32 state = 0x5a17c0deu;
    size_t iteration;

    for (iteration = 0u; iteration < 5000u; iteration++) {
        Arena arena;
        Interner interner;
        Grid grid;
        Render render;
        TtyCaps caps = {0};
        Bytebuf out;
        VtRef vt;
        size_t op;

        arena_init(&arena);
        interner_init(&interner, &arena);
        SAG_ASSERT(sag_grid_init(&grid, &interner, 4u, 12u));
        for (op = 0u; op < 16u; op++) {
            u32 value = roundtrip_rand(&state);
            u16 row = (u16)(value % grid.rows);
            u16 col = (u16)((value >> 4) % grid.cols);
            u16 attrs = (u16)((value >> 8) & 0x03ffu);
            SagColor fg = {SAG_COLOR_INDEXED, (u8)((value >> 18) & 0xffu),
                           0u, 0u};
            SagColor bg = {SAG_COLOR_DEFAULT, 0u, 0u, 0u};

            if ((value & 3u) == 0u)
                fg.tag = SAG_COLOR_DEFAULT;
            if ((attrs & SAG_ATTR_UNDERCURL) != 0u)
                attrs &= (u16)~SAG_ATTR_UNDERLINE;
            if ((value & 7u) == 1u && col + 1u < grid.cols)
                sag_grid_put(&grid, row, col, cjk, sizeof(cjk), fg, bg, attrs);
            else
                sag_grid_put(&grid, row, col,
                             (const u8 *)(value & 1u ? "x" : "Q"), 1u,
                             fg, bg, attrs);
        }
        sag_grid_cursor(&grid, (u16)(iteration % 4u),
                        (u16)(iteration % 12u), (iteration & 1u) != 0u);
        caps.truecolor = true;
        caps.sync_output = true;
        bytebuf_init(&out);
        sag_render_init(&render, &caps, roundtrip_env);
        SAG_ASSERT(sag_render_frame(&render, &grid, &out) != 0u);
        vt_init(&vt, grid.rows, grid.cols);
        vt_replay(&vt, out.data, out.len);
        SAG_ASSERT_EQ_MEM(vt.cells, grid.back,
                          (size_t)grid.rows * grid.cols * sizeof(Cell));
        SAG_ASSERT_EQ_U64(vt.bsu, 1u);
        SAG_ASSERT_EQ_U64(vt.esu, 1u);
        free(vt.cells);
        bytebuf_free(&out);
        sag_grid_free(&grid);
        interner_free(&interner);
        arena_free_all(&arena);
    }
}
