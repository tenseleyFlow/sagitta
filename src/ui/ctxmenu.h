#ifndef YEW_UI_CTXMENU_H
#define YEW_UI_CTXMENU_H

/*
 * Sprint 27 §5, reshaped by Sprint 57.11 §2: context menus.
 *
 * This module owns geometry, drawing, hit-testing and key navigation
 * and nothing else.  It deliberately does NOT include edit/ed.h: the
 * caller decides which rows exist, whether each is enabled, what each
 * one does and what it looks like, and gets an opaque `action` back.
 * That keeps it low in the dependency graph — the renderer uses it, so
 * it cannot use the renderer — and unit-testable without an editor.
 *
 * (The sprint sketched `yew_ctx_draw(Ed *)` / `yew_ctx_key(Ed *, …)`.
 * The names are the ledger's; the parameters are narrowed to the Grid
 * and the Key, because an `Ed *` that could not be dereferenced would
 * be a promise the signature could not keep.  Nothing else changed.)
 *
 * THE PITFALL, AND THE REASON THIS FILE IS SHAPED THIS WAY: the menu
 * CAPTURES ITS TARGET AT OPEN TIME AND NEVER RE-RESOLVES IT.  The tab
 * menu stores the tab_id and the canonical path; the group menu stores
 * the gid; the document menu stores the pane cell.  Every row action
 * re-finds its target from that identity when it is invoked.
 *
 * It must not read the region payload at invoke time, because the tab
 * strip can scroll under an open menu, tabs can close, and every index
 * renumbers when one does — so the entry at those coordinates may be a
 * different file by the time the row is clicked.  A row handler that
 * reaches for a payload is a bug, and it is one this module makes
 * IMPOSSIBLE rather than merely forbidden: the region table is frozen
 * for the duration of an invocation and yew_region_hit aborts if it is
 * asked anything while it is (see ui/region.h).
 *
 * THE BOX (57.11).  Facsimile-style: a one-cell border on every side,
 * no title.  `box.h = rows + 2`, `box.w = widest + 2·pad + 2`.  The
 * box's top-left corner is the cell BELOW-RIGHT of the anchor, so the
 * cell the pointer is on when the menu opens is a border cell, never a
 * row — a release on the very cell that opened the menu activates
 * nothing.
 *
 * SHEDDING (57.11).  Every row carries a priority, 0 = never shed …
 * 3 = shed first.  When the allowed rectangle cannot hold the box the
 * priority-3 rows go, then 2, then 1, a whole level at a time, and
 * the menu refuses only when the priority-0 rows alone do not fit.
 * A separator inherits the priority of the row above it and is dropped
 * whenever it would lead, trail or double up.  Same input → same rows
 * (invariant 5).
 */

#include "syn/theme.h"
#include "term/grid.h"
#include "term/input.h"
#include "ui/ctxrows.h"
#include "ui/layout.h"
#include "util/base.h"

enum {
    /* Beyond this a menu is a list and wants a picker (s26), not a
     * pop-up the eye can take in at a glance. */
    YEW_CTX_MAX_ROWS = 32,
    /* Narrower than this and the labels clip to meaninglessness — the
     * box is never made narrower than this by its OWN contents; a
     * narrower allowed rectangle still clamps it (57.11). */
    YEW_CTX_MIN_WIDTH = 18,
    /* Highest priority a row may carry: shed first. */
    YEW_CTX_PRIORITY_MAX = 3
};

/*
 * `CtxKind`, `CtxContext` and the action table live in ui/ctxrows.h —
 * WHICH rows a surface deserves is editor policy, and this module knows
 * none.  It is included rather than forward-declared only because the
 * kind is this module's `yew_ctx_begin` argument; nothing here reads a
 * kind's meaning, and ctxrows.h is as editor-ignorant as this file is.
 */

/*
 * The menu's look, resolved by the CALLER from the theme's `menu.*`
 * roles (Sprint 57.11 §6) — this module knows no colours of its own.
 * `surface` paints the box background and its border; `row` an enabled
 * row; `hover` the highlighted row; `disabled` a greyed row; `accel`
 * the accelerator column; `sep` a separator rule.
 */
typedef struct CtxStyle {
    ThemeEnt surface;
    ThemeEnt row;
    ThemeEnt hover;
    ThemeEnt disabled;
    ThemeEnt accel;
    ThemeEnt sep;
} CtxStyle;

/* Begins building a menu.  Discards whatever was open — two menus can
 * never both be up. */
void yew_ctx_begin(u32 kind);
/* `priority` is clamped to YEW_CTX_PRIORITY_MAX; 0 is never shed. */
void yew_ctx_item(const char *label, const char *accel, u32 action,
                  bool enabled, u8 priority);
void yew_ctx_sep(void);

/*
 * The TARGET, captured now.  `id` is a tab_id or a gid — this module
 * does not know or care which; `path` is copied, because the tab that
 * owns the original can be closed while the menu is up.  `rect` is the
 * pane-relative cell for document menus (which leaf, which cell), so
 * the cursor can be placed where the click was when a row fires.
 */
void yew_ctx_target(u32 id, const char *path);
void yew_ctx_target_rect(Rect rect);
u32 yew_ctx_target_id(void);
const char *yew_ctx_target_path(void);
Rect yew_ctx_target_rect_get(void);

/*
 * Places and opens the menu.  False when the allowed rectangle cannot
 * hold even the priority-0 rows, in which case nothing opens — a menu
 * drawn half off the screen is worse than none.  Otherwise the lowest
 * priorities are shed until the box fits, the width is clamped to the
 * allowed rectangle (accelerators go first, then labels are clipped).
 *
 * PLACEMENT CLAMPS, NEVER FLIPS.  Sliding the box back inside the
 * allowed rectangle keeps the row the user aimed at under the pointer;
 * flipping the menu above the anchor puts a DIFFERENT row there, and
 * the click that follows opens something the user never chose.
 */
bool yew_ctx_show(u16 anchor_x, u16 anchor_y, Rect allowed);

bool yew_ctx_active(void);
void yew_ctx_close(void);
/* The menu's own keymap layer: up/down/home/end skip separators and
 * disabled rows, Enter invokes, Esc closes.  True when the key was
 * consumed — which is always, while a menu is open. */
bool yew_ctx_key(const Key *k);
/* Draws the box and registers its regions: YEW_REGION_BLOCK over the
 * whole box (border included), then one YEW_REGION_CTX_ROW per drawn
 * row — the row rects exclude the border.  A NULL style draws with
 * attributes only (reverse hover, dim disabled) and no colour. */
void yew_ctx_draw(Grid *grid, const CtxStyle *style);
/*
 * The chosen action, or 0 when nothing has been chosen yet.  Taking it
 * clears it, so one choice is acted on once.
 */
u32 yew_ctx_take(void);

/* Mouse: highlight a row under the pointer, and invoke one. */
void yew_ctx_hover(i32 row);
/*
 * Maps a screen cell to a row BY THE BOX GEOMETRY — no region lookup,
 * because the table may be mid-frame — and highlights it.  Border
 * cells, separators, disabled rows and cells outside the box map to
 * nothing.  True only when the highlight MOVED, so the router repaints
 * on change and never on every motion report.
 */
bool yew_ctx_hover_at(u16 x, u16 y);
void yew_ctx_invoke(i32 row);

/* Test seams. */
u32 yew_ctx_kind(void);
Rect yew_ctx_box(void);
i32 yew_ctx_cursor(void);
/* Rows the menu DRAWS — after shedding, once shown. */
u32 yew_ctx_rows(void);
bool yew_ctx_row_enabled(u32 row);
/* Rows (items and separators) shed at the last show. */
u32 yew_ctx_shed_count(void);
u8 yew_ctx_priority(u32 row);
/* True when the width clamp dropped the accelerator column. */
bool yew_ctx_accels_hidden(void);

#endif
