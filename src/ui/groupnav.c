/*
 * Sprint 24 §6.  See groupnav.h for why the line is continuous.
 */
#include "ui/groupnav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/cmdline.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/strip.h"
#include "ui/tabs.h"
#include "util/log.h"

/* DoD 6 test hook; see the header. */
static u64 resume_reads;

u64 yew_group_resume_reads(void)
{
    return resume_reads;
}

void yew_group_enter_at_edge(Ed *ed, u32 gid, int delta)
{
    int members[YEW_TAB_MAX];
    int n;

    if (ed == NULL || gid == 0U)
        return;
    n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));
    if (n <= 0)
        return;
    /*
     * The side the walk came from.  Note what is NOT here: any look at
     * last_active_member.  Resuming mid-walk would skip every member
     * between the edge and wherever the user last was, and the skip is
     * invisible — the walk still moves, it just misses files.
     */
    yew_tab_switch(ed, delta >= 0 ? members[0] : members[n - 1]);
}

void yew_group_enter(Ed *ed, u32 gid)
{
    int members[YEW_TAB_MAX];
    int n;
    int i;
    TabGroup *g;

    if (ed == NULL || gid == 0U)
        return;
    n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));
    if (n <= 0)
        return;
    g = yew_group_at(ed, gid);
    resume_reads++;
    if (g != NULL && g->last_active_member != NULL) {
        /*
         * Resolved against the CURRENT members by PATH.  The stored
         * path is allowed to dangle — the member it names can have been
         * closed since — so a miss falls back to the lowest ordinal
         * rather than refusing to enter.
         */
        for (i = 0; i < n; i++) {
            const Tab *t = yew_tab_at(ed, members[i]);

            if (t != NULL && t->path != NULL &&
                strcmp(t->path, g->last_active_member) == 0) {
                yew_tab_switch(ed, members[i]);
                return;
            }
        }
    }
    yew_tab_switch(ed, members[0]);
}

bool yew_group_leave(Ed *ed)
{
    u32 gid;
    int n;
    int i;

    if (ed == NULL)
        return false;
    gid = yew_active_group_id(ed);
    if (gid == 0U)
        return false;
    /* Record where we were before the switch changes what "active"
     * means, so coming back resumes here. */
    yew_group_note_position(ed);
    n = (int)yew_tab_count(ed);
    /* The nearest tab AFTER the group in array order, else the nearest
     * before — leaving forwards is the common direction. */
    for (i = ed->tabs.active + 1; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != gid) {
            yew_tab_switch(ed, i);
            return true;
        }
    }
    for (i = ed->tabs.active - 1; i >= 0; i--) {
        if (ed->tabs.v.data[i].group_id != gid) {
            yew_tab_switch(ed, i);
            return true;
        }
    }
    yew_msg(ed, YEW_MSG_INFO, "every open file is in this group");
    return false;
}

void yew_file_step(Ed *ed, int delta)
{
    StripEntry entries[YEW_TAB_MAX];
    int members[YEW_TAB_MAX];
    u32 gid;
    int n;
    int here;
    int target;

    if (ed == NULL || delta == 0 || yew_tab_count(ed) == 0U)
        return;
    gid = yew_active_group_id(ed);
    if (gid != 0U) {
        /*
         * Step 1: inside a group, walk its members by ordinal.  A
         * target in range is the whole answer.
         */
        int nm = yew_group_members(ed, gid, members,
                                   (int)YEW_ARRAY_LEN(members));
        int at = -1;
        int i;

        for (i = 0; i < nm; i++) {
            if (members[i] == ed->tabs.active)
                at = i;
        }
        if (at >= 0) {
            int want = at + delta;

            if (want >= 0 && want < nm) {
                yew_tab_switch(ed, members[want]);
                return;
            }
            /*
             * Off either end: FALL THROUGH to the row-1 walk with the
             * group's own entry as "here".  This is what makes the line
             * continuous, and it is the exit that survives a window
             * manager eating the leave chord.
             */
            yew_group_note_position(ed);
        }
    }

    /* Step 2: the row-1 entry list — the renderer's own construction. */
    n = yew_tab_row1_entries(ed, entries, (int)YEW_ARRAY_LEN(entries));
    if (n <= 0)
        return;
    here = yew_tab_row1_active(ed, entries, n);
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
        yew_tab_switch(ed, entries[target].payload);
    else
        yew_group_enter_at_edge(ed, (u32)(-entries[target].payload),
                                delta);
}

CmdStatus yew_file_cmd_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_file_step(cx->ed, 1);
    return YEW_CMD_OK;
}

CmdStatus yew_file_cmd_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_file_step(cx->ed, -1);
    return YEW_CMD_OK;
}

/*
 * Explicit enter: the group under the cursor on row 1, or — when the
 * active tab is ungrouped — the nearest group to its right, so the
 * command is reachable without a mouse.
 */
CmdStatus yew_group_cmd_enter(CmdCtx *cx)
{
    Ed *ed;
    int i;
    int n;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    if (yew_active_group_id(ed) != 0U)
        return YEW_CMD_OK; /* already inside it */
    n = (int)yew_tab_count(ed);
    for (i = ed->tabs.active + 1; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != 0U) {
            yew_group_enter(ed, ed->tabs.v.data[i].group_id);
            return YEW_CMD_OK;
        }
    }
    for (i = 0; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != 0U) {
            yew_group_enter(ed, ed->tabs.v.data[i].group_id);
            return YEW_CMD_OK;
        }
    }
    yew_msg(ed, YEW_MSG_ERROR, "no tab groups");
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_group_cmd_leave(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (yew_active_group_id(cx->ed) == 0U) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "not in a tab group");
        return YEW_CMD_ERR_STATE;
    }
    return yew_group_leave(cx->ed) ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_group_cmd_dissolve(CmdCtx *cx)
{
    u32 gid;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    gid = yew_active_group_id(cx->ed);
    if (gid == 0U) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "not in a tab group");
        return YEW_CMD_ERR_STATE;
    }
    /* The tabs stay open and become ungrouped; dissolving a group is
     * not a way to close files. */
    yew_group_dissolve(cx->ed, gid);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_group_cmd_remove_tab(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (yew_active_group_id(cx->ed) == 0U) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "not in a tab group");
        return YEW_CMD_ERR_STATE;
    }
    yew_group_remove_member(cx->ed, cx->ed->tabs.active);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

/* ---------------------------------------------------------------- */
/* Sprint 27 §5/§8: the group menu's rows, as registry commands     */
/* ---------------------------------------------------------------- */

/*
 * Adds the ACTIVE tab to the group whose label starts with `name`.
 *
 * This exists to close invariant 9's audit for "drop a tab into a
 * group": the drag has to have a keyboard twin, and the twin has to run
 * Sprint 24's exact sequence rather than a second implementation of it.
 */
CmdStatus yew_group_cmd_add_tab(CmdCtx *cx)
{
    Ed *ed;
    u32 gid = 0U;
    size_t i;
    int tab_idx;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    tab_idx = ed->tabs.active;
    if (tab_idx < 0)
        return YEW_CMD_ERR_STATE;
    if (cx->sarg == NULL || cx->sarg_len == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "which group?");
        return YEW_CMD_ERR_ARG;
    }
    for (i = 0U; i < ed->groups.v.len; i++) {
        const TabGroup *g = &ed->groups.v.data[i];

        if (g->label != NULL && strlen(g->label) >= cx->sarg_len &&
            memcmp(g->label, cx->sarg, cx->sarg_len) == 0) {
            gid = g->id;
            break;
        }
    }
    if (gid == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "no such group");
        return YEW_CMD_ERR_ARG;
    }
    if (yew_tab_at(ed, tab_idx)->group_id == gid) {
        yew_msg(ed, YEW_MSG_ERROR, "already in that group");
        return YEW_CMD_ERR_STATE;
    }
    /* Sprint 24's sequence, in Sprint 24's order — the ordinal
     * off-by-one is its pinned pitfall and must not be reinvented. */
    if (yew_tab_at(ed, tab_idx)->group_id != 0U)
        yew_group_remove_member(ed, tab_idx);
    yew_group_add_member(ed, gid, tab_idx);
    yew_group_set_ordinal(ed, tab_idx, yew_group_member_count(ed, gid));
    {
        int members[YEW_TAB_MAX];
        int n = yew_group_members(ed, gid, members,
                                  (int)YEW_ARRAY_LEN(members));
        int lowest = -1;
        int k;

        for (k = 0; k < n; k++) {
            if (lowest < 0 || members[k] < lowest)
                lowest = members[k];
        }
        if (lowest >= 0)
            yew_group_reorder_block(ed, gid, lowest);
    }
    yew_state_mark_dirty(ed);
    ed->layout_dirty = true;
    ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_group_cmd_rename(CmdCtx *cx)
{
    Ed *ed;
    u32 gid;
    TabGroup *g;
    char *nu;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    gid = yew_active_group_id(ed);
    if (gid == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "not in a tab group");
        return YEW_CMD_ERR_STATE;
    }
    g = yew_group_at(ed, gid);
    if (g == NULL)
        return YEW_CMD_ERR_STATE;
    if (cx->sarg == NULL || cx->sarg_len == 0U) {
        /*
         * No argument: hand the user the command line already holding
         * the current name, so renaming is an edit rather than a
         * retype.  The menu row takes this path.
         */
        char seed[128];

        (void)snprintf(seed, sizeof(seed), "grename %s",
                       g->label == NULL ? "" : g->label);
        yew_cmdline_open(ed, YEW_PROMPT_CMD, seed);
        return YEW_CMD_OK;
    }
    nu = yew_xmalloc(cx->sarg_len + 1U);
    (void)memcpy(nu, cx->sarg, cx->sarg_len);
    nu[cx->sarg_len] = '\0';
    free(g->label);
    g->label = nu;
    yew_state_mark_dirty(ed);
    ed->full_damage = true;
    return YEW_CMD_OK;
}
