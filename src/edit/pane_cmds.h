#ifndef SAG_EDIT_PANE_CMDS_H
#define SAG_EDIT_PANE_CMDS_H

#include "edit/cmd.h"
#include "ui/layout.h"
#include "ui/region.h"

typedef struct Ed Ed;

CmdStatus sag_pane_cmd_split_h(CmdCtx *cx);
CmdStatus sag_pane_cmd_split_v(CmdCtx *cx);
CmdStatus sag_pane_cmd_close(CmdCtx *cx);
CmdStatus sag_pane_cmd_focus_left(CmdCtx *cx);
CmdStatus sag_pane_cmd_focus_right(CmdCtx *cx);
CmdStatus sag_pane_cmd_focus_up(CmdCtx *cx);
CmdStatus sag_pane_cmd_focus_down(CmdCtx *cx);
CmdStatus sag_pane_cmd_focus_next(CmdCtx *cx);
CmdStatus sag_pane_cmd_grow(CmdCtx *cx);
CmdStatus sag_pane_cmd_shrink(CmdCtx *cx);

/*
 * Border drag.  Deliberately minimal this sprint: press, motion,
 * release, with Esc restoring the entry ratio.  Sprint 27 re-routes it
 * through the full mouse router.
 */
typedef struct PaneDrag {
    Pane *split;
    float entry_ratio;
    u16 origin; /* the cell the boundary was at when the drag began */
    bool active;
} PaneDrag;

void sag_pane_drag_begin(Ed *ed, Pane *split, u16 x, u16 y);
void sag_pane_drag_motion(Ed *ed, u16 x, u16 y);
void sag_pane_drag_end(Ed *ed);
void sag_pane_drag_cancel(Ed *ed);

/* Click-to-focus, through the region registry.  True when the click was
 * consumed. */
bool sag_pane_click(Ed *ed, u16 x, u16 y);

/*
 * The per-frame tables the region payloads index into.  Render fills
 * them; the click router reads them.  Indices rather than pointers so a
 * stale payload from a previous frame cannot dereference a freed node.
 */
void sag_pane_tables_reset(Ed *ed);
i32 sag_pane_table_add_leaf(Ed *ed, Pane *leaf);
i32 sag_pane_table_add_split(Ed *ed, Pane *split);
Pane *sag_pane_leaf_by_index(Ed *ed, i32 index);
Pane *sag_pane_split_by_index(Ed *ed, i32 index);

#endif
