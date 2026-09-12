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
 * pointer — the strip scrolls, tabs close, and the tree rebuilds while
 * a menu is up.
 *
 * TWO LAWS GOVERN WHICH ROWS EXIST, and they are not the same law:
 *
 *   A ROW is GREYED, NEVER HIDDEN (Sprint 27).  `Stage` with nothing to
 *   stage stays where it was, greyed.  A menu whose rows move between
 *   two right-clicks on the same thing is a menu the hand cannot learn.
 *
 *   A SECTION is OMITTED WHOLE when its FEATURE is not available in
 *   this build or for this buffer (57.11 §4): no LSP server attached,
 *   no git repository, FUSS compiled out.  Four greyed rows naming a
 *   language server that will never exist teach nothing; they are just
 *   furniture.  Shape stability then holds per (kind, availability),
 *   which is the strongest promise an optional feature can keep.
 *
 * ELLIPSES ARE THREE ASCII DOTS, not U+2026.  The sprint writes `…` as
 * typography; Sprint 27's shipped rows spell it `...` and the ASCII
 * rendition (`degrade` / the `_ascii` PTY lane) has to survive without
 * a fallback table for one glyph that appears in a dozen labels.
 */

#include "ui/ctxrows.h"

#include <stdio.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "mod/git/fussmode.h"
#include "mod/git/git.h"
#include "mod/lsp/lsp.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/ctxmenu.h"
#include "ui/groups.h"
#include "ui/gutter.h"
#include "ui/layout.h"
#include "ui/picker.h"
#include "ui/tabs.h"
#include "ui/win.h"
#include "util/intern.h"

/*
 * THE DISPATCH, as data.
 *
 * Sprint 27 spent a switch per menu kind on this, so every kind added
 * a second switch somewhere else and the two drifted.  A row is now a
 * registry command name plus how its captured target is applied plus
 * one integer argument, and ui/mouse.c's `apply_menu_action` is the
 * only reader.  A NULL `cmd` means the row is handled inline by the
 * router (closing an overlay, which no registry command owns) or by the
 * target itself (a picker row, where accepting IS the action).
 */
const CtxActionDesc yew_ctx_actions[CTXA__N] = {
    /* CTXA_NONE             */ {NULL, CTX_TGT_NONE, 0},
    /* CTXA_TAB_CLOSE        */ {"ed.tab.close", CTX_TGT_TAB, 0},
    /* CTXA_TAB_CLOSE_OTHERS */ {"ed.tab.close_others", CTX_TGT_TAB, 0},
    /* CTXA_TAB_COPY_PATH    */ {"ed.tab.copy_path", CTX_TGT_TAB, 0},
    /* CTXA_TAB_LEAVE_GROUP  */ {"ed.group.remove_tab", CTX_TGT_TAB, 0},
    /* CTXA_GROUP_EDIT       */ {"ed.group.edit", CTX_TGT_GROUP, 0},
    /* CTXA_GROUP_RENAME     */ {"ed.group.rename", CTX_TGT_GROUP, 0},
    /* CTXA_GROUP_DISSOLVE   */ {"ed.group.dissolve", CTX_TGT_GROUP, 0},
    /* CTXA_PALETTE          */ {"ed.find.command", CTX_TGT_NONE, 0},
    /* CTXA_PALETTE_HERE     */ {"ed.find.command", CTX_TGT_PANE, 0},
    /* CTXA_FUSS_OPEN        */ {"ed.git.open", CTX_TGT_PATH, 0},

    /*
     * The document rows.  CTX_TGT_LEAF for the three that consume the
     * selection, CTX_TGT_PANE for everything that means "here" — see
     * ctxrows.h for why the caret may not move under the first three.
     */
    /* CTXA_DOC_CUT          */ {"ed.sel.cut", CTX_TGT_LEAF, 0},
    /* CTXA_DOC_COPY         */ {"ed.sel.yank", CTX_TGT_LEAF, 0},
    /* CTXA_DOC_PASTE        */ {"ed.edit.paste", CTX_TGT_PANE, 0},
    /* CTXA_DOC_DELETE       */ {"ed.sel.delete", CTX_TGT_LEAF, 0},
    /* CTXA_DOC_SELECT_ALL   */ {"ed.sel.all", CTX_TGT_LEAF, 0},
    /* CTXA_DOC_UNDO         */ {"ed.edit.undo", CTX_TGT_LEAF, 0},
    /* CTXA_DOC_REDO         */ {"ed.edit.redo", CTX_TGT_LEAF, 0},
    /* CTXA_DOC_SPLIT_RIGHT  */ {"ed.pane.split_h", CTX_TGT_PANE, 0},
    /* CTXA_DOC_SPLIT_BELOW  */ {"ed.pane.split_v", CTX_TGT_PANE, 0},
    /* CTXA_DOC_CLOSE_PANE   */ {"ed.pane.close", CTX_TGT_PANE, 0},
    /* CTXA_DOC_SAVE         */ {"ed.file.save", CTX_TGT_PANE, 0},
    /* CTXA_DOC_RELOAD       */ {"ed.file.reload", CTX_TGT_PANE, 0},
    /* CTXA_DOC_LSP_DEF      */ {"ed.lsp.goto_def", CTX_TGT_PANE, 0},
    /* CTXA_DOC_LSP_REFS     */ {"ed.lsp.references", CTX_TGT_PANE, 0},
    /* CTXA_DOC_LSP_RENAME   */ {"ed.lsp.rename", CTX_TGT_PANE, 0},
    /* CTXA_DOC_LSP_HOVER    */ {"ed.lsp.hover", CTX_TGT_PANE, 0},
    /* CTXA_DOC_BLAME_TOGGLE */ {"ed.git.blame.toggle", CTX_TGT_PANE, 0},
    /*
     * `ed.git.diff.view`, not `ed.git.diff`: the document's Diff is the
     * side-by-side view of THIS buffer against its index blob, while
     * `ed.git.diff` is the FUSS path verb that spawns `git diff` for a
     * selected tree row.
     */
    /* CTXA_DOC_DIFF         */ {"ed.git.diff.view", CTX_TGT_PANE, 0},
    /* CTXA_DOC_FIND_FILE    */ {"ed.find.file", CTX_TGT_PANE, 0},
    /* CTXA_DOC_GOTO_LINE    */ {"ed.view.goto_line", CTX_TGT_PANE, 0},
    /* CTXA_DOC_TOGGLE_WRAP  */ {"ed.view.toggle_wrap", CTX_TGT_PANE, 0},

    /* CTXA_TAB_NEW          */ {"ed.tab.new", CTX_TGT_NONE, 0},
    /* CTXA_TAB_OPEN_SPLIT_RIGHT */
    {"ed.tab.open_split_h", CTX_TGT_TAB, 0},
    /* CTXA_TAB_OPEN_SPLIT_BELOW */
    {"ed.tab.open_split_v", CTX_TGT_TAB, 0},
    /* CTXA_GROUP_CLOSE      */ {"ed.group.close", CTX_TGT_GROUP, 0},
    /* CTXA_GROUP_NEW        */ {"ed.group.new", CTX_TGT_NONE, 0},
    /* CTXA_FIND_FILE        */ {"ed.find.file", CTX_TGT_NONE, 0},
    /* CTXA_FIND_BUFFER      */ {"ed.find.buffer", CTX_TGT_NONE, 0},

    /*
     * The border rows act on the FOCUSED pane, not on a captured one: a
     * border's region payload names the SPLIT it can drag, not a leaf,
     * and inventing a leaf from it would grow and close panes the user
     * did not point at.  Resizing by pointing is the drag gesture;
     * these rows are the keyboard's four, reachable from the pointer.
     */
    /* CTXA_PANE_CLOSE       */ {"ed.pane.close", CTX_TGT_NONE, 0},
    /* CTXA_PANE_GROW        */ {"ed.pane.grow", CTX_TGT_NONE, 0},
    /* CTXA_PANE_SHRINK      */ {"ed.pane.shrink", CTX_TGT_NONE, 0},
    /* CTXA_PANE_FOCUS_NEXT  */ {"ed.pane.focus_next", CTX_TGT_NONE, 0},

    /* CTXA_GOTO_LINE        */ {"ed.view.goto_line", CTX_TGT_NONE, 0},
    /* CTXA_TOGGLE_WRAP      */ {"ed.view.toggle_wrap", CTX_TGT_NONE, 0},
    /* CTXA_NUMBER_CYCLE     */ {"ed.view.number_cycle", CTX_TGT_NONE, 0},
    /* CTXA_MOUSE_DISABLE    */ {"ed.mouse.disable", CTX_TGT_NONE, 0},

    /* CTXA_FUSS_OPEN_SPLIT_RIGHT */
    {"ed.git.open_split_h", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_OPEN_SPLIT_BELOW */
    {"ed.git.open_split_v", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_PREVIEW     */ {"ed.git.view", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_STAGE       */ {"ed.git.stage", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_UNSTAGE     */ {"ed.git.unstage", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_DISCARD     */ {"ed.git.discard", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_DIFF        */ {"ed.git.diff", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_BLAME       */ {"ed.git.blame", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_RENAME      */ {"ed.git.file.rename", CTX_TGT_PATH, 0},
    /* CTXA_FUSS_DELETE      */ {"ed.git.file.delete", CTX_TGT_PATH, 0},
    /*
     * Expand/Collapse reads the SELECTED row, and `ed.git.nav.toggle`
     * takes NO argument — a command invoked with an `sarg` its arity
     * does not accept is refused by `yew_cmd_prepare` before it runs,
     * which is a row that silently does nothing.  CTX_TGT_FUSS_ROW is
     * the half of CTX_TGT_PATH this row needs: move the selection to
     * the clicked row, pass nothing.
     */
    /* CTXA_FUSS_TOGGLE      */ {"ed.git.nav.toggle", CTX_TGT_FUSS_ROW, 0},
    /* CTXA_FUSS_GROUP_FROM_DIR */
    {"ed.group.from_dir", CTX_TGT_FUSS_ROW, 0},
    /* CTXA_FUSS_REFRESH     */ {"ed.git.refresh", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_COMMIT      */ {"ed.git.commit", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_PUSH        */ {"ed.git.push", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_PULL        */ {"ed.git.pull", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_FETCH       */ {"ed.git.fetch", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_STATUS      */ {"ed.git.status", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_HISTORY     */ {"ed.git.history", CTX_TGT_NONE, 0},
    /* CTXA_FUSS_LEAVE       */ {"ed.git.mode.leave", CTX_TGT_NONE, 0},

    /*
     * A picker row's command IS its accept, and `iarg` is the accept
     * MODE — the same three the keyboard has.  HSPLIT is the side-by-
     * side one (YEW_SPLIT_H) and VSPLIT the stacked one, which is why
     * `Open in Split Right` carries 2 and `Open in Split Below` 1.
     */
    /* CTXA_PICK_OPEN        */
    {NULL, CTX_TGT_PICK, (i64)YEW_PICK_ACCEPT_HERE},
    /* CTXA_PICK_SPLIT_RIGHT */
    {NULL, CTX_TGT_PICK, (i64)YEW_PICK_ACCEPT_HSPLIT},
    /* CTXA_PICK_SPLIT_BELOW */
    {NULL, CTX_TGT_PICK, (i64)YEW_PICK_ACCEPT_VSPLIT},
    /* CTXA_COMPL_ACCEPT     */ {"ed.compl.accept", CTX_TGT_COMPL, 0},
    /* CTXA_COMPL_DOCS       */ {"ed.compl.doc_toggle", CTX_TGT_NONE, 0},
    /* CTXA_COMPL_CANCEL     */ {"ed.compl.cancel", CTX_TGT_NONE, 0},
    /* CTXA_OVERLAY_CLOSE    */ {NULL, CTX_TGT_NONE, 0}
};

/* ---------------------------------------------------------------- */
/* What the rows turn on                                            */
/* ---------------------------------------------------------------- */

/* The window a document menu is about: the leaf that was POINTED AT,
 * which is not necessarily the focused one. */
static Win *doc_win(Ed *ed, const CtxContext *c)
{
    Pane *leaf = yew_pane_leaf_by_index(ed, (i32)c->id);

    return leaf == NULL ? NULL : leaf->win;
}

static Buffer *doc_buf(Win *w)
{
    return w == NULL ? NULL : w->buf;
}

/*
 * Is there something to cut?
 *
 * H mode counts even with pos == anchor, because H over a single
 * grapheme IS a selection of one — the same rule `ed.sel.*` applies
 * when it runs.
 */
static bool has_selection(const Ed *ed, const Win *w)
{
    const Cursor *c;

    if (w == NULL || w->cs.curs.len == 0U)
        return false;
    if (ed->mode == YEW_MODE_H)
        return true;
    c = &w->cs.curs.data[w->cs.primary];
    return c->pos.v != c->anchor.v;
}

/* Register `"` is what `ed.edit.paste` pastes; empty means the row has
 * nothing to insert. */
static bool register_has_text(Ed *ed)
{
    const RegVal *v = yew_reg_get(&ed->regs, (u8)'"');

    return v != NULL && v->bytes.len != 0U;
}

static bool can_undo(const Buffer *b)
{
    return b != NULL && b->undo != NULL &&
           yew_undo_current(b->undo) != b->undo->root;
}

/* Redo is available when the node the buffer sits on has a child to
 * redo INTO — which is also true after an undo on a branch, and that is
 * the case the row exists for. */
static bool can_redo(const Buffer *b)
{
    if (b == NULL || b->undo == NULL)
        return false;
    return yew_undo_children(b->undo, yew_undo_current(b->undo), NULL,
                             0U) != 0U;
}

static bool more_than_one_leaf(const Ed *ed)
{
    return yew_pane_leaf_count(ed->pane_root) > 1U;
}

/*
 * Is this buffer inside the repository FUSS found?
 *
 * The git SECTION of the document menu hangs on this, so it has to be
 * answerable without spawning anything: `yew_git_repo_cached` is the
 * already-detected answer and is NULL in a build with no git module at
 * all, which is what makes the section disappear rather than grey.
 */
#if YEW_WITH_FUSS
static bool buf_in_repo(const Ed *ed, const Buffer *b)
{
    const GitRepo *repo = yew_git_repo_cached(ed);
    const char *path;
    size_t n;

    if (repo == NULL || !repo->inside_work_tree ||
        repo->top_level == NULL || b == NULL)
        return false;
    path = b->meta.realpath != NULL ? b->meta.realpath : b->path;
    if (path == NULL)
        return false;
    n = strlen(repo->top_level);
    if (n == 0U || strncmp(path, repo->top_level, n) != 0)
        return false;
    /* A prefix match is not containment: `/repo-2/x` starts with
     * `/repo`. */
    return path[n] == '/' || (n > 0U && repo->top_level[n - 1U] == '/');
}
#endif

/* ---------------------------------------------------------------- */
/* The document                                                     */
/* ---------------------------------------------------------------- */

static void build_doc(Ed *ed, const CtxContext *c)
{
    Win *w = doc_win(ed, c);
    Buffer *b = doc_buf(w);
    bool sel = has_selection(ed, w);
    bool ro = yew_buf_readonly(b);

    yew_ctx_begin((u32)YEW_CTX_KIND_DOC);
    /* The leaf index is the target; the CELL is the router's to capture
     * after the build, because a CtxContext carries the pane's
     * rectangle rather than the click. */
    yew_ctx_target(c->id, NULL);

    yew_ctx_item("Cut", NULL, (u32)CTXA_DOC_CUT, sel && !ro, 0U);
    yew_ctx_item("Copy", NULL, (u32)CTXA_DOC_COPY, sel, 0U);
    yew_ctx_item("Paste", NULL, (u32)CTXA_DOC_PASTE,
                 !ro && register_has_text(ed), 0U);
    yew_ctx_item("Delete", NULL, (u32)CTXA_DOC_DELETE, sel && !ro, 1U);
    yew_ctx_item("Select All", NULL, (u32)CTXA_DOC_SELECT_ALL, true, 1U);
    yew_ctx_sep();
    yew_ctx_item("Undo", NULL, (u32)CTXA_DOC_UNDO, can_undo(b), 1U);
    yew_ctx_item("Redo", NULL, (u32)CTXA_DOC_REDO, can_redo(b), 1U);
    yew_ctx_sep();
    yew_ctx_item("Split Right", NULL, (u32)CTXA_DOC_SPLIT_RIGHT, true, 2U);
    yew_ctx_item("Split Below", NULL, (u32)CTXA_DOC_SPLIT_BELOW, true, 2U);
    yew_ctx_item("Close Pane", NULL, (u32)CTXA_DOC_CLOSE_PANE,
                 more_than_one_leaf(ed), 2U);
    yew_ctx_sep();
    yew_ctx_item("Save", NULL, (u32)CTXA_DOC_SAVE, yew_buf_dirty(b), 1U);
    /*
     * §4's `Save As...` is NOT here.  Every other row on this menu is a
     * registry command with a keyboard route (DoD 3), and nothing in
     * the registry opens a "write to which path?" prompt: `ed.file.write`
     * takes the path as an argument the E-mode line supplies, and a
     * menu row has no way to ask for one.  A row that saved to the path
     * the file already has would be `Save` wearing another label, which
     * is worse than an absent row.  It lands with the command.
     */
    yew_ctx_item("Reload", NULL, (u32)CTXA_DOC_RELOAD,
                 b != NULL && b->path != NULL, 3U);

#if YEW_WITH_LSP
    /* THE SECTION, or nothing: no server attached to THIS buffer means
     * these four rows are furniture (§4's section-omission rule). */
    if (yew_lsp_attached(ed, b)) {
        yew_ctx_sep();
        yew_ctx_item("Go to Definition", NULL, (u32)CTXA_DOC_LSP_DEF,
                     true, 2U);
        yew_ctx_item("Find References...", NULL, (u32)CTXA_DOC_LSP_REFS,
                     true, 2U);
        yew_ctx_item("Rename...", NULL, (u32)CTXA_DOC_LSP_RENAME, !ro,
                     2U);
        yew_ctx_item("Hover", NULL, (u32)CTXA_DOC_LSP_HOVER, true, 3U);
    }
#endif
#if YEW_WITH_FUSS
    if (buf_in_repo(ed, b)) {
        yew_ctx_sep();
        yew_ctx_item("Toggle Blame", NULL, (u32)CTXA_DOC_BLAME_TOGGLE,
                     true, 3U);
        yew_ctx_item("Diff", NULL, (u32)CTXA_DOC_DIFF, true, 3U);
    }
#endif

    yew_ctx_sep();
    yew_ctx_item("Command Palette...", NULL, (u32)CTXA_PALETTE_HERE, true,
                 0U);
    yew_ctx_item("Find File...", NULL, (u32)CTXA_DOC_FIND_FILE, true, 1U);
    yew_ctx_item("Go to Line...", NULL, (u32)CTXA_DOC_GOTO_LINE, true, 2U);
    yew_ctx_item("Toggle Wrap", NULL, (u32)CTXA_DOC_TOGGLE_WRAP, true, 3U);
}

/* ---------------------------------------------------------------- */
/* The tab strip                                                    */
/* ---------------------------------------------------------------- */

static void build_tab(Ed *ed, const CtxContext *c)
{
    int idx = yew_tab_index_of_id(ed, c->id);
    Tab *t = yew_tab_at(ed, idx);
    bool many = yew_tab_count(ed) > 1U;

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
    yew_ctx_item("Close Tab", "C-w", (u32)CTXA_TAB_CLOSE, many, 0U);
    /* Disabled rows are GREYED, never hidden, so the menu keeps its
     * shape and a row does not move under the pointer between one
     * right-click and the next. */
    yew_ctx_item("Close Other Tabs", NULL, (u32)CTXA_TAB_CLOSE_OTHERS,
                 many, 1U);
    yew_ctx_sep();
    yew_ctx_item("Copy Path", NULL, (u32)CTXA_TAB_COPY_PATH,
                 t->path != NULL, 1U);
    yew_ctx_item("Remove from Group", NULL, (u32)CTXA_TAB_LEAVE_GROUP,
                 t->group_id != 0U, 2U);
    yew_ctx_sep();
    yew_ctx_item("New Tab", NULL, (u32)CTXA_TAB_NEW, true, 2U);
    yew_ctx_item("Open in Split Right", NULL,
                 (u32)CTXA_TAB_OPEN_SPLIT_RIGHT, true, 3U);
    yew_ctx_item("Open in Split Below", NULL,
                 (u32)CTXA_TAB_OPEN_SPLIT_BELOW, true, 3U);
}

static void build_group(Ed *ed, const CtxContext *c)
{
    if (yew_group_at(ed, c->id) == NULL)
        return;
    yew_ctx_begin((u32)YEW_CTX_KIND_GROUP);
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Edit Group...", NULL, (u32)CTXA_GROUP_EDIT, true, 0U);
    yew_ctx_item("Rename Group...", NULL, (u32)CTXA_GROUP_RENAME, true,
                 1U);
    yew_ctx_sep();
    yew_ctx_item("Close Group", NULL, (u32)CTXA_GROUP_CLOSE, true, 1U);
    yew_ctx_item("Dissolve Group", NULL, (u32)CTXA_GROUP_DISSOLVE, true,
                 1U);
}

/*
 * The strip's blank tail, its chevrons, its `+` — and the bare editor
 * backdrop, which gets the same rows because the two mean the same
 * thing: the pointer is on the workspace rather than on anything in it.
 */
static void build_strip(Ed *ed, const CtxContext *c)
{
    (void)ed;
    yew_ctx_begin((u32)c->kind);
    yew_ctx_target(0U, NULL);
    yew_ctx_item("New Tab", NULL, (u32)CTXA_TAB_NEW, true, 0U);
    /* `Open File...` is the file picker: it is the only route to a file
     * that asks for the name, and `ed.file.open` takes a path it has no
     * way to prompt for. */
    yew_ctx_item("Open File...", NULL, (u32)CTXA_FIND_FILE, true, 0U);
    yew_ctx_item("New Group...", NULL, (u32)CTXA_GROUP_NEW, true, 1U);
    yew_ctx_sep();
    yew_ctx_item("Command Palette...", NULL, (u32)CTXA_PALETTE, true, 0U);
    yew_ctx_item("Find Buffer...", NULL, (u32)CTXA_FIND_BUFFER, true, 2U);
}

/* ---------------------------------------------------------------- */
/* The pane border and the footer                                   */
/* ---------------------------------------------------------------- */

static void build_border(Ed *ed, const CtxContext *c)
{
    yew_ctx_begin((u32)YEW_CTX_KIND_BORDER);
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Close Pane", NULL, (u32)CTXA_PANE_CLOSE,
                 more_than_one_leaf(ed), 0U);
    yew_ctx_item("Grow", NULL, (u32)CTXA_PANE_GROW, more_than_one_leaf(ed),
                 1U);
    yew_ctx_item("Shrink", NULL, (u32)CTXA_PANE_SHRINK,
                 more_than_one_leaf(ed), 1U);
    yew_ctx_sep();
    yew_ctx_item("Focus Next", NULL, (u32)CTXA_PANE_FOCUS_NEXT,
                 more_than_one_leaf(ed), 2U);
}

static const char *number_style_name(const Ed *ed)
{
    NumStyle style = ed->win == NULL ? YEW_NUM_NONE : ed->win->number_style;

    switch (style) {
    case YEW_NUM_ABS:
        return "abs";
    case YEW_NUM_REL:
        return "rel";
    case YEW_NUM_HYBRID:
        return "hybrid";
    case YEW_NUM_NONE:
    default:
        return "none";
    }
}

static void build_footer(Ed *ed, const CtxContext *c)
{
    char numbers[32];

    (void)c;
    /* The label NAMES THE CURRENT STATE rather than the next one: a row
     * that reads `Line Numbers: rel` while the gutter shows absolute
     * numbers is a row nobody can predict the effect of. */
    (void)snprintf(numbers, sizeof(numbers), "Line Numbers: %s",
                   number_style_name(ed));
    yew_ctx_begin((u32)YEW_CTX_KIND_FOOTER);
    yew_ctx_target(0U, NULL);
    yew_ctx_item("Command Palette...", NULL, (u32)CTXA_PALETTE, true, 0U);
    yew_ctx_item("Go to Line...", NULL, (u32)CTXA_GOTO_LINE, true, 1U);
    yew_ctx_sep();
    yew_ctx_item("Toggle Wrap", NULL, (u32)CTXA_TOGGLE_WRAP, true, 2U);
    yew_ctx_item(numbers, NULL, (u32)CTXA_NUMBER_CYCLE, true, 2U);
    yew_ctx_sep();
    yew_ctx_item("Disable Mouse", NULL, (u32)CTXA_MOUSE_DISABLE, true, 3U);
}

/* ---------------------------------------------------------------- */
/* FUSS                                                             */
/* ---------------------------------------------------------------- */

/*
 * A FUSS row's path, COPIED into the menu: the tree that owns the
 * original is rebuilt by every status result, and a menu can outlive
 * several.
 */
static const char *fuss_path(Ed *ed, const CtxContext *c)
{
    return yew_intern_str(&ed->interner, c->id);
}

static void build_fuss_file(Ed *ed, const CtxContext *c)
{
    const char *path = fuss_path(ed, c);
    FussTarget t;
    bool known = yew_fuss_path_target(ed, c->id, &t);
    bool have = path != NULL;
    /* The same three questions `fuss_target_guard` asks, asked here so
     * a greyed row and a refused command can never disagree. */
    bool stageable = known && (t.unstaged || t.untracked);
    bool unstageable = known && t.staged;
    bool discardable =
        known && (t.staged || t.unstaged) && !t.untracked && !t.conflicted;

    yew_ctx_begin((u32)YEW_CTX_KIND_FUSS_FILE);
    yew_ctx_target(c->id, path);
    yew_ctx_item("Open", NULL, (u32)CTXA_FUSS_OPEN, have, 0U);
    yew_ctx_item("Open in Split Right", NULL,
                 (u32)CTXA_FUSS_OPEN_SPLIT_RIGHT, have, 1U);
    yew_ctx_item("Open in Split Below", NULL,
                 (u32)CTXA_FUSS_OPEN_SPLIT_BELOW, have, 1U);
    yew_ctx_item("Preview", NULL, (u32)CTXA_FUSS_PREVIEW, have, 2U);
    yew_ctx_sep();
    yew_ctx_item("Stage", NULL, (u32)CTXA_FUSS_STAGE, have && stageable,
                 0U);
    yew_ctx_item("Unstage", NULL, (u32)CTXA_FUSS_UNSTAGE,
                 have && unstageable, 0U);
    yew_ctx_item("Discard...", NULL, (u32)CTXA_FUSS_DISCARD,
                 have && discardable, 1U);
    yew_ctx_sep();
    yew_ctx_item("Diff", NULL, (u32)CTXA_FUSS_DIFF, have, 2U);
    yew_ctx_item("Blame", NULL, (u32)CTXA_FUSS_BLAME, have, 2U);
    yew_ctx_sep();
    yew_ctx_item("Rename...", NULL, (u32)CTXA_FUSS_RENAME, have, 3U);
    yew_ctx_item("Delete...", NULL, (u32)CTXA_FUSS_DELETE, have, 3U);
    /*
     * §4's `Copy Path` is NOT here, and neither is the directory
     * menu's.  `ed.tab.copy_path` copies the ACTIVE TAB's path — it
     * takes no argument — and nothing in the registry puts an arbitrary
     * path in register `+`.  A row that copied a different file's name
     * than the one it was opened over is the one failure a clipboard
     * row must never have, so it waits for its command.
     */
}

static void build_fuss_dir(Ed *ed, const CtxContext *c)
{
    const char *path = fuss_path(ed, c);
    FussTarget t;
    bool known = yew_fuss_path_target(ed, c->id, &t);
    bool have = path != NULL;

    yew_ctx_begin((u32)YEW_CTX_KIND_FUSS_DIR);
    yew_ctx_target(c->id, path);
    yew_ctx_item("Open as Group...", NULL, (u32)CTXA_FUSS_GROUP_FROM_DIR,
                 have, 0U);
    /*
     * ONE ROW, TWO LABELS, by state — not two rows of which one is
     * always greyed.  The row is the toggle the tree already has on
     * Enter, and naming it after what it will do is the only way the
     * label is worth reading.
     */
    yew_ctx_item(known && t.expanded ? "Collapse" : "Expand", NULL,
                 (u32)CTXA_FUSS_TOGGLE, have, 0U);
    yew_ctx_sep();
    /* A directory node's flags are the AGGREGATE of its subtree, which
     * is exactly what "all below" means. */
    yew_ctx_item("Stage All Below", NULL, (u32)CTXA_FUSS_STAGE,
                 have && known && (t.unstaged || t.untracked), 1U);
    yew_ctx_item("Unstage All Below", NULL, (u32)CTXA_FUSS_UNSTAGE,
                 have && known && t.staged, 1U);
}

/* The drawer's header, its blank tail and the backdrop behind it: the
 * pointer is on the REPOSITORY rather than on a path in it. */
static void build_fuss_blank(Ed *ed, const CtxContext *c)
{
    (void)ed;
    (void)c;
    yew_ctx_begin((u32)YEW_CTX_KIND_FUSS_BLANK);
    yew_ctx_target(0U, NULL);
    yew_ctx_item("Refresh", NULL, (u32)CTXA_FUSS_REFRESH, true, 0U);
    yew_ctx_item("Commit...", NULL, (u32)CTXA_FUSS_COMMIT, true, 0U);
    yew_ctx_sep();
    yew_ctx_item("Push", NULL, (u32)CTXA_FUSS_PUSH, true, 1U);
    yew_ctx_item("Pull", NULL, (u32)CTXA_FUSS_PULL, true, 1U);
    yew_ctx_item("Fetch", NULL, (u32)CTXA_FUSS_FETCH, true, 2U);
    yew_ctx_sep();
    yew_ctx_item("Status", NULL, (u32)CTXA_FUSS_STATUS, true, 2U);
    yew_ctx_item("History", NULL, (u32)CTXA_FUSS_HISTORY, true, 2U);
    yew_ctx_sep();
    yew_ctx_item("Leave FUSS", NULL, (u32)CTXA_FUSS_LEAVE, true, 0U);
}

/* ---------------------------------------------------------------- */
/* The transient overlays                                           */
/* ---------------------------------------------------------------- */

static void build_pick_row(Ed *ed, const CtxContext *c)
{
    (void)ed;
    yew_ctx_begin((u32)YEW_CTX_KIND_PICK_ROW);
    /* The ITEM PAYLOAD, not the row: the list refilters while the menu
     * is up and row 4 becomes a different file. */
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Open", NULL, (u32)CTXA_PICK_OPEN, true, 0U);
    yew_ctx_item("Open in Split Right", NULL, (u32)CTXA_PICK_SPLIT_RIGHT,
                 true, 1U);
    yew_ctx_item("Open in Split Below", NULL, (u32)CTXA_PICK_SPLIT_BELOW,
                 true, 1U);
    yew_ctx_sep();
    yew_ctx_item("Close", NULL, (u32)CTXA_OVERLAY_CLOSE, true, 0U);
}

/* The picker's box, off its rows: there is no item under the pointer,
 * so the only honest row is the one that puts the box away. */
static void build_picker(Ed *ed, const CtxContext *c)
{
    (void)ed;
    yew_ctx_begin((u32)c->kind);
    yew_ctx_target(0U, NULL);
    yew_ctx_item("Close", NULL, (u32)CTXA_OVERLAY_CLOSE, true, 0U);
}

static void build_compl_row(Ed *ed, const CtxContext *c)
{
    (void)ed;
    yew_ctx_begin((u32)YEW_CTX_KIND_COMPL_ROW);
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Accept", NULL, (u32)CTXA_COMPL_ACCEPT, true, 0U);
    yew_ctx_item("Toggle Docs", NULL, (u32)CTXA_COMPL_DOCS, true, 1U);
    yew_ctx_sep();
    yew_ctx_item("Cancel", NULL, (u32)CTXA_COMPL_CANCEL, true, 0U);
}

/*
 * The hover / signature / rename-confirm panel.
 *
 * §4 asks for `Apply`, `Show Diff` and `Cancel` on the rename-confirm
 * panel specifically.  The rename confirmation is a KEY HANDLER
 * (`yew_lsp_rename_key`) with no registry commands behind its three
 * answers, so those rows have no action to carry and are not built; the
 * row that is always true — put the panel away — is.  They land with
 * the commands.
 */
static void build_panel(Ed *ed, const CtxContext *c)
{
    (void)ed;
    (void)c;
    yew_ctx_begin((u32)YEW_CTX_KIND_PANEL);
    yew_ctx_target(0U, NULL);
    yew_ctx_item("Close", NULL, (u32)CTXA_OVERLAY_CLOSE, true, 0U);
}

/*
 * The group picker, rows and box.
 *
 * §4 asks for `Toggle` and `Confirm` as well.  The dialog is keyed
 * entirely through `yew_gp_key` — ticking a row and confirming a group
 * are not registry commands — so, as with the rename panel, the rows
 * that have no command are not invented here.
 */
static void build_gp(Ed *ed, const CtxContext *c)
{
    (void)ed;
    yew_ctx_begin((u32)c->kind);
    yew_ctx_target(c->id, NULL);
    yew_ctx_item("Cancel", NULL, (u32)CTXA_OVERLAY_CLOSE, true, 0U);
}

/* ---------------------------------------------------------------- */
/* The one entry point                                              */
/* ---------------------------------------------------------------- */

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
    case YEW_CTX_KIND_DOC:
        build_doc(ed, c);
        break;
    case YEW_CTX_KIND_STRIP:
    case YEW_CTX_KIND_EDITOR:
        build_strip(ed, c);
        break;
    case YEW_CTX_KIND_BORDER:
        build_border(ed, c);
        break;
    case YEW_CTX_KIND_FOOTER:
        build_footer(ed, c);
        break;
    case YEW_CTX_KIND_FUSS_FILE:
        build_fuss_file(ed, c);
        break;
    case YEW_CTX_KIND_FUSS_DIR:
        build_fuss_dir(ed, c);
        break;
    case YEW_CTX_KIND_FUSS_BLANK:
        build_fuss_blank(ed, c);
        break;
    case YEW_CTX_KIND_PICK_ROW:
        build_pick_row(ed, c);
        break;
    case YEW_CTX_KIND_PICKER:
        build_picker(ed, c);
        break;
    case YEW_CTX_KIND_COMPL_ROW:
        build_compl_row(ed, c);
        break;
    case YEW_CTX_KIND_PANEL:
        build_panel(ed, c);
        break;
    case YEW_CTX_KIND_GP_ROW:
    case YEW_CTX_KIND_GP:
        build_gp(ed, c);
        break;
    case YEW_CTX_KIND_NONE:
    default:
        /* Nothing is under the pointer in a sense that even the
         * fall-through could name; opening a menu would be inventing a
         * surface.  `yew_mouse_context_at` never returns it. */
        break;
    }
}
