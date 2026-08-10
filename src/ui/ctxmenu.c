/*
 * Sprint 27 §5.  See ctxmenu.h for the capture-at-open law this file
 * exists to enforce.  Deliberately no edit/ed.h.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/ctxmenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/region.h"
#include "unicode/width.h"
#include "util/base.h"

enum {
    CTX_LABEL_MAX = 48,
    CTX_ACCEL_MAX = 12,
    /* One cell of padding each side, plus a gap before the accel. */
    CTX_PAD = 2
};

typedef struct CtxRow {
    char label[CTX_LABEL_MAX];
    char accel[CTX_ACCEL_MAX];
    u32 action;
    bool enabled;
    bool separator;
} CtxRow;

static struct {
    CtxRow rows[YEW_CTX_MAX_ROWS];
    u32 n;
    u32 kind;
    bool active;
    i32 cursor;
    u32 chosen;
    Rect box;
    /* THE TARGET, captured at open time.  See ctxmenu.h. */
    u32 target_id;
    char *target_path;
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
    free(ctx.target_path);
    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.kind = kind;
    ctx.cursor = -1;
}

void yew_ctx_item(const char *label, const char *accel, u32 action,
                  bool enabled)
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
}

void yew_ctx_sep(void)
{
    CtxRow *r;

    if (ctx.n >= (u32)YEW_CTX_MAX_ROWS)
        return;
    r = &ctx.rows[ctx.n++];
    (void)memset(r, 0, sizeof(*r));
    r->separator = true;
}

void yew_ctx_target(u32 id, const char *path)
{
    free(ctx.target_path);
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

u32 yew_ctx_target_id(void)
{
    return ctx.target_id;
}

const char *yew_ctx_target_path(void)
{
    return ctx.target_path;
}

/* ---------------------------------------------------------------- */
/* Geometry                                                         */
/* ---------------------------------------------------------------- */

static u16 row_cells(const CtxRow *r)
{
    int label = 0;
    int accel = 0;

    if (r->separator)
        return 0U;
    (void)yew_str_clip((const u8 *)r->label, strlen(r->label), 1000,
                       &label);
    if (r->accel[0] != '\0') {
        (void)yew_str_clip((const u8 *)r->accel, strlen(r->accel), 1000,
                           &accel);
        accel += 2; /* the gap between label and accelerator */
    }
    return (u16)(label + accel);
}

static u16 menu_width(void)
{
    u16 widest = 0U;
    u32 i;

    for (i = 0U; i < ctx.n; i++) {
        u16 w = row_cells(&ctx.rows[i]);

        if (w > widest)
            widest = w;
    }
    widest = (u16)(widest + CTX_PAD);
    return widest < (u16)YEW_CTX_MIN_WIDTH ? (u16)YEW_CTX_MIN_WIDTH
                                           : widest;
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
    u16 w = menu_width();
    u16 h = (u16)ctx.n;
    i32 x;
    i32 y;

    if (ctx.n == 0U || allowed.w < w || allowed.h < h)
        return false; /* a menu drawn half off the screen is worse than
                       * none, so it does not open at all */
    /*
     * CLAMP, NEVER FLIP.  Sliding the box back inside `allowed` keeps
     * the row the user aimed at under the pointer; flipping it above
     * the anchor puts a different row there, and the click that follows
     * opens something the user never chose.
     *
     * The anchor is the row BELOW the one clicked, not below the whole
     * bar: inside a group the bar is two rows, and the general rule
     * would leave a row of dead space between the pointer and the thing
     * it is travelling to.
     */
    x = (i32)anchor_x;
    y = (i32)anchor_y;
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
    free(ctx.target_path);
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

void yew_ctx_draw(Grid *grid)
{
    YewColor fg = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    YewColor bg = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    YewColor dim = {YEW_COLOR_RGB, 120U, 120U, 120U};
    Cell blank;
    u32 i;

    if (!ctx.active || grid == NULL)
        return;
    (void)memset(&blank, 0, sizeof(blank));
    /*
     * YEW_REGION_BLOCK over the whole box, so the document beneath is
     * inert, plus one YEW_REGION_CTX_ROW per drawn row from the SAME
     * Rect the row was drawn with (the Sprint 22 law).  Last-added-wins
     * makes the menu shadow everything under it with no z-order
     * machinery at all.
     */
    yew_region_add(YEW_REGION_BLOCK, ctx.box, 0);
    for (i = 0U; i < ctx.n; i++) {
        const CtxRow *r = &ctx.rows[i];
        u16 y = (u16)(ctx.box.y + i);
        Rect row_rect = {ctx.box.x, y, ctx.box.w, 1U};
        u16 attrs = 0U;
        YewColor colour = fg;

        yew_grid_fill(grid, y, ctx.box.x, (u16)(ctx.box.x + ctx.box.w),
                      blank);
        if (r->separator) {
            u16 x;

            for (x = 0U; x < ctx.box.w; x++) {
                (void)yew_grid_puts(grid, y, (u16)(ctx.box.x + x),
                                    (const u8 *)"-", 1U, dim, bg,
                                    YEW_ATTR_DIM);
            }
            yew_region_add(YEW_REGION_CTX_ROW, row_rect, (i32)i);
            continue;
        }
        if (!r->enabled) {
            /* GREYED, never hidden: the menu keeps the same shape, so a
             * row does not move under the pointer between one
             * right-click and the next. */
            colour = dim;
            attrs = YEW_ATTR_DIM;
        } else if ((i32)i == ctx.cursor) {
            attrs = YEW_ATTR_REVERSE;
        }
        (void)yew_grid_puts(grid, y, (u16)(ctx.box.x + 1U),
                            (const u8 *)r->label, strlen(r->label),
                            colour, bg, attrs);
        if (r->accel[0] != '\0') {
            int cells = 0;

            (void)yew_str_clip((const u8 *)r->accel, strlen(r->accel),
                               1000, &cells);
            if ((u16)cells + 1U < ctx.box.w) {
                (void)yew_grid_puts(grid, y,
                                    (u16)(ctx.box.x + ctx.box.w - 1U -
                                          (u16)cells),
                                    (const u8 *)r->accel,
                                    strlen(r->accel), dim, bg,
                                    YEW_ATTR_DIM);
            }
        }
        yew_region_add(YEW_REGION_CTX_ROW, row_rect, (i32)i);
    }
}
