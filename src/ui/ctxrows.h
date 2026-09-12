#ifndef YEW_UI_CTXROWS_H
#define YEW_UI_CTXROWS_H

/*
 * Sprint 57.11 §3/§4: WHAT IS UNDER THE POINTER, and WHICH ROWS THAT
 * DESERVES.
 *
 * Two modules meet here and neither may include the other's world:
 *
 *   ui/ctxmenu.c owns geometry, drawing, hit-testing and keys, and is
 *   deliberately editor-ignorant — it must stay below the editor in the
 *   dependency graph because the renderer uses it.
 *
 *   ui/mouse.c owns the router, and is deliberately ALLOCATION-FREE —
 *   tests/perf/mouse.c reads its source and fails on any allocation
 *   call it finds — so it cannot be where menu rows are built: a row
 *   set is strings and a captured path.
 *
 * So the vocabulary — the kinds, the context, the target discipline and
 * the action table — lives in this header, which knows only `Ed *` by
 * name; the builders live in ui/ctxrows.c, which may include edit/ed.h
 * and may allocate; and the router reads the table without ever writing
 * a row.
 *
 * THE ACTION TABLE IS THE DISPATCH.  Sprint 27 dispatched a row with a
 * switch per menu kind, which meant every new kind grew a second switch
 * somewhere else and the two drifted.  A row's meaning is now DATA: a
 * registry command name, how the target is applied before it runs, and
 * one integer argument.  `yew_ctx_actions[action]` is the whole of it,
 * and `apply_menu_action` is the only reader.
 */

#include "ui/layout.h"
#include "util/base.h"

typedef struct Ed Ed;

/*
 * What the pointer is over.  NONE/TAB/GROUP keep the values Sprint 27
 * gave them: `yew_ctx_kind()` is a test seam and a golden's vocabulary,
 * so the two that already existed stay where they were.
 */
typedef enum CtxKind {
    YEW_CTX_KIND_NONE = 0,
    YEW_CTX_KIND_TAB,
    YEW_CTX_KIND_GROUP,
    YEW_CTX_KIND_STRIP,     /* strip blank tail, chevrons, the + */
    YEW_CTX_KIND_DOC,       /* pane text or gutter */
    YEW_CTX_KIND_BORDER,    /* pane border */
    YEW_CTX_KIND_FOOTER,    /* statusline / message row */
    YEW_CTX_KIND_FUSS_FILE,
    YEW_CTX_KIND_FUSS_DIR,
    YEW_CTX_KIND_FUSS_BLANK, /* header, blank drawer, backdrop */
    YEW_CTX_KIND_PICK_ROW,
    YEW_CTX_KIND_PICKER,
    YEW_CTX_KIND_COMPL_ROW,
    YEW_CTX_KIND_PANEL,     /* hover / signature / rename-confirm */
    YEW_CTX_KIND_GP_ROW,
    YEW_CTX_KIND_GP,
    YEW_CTX_KIND_EDITOR     /* nothing under the pointer */
} CtxKind;

/*
 * The answer `yew_mouse_context_at` gives, and the only thing a builder
 * is told about the click.
 *
 * `id` is the IDENTITY the rows will act on — a tab_id, a gid, an
 * interned path id, a leaf index — never a row index, because the strip
 * scrolls and the tree rebuilds.  `payload` is the raw region payload,
 * kept for the row kinds whose selection IS the payload (picker,
 * completion, group picker).  `rect` is the region's rectangle, or the
 * pane's for DOC.
 */
typedef struct CtxContext {
    CtxKind kind;
    u32 id;
    i32 payload;
    Rect rect;
} CtxContext;

/*
 * What has to be true before a row's command runs.  The menu captured
 * its target at open time; this says how that capture is spent.
 */
typedef enum CtxTarget {
    CTX_TGT_NONE = 0, /* invoke as-is */
    CTX_TGT_TAB,      /* switch to the captured tab_id first */
    CTX_TGT_GROUP,    /* enter the captured gid first */
    CTX_TGT_PANE,     /* focus the captured leaf, cursor to the click cell */
    /*
     * Focus the captured leaf and LEAVE THE CARET ALONE.
     *
     * Sprint 57.11 §3 listed six targets and Deliverable 4 found it
     * needed a seventh: placing the caret where the click was COLLAPSES
     * THE SELECTION (yew_win_click_to_cursor sets anchor = pos), so
     * `Cut`, `Copy` and `Delete` — the three rows a user right-clicks a
     * selection to reach — would every one of them act on nothing.  The
     * rows that mean "here" (paste, split, go to definition) keep
     * CTX_TGT_PANE; the rows that mean "this selection" take this.
     */
    CTX_TGT_LEAF,
    CTX_TGT_PATH,     /* cx.sarg = the captured path (and select it) */
    /*
     * Move the FUSS selection to the captured path and invoke with NO
     * sarg.  For the one row whose command resolves the selection into
     * an ABSOLUTE path itself (`ed.group.from_dir`): the capture is the
     * workspace-relative path every other `ed.git.*` row wants, and
     * handing that to a command that calls realpath() on it would
     * resolve it against the process's working directory, which yew
     * never chdir()s to the workspace.
     */
    CTX_TGT_FUSS_ROW,
    CTX_TGT_PICK,     /* select the captured payload, then accept by iarg */
    CTX_TGT_COMPL,    /* select the captured completion item first */
    /*
     * Move the GROUP PICKER's focus to the captured listing index, then
     * invoke with no argument.
     *
     * A NINTH TARGET RATHER THAN A REUSED ONE.  CTX_TGT_PICK is the
     * fuzzy picker and its accept modes — a different widget with a
     * different selection model — and CTX_TGT_COMPL is the completion
     * menu's; neither knows the group dialog exists, and teaching one
     * of them to would be a target that means two things depending on
     * what happens to be open.  The payload here is a LISTING INDEX,
     * which is the one identity the GP_ROW region carries, so the
     * capture has to be spent through `yew_gp_select_row` — the same
     * function `yew_gp_click` uses — before `Toggle` can tick the row
     * the user actually pointed at rather than the focused one.
     */
    CTX_TGT_GP_ROW
} CtxTarget;

typedef struct CtxActionDesc {
    /* Registry name, or NULL when the row closes an overlay inline. */
    const char *cmd;
    CtxTarget target;
    i64 iarg;
} CtxActionDesc;

/*
 * Row actions.  OPAQUE to ctxmenu.c by construction — it stores the
 * number and hands it back — and an index into `yew_ctx_actions` here.
 *
 * CTXA_NONE is 0 because `yew_ctx_take` returns 0 for "nothing chosen".
 */
typedef enum CtxAction {
    CTXA_NONE = 0,
    CTXA_TAB_CLOSE,
    CTXA_TAB_CLOSE_OTHERS,
    CTXA_TAB_COPY_PATH,
    CTXA_TAB_LEAVE_GROUP,
    CTXA_GROUP_EDIT,
    CTXA_GROUP_RENAME,
    CTXA_GROUP_DISSOLVE,
    CTXA_PALETTE,
    /*
     * The palette row of a DOCUMENT menu, which focuses the pane the
     * user pointed at and puts the caret on the clicked cell first —
     * every §4 document row does, and a palette opened for a pane the
     * user did not point at would be the wrong buffer's palette.
     */
    CTXA_PALETTE_HERE,
    CTXA_FUSS_OPEN,

    /*
     * Sprint 57.11 §4 proper.
     *
     * WHY SOME ROWS APPEAR TWICE (CTXA_GOTO_LINE and
     * CTXA_DOC_GOTO_LINE, CTXA_FIND_FILE and CTXA_DOC_FIND_FILE …):
     * the TARGET is a property of the action, not of the menu, and the
     * same label means different things from two surfaces.  `Go to
     * Line...` in the document menu belongs to the pane that was
     * pointed at; the same row in the footer menu belongs to whatever
     * is focused, because the footer is not a pane.  One action with
     * two targets is not expressible, and a target chosen at dispatch
     * time by guessing the menu's kind is exactly the second switch
     * this table exists to delete.
     */

    /* Document: the clipboard trio and its neighbours. */
    CTXA_DOC_CUT,
    CTXA_DOC_COPY,
    CTXA_DOC_PASTE,
    CTXA_DOC_DELETE,
    CTXA_DOC_SELECT_ALL,
    CTXA_DOC_UNDO,
    CTXA_DOC_REDO,
    /* Document: the pane rows. */
    CTXA_DOC_SPLIT_RIGHT,
    CTXA_DOC_SPLIT_BELOW,
    CTXA_DOC_CLOSE_PANE,
    /* Document: the file rows. */
    CTXA_DOC_SAVE,
    CTXA_DOC_SAVE_AS,
    CTXA_DOC_RELOAD,
    /* Document: the LSP section (omitted whole with no server). */
    CTXA_DOC_LSP_DEF,
    CTXA_DOC_LSP_REFS,
    CTXA_DOC_LSP_RENAME,
    CTXA_DOC_LSP_HOVER,
    /* Document: the git section (omitted whole outside a repository). */
    CTXA_DOC_BLAME_TOGGLE,
    CTXA_DOC_DIFF,
    /* Document: the tail. */
    CTXA_DOC_FIND_FILE,
    CTXA_DOC_GOTO_LINE,
    CTXA_DOC_TOGGLE_WRAP,

    /* Tab strip. */
    CTXA_TAB_NEW,
    CTXA_TAB_OPEN_SPLIT_RIGHT,
    CTXA_TAB_OPEN_SPLIT_BELOW,
    CTXA_GROUP_CLOSE,
    CTXA_GROUP_NEW,
    CTXA_FIND_FILE,
    CTXA_FIND_BUFFER,

    /* Pane border. */
    CTXA_PANE_CLOSE,
    CTXA_PANE_GROW,
    CTXA_PANE_SHRINK,
    CTXA_PANE_FOCUS_NEXT,

    /* Footer. */
    CTXA_GOTO_LINE,
    CTXA_TOGGLE_WRAP,
    CTXA_NUMBER_CYCLE,
    CTXA_MOUSE_DISABLE,

    /* FUSS rows, every one of them path-addressed. */
    CTXA_FUSS_OPEN_SPLIT_RIGHT,
    CTXA_FUSS_OPEN_SPLIT_BELOW,
    CTXA_FUSS_PREVIEW,
    CTXA_FUSS_STAGE,
    CTXA_FUSS_UNSTAGE,
    CTXA_FUSS_DISCARD,
    CTXA_FUSS_DIFF,
    CTXA_FUSS_BLAME,
    CTXA_FUSS_RENAME,
    CTXA_FUSS_DELETE,
    CTXA_FUSS_TOGGLE,
    CTXA_FUSS_COPY_PATH,
    CTXA_FUSS_GROUP_FROM_DIR,
    /* FUSS drawer, header and backdrop: the repository rows. */
    CTXA_FUSS_REFRESH,
    CTXA_FUSS_COMMIT,
    CTXA_FUSS_PUSH,
    CTXA_FUSS_PULL,
    CTXA_FUSS_FETCH,
    CTXA_FUSS_STATUS,
    CTXA_FUSS_HISTORY,
    CTXA_FUSS_LEAVE,

    /* Picker, completion, panel and group picker. */
    CTXA_PICK_OPEN,
    CTXA_PICK_SPLIT_RIGHT,
    CTXA_PICK_SPLIT_BELOW,
    CTXA_COMPL_ACCEPT,
    CTXA_COMPL_DOCS,
    CTXA_COMPL_CANCEL,

    /*
     * The rename-confirm panel's three answers, and the group picker's
     * two rows: Deliverable 4's other half, the rows that shipped as
     * nothing because the state machines behind them were raw key
     * handlers with no commands.
     */
    CTXA_RENAME_APPLY,
    CTXA_RENAME_DIFF,
    CTXA_RENAME_CANCEL,
    CTXA_GP_TOGGLE,
    CTXA_GP_CONFIRM,
    /*
     * The one action with no command: closing a transient overlay.  No
     * registry command owns "make this go away" — Esc does — and
     * inventing four whose only caller is a menu row would put four
     * names in the palette for a key everybody already has.  WHICH
     * overlay closes is the MENU'S KIND, read by the router.
     */
    CTXA_OVERLAY_CLOSE,
    CTXA__N
} CtxAction;

/* Indexed by CtxAction.  CTXA_NONE's entry is inert. */
extern const CtxActionDesc yew_ctx_actions[CTXA__N];

/*
 * Builds the rows for `c` into the widget: `yew_ctx_begin`, the target
 * capture, then the items.  It does NOT place the menu — the anchor and
 * the allowed rectangle are the router's, and `yew_ctx_show` is what
 * decides whether the box fits.  A kind with no rows leaves the widget
 * empty, and `yew_ctx_show` then refuses.
 */
void yew_ctx_build(Ed *ed, const CtxContext *c);

#endif
