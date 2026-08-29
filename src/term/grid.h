#ifndef YEW_TERM_GRID_H
#define YEW_TERM_GRID_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"
#include "util/intern.h"

typedef struct YewColor {
    u8 tag;
    u8 r;
    u8 g;
    u8 b;
} YewColor;

_Static_assert(sizeof(YewColor) == 4, "colour must be word-sized");

enum {
    YEW_COLOR_DEFAULT = 0,
    YEW_COLOR_INDEXED = 1,
    YEW_COLOR_RGB = 2
};

enum {
    YEW_ATTR_BOLD = 1u << 0,
    YEW_ATTR_DIM = 1u << 1,
    YEW_ATTR_ITALIC = 1u << 2,
    YEW_ATTR_UNDERLINE = 1u << 3,
    YEW_ATTR_UNDERCURL = 1u << 4,
    YEW_ATTR_BLINK = 1u << 5,
    YEW_ATTR_REVERSE = 1u << 6,
    YEW_ATTR_CONCEAL = 1u << 7,
    YEW_ATTR_STRIKE = 1u << 8,
    YEW_ATTR_OVERLINE = 1u << 9,
    YEW_CELL_UL_SHIFT = 10,
    YEW_CELL_UL_MASK = 3u << YEW_CELL_UL_SHIFT,
    YEW_CELL_UL_ERROR = 1u << YEW_CELL_UL_SHIFT,
    YEW_CELL_UL_WARN = 2u << YEW_CELL_UL_SHIFT,
    YEW_CELL_UL_INFO = 3u << YEW_CELL_UL_SHIFT,
    YEW_ATTR_INVALID_BYTE = 1u << 12,
    YEW_CELL_ATTR_MASK = (YEW_ATTR_INVALID_BYTE << 1) - 1u
};

_Static_assert((YEW_CELL_UL_MASK & ((1u << YEW_CELL_UL_SHIFT) - 1u)) == 0u,
               "underline selector must not overlap terminal attrs");
_Static_assert((YEW_CELL_UL_MASK & YEW_ATTR_INVALID_BYTE) == 0u,
               "invalid-byte marker must not overlap underline selector");

enum {
    CELL_INTERNED = 1u << 0
};

enum {
    YEW_OVERLAY_FG = 1u << 0,
    YEW_OVERLAY_BG = 1u << 1,
    YEW_OVERLAY_ATTRS = 1u << 2
};

typedef enum YewCursorShape {
    YEW_CURSOR_BLOCK = 0,
    YEW_CURSOR_BAR
} YewCursorShape;

/*
 * Eight inline bytes keep ordinary Latin, CJK, and emoji clusters out of
 * the interner. The 20-byte cell keeps the double-buffered diff working set
 * within the intended cache tier at representative terminal sizes:
 *
 *   Grid      Cells    One buffer    Both buffers
 *   80x24      1,920     37.5 KiB       75 KiB (L1/L2)
 *   200x50    10,000      195 KiB      391 KiB (L2)
 *   400x100   40,000      781 KiB      1.5 MiB (L3)
 *
 * Widening the inline store would make the common repaint path pay for rare
 * long combining and ZWJ sequences; shrinking it would intern common text.
 */
typedef struct Cell {
    union {
        u8 utf8[8];
        u32 id;
    };
    YewColor fg;
    YewColor bg;
    u16 attrs;
    u8 w;
    u8 flags;
} Cell;

_Static_assert(sizeof(Cell) == 20, "Cell size budget -- see grid.h");

typedef struct Damage {
    u16 lo;
    u16 hi;
} Damage;

typedef struct Grid {
    u16 rows;
    u16 cols;
    Cell *front;
    Cell *back;
    Damage *dmg;
    u16 dmg_lo;
    u16 dmg_hi;
    Interner *gi;
    u16 cur_row;
    u16 cur_col;
    bool cur_vis;
    YewCursorShape cur_shape;
    u64 cursor_generation;
    u64 cursor_overlay_signature;
    bool cursor_overlay_valid;
    u64 cursor_overlay_primary_pos;
    u64 cursor_overlay_primary_anchor;
    bool cursor_overlay_primary_valid;
    Cell blank;
} Grid;

bool yew_grid_init(Grid *g, Interner *gi, u16 rows, u16 cols);
void yew_grid_free(Grid *g);
bool yew_grid_resize(Grid *g, u16 rows, u16 cols);
void yew_grid_clear(Grid *g);
/* Forget the physical terminal image while preserving the desired back
 * buffer.  The next frame must emit every cell, even when model state did
 * not change (for example after re-entering the alternate screen). */
void yew_grid_invalidate(Grid *g);
u16 yew_grid_put(Grid *g, u16 row, u16 col, const u8 *cluster, size_t n,
                 YewColor fg, YewColor bg, u16 attrs);
u16 yew_grid_puts(Grid *g, u16 row, u16 col, const u8 *s, size_t n,
                  YewColor fg, YewColor bg, u16 attrs);
void yew_grid_fill(Grid *g, u16 row, u16 c0, u16 c1, Cell c);
/* Applies selected style fields without replacing the glyph or its width.
 * An overlap with either half of a wide glyph styles the complete pair. */
void yew_grid_overlay(Grid *g, u16 row, u16 c0, u16 c1,
                      const Cell *style, u8 fields);
void yew_grid_cursor(Grid *g, u16 row, u16 col, bool visible);
void yew_grid_cursor_shape(Grid *g, YewCursorShape shape);
void yew_grid_mark_all(Grid *g);
void yew_grid_flip(Grid *g);
bool yew_cell_eq(const Cell *a, const Cell *b);

#endif
