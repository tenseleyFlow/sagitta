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
 *   ui/mouse.c owns the router, and is deliberately ALLOCATION-FREE
 *   (tests/perf/mouse.c reads its source and fails on a `malloc(`), so
 *   it cannot be where menu rows are built: a row set is strings and a
 *   captured path.
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
    CTX_TGT_PATH,     /* cx.sarg = the captured path */
    CTX_TGT_PICK,     /* select the captured payload, then accept by iarg */
    CTX_TGT_COMPL     /* select the captured completion item first */
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
