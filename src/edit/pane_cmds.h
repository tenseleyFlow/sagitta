#ifndef YEW_EDIT_PANE_CMDS_H
#define YEW_EDIT_PANE_CMDS_H

#include "edit/cmd.h"
#include "ui/layout.h"
#include "ui/region.h"

typedef struct Ed Ed;

CmdStatus yew_pane_cmd_split_h(CmdCtx *cx);
CmdStatus yew_pane_cmd_split_v(CmdCtx *cx);
CmdStatus yew_pane_cmd_close(CmdCtx *cx);
CmdStatus yew_pane_cmd_focus_left(CmdCtx *cx);
CmdStatus yew_pane_cmd_focus_right(CmdCtx *cx);
CmdStatus yew_pane_cmd_focus_up(CmdCtx *cx);
CmdStatus yew_pane_cmd_focus_down(CmdCtx *cx);
CmdStatus yew_pane_cmd_focus_next(CmdCtx *cx);
CmdStatus yew_pane_cmd_focus_prev(CmdCtx *cx);
CmdStatus yew_pane_cmd_grow(CmdCtx *cx);
CmdStatus yew_pane_cmd_shrink(CmdCtx *cx);
/*
 * Sprint 57.21 §5: open a TAB's buffer in a new pane on a chosen side.
 *
 * The keyboard half of the drag-to-spawn gesture, and the function the
 * gesture itself calls at release — one implementation, so the pointer
 * and the keyboard cannot drift into meaning different things.
 *
 * `tab_id`, never an index: the strip scrolls and tabs close, and an
 * index captured a moment ago names a different file afterwards.
 * `leaf` is the leaf to split, or NULL for the focused one.  `dir` and
 * `new_first` are yew_pane_split_side's, so LEFT is
 * (YEW_SPLIT_H, true), RIGHT is (YEW_SPLIT_H, false) and BELOW is
 * (YEW_SPLIT_V, false).
 *
 * On success the new leaf takes focus and `*out` receives it; `out` may
 * be NULL.  A refusal messages exactly as the commands do and leaves
 * the layout as it was.
 */
CmdStatus yew_pane_open_tab_in_split(Ed *ed, u32 tab_id, Pane *leaf,
                                     SplitDir dir, bool new_first,
                                     Pane **out);
CmdStatus yew_pane_cmd_tab_split_left(CmdCtx *cx);
CmdStatus yew_pane_cmd_tab_split_right(CmdCtx *cx);
CmdStatus yew_pane_cmd_tab_split_down(CmdCtx *cx);
void yew_pane_refocus(Ed *ed, Pane *want);

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

void yew_pane_drag_begin(Ed *ed, Pane *split, u16 x, u16 y);
void yew_pane_drag_motion(Ed *ed, u16 x, u16 y);
void yew_pane_drag_end(Ed *ed);
void yew_pane_drag_cancel(Ed *ed);

/* Click-to-focus, through the region registry.  True when the click was
 * consumed. */
bool yew_pane_click(Ed *ed, u16 x, u16 y);

/*
 * The per-frame tables the region payloads index into.  Render fills
 * them; the click router reads them.  Indices rather than pointers so a
 * stale payload from a previous frame cannot dereference a freed node.
 */
void yew_pane_tables_reset(Ed *ed);
i32 yew_pane_table_add_leaf(Ed *ed, Pane *leaf);
i32 yew_pane_table_add_split(Ed *ed, Pane *split);
Pane *yew_pane_leaf_by_index(Ed *ed, i32 index);
Pane *yew_pane_split_by_index(Ed *ed, i32 index);

#endif
