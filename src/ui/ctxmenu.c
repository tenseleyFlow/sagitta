/*
 * Sprint 27 §5, Sprint 57.13 §2.  See ctxmenu.h for the capture-at-open
 * law this file exists to enforce, and for the box, shedding and
 * placement contracts.  Deliberately no edit/ed.h.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/ctxmenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/glyphs.h"
#include "ui/region.h"
#include "unicode/width.h"
#include "util/base.h"

enum {
    CTX_LABEL_MAX = 48,
    CTX_ACCEL_MAX = 12,
    /* One cell of padding each side of the content, inside the border. */
    CTX_PAD = 1,
    /* Border + padding, per side. */
    CTX_EDGE = CTX_PAD + 1,
    /* The gap between a label and its accelerator. */
    CTX_GAP = 2,
    /* Two borders, two pads, and two label cells: narrower than this a
     * row cannot carry even the first letter of what it does. */
    CTX_FLOOR_WIDTH = 2 * CTX_EDGE + 2,
    /* Two borders and one row. */
    CTX_FLOOR_HEIGHT = 3,
    /* Long enough that nothing a caller can build ever hits it. */
    CTX_UNCLIPPED = 1000
};

typedef struct CtxRow {
    char label[CTX_LABEL_MAX];
    char accel[CTX_ACCEL_MAX];
    u32 action;
    bool enabled;
    bool separator;
    u8 priority;
} CtxRow;

static struct {
    CtxRow rows[YEW_CTX_MAX_ROWS];
    u32 n;
    u32 kind;
    bool active;
    /* The width clamp dropped the accelerator column. */
    bool accels_hidden;
    i32 cursor;
    u32 chosen;
    u32 shed;
    Rect box;
    /* THE TARGET, captured at open time.  See ctxmenu.h. */
    u32 target_id;
    char *target_path;
    Rect target_rect;
} ctx;

/* ---------------------------------------------------------------- */
/* Building                                                         */
/* ---------------------------------------------------------------- */

void yew_ctx_begin(u32 kind)
{
    /*
     * Whatever was open is discarded.  Two menus can never both be up:
     * the second would shadow the first in the region table and the
     * first would keep answering keys, which is how a menu ends up
     * acting on a target nobody can see.
     */
    yew_xfree(ctx.target_path);
    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.kind = kind;
    ctx.cursor = -1;
}

void yew_ctx_item(const char *label, const char *accel, u32 action,
                  bool enabled, u8 priority)
{
    CtxRow *r;

    if (ctx.n >= (u32)YEW_CTX_MAX_ROWS || label == NULL)
        return;
    r = &ctx.rows[ctx.n++];
    (void)memset(r, 0, sizeof(*r));
    (void)snprintf(r->label, sizeof(r->label), "%s", label);
    if (accel != NULL)
        (void)snprintf(r->accel, sizeof(r->accel), "%s", accel);
    r->action = action;
    r->enabled = enabled;
    r->priority = priority > (u8)YEW_CTX_PRIORITY_MAX
                      ? (u8)YEW_CTX_PRIORITY_MAX : priority;
}

void yew_ctx_sep(void)
{
    CtxRow *r;

    if (ctx.n >= (u32)YEW_CTX_MAX_ROWS)
        return;
    r = &ctx.rows[ctx.n++];
    (void)memset(r, 0, sizeof(*r));
    r->separator = true;
    /*
     * A separator INHERITS the priority of the row above it, so it
     * leaves the menu with the section it closes.  A leading one has
     * nothing to inherit from and is dropped at show time anyway.
     */
    if (ctx.n >= 2U)
        r->priority = ctx.rows[ctx.n - 2U].priority;
}

void yew_ctx_target(u32 id, const char *path)
{
    yew_xfree(ctx.target_path);
    ctx.target_path = NULL;
    ctx.target_id = id;
    if (path != NULL) {
        size_t n = strlen(path) + 1U;

        /* COPIED, not aliased: the tab that owns the original can be
         * closed while the menu is up, and the menu would then hold a
         * pointer into freed memory. */
        ctx.target_path = yew_xmalloc(n);
        (void)memcpy(ctx.target_path, path, n);
    }
}

void yew_ctx_target_rect(Rect rect)
{
    ctx.target_rect = rect;
}

u32 yew_ctx_target_id(void)
{
    return ctx.target_id;
}

const char *yew_ctx_target_path(void)
{
    return ctx.target_path;
}

Rect yew_ctx_target_rect_get(void)
{
    return ctx.target_rect;
}

/* ---------------------------------------------------------------- */
/* Geometry                                                         */
/* ---------------------------------------------------------------- */

static u16 cells_of(const char *s)
{
    int cells = 0;

    (void)yew_str_clip((const u8 *)s, strlen(s), CTX_UNCLIPPED, &cells);
    return (u16)cells;
}

/* Content cells a row needs: label, and the accelerator with its gap
 * unless the column has been dropped. */
static u16 row_cells(const CtxRow *r, bool with_accel)
{
    u16 w;

    if (r->separator)
        return 0U;
    w = cells_of(r->label);
    if (with_accel && r->accel[0] != '\0')
        w = (u16)(w + CTX_GAP + cells_of(r->accel));
    return w;
}

static u16 widest_row(bool with_accel)
{
    u16 widest = 0U;
    u32 i;

    for (i = 0U; i < ctx.n; i++) {
        u16 w = row_cells(&ctx.rows[i], with_accel);

        if (w > widest)
            widest = w;
    }
    return widest;
}

static u16 box_width_for(u16 widest)
{
    u16 w = (u16)(widest + 2U * CTX_EDGE);

    return w < (u16)YEW_CTX_MIN_WIDTH ? (u16)YEW_CTX_MIN_WIDTH : w;
}

/*
 * Drops every separator that would lead, trail or double up.  Run
 * after every shed, and once when nothing was shed, so the drawn list
 * never depends on WHICH rows went — only on the rows that stayed.
 */
static void drop_stray_separators(void)
{
    u32 out = 0U;
    u32 i;

    for (i = 0U; i < ctx.n; i++) {
        if (ctx.rows[i].separator) {
            if (out == 0U || ctx.rows[out - 1U].separator)
                continue;
        }
        if (out != i)
            ctx.rows[out] = ctx.rows[i];
        out++;
    }
    while (out > 0U && ctx.rows[out - 1U].separator)
        out--;
    ctx.n = out;
}

static void shed_priority(u8 level)
{
    u32 out = 0U;
    u32 i;

    for (i = 0U; i < ctx.n; i++) {
        if (ctx.rows[i].priority >= level)
            continue;
        if (out != i)
            ctx.rows[out] = ctx.rows[i];
        out++;
    }
    ctx.n = out;
}

/*
 * Sheds whole priority levels, lowest first, until the rows fit
 * `avail`.  Every level goes as a unit: shedding "just enough" would
 * make the surviving rows depend on the terminal height in ways the
 * eye cannot predict, and invariant 5 wants the same input to give the
 * same menu.  False when the priority-0 rows alone do not fit.
 */
static bool shed_to_fit(u32 avail)
{
    u32 built = ctx.n;
    u8 level;

    drop_stray_separators();
    for (level = (u8)YEW_CTX_PRIORITY_MAX; level >= 1U && ctx.n > avail;
         level--) {
        shed_priority(level);
        drop_stray_separators();
    }
    ctx.shed = built - ctx.n;
    return ctx.n > 0U && ctx.n <= avail;
}

/* The first row a cursor may land on, walking `step` from `from`.
 * Separators and disabled rows are skipped — they are drawn, they are
 * simply not reachable. */
static i32 next_usable(i32 from, int step)
{
    i32 at = from;
    u32 guard;

    if (ctx.n == 0U)
        return -1;
    for (guard = 0U; guard <= ctx.n; guard++) {
        at += step;
        if (at < 0)
            at = (i32)ctx.n - 1;
        if (at >= (i32)ctx.n)
            at = 0;
        if (!ctx.rows[at].separator && ctx.rows[at].enabled)
            return at;
    }
    return -1;
}

bool yew_ctx_show(u16 anchor_x, u16 anchor_y, Rect allowed)
{
    u16 w;
    u16 h;
    i32 x;
    i32 y;

    ctx.active = false;
    ctx.shed = 0U;
    ctx.accels_hidden = false;
    if (ctx.n == 0U || allowed.h < (u16)CTX_FLOOR_HEIGHT ||
        allowed.w < (u16)CTX_FLOOR_WIDTH)
        return false; /* a menu drawn half off the screen is worse than
                       * none, so it does not open at all */
    /* Height: shed until the rows fit between the two border rows. */
    if (!shed_to_fit((u32)allowed.h - 2U))
        return false;
    h = (u16)(ctx.n + 2U);
    /*
     * Width: the box wants its widest row.  When the allowed rectangle
     * is narrower the ACCELERATORS go first, as a column — a menu with
     * half its accelerators is worse than one with none — and only
     * then are labels clipped, at draw time, to whatever is left.
     */
    w = box_width_for(widest_row(true));
    if (w > allowed.w) {
        u16 bare = box_width_for(widest_row(false));

        /* Only claim the column was dropped when dropping it actually
         * bought a cell: a menu with no accelerators at all, squeezed
         * by a narrow rectangle, has not hidden anything. */
        ctx.accels_hidden = bare < w;
        w = bare > allowed.w ? allowed.w : bare;
    }
    /*
     * CLAMP, NEVER FLIP.  Sliding the box back inside `allowed` keeps
     * the row the user aimed at under the pointer; flipping it above
     * the anchor puts a different row there, and the click that follows
     * opens something the user never chose.
     *
     * The corner goes BELOW-RIGHT of the anchor: the cell the pointer is
     * on is then the top-left border cell, so a release on the cell
     * that opened the menu activates nothing.
     */
    x = (i32)anchor_x + 1;
    y = (i32)anchor_y + 1;
    if (x + (i32)w > (i32)allowed.x + (i32)allowed.w)
        x = (i32)allowed.x + (i32)allowed.w - (i32)w;
    if (x < (i32)allowed.x)
        x = (i32)allowed.x;
    if (y + (i32)h > (i32)allowed.y + (i32)allowed.h)
        y = (i32)allowed.y + (i32)allowed.h - (i32)h;
    if (y < (i32)allowed.y)
        y = (i32)allowed.y;
    ctx.box = (Rect){(u16)x, (u16)y, w, h};
    ctx.active = true;
    ctx.chosen = 0U;
    ctx.cursor = next_usable(-1, 1);
    return true;
}

bool yew_ctx_active(void)
{
    return ctx.active;
}

void yew_ctx_close(void)
{
    yew_xfree(ctx.target_path);
    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.cursor = -1;
}

Rect yew_ctx_box(void)
{
    return ctx.box;
}

u32 yew_ctx_kind(void)
{
    return ctx.kind;
}

i32 yew_ctx_cursor(void)
{
    return ctx.cursor;
}

u32 yew_ctx_rows(void)
{
    return ctx.n;
}

bool yew_ctx_row_enabled(u32 row)
{
    return row < ctx.n && ctx.rows[row].enabled && !ctx.rows[row].separator;
}

u32 yew_ctx_shed_count(void)
{
    return ctx.shed;
}

u8 yew_ctx_priority(u32 row)
{
    return row < ctx.n ? ctx.rows[row].priority : 0U;
}

const char *yew_ctx_row_label(u32 row)
{
    if (row >= ctx.n || ctx.rows[row].separator)
        return "";
    return ctx.rows[row].label;
}

u32 yew_ctx_row_action(u32 row)
{
    return row < ctx.n ? ctx.rows[row].action : 0U;
}

bool yew_ctx_row_is_sep(u32 row)
{
    return row < ctx.n && ctx.rows[row].separator;
}

bool yew_ctx_accels_hidden(void)
{
    return ctx.accels_hidden;
}

/* ---------------------------------------------------------------- */
/* Choosing                                                         */
/* ---------------------------------------------------------------- */

void yew_ctx_hover(i32 row)
{
    if (!ctx.active || row < 0 || row >= (i32)ctx.n)
        return;
    /* A disabled row does not take the highlight: the highlight is a
     * promise that Enter will do something. */
    if (ctx.rows[row].separator || !ctx.rows[row].enabled)
        return;
    ctx.cursor = row;
}

bool yew_ctx_hover_at(u16 x, u16 y)
{
    i32 row;

    if (!ctx.active)
        return false;
    /* Inside the border, or nowhere.  The row rects registered by
     * yew_ctx_draw exclude the border for the same reason. */
    if (x < (u16)(ctx.box.x + 1U) || x >= (u16)(ctx.box.x + ctx.box.w - 1U))
        return false;
    if (y < (u16)(ctx.box.y + 1U) || y >= (u16)(ctx.box.y + ctx.box.h - 1U))
        return false;
    row = (i32)y - (i32)ctx.box.y - 1;
    if (row >= (i32)ctx.n || ctx.rows[row].separator ||
        !ctx.rows[row].enabled)
        return false;
    if (ctx.cursor == row)
        return false;
    ctx.cursor = row;
    return true;
}

void yew_ctx_invoke(i32 row)
{
    if (!ctx.active || row < 0 || row >= (i32)ctx.n)
        return;
    if (ctx.rows[row].separator || !ctx.rows[row].enabled)
        return;
    ctx.chosen = ctx.rows[row].action;
    ctx.active = false;
}

u32 yew_ctx_take(void)
{
    u32 chosen = ctx.chosen;

    /* Cleared on the way out, so one choice is acted on exactly once —
     * a menu whose action survived a second poll would fire twice on
     * the same frame the second time anything asked. */
    ctx.chosen = 0U;
    return chosen;
}

bool yew_ctx_key(const Key *k)
{
    if (!ctx.active || k == NULL)
        return false;
    if (k->ev == (u8)YEW_KEY_RELEASE)
        return true;
    switch (k->code) {
    case YEW_KEY_ESCAPE:
        yew_ctx_close();
        return true;
    case YEW_KEY_UP:
        ctx.cursor = next_usable(ctx.cursor, -1);
        return true;
    case YEW_KEY_DOWN:
        ctx.cursor = next_usable(ctx.cursor, 1);
        return true;
    case YEW_KEY_HOME:
        /* The first usable row: walking down from before the top. */
        ctx.cursor = next_usable(-1, 1);
        return true;
    case YEW_KEY_END:
        /* The last usable row: walking up from past the bottom. */
        ctx.cursor = next_usable((i32)ctx.n, -1);
        return true;
    case YEW_KEY_ENTER:
        yew_ctx_invoke(ctx.cursor);
        return true;
    default:
        break;
    }
    /*
     * Everything else is SWALLOWED.  A menu that let `d` through would
     * delete a line behind an open pop-up — the same law the pickers
     * obey (s24, s26).
     */
    return true;
}

/* ---------------------------------------------------------------- */
/* Drawing                                                          */
/* ---------------------------------------------------------------- */

static Cell blank_of(ThemeEnt style)
{
    Cell blank;

    (void)memset(&blank, 0, sizeof(blank));
    blank.fg = style.fg;
    blank.bg = style.bg;
    blank.attrs = style.attrs;
    return blank;
}

static void put_glyph(Grid *grid, u16 y, u16 x, YewGlyph g, ThemeEnt style)
{
    (void)yew_grid_puts(grid, y, x, (const u8 *)yew_glyph(g),
                        yew_glyph_len(g), style.fg, style.bg, style.attrs);
}

/* A horizontal rule from `left` to `right` (exclusive), one glyph per
 * cell — the H glyph is one cell wide in both vocabularies. */
static void put_rule(Grid *grid, u16 y, u16 left, u16 right, ThemeEnt style)
{
    u16 x;

    for (x = left; x < right; x++)
        put_glyph(grid, y, x, YEW_GLYPH_BORDER_H, style);
}

/*
 * The colourless fallback: attributes only, so a caller that has no
 * theme at all still gets a menu whose highlighted row reads as
 * highlighted and whose greyed row reads as greyed.
 */
static CtxStyle plain_style(void)
{
    CtxStyle s;

    (void)memset(&s, 0, sizeof(s));
    s.hover.attrs = YEW_ATTR_REVERSE;
    s.disabled.attrs = YEW_ATTR_DIM;
    s.accel.attrs = YEW_ATTR_DIM;
    s.sep.attrs = YEW_ATTR_DIM;
    return s;
}

void yew_ctx_draw(Grid *grid, const CtxStyle *style)
{
    CtxStyle fallback;
    u16 left;
    u16 right;
    u16 inner_l;
    u16 inner_r;
    u16 content;
    u32 i;

    if (!ctx.active || grid == NULL)
        return;
    if (style == NULL) {
        fallback = plain_style();
        style = &fallback;
    }
    left = ctx.box.x;
    right = (u16)(ctx.box.x + ctx.box.w);
    inner_l = (u16)(left + 1U);
    inner_r = (u16)(right - 1U);
    content = (u16)(ctx.box.w - 2U * CTX_EDGE);

    /* Top border. */
    yew_grid_fill(grid, ctx.box.y, left, right, blank_of(style->surface));
    put_glyph(grid, ctx.box.y, left, YEW_GLYPH_BORDER_TL, style->surface);
    put_rule(grid, ctx.box.y, inner_l, inner_r, style->surface);
    put_glyph(grid, ctx.box.y, inner_r, YEW_GLYPH_BORDER_TR, style->surface);

    /*
     * YEW_REGION_BLOCK over the whole box, so the document beneath is
     * inert, plus one YEW_REGION_CTX_ROW per drawn row from the SAME
     * cells the row was drawn with (the Sprint 22 law) — the border
     * excluded, so a press on it highlights nothing.  Last-added-wins
     * makes the menu shadow everything under it with no z-order
     * machinery at all.
     */
    yew_region_add(YEW_REGION_BLOCK, ctx.box, 0);
    for (i = 0U; i < ctx.n; i++) {
        const CtxRow *r = &ctx.rows[i];
        u16 y = (u16)(ctx.box.y + 1U + i);
        Rect row_rect = {inner_l, y, (u16)(inner_r - inner_l), 1U};
        ThemeEnt text;
        ThemeEnt accel;
        u16 label_room = content;

        yew_grid_fill(grid, y, left, right, blank_of(style->surface));
        if (r->separator) {
            /* A rule joined to the border on both sides, so it reads as
             * part of the frame and not as a row that says "------". */
            put_glyph(grid, y, left, YEW_GLYPH_BORDER_TEE_R,
                      style->surface);
            put_rule(grid, y, inner_l, inner_r, style->sep);
            put_glyph(grid, y, inner_r, YEW_GLYPH_BORDER_TEE_L,
                      style->surface);
            yew_region_add(YEW_REGION_CTX_ROW, row_rect, (i32)i);
            continue;
        }
        put_glyph(grid, y, left, YEW_GLYPH_BORDER_V, style->surface);
        put_glyph(grid, y, inner_r, YEW_GLYPH_BORDER_V, style->surface);
        if (!r->enabled) {
            /* GREYED, never hidden: the menu keeps the same shape, so a
             * row does not move under the pointer between one
             * right-click and the next. */
            text = style->disabled;
            accel = style->disabled;
        } else if ((i32)i == ctx.cursor) {
            text = style->hover;
            accel = style->hover;
        } else {
            text = style->row;
            accel = style->accel;
        }
        /* The row's own surface, border to border, so the highlight
         * spans the width rather than hugging the label. */
        yew_grid_fill(grid, y, inner_l, inner_r, blank_of(text));
        if (!ctx.accels_hidden && r->accel[0] != '\0') {
            u16 cells = cells_of(r->accel);

            if ((u16)(cells + CTX_GAP) < content) {
                (void)yew_grid_puts(grid, y,
                                    (u16)(inner_r - CTX_PAD - cells),
                                    (const u8 *)r->accel,
                                    strlen(r->accel), accel.fg, accel.bg,
                                    accel.attrs);
                label_room = (u16)(content - cells - CTX_GAP);
            }
        }
        {
            /* Clipped to the room the row has: a label that ran under
             * the accelerator, or through the border, is the width
             * clamp failing silently. */
            int cells = 0;
            size_t bytes = yew_str_clip((const u8 *)r->label,
                                        strlen(r->label), (int)label_room,
                                        &cells);

            (void)yew_grid_puts(grid, y, (u16)(inner_l + CTX_PAD),
                                (const u8 *)r->label, bytes, text.fg,
                                text.bg, text.attrs);
        }
        yew_region_add(YEW_REGION_CTX_ROW, row_rect, (i32)i);
    }

    /* Bottom border. */
    {
        u16 y = (u16)(ctx.box.y + ctx.box.h - 1U);

        yew_grid_fill(grid, y, left, right, blank_of(style->surface));
        put_glyph(grid, y, left, YEW_GLYPH_BORDER_BL, style->surface);
        put_rule(grid, y, inner_l, inner_r, style->surface);
        put_glyph(grid, y, inner_r, YEW_GLYPH_BORDER_BR, style->surface);
    }
}
