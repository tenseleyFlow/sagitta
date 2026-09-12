#ifndef YEW_UI_GROUPPICKER_H
#define YEW_UI_GROUPPICKER_H

/*
 * Sprint 24 §4: the group picker.
 *
 * ONE DIALOG, TWO MODES.  Creating and editing a group are the same act
 * — choose files, name the result — so Edit is the same list opened
 * with the current members already ticked.  The mode reaches no further
 * than the title and the footer hint.
 *
 * The picker knows NOTHING about groups.  It is a file picker with a
 * name field; what a confirmed result means lives in the command layer.
 * That is what keeps "create a group" and "change a group's membership"
 * from being two dialogs that drift apart.
 *
 * THE DECISION THIS FILE IS BUILT AROUND: ticks are a SET OF CANONICAL
 * PATHS, never a flag on a listed row.  Walking to `../` re-lists a
 * different directory, and per-row flags would drop every tick the
 * moment you moved — defeating the entire point of the `../` row, which
 * is assembling a group that spans directories.
 */

#include "edit/cmd.h"
#include "term/input.h"
#include "ui/layout.h"
#include "util/base.h"

typedef struct Ed Ed;

typedef enum {
    YEW_GP_PENDING = 0,
    YEW_GP_CANCELLED,
    YEW_GP_CONFIRMED
} GpResult;

enum {
    YEW_GP_WIDTH_MAX = 62,
    YEW_GP_VISIBLE_ROWS = 12,
    YEW_GP_PATH_MAX = 1024,
    YEW_GP_NAME_MAX = 64
};

/* New mode: the name field is pre-filled `basename(dir)/` so the happy
 * path is Enter-Enter. */
bool yew_gp_show(Ed *ed, const char *dir);
/* Edit mode: same list, current members already ticked. */
bool yew_gp_show_edit(Ed *ed, const char *dir, const char *name);
void yew_gp_close(Ed *ed);
bool yew_gp_active(void);

/*
 * Ticks a path WITHOUT listing it.  An edit-mode member may live
 * outside `dir` entirely, and there is no index into the current
 * listing that could express it.
 */
void yew_gp_preselect(const char *path);
/* Rows for ticked paths with unsaved changes show a `•`.  In Edit mode
 * unticking closes that member's tab, so the warning has to be readable
 * BEFORE Enter, not in a prompt after. */
void yew_gp_mark_dirty(const char *path);

GpResult yew_gp_result(void);
const char *yew_gp_name(void);
int yew_gp_count(void);
const char *yew_gp_path(int i);

/* True when the key was consumed by the dialog. */
bool yew_gp_key(Ed *ed, Key key);
void yew_gp_draw(Ed *ed);
/* True when the click was consumed. */
bool yew_gp_click(Ed *ed, u16 x, u16 y);
/* Sprint 27 §2: the wheel.  Moves the focused row — see grouppicker.c
 * for why there is no separate scroll offset to move instead. */
void yew_gp_scroll(Ed *ed, int rows);

/*
 * Applies a CONFIRMED result, and does nothing while the dialog is
 * still up.  Called after every key and click the picker consumed.
 *
 * This is where the picker's ignorance of groups is paid for: the
 * dialog produced a name and a set of paths, and the MEANING of that —
 * create versus diff-against-current-members — lives here.
 */
void yew_gp_apply(Ed *ed);

CmdStatus yew_gp_cmd_new(CmdCtx *cx);
CmdStatus yew_gp_cmd_edit(CmdCtx *cx);

/*
 * Sprint 57.13 §4: the dialog's `Toggle` and `Confirm` rows.
 *
 * The picker was keyed entirely through `yew_gp_key`, so its menu rows
 * had no command to carry and shipped as nothing.  These share one
 * implementation with the key handler — Space and Enter land in the
 * same two functions — and refuse with a message when the dialog is
 * down.  Both run `yew_gp_apply` the way the key loop does.
 */
CmdStatus yew_gp_cmd_toggle(CmdCtx *cx);
CmdStatus yew_gp_cmd_confirm(CmdCtx *cx);

/*
 * Moves the focus to a LISTING INDEX — what a GP_ROW region's payload
 * is, and what the context menu spends its captured target on before
 * `Toggle` runs.  False when the dialog is down or the row has gone.
 */
bool yew_gp_select_row(Ed *ed, int idx);

#endif
