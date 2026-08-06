#ifndef SAG_UI_GROUPNAV_H
#define SAG_UI_GROUPNAV_H

/*
 * Sprint 24 §6: navigation as ONE CONTINUOUS LINE.
 *
 * Left and right walk through every open file, whether or not it is in
 * a group: inside a group you step through its members; step off either
 * end and you fall through to the row-1 walk, landing on the entry
 * beside the group.  That fall-through is not a convenience — it is the
 * guaranteed exit.  A user whose window manager eats `super+ctrl+up`
 * must still be able to get out of a group, and pressing right until
 * you are out always works.
 *
 * The walk builds its entry list with sag_tab_row1_entries, the same
 * call the renderer uses.  A second construction would let the walk
 * skip an entry the user can see on screen.
 */

#include "edit/cmd.h"
#include "util/base.h"

typedef struct Ed Ed;

/*
 * Enter from the side the walk arrived from: moving right lands on the
 * FIRST member, moving left on the LAST.
 *
 * That is what makes the walk reversible — retrace your steps and you
 * visit the same files in reverse.  Deliberately NOT the
 * resume-at-last-active behaviour: right for an explicit enter, wrong
 * mid-walk, where it would skip every member between the edge and
 * wherever you last happened to be.
 */
void sag_group_enter_at_edge(Ed *ed, u32 gid, int delta);
/* Explicit entry: resume at `last_active_member`, resolved against the
 * CURRENT members; a dangling path falls back to the first. */
void sag_group_enter(Ed *ed, u32 gid);
/* False when every tab is in this group and there is nowhere to go. */
bool sag_group_leave(Ed *ed);
/* One step along the continuous line. */
void sag_file_step(Ed *ed, int delta);

CmdStatus sag_file_cmd_next(CmdCtx *cx);
CmdStatus sag_file_cmd_prev(CmdCtx *cx);
CmdStatus sag_group_cmd_enter(CmdCtx *cx);
CmdStatus sag_group_cmd_leave(CmdCtx *cx);
CmdStatus sag_group_cmd_dissolve(CmdCtx *cx);
CmdStatus sag_group_cmd_remove_tab(CmdCtx *cx);
/*
 * Sprint 27 §5/§8.  add_tab is the keyboard twin of dropping a tab into
 * a group, and rename is the group menu's row; both exist as commands
 * so the mouse and the keyboard run the same code rather than two
 * implementations that drift.
 */
CmdStatus sag_group_cmd_add_tab(CmdCtx *cx);
CmdStatus sag_group_cmd_rename(CmdCtx *cx);
/* ed.group.from_dir is registered with the DEFER convention in cmd.c —
 * it hard-errors naming Sprint 53 rather than existing here as a stub
 * that does nothing. */

/*
 * DoD 6: the mid-walk path must never consult last_active_member.
 *
 * Counted rather than reasoned about, because the two entry paths are
 * one function call apart and the wrong one produces a walk that still
 * works — it just skips files, which no one notices until they are
 * looking for one.
 */
u64 sag_group_resume_reads(void);

#endif
