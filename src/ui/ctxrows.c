#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.11 §4: one builder per context kind.
 *
 * WHY THIS IS NOT IN ui/mouse.c.  The router is an allocation-free file
 * by gate (tests/perf/mouse.c reads its source), and capturing a menu's
 * target copies a path.  WHY IT IS NOT IN ui/ctxmenu.c: that module is
 * editor-ignorant so the renderer may use it.  This file is the one
 * place allowed to know both — it includes edit/ed.h, it may allocate,
 * and it may not be called from a hit-test.
 *
 * THE TARGET IS CAPTURED HERE, ONCE, at build time: the tab_id, the
 * gid, the interned path, the leaf index.  Rows re-find their target
 * from that identity when they fire, never from the cells under the
 * pointer — the strip scrolls, tabs close, and the
 * tree rebuilds while a menu is up.
 *
 * SPRINT 57.11 DELIVERABLE 4 fills in the per-kind row sets.  Until it
 * does, every kind this file does not yet enumerate opens the one row
 * that can always take the user the rest of the way — the command
 * palette — rather than opening nothing: "right-click does nothing
 * here" is the exact feel this sprint exists to remove, and a surface
 * that silently refuses teaches the gesture is unreliable.
 */

#include "ui/ctxrows.h"

#include "edit/ed.h"
#include "ui/ctxmenu.h"
#include "ui/groups.h"
#include "ui/tabs.h"

/*
 * THE DISPATCH, as data.
 *
 * Sprint 27 spent a switch per menu kind on this, so every kind added
 * a second switch somewhere else and the two drifted.  A row is now a
 * registry command name plus how its captured target is applied plus
 * one integer argument, and ui/mouse.c's `apply_menu_action` is the
 * only reader.  A NULL `cmd` means the row is handled inline by the
 * router (closing an overlay, which no registry command owns).
 */
const CtxActionDesc yew_ctx_actions[CTXA__N] = {
    /* CTXA_NONE            */ {NULL, CTX_TGT_NONE, 0},
    /* CTXA_TAB_CLOSE       */ {"ed.tab.close", CTX_TGT_TAB, 0},
    /* CTXA_TAB_CLOSE_OTHERS*/ {"ed.tab.close_others", CTX_TGT_TAB, 0},
    /* CTXA_TAB_COPY_PATH   */ {"ed.tab.copy_path", CTX_TGT_TAB, 0},
    /* CTXA_TAB_LEAVE_GROUP */ {"ed.group.remove_tab", CTX_TGT_TAB, 0},
    /* CTXA_GROUP_EDIT      */ {"ed.group.edit", CTX_TGT_GROUP, 0},
    /* CTXA_GROUP_RENAME    */ {"ed.group.rename", CTX_TGT_GROUP, 0},
    /* CTXA_GROUP_DISSOLVE  */ {"ed.group.dissolve", CTX_TGT_GROUP, 0},
    /* CTXA_PALETTE         */ {"ed.find.command", CTX_TGT_NONE, 0}
};

/* ---------------------------------------------------------------- */
/* The two kinds Sprint 27 already had                              */
/* ---------------------------------------------------------------- */

static void build_tab(Ed *ed, const CtxContext *c)
{
    int idx = yew_tab_index_of_id(ed, c->id);
    Tab *t = yew_tab_at(ed, idx);

    if (t == NULL)
        return; /* the tab went away between the press and here */
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    /*
     * The target, captured NOW: the tab_id and the canonical path.
     * Every row re-finds the tab from the id when it is invoked,
     * because the strip can scroll and tabs can close while the menu is
     * up — and then the entry at those cells is a different file.
     */
    yew_ctx_target(c->id, t->path);
    yew_ctx_item("Close Tab", "C-w", (u32)CTXA_TAB_CLOSE,
                 yew_tab_count(ed) > 1U, 0U);
    /* Disabled rows are GREYED, never hidden, so the menu keeps its
     * shape and a row does not move under the pointer between one
     * right-click and the next. */
    yew_ctx_item("Close Other Tabs", NULL, (u32)CTXA_TAB_CLOSE_OTHERS,
                 yew_tab_count(ed) > 1U, 0U);
    yew_ctx_sep();
    yew_ctx_item("Copy Path", NULL, (u32)CTXA_TAB_COPY_PATH,
                 t->path != NULL, 0U);
    yew_ctx_item("Remove from Group", NULL, (u32)CTXA_TAB_LEAVE_GROUP,
                 t->group_id != 0U, 0U);
}

static void build_group(Ed *ed, const CtxContext *c)
{
    if (yew_group_at(ed, c->id) == NULL)
        return;
    yew_ctx_begin((u32)YEW_CTX_KIND_GROUP);
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Edit Group...", NULL, (u32)CTXA_GROUP_EDIT, true, 0U);
    yew_ctx_item("Rename Group...", NULL, (u32)CTXA_GROUP_RENAME, true,
                 0U);
    yew_ctx_sep();
    yew_ctx_item("Dissolve Group", NULL, (u32)CTXA_GROUP_DISSOLVE, true,
                 0U);
}

/* ---------------------------------------------------------------- */
/* Deliverable 4's placeholder                                      */
/* ---------------------------------------------------------------- */

/*
 * One enabled row, on every surface that has no row set yet.
 *
 * This is NOT a silent stub (invariant 3): it does exactly what it
 * says, it is reachable by keyboard, and it leaves the user somewhere
 * useful.  What it is not yet is the §4 row list for its kind, and the
 * builder it will be replaced by is named in the comment at the top of
 * this file.
 *
 * The target IDENTITY is still captured, because the kinds that carry
 * one carry it into Deliverable 4 unchanged.  The clicked CELL is the
 * router's to capture, after the build: it is not in a CtxContext,
 * whose `rect` is the region's (the pane's, for DOC).
 */
static void build_placeholder(Ed *ed, const CtxContext *c)
{
    (void)ed;
    yew_ctx_begin((u32)c->kind);
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Command Palette...", NULL, (u32)CTXA_PALETTE, true, 0U);
}

void yew_ctx_build(Ed *ed, const CtxContext *c)
{
    if (ed == NULL || c == NULL)
        return;
    switch (c->kind) {
    case YEW_CTX_KIND_TAB:
        build_tab(ed, c);
        break;
    case YEW_CTX_KIND_GROUP:
        build_group(ed, c);
        break;
    case YEW_CTX_KIND_NONE:
        /* Nothing is under the pointer in a sense that even the
         * fall-through could name; opening a menu would be inventing a
         * surface.  `yew_mouse_context_at` never returns it. */
        break;
    case YEW_CTX_KIND_STRIP:
    case YEW_CTX_KIND_DOC:
    case YEW_CTX_KIND_BORDER:
    case YEW_CTX_KIND_FOOTER:
    case YEW_CTX_KIND_FUSS_FILE:
    case YEW_CTX_KIND_FUSS_DIR:
    case YEW_CTX_KIND_FUSS_BLANK:
    case YEW_CTX_KIND_PICK_ROW:
    case YEW_CTX_KIND_PICKER:
    case YEW_CTX_KIND_COMPL_ROW:
    case YEW_CTX_KIND_PANEL:
    case YEW_CTX_KIND_GP_ROW:
    case YEW_CTX_KIND_GP:
    case YEW_CTX_KIND_EDITOR:
    default:
        build_placeholder(ed, c);
        break;
    }
}
