#include "harness.h"

#include "term/grid.h"
#include "util/arena.h"
#include "util/intern.h"

static const u8 WIDE_CJK[] = {0xe6u, 0xbcu, 0xa2u};
static const u8 COMBINING_ACUTE[] = {0xccu, 0x81u};

static YewColor wide_color(u8 tag, u8 r, u8 g, u8 b)
{
    YewColor color = {tag, r, g, b};

    return color;
}

static void wide_init(Grid *grid, Arena *arena, Interner *interner, u16 cols)
{
    arena_init(arena);
    interner_init(interner, arena);
    YEW_ASSERT(yew_grid_init(grid, interner, 1u, cols));
}

static void wide_free(Grid *grid, Arena *arena, Interner *interner)
{
    yew_grid_free(grid);
    interner_free(interner);
    arena_free_all(arena);
}

void test_grid_wide_tail_copies_style(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor fg = wide_color(2u, 10u, 20u, 30u);
    YewColor bg = wide_color(1u, 4u, 0u, 0u);
    Cell overlay;

    wide_init(&grid, &arena, &interner, 4u);
    yew_grid_put(&grid, 0u, 1u, WIDE_CJK, sizeof(WIDE_CJK), fg, bg,
                 YEW_ATTR_INVALID_BYTE);
    YEW_ASSERT_EQ_U64(grid.back[1].w, 2u);
    YEW_ASSERT_EQ_U64(grid.back[2].w, 0u);
    YEW_ASSERT_EQ_MEM(&grid.back[1].fg, &grid.back[2].fg, sizeof(YewColor));
    YEW_ASSERT_EQ_MEM(&grid.back[1].bg, &grid.back[2].bg, sizeof(YewColor));
    YEW_ASSERT_EQ_U64(grid.back[1].attrs, grid.back[2].attrs);
    yew_grid_flip(&grid);
    overlay = grid.blank;
    overlay.bg = wide_color(YEW_COLOR_RGB, 90u, 80u, 70u);
    overlay.attrs = YEW_ATTR_REVERSE;
    yew_grid_overlay(&grid, 0u, 2u, 3u, &overlay,
                     YEW_OVERLAY_BG | YEW_OVERLAY_ATTRS);
    YEW_ASSERT_EQ_U64(grid.back[1].w, 2u);
    YEW_ASSERT_EQ_U64(grid.back[2].w, 0u);
    YEW_ASSERT_EQ_MEM(grid.back[1].utf8, WIDE_CJK, sizeof(WIDE_CJK));
    YEW_ASSERT_EQ_MEM(&grid.back[1].bg, &overlay.bg, sizeof(YewColor));
    YEW_ASSERT_EQ_MEM(&grid.back[2].bg, &overlay.bg, sizeof(YewColor));
    YEW_ASSERT_EQ_U64(grid.back[1].attrs,
                      YEW_ATTR_INVALID_BYTE | YEW_ATTR_REVERSE);
    YEW_ASSERT_EQ_U64(grid.back[2].attrs,
                      YEW_ATTR_INVALID_BYTE | YEW_ATTR_REVERSE);
    YEW_ASSERT_EQ_U64(grid.dmg[0].lo, 1u);
    YEW_ASSERT_EQ_U64(grid.dmg[0].hi, 3u);
    wide_free(&grid, &arena, &interner);
}

void test_grid_overwrite_wide_head_blanks_tail(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);

    wide_init(&grid, &arena, &interner, 5u);
    yew_grid_put(&grid, 0u, 1u, WIDE_CJK, sizeof(WIDE_CJK), color, color, 0u);
    yew_grid_flip(&grid);
    yew_grid_put(&grid, 0u, 1u, (const u8 *)"x", 1u, color, color, 0u);
    YEW_ASSERT(yew_cell_eq(&grid.back[2], &grid.blank));
    YEW_ASSERT_EQ_U64(grid.dmg[0].lo, 1u);
    YEW_ASSERT(grid.dmg[0].hi >= 3u);
    wide_free(&grid, &arena, &interner);
}

void test_grid_overwrite_wide_tail_blanks_head(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);

    wide_init(&grid, &arena, &interner, 5u);
    yew_grid_put(&grid, 0u, 1u, WIDE_CJK, sizeof(WIDE_CJK), color, color, 0u);
    yew_grid_flip(&grid);
    yew_grid_put(&grid, 0u, 2u, (const u8 *)"x", 1u, color, color, 0u);
    YEW_ASSERT(yew_cell_eq(&grid.back[1], &grid.blank));
    YEW_ASSERT_EQ_U64(grid.dmg[0].lo, 1u);
    YEW_ASSERT(grid.dmg[0].hi >= 3u);
    wide_free(&grid, &arena, &interner);
}

void test_grid_wide_at_last_column_becomes_space(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor fg = wide_color(1u, 2u, 0u, 0u);
    YewColor bg = wide_color(0u, 0u, 0u, 0u);

    wide_init(&grid, &arena, &interner, 3u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 2u, WIDE_CJK,
                                  sizeof(WIDE_CJK), fg, bg, 1u), 3u);
    YEW_ASSERT_EQ_U64(grid.back[2].w, 1u);
    YEW_ASSERT_EQ_U64(grid.back[2].utf8[0], (u8)' ');
    YEW_ASSERT_EQ_MEM(&grid.back[2].fg, &fg, sizeof(fg));
    wide_free(&grid, &arena, &interner);
}

void test_grid_zero_width_appends_to_previous_cell(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);
    const u8 expected[] = {'e', 0xccu, 0x81u};

    wide_init(&grid, &arena, &interner, 3u);
    yew_grid_put(&grid, 0u, 0u, (const u8 *)"e", 1u, color, color, 0u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 1u, COMBINING_ACUTE,
                                  sizeof(COMBINING_ACUTE), color, color, 0u),
                      1u);
    YEW_ASSERT_EQ_MEM(grid.back[0].utf8, expected, sizeof(expected));
    YEW_ASSERT_EQ_U64(grid.back[0].utf8[3], 0u);
    wide_free(&grid, &arena, &interner);
}

void test_grid_zero_width_at_column_zero_is_dropped(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);

    wide_init(&grid, &arena, &interner, 3u);
    yew_grid_flip(&grid);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 0u, COMBINING_ACUTE,
                                  sizeof(COMBINING_ACUTE), color, color, 0u),
                      0u);
    YEW_ASSERT(yew_cell_eq(&grid.back[0], &grid.blank));
    YEW_ASSERT(grid.dmg[0].lo >= grid.dmg[0].hi);
    wide_free(&grid, &arena, &interner);
}

void test_grid_wide_write_snaps_existing_cursor_left(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);

    wide_init(&grid, &arena, &interner, 6u);
    yew_grid_cursor(&grid, 0u, 3u, true);
    yew_grid_put(&grid, 0u, 2u, WIDE_CJK, sizeof(WIDE_CJK),
                 color, color, 0u);
    YEW_ASSERT_EQ_U64(grid.back[3].w, 0u);
    YEW_ASSERT_EQ_U64(grid.cur_col, 2u);
    wide_free(&grid, &arena, &interner);
}

void test_grid_zero_width_after_blank_is_dropped(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);

    wide_init(&grid, &arena, &interner, 3u);
    yew_grid_flip(&grid);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 1u, COMBINING_ACUTE,
                                  sizeof(COMBINING_ACUTE), color, color, 0u),
                      1u);
    YEW_ASSERT(yew_cell_eq(&grid.back[0], &grid.blank));
    YEW_ASSERT(grid.dmg[0].lo >= grid.dmg[0].hi);
    wide_free(&grid, &arena, &interner);
}

void test_grid_combining_overflow_preserves_wide_interned_base(void)
{
    static const u8 family[] = {
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa7u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa6u
    };
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = wide_color(0u, 0u, 0u, 0u);
    const char *stored;

    wide_init(&grid, &arena, &interner, 4u);
    yew_grid_put(&grid, 0u, 0u, family, sizeof(family), color, color, 0u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 2u, COMBINING_ACUTE,
                                  sizeof(COMBINING_ACUTE), color, color, 0u),
                      2u);
    YEW_ASSERT((grid.back[0].flags & CELL_INTERNED) != 0u);
    YEW_ASSERT_EQ_U64(grid.back[0].w, 2u);
    YEW_ASSERT_EQ_U64(grid.back[1].w, 0u);
    stored = yew_intern_str(&interner, grid.back[0].id);
    YEW_ASSERT_NOT_NULL(stored);
    YEW_ASSERT_EQ_MEM(stored, family, sizeof(family));
    YEW_ASSERT_EQ_MEM(stored + sizeof(family), COMBINING_ACUTE,
                      sizeof(COMBINING_ACUTE));
    wide_free(&grid, &arena, &interner);
}
