#include "harness.h"

#include "term/grid.h"
#include "util/arena.h"
#include "util/intern.h"

static YewColor grid_default_color(void)
{
    YewColor color = {0u, 0u, 0u, 0u};

    return color;
}

static void grid_fixture_init(Grid *grid, Arena *arena, Interner *interner,
                              u16 rows, u16 cols)
{
    arena_init(arena);
    interner_init(interner, arena);
    YEW_ASSERT(yew_grid_init(grid, interner, rows, cols));
}

static void grid_fixture_free(Grid *grid, Arena *arena, Interner *interner)
{
    yew_grid_free(grid);
    interner_free(interner);
    arena_free_all(arena);
}

void test_grid_cell_layout(void)
{
    YEW_ASSERT_EQ_U64(sizeof(YewColor), 4u);
    YEW_ASSERT_EQ_U64(sizeof(Cell), 20u);
}

void test_grid_put_zeroes_inline_tail(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = grid_default_color();
    const u8 long_cluster[] = {'e', 0xccu, 0x81u, 0xccu, 0x82u};
    const u8 short_cluster[] = {'x'};
    size_t i;

    grid_fixture_init(&grid, &arena, &interner, 1u, 3u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 0u, long_cluster,
                                  sizeof(long_cluster), color, color, 0u), 1u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 0u, short_cluster,
                                  sizeof(short_cluster), color, color, 0u), 1u);
    YEW_ASSERT_EQ_U64(grid.back[0].utf8[0], (u8)'x');
    for (i = 1u; i < sizeof(grid.back[0].utf8); i++)
        YEW_ASSERT_EQ_U64(grid.back[0].utf8[i], 0u);
    grid_fixture_free(&grid, &arena, &interner);
}

void test_grid_interns_long_cluster(void)
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
    YewColor color = grid_default_color();
    const char *stored;

    grid_fixture_init(&grid, &arena, &interner, 1u, 3u);
    YEW_ASSERT_EQ_U64(sizeof(family), 25u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 0u, family, sizeof(family),
                                  color, color, 0u), 2u);
    YEW_ASSERT(grid.back[0].flags != 0u);
    stored = yew_intern_str(&interner, grid.back[0].id);
    YEW_ASSERT_NOT_NULL(stored);
    YEW_ASSERT_EQ_MEM(stored, family, sizeof(family));
    YEW_ASSERT_EQ_U64((u8)stored[sizeof(family)], 0u);
    grid_fixture_free(&grid, &arena, &interner);
}

void test_grid_damage_unions_writes(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = grid_default_color();

    grid_fixture_init(&grid, &arena, &interner, 2u, 8u);
    yew_grid_flip(&grid);
    yew_grid_put(&grid, 1u, 5u, (const u8 *)"x", 1u, color, color, 0u);
    yew_grid_put(&grid, 1u, 2u, (const u8 *)"y", 1u, color, color, 0u);
    YEW_ASSERT_EQ_U64(grid.dmg[1].lo, 2u);
    YEW_ASSERT_EQ_U64(grid.dmg[1].hi, 7u);
    YEW_ASSERT_EQ_U64(grid.dmg_lo, 1u);
    YEW_ASSERT_EQ_U64(grid.dmg_hi, 2u);
    grid_fixture_free(&grid, &arena, &interner);
}

void test_grid_resize_discards_cells_and_marks_all(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = grid_default_color();
    size_t i;

    grid_fixture_init(&grid, &arena, &interner, 2u, 3u);
    yew_grid_put(&grid, 0u, 0u, (const u8 *)"x", 1u, color, color, 0u);
    yew_grid_flip(&grid);
    YEW_ASSERT(yew_grid_resize(&grid, 3u, 2u));
    YEW_ASSERT_EQ_U64(grid.rows, 3u);
    YEW_ASSERT_EQ_U64(grid.cols, 2u);
    YEW_ASSERT_EQ_U64(grid.dmg_lo, 0u);
    YEW_ASSERT_EQ_U64(grid.dmg_hi, 3u);
    for (i = 0u; i < 6u; i++) {
        YEW_ASSERT(yew_cell_eq(&grid.back[i], &grid.blank));
        YEW_ASSERT_EQ_U64(grid.front[i].w, 0xffu);
    }
    for (i = 0u; i < 3u; i++) {
        YEW_ASSERT_EQ_U64(grid.dmg[i].lo, 0u);
        YEW_ASSERT_EQ_U64(grid.dmg[i].hi, 2u);
    }
    grid_fixture_free(&grid, &arena, &interner);
}

void test_grid_cursor_snaps_left_from_continuation(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = grid_default_color();
    static const u8 cjk[] = {0xe6u, 0xbcu, 0xa2u};

    grid_fixture_init(&grid, &arena, &interner, 1u, 4u);
    yew_grid_put(&grid, 0u, 1u, cjk, sizeof(cjk), color, color, 0u);
    yew_grid_cursor(&grid, 0u, 2u, true);
    YEW_ASSERT_EQ_U64(grid.cur_row, 0u);
    YEW_ASSERT_EQ_U64(grid.cur_col, 1u);
    YEW_ASSERT(grid.cur_vis);
    grid_fixture_free(&grid, &arena, &interner);
}

void test_grid_controls_are_lowered_to_printable_cells(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = grid_default_color();
    static const u8 controls[] = {0x01u, 0x7fu};

    grid_fixture_init(&grid, &arena, &interner, 1u, 5u);
    YEW_ASSERT_EQ_U64(yew_grid_puts(&grid, 0u, 0u, controls,
                                   sizeof(controls), color, color, 0u), 4u);
    YEW_ASSERT_EQ_U64(grid.back[0].utf8[0], '^');
    YEW_ASSERT_EQ_U64(grid.back[1].utf8[0], 'A');
    YEW_ASSERT_EQ_U64(grid.back[2].utf8[0], '^');
    YEW_ASSERT_EQ_U64(grid.back[3].utf8[0], '?');
    grid_fixture_free(&grid, &arena, &interner);
}

void test_grid_invalid_and_c1_bytes_are_lowered(void)
{
    Grid grid;
    Arena arena;
    Interner interner;
    YewColor color = grid_default_color();
    static const u8 invalid[] = {0xffu};
    static const u8 c1[] = {0xc2u, 0x85u};
    size_t i;

    grid_fixture_init(&grid, &arena, &interner, 1u, 9u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 0u, invalid, sizeof(invalid),
                                  color, color, 0u), 4u);
    YEW_ASSERT_EQ_MEM(grid.back[0].utf8, "<", 1u);
    YEW_ASSERT_EQ_MEM(grid.back[1].utf8, "F", 1u);
    YEW_ASSERT_EQ_MEM(grid.back[2].utf8, "F", 1u);
    YEW_ASSERT_EQ_MEM(grid.back[3].utf8, ">", 1u);
    for (i = 0u; i < 4u; i++)
        YEW_ASSERT((grid.back[i].attrs & YEW_ATTR_INVALID_BYTE) != 0u);
    YEW_ASSERT_EQ_U64(yew_grid_put(&grid, 0u, 4u, c1, sizeof(c1),
                                  color, color, 0u), 8u);
    YEW_ASSERT_EQ_MEM(grid.back[4].utf8, "<", 1u);
    YEW_ASSERT_EQ_MEM(grid.back[5].utf8, "8", 1u);
    YEW_ASSERT_EQ_MEM(grid.back[6].utf8, "5", 1u);
    YEW_ASSERT_EQ_MEM(grid.back[7].utf8, ">", 1u);
    for (i = 4u; i < 8u; i++)
        YEW_ASSERT((grid.back[i].attrs & YEW_ATTR_INVALID_BYTE) == 0u);
    grid_fixture_free(&grid, &arena, &interner);
}
