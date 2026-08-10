#ifndef YEW_UI_CTXMENU_H
#define YEW_UI_CTXMENU_H

/*
 * Sprint 27 §5: context menus.
 *
 * This module owns geometry, drawing, hit-testing and key navigation
 * and nothing else.  It deliberately does NOT include edit/ed.h: the
 * caller decides which rows exist, whether each is enabled, and what
 * each one does, and gets an opaque `action` back.  That keeps it low
 * in the dependency graph — the renderer uses it, so it cannot use the
 * renderer — and unit-testable without an editor.
 *
 * (The sprint sketched `yew_ctx_draw(Ed *)` / `yew_ctx_key(Ed *, …)`.
 * The names are the ledger's; the parameters are narrowed to the Grid
 * and the Key, because an `Ed *` that could not be dereferenced would
 * be a promise the signature could not keep.  Nothing else changed.)
 *
 * THE PITFALL, AND THE REASON THIS FILE IS SHAPED THIS WAY: the menu
 * CAPTURES ITS TARGET AT OPEN TIME AND NEVER RE-RESOLVES IT.  The tab
 * menu stores the tab_id and the canonical path; the group menu stores
 * the gid.  Every row action re-finds its target from that identity
 * when it is invoked.
 *
 * It must not read the region payload at invoke time, because the tab
 * strip can scroll under an open menu, tabs can close, and every index
 * renumbers when one does — so the entry at those coordinates may be a
 * different file by the time the row is clicked.  A row handler that
 * reaches for a payload is a bug, and it is one this module makes
 * IMPOSSIBLE rather than merely forbidden: the region table is frozen
 * for the duration of an invocation and yew_region_hit aborts if it is
 * asked anything while it is (see ui/region.h).
 */

#include "term/grid.h"
#include "term/input.h"
#include "ui/layout.h"
#include "util/base.h"

enum {
    /* Beyond this a menu is a list and wants a picker (s26), not a
     * pop-up the eye can take in at a glance. */
    YEW_CTX_MAX_ROWS = 24,
    /* Narrower than this and the labels clip to meaninglessness. */
    YEW_CTX_MIN_WIDTH = 18
};

typedef enum {
    YEW_CTX_KIND_NONE = 0,
    YEW_CTX_KIND_TAB,
    YEW_CTX_KIND_GROUP
} CtxKind;

/* Begins building a menu.  Discards whatever was open — two menus can
 * never both be up. */
void yew_ctx_begin(u32 kind);
void yew_ctx_item(const char *label, const char *accel, u32 action,
                  bool enabled);
void yew_ctx_sep(void);

/*
 * The TARGET, captured now.  `id` is a tab_id or a gid — this module
 * does not know or care which; `path` is copied, because the tab that
 * owns the original can be closed while the menu is up.
 */
void yew_ctx_target(u32 id, const char *path);
u32 yew_ctx_target_id(void);
const char *yew_ctx_target_path(void);

/*
 * Places and opens the menu.  False when the allowed rectangle cannot
 * hold the box, in which case nothing opens — a menu drawn half off the
 * screen is worse than none.
 *
 * PLACEMENT CLAMPS, NEVER FLIPS.  Sliding the box back inside the
 * allowed rectangle keeps the row the user aimed at under the pointer;
 * flipping the menu above the anchor puts a DIFFERENT row there, and
 * the click that follows opens something the user never chose.
 */
bool yew_ctx_show(u16 anchor_x, u16 anchor_y, Rect allowed);

bool yew_ctx_active(void);
void yew_ctx_close(void);
/* The menu's own keymap layer: up/down skip separators and disabled
 * rows, Enter invokes, Esc closes.  True when the key was consumed —
 * which is always, while a menu is open. */
bool yew_ctx_key(const Key *k);
void yew_ctx_draw(Grid *grid);
/*
 * The chosen action, or 0 when nothing has been chosen yet.  Taking it
 * clears it, so one choice is acted on once.
 */
u32 yew_ctx_take(void);

/* Mouse: highlight a row under the pointer, and invoke one. */
void yew_ctx_hover(i32 row);
void yew_ctx_invoke(i32 row);

/* Test seams. */
u32 yew_ctx_kind(void);
Rect yew_ctx_box(void);
i32 yew_ctx_cursor(void);
u32 yew_ctx_rows(void);
bool yew_ctx_row_enabled(u32 row);

#endif
