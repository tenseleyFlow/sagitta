/*
 * Sprint 24 §6.  See groupnav.h for why the line is continuous.
 */
#include "ui/groupnav.h"

#include <string.h>

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/strip.h"
#include "ui/tabs.h"
#include "util/log.h"

/* DoD 6 test hook; see the header. */
static u64 resume_reads;

u64 sag_group_resume_reads(void)
{
    return resume_reads;
}

void sag_group_enter_at_edge(Ed *ed, u32 gid, int delta)
{
    int members[SAG_TAB_MAX];
    int n;

    if (ed == NULL || gid == 0U)
        return;
    n = sag_group_members(ed, gid, members, (int)SAG_ARRAY_LEN(members));
    if (n <= 0)
        return;
    /*
     * The side the walk came from.  Note what is NOT here: any look at
     * last_active_member.  Resuming mid-walk would skip every member
     * between the edge and wherever the user last was, and the skip is
     * invisible — the walk still moves, it just misses files.
     */
    sag_tab_switch(ed, delta >= 0 ? members[0] : members[n - 1]);
}

void sag_group_enter(Ed *ed, u32 gid)
{
    int members[SAG_TAB_MAX];
    int n;
    int i;
    TabGroup *g;

    if (ed == NULL || gid == 0U)
        return;
    n = sag_group_members(ed, gid, members, (int)SAG_ARRAY_LEN(members));
    if (n <= 0)
        return;
    g = sag_group_at(ed, gid);
    resume_reads++;
    if (g != NULL && g->last_active_member != NULL) {
        /*
         * Resolved against the CURRENT members by PATH.  The stored
         * path is allowed to dangle — the member it names can have been
         * closed since — so a miss falls back to the lowest ordinal
         * rather than refusing to enter.
         */
        for (i = 0; i < n; i++) {
            const Tab *t = sag_tab_at(ed, members[i]);

            if (t != NULL && t->path != NULL &&
                strcmp(t->path, g->last_active_member) == 0) {
                sag_tab_switch(ed, members[i]);
                return;
            }
        }
    }
    sag_tab_switch(ed, members[0]);
}

bool sag_group_leave(Ed *ed)
{
    u32 gid;
    int n;
    int i;

    if (ed == NULL)
        return false;
    gid = sag_active_group_id(ed);
    if (gid == 0U)
        return false;
    /* Record where we were before the switch changes what "active"
     * means, so coming back resumes here. */
    sag_group_note_position(ed);
    n = (int)sag_tab_count(ed);
    /* The nearest tab AFTER the group in array order, else the nearest
     * before — leaving forwards is the common direction. */
    for (i = ed->tabs.active + 1; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != gid) {
            sag_tab_switch(ed, i);
            return true;
        }
    }
    for (i = ed->tabs.active - 1; i >= 0; i--) {
        if (ed->tabs.v.data[i].group_id != gid) {
            sag_tab_switch(ed, i);
            return true;
        }
    }
    sag_msg(ed, SAG_MSG_INFO, "every open file is in this group");
    return false;
}

void sag_file_step(Ed *ed, int delta)
{
    StripEntry entries[SAG_TAB_MAX];
    int members[SAG_TAB_MAX];
    u32 gid;
    int n;
    int here;
    int target;

    if (ed == NULL || delta == 0 || sag_tab_count(ed) == 0U)
        return;
    gid = sag_active_group_id(ed);
    if (gid != 0U) {
        /*
         * Step 1: inside a group, walk its members by ordinal.  A
         * target in range is the whole answer.
         */
        int nm = sag_group_members(ed, gid, members,
                                   (int)SAG_ARRAY_LEN(members));
        int at = -1;
        int i;

        for (i = 0; i < nm; i++) {
            if (members[i] == ed->tabs.active)
                at = i;
        }
        if (at >= 0) {
            int want = at + delta;

            if (want >= 0 && want < nm) {
                sag_tab_switch(ed, members[want]);
                return;
            }
            /*
             * Off either end: FALL THROUGH to the row-1 walk with the
             * group's own entry as "here".  This is what makes the line
             * continuous, and it is the exit that survives a window
             * manager eating the leave chord.
             */
            sag_group_note_position(ed);
        }
    }

    /* Step 2: the row-1 entry list — the renderer's own construction. */
    n = sag_tab_row1_entries(ed, entries, (int)SAG_ARRAY_LEN(entries));
    if (n <= 0)
        return;
    here = sag_tab_row1_active(ed, entries, n);
    if (here < 0)
        here = 0;
    target = here + delta;
    /* Wrapping at both ends: next-from-the-last meaning "nothing" is a
     * worse answer than wrapping when a strip is showing the ring. */
    while (target < 0)
        target += n;
    target %= n;
    if (target == here && n == 1)
        return;

    /* Step 3: a tab is a switch; a group is entered FROM THE SIDE the
     * walk arrived from, so the walk is reversible. */
    if (entries[target].payload >= 0)
        sag_tab_switch(ed, entries[target].payload);
    else
        sag_group_enter_at_edge(ed, (u32)(-entries[target].payload),
                                delta);
}

CmdStatus sag_file_cmd_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_file_step(cx->ed, 1);
    return SAG_CMD_OK;
}

CmdStatus sag_file_cmd_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    sag_file_step(cx->ed, -1);
    return SAG_CMD_OK;
}

/*
 * Explicit enter: the group under the cursor on row 1, or — when the
 * active tab is ungrouped — the nearest group to its right, so the
 * command is reachable without a mouse.
 */
CmdStatus sag_group_cmd_enter(CmdCtx *cx)
{
    Ed *ed;
    int i;
    int n;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    if (sag_active_group_id(ed) != 0U)
        return SAG_CMD_OK; /* already inside it */
    n = (int)sag_tab_count(ed);
    for (i = ed->tabs.active + 1; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != 0U) {
            sag_group_enter(ed, ed->tabs.v.data[i].group_id);
            return SAG_CMD_OK;
        }
    }
    for (i = 0; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != 0U) {
            sag_group_enter(ed, ed->tabs.v.data[i].group_id);
            return SAG_CMD_OK;
        }
    }
    sag_msg(ed, SAG_MSG_ERROR, "no tab groups");
    return SAG_CMD_ERR_STATE;
}

CmdStatus sag_group_cmd_leave(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    if (sag_active_group_id(cx->ed) == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "not in a tab group");
        return SAG_CMD_ERR_STATE;
    }
    return sag_group_leave(cx->ed) ? SAG_CMD_OK : SAG_CMD_ERR_STATE;
}

CmdStatus sag_group_cmd_dissolve(CmdCtx *cx)
{
    u32 gid;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    gid = sag_active_group_id(cx->ed);
    if (gid == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "not in a tab group");
        return SAG_CMD_ERR_STATE;
    }
    /* The tabs stay open and become ungrouped; dissolving a group is
     * not a way to close files. */
    sag_group_dissolve(cx->ed, gid);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_group_cmd_remove_tab(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    if (sag_active_group_id(cx->ed) == 0U) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "not in a tab group");
        return SAG_CMD_ERR_STATE;
    }
    sag_group_remove_member(cx->ed, cx->ed->tabs.active);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}
