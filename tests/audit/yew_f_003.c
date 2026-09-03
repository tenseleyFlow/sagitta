/*
 * YEW-F-003 — ASCII-base keycap leaves inconsistent grid width.
 *
 * Correct behavior: yew_grid_puts segments `1` + VS16 + U+20E3 as one
 * two-cell grapheme, storing a width-2 head and width-0 continuation.
 *
 * Baseline failure: the printable-ASCII fast path stores `1` at width 1,
 * then appends the zero-width suffix without recomputing the complete
 * cluster width. The renderer consequently takes its YEW_BUG path on valid
 * keycap text.
 */
#include "audit.h"

#include <stdio.h>

#include "term/grid.h"
#include "util/arena.h"
#include "util/intern.h"

bool test_yew_f_003(char *why, size_t why_cap)
{
    static const u8 keycap[] = "1\xef\xb8\x8f\xe2\x83\xa3";
    Arena arena;
    Interner interner;
    Grid grid;
    YewColor color = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    u16 placed;
    bool ok;

    arena_init(&arena);
    interner_init(&interner, &arena);
    if (!yew_grid_init(&grid, &interner, 1U, 4U)) {
        interner_free(&interner);
        arena_free_all(&arena);
        (void)snprintf(why, why_cap, "could not initialize grid");
        return false;
    }
    placed = yew_grid_puts(&grid, 0U, 0U, keycap, sizeof(keycap) - 1U,
                           color, color, 0U);
    ok = placed == 2U && grid.back[0].w == 2U && grid.back[1].w == 0U;
    if (!ok) {
        (void)snprintf(why, why_cap,
                       "placed=%u head-width=%u tail-width=%u, expected 2/2/0",
                       (unsigned)placed, (unsigned)grid.back[0].w,
                       (unsigned)grid.back[1].w);
    }
    yew_grid_free(&grid);
    interner_free(&interner);
    arena_free_all(&arena);
    return ok;
}
