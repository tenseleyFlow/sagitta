/* Sprint 58 F01 Q6: 10,000-cluster model/renderer/VT differential. */
#include <stdio.h>
#include <string.h>

#include "pty/vt.h"
#include "term/grid.h"
#include "term/render.h"
#include "unicode/width.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum { CLUSTER_COUNT = 10000U };

typedef struct ClusterCase {
    const u8 *bytes;
    size_t len;
} ClusterCase;

static const u8 ascii[] = "x";
static const u8 cjk[] = "\xe6\xbc\xa2";
static const u8 emoji[] = "\xf0\x9f\x98\x80";
static const u8 combining[] = "e\xcc\x81\xcc\xa7";
static const u8 family[] =
    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
    "\xe2\x80\x8d\xf0\x9f\x91\xa7";
static const u8 flag[] = "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8";
static const u8 keycap[] = "1\xef\xb8\x8f\xe2\x83\xa3";
static const u8 devanagari[] = "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7";
static const u8 ambiguous[] = "\xc2\xb7";
static const u8 invalid[] = {0xffU};

static const ClusterCase cases[] = {
    {ascii, sizeof(ascii) - 1U},
    {cjk, sizeof(cjk) - 1U},
    {emoji, sizeof(emoji) - 1U},
    {combining, sizeof(combining) - 1U},
    {family, sizeof(family) - 1U},
    {flag, sizeof(flag) - 1U},
    {keycap, sizeof(keycap) - 1U},
    {devanagari, sizeof(devanagari) - 1U},
    {ambiguous, sizeof(ambiguous) - 1U},
    {invalid, sizeof(invalid)}
};

static const u8 *grid_cell_bytes(const Grid *grid, const Cell *cell,
                                 size_t *len)
{
    size_t n;

    if ((cell->flags & CELL_INTERNED) != 0U) {
        const char *text = yew_intern_str(grid->gi, cell->id);

        *len = yew_intern_len(grid->gi, cell->id);
        return (const u8 *)text;
    }
    for (n = 0U; n < sizeof(cell->utf8) && cell->utf8[n] != 0U; n++)
        ;
    if (n == 1U && cell->utf8[0] == (u8)' ')
        n = 0U;
    *len = n;
    return cell->utf8;
}

static u16 rendered_attrs(u16 attrs)
{
    if ((attrs & YEW_ATTR_INVALID_BYTE) != 0U)
        attrs |= YEW_ATTR_REVERSE;
    return (u16)(attrs & ((1U << YEW_CELL_UL_SHIFT) - 1U));
}

static bool screen_matches(const Grid *grid, const VtScreen *vt,
                           size_t model_cells)
{
    size_t count = (size_t)grid->rows * grid->cols;
    size_t i;

    if (vt->rows != grid->rows || vt->cols != grid->cols ||
        vt->cur_r != 0 || vt->cur_c != (int)model_cells) {
        (void)fprintf(stderr,
                      "f01-vt-width: geometry/cursor grid=%ux%u "
                      "vt=%dx%d cursor=%d,%d expected=0,%zu\n",
                      (unsigned)grid->rows, (unsigned)grid->cols,
                      vt->rows, vt->cols, vt->cur_r, vt->cur_c,
                      model_cells);
        return false;
    }
    for (i = 0U; i < count; i++) {
        const Cell *want = &grid->back[i];
        const VtCell *got = &vt->cells[i];
        const u8 *want_bytes;
        const u8 *got_bytes;
        size_t want_len;
        size_t got_len;

        want_bytes = grid_cell_bytes(grid, want, &want_len);
        got_bytes = vt_cell_bytes(vt, got, &got_len);
        if (want_len != got_len ||
            (want_len != 0U && (want_bytes == NULL || got_bytes == NULL ||
             memcmp(want_bytes, got_bytes, want_len) != 0)) ||
            want->w != got->w || rendered_attrs(want->attrs) != got->attrs ||
            memcmp(&want->fg, &got->fg, sizeof(want->fg)) != 0 ||
            memcmp(&want->bg, &got->bg, sizeof(want->bg)) != 0) {
            (void)fprintf(stderr,
                          "f01-vt-width: cell=%zu len=%zu/%zu width=%u/%u "
                          "attrs=%u/%u\n",
                          i, want_len, got_len, (unsigned)want->w,
                          (unsigned)got->w, (unsigned)rendered_attrs(want->attrs),
                          (unsigned)got->attrs);
            return false;
        }
    }
    return true;
}

static bool run_width(bool ambiguous_wide, size_t *out_cells,
                      size_t *out_bytes)
{
    YewWidthOpts width_opts = {ambiguous_wide};
    size_t model_cells = 0U;
    size_t i;
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    TtyCaps caps = {0};
    YewColor color = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    Bytebuf frame;
    VtScreen vt;
    u16 placed;
    u16 cols;
    bool ok;

    yew_width_set_opts(&width_opts);
    for (i = 0U; i < CLUSTER_COUNT; i++) {
        const ClusterCase *one = &cases[i % YEW_ARRAY_LEN(cases)];
        int cells = yew_cluster_width(one->bytes, one->len);

        if (cells <= 0)
            return false;
        model_cells += (size_t)cells;
    }
    if (model_cells >= UINT16_MAX)
        return false;
    cols = (u16)(model_cells + 1U);
    arena_init(&arena);
    interner_init(&interner, &arena);
    if (!yew_grid_init(&grid, &interner, 1U, cols)) {
        interner_free(&interner);
        arena_free_all(&arena);
        return false;
    }
    placed = 0U;
    for (i = 0U; i < CLUSTER_COUNT; i++) {
        const ClusterCase *one = &cases[i % YEW_ARRAY_LEN(cases)];

        placed = yew_grid_put(&grid, 0U, placed, one->bytes, one->len,
                              color, color, 0U);
    }
    yew_grid_cursor(&grid, 0U, placed, false);
    caps.truecolor = true;
    caps.sync_output = true;
    yew_render_init(&render, &caps, NULL);
    bytebuf_init(&frame);
    vt_init(&vt, 1, cols);
    vt_feed(&vt, (const u8 *)"\033[?1049h", 8U);
    (void)yew_render_frame(&render, &grid, &frame);
    vt_feed(&vt, frame.data, frame.len);
    if (placed != model_cells)
        (void)fprintf(stderr,
                      "f01-vt-width: placed=%u model=%zu ambiguous=%u\n",
                      (unsigned)placed, model_cells,
                      ambiguous_wide ? 1U : 0U);
    if (vt.nerrors != 0U)
        (void)fprintf(stderr, "f01-vt-width: VT errors=%u: %.*s",
                      (unsigned)vt.nerrors, (int)vt.errors.len,
                      (const char *)vt.errors.data);
    ok = placed == model_cells && vt.nerrors == 0U &&
         screen_matches(&grid, &vt, model_cells);
    *out_cells = model_cells;
    *out_bytes = frame.len;
    vt_free(&vt);
    bytebuf_free(&frame);
    yew_grid_free(&grid);
    interner_free(&interner);
    arena_free_all(&arena);
    yew_width_set_opts(NULL);
    return ok;
}

int main(void)
{
    size_t narrow_cells = 0U;
    size_t narrow_bytes = 0U;
    size_t wide_cells = 0U;
    size_t wide_bytes = 0U;

    if (!run_width(false, &narrow_cells, &narrow_bytes) ||
        !run_width(true, &wide_cells, &wide_bytes)) {
        (void)fprintf(stderr,
                      "f01-vt-width: model/renderer/VT disagreement\n");
        return 1;
    }
    (void)printf("f01-vt-width: clusters=%u narrow_cells=%zu "
                 "wide_cells=%zu frame_bytes=%zu/%zu ok\n",
                 CLUSTER_COUNT, narrow_cells, wide_cells,
                 narrow_bytes, wide_bytes);
    return 0;
}
