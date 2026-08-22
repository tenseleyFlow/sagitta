#include "ui/groupfromdir.h"
#include "ui/groups.h"
#include "ui/grouppicker.h"
#include "ui/tabs.h"
#include "ws/walk.h"
#include "edit/ed.h"
#include <string.h>

u32 yew_group_from_dir(Ed *ed, const char *dir, const GroupFromDirOpts *in)
{
    GroupFromDirOpts o = {true, true, 200U, NULL};
    WalkOpts wo = {0};
    FileList files;
    WalkState *walk;
    u32 gid;
    size_t i;
    if (ed == NULL || dir == NULL || dir[0] == '\0') return 0U;
    if (in != NULL) o = *in;
    if (o.max_members == 0U) o.max_members = 200U;
    wo.use_gitignore = o.respect_ignore; wo.include_dirs = false;
    wo.max_entries = (u64)o.max_members + 1U;
    yew_filelist_init(&files);
    walk = yew_walk_begin(dir, &wo, &files);
    if (walk == NULL) { yew_filelist_free(&files); return 0U; }
    (void)yew_walk_step(walk, 0); yew_walk_end(walk);
    if (files.n_files == 0U) { yew_filelist_free(&files); return 0U; }
    if (files.truncated || files.paths.len > (size_t)o.max_members) {
        (void)yew_gp_show(ed, dir);
        for (i = 0; i < files.paths.len; i++)
            yew_gp_preselect(files.paths.data[i]);
        yew_filelist_free(&files); return 0U;
    }
    gid = yew_group_create(ed, dir, o.label);
    if (gid == 0U) { yew_filelist_free(&files); return 0U; }
    for (i = 0; i < files.paths.len; i++) {
        int idx = yew_tab_find_by_path(ed, files.paths.data[i]);
        if (idx < 0) idx = yew_tab_open(ed, files.paths.data[i]);
        if (idx >= 0) { yew_group_add_member(ed, gid, idx); yew_tab_defer(ed, idx); }
    }
    { int *members = yew_xcalloc(files.paths.len, sizeof(*members)); int n = yew_group_members(ed, gid, members, (int)files.paths.len); if (n > 0) { yew_tab_switch(ed, members[0]); (void)yew_tab_hydrate(ed, members[0]); } free(members); }
    yew_filelist_free(&files); return gid;
}

CmdStatus yew_group_cmd_from_dir(CmdCtx *cx)
{
    GroupFromDirOpts o = {true, true, 200U, NULL};
    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL || cx->sarg_len == 0U) return YEW_CMD_ERR_ARG;
    return yew_group_from_dir(cx->ed, cx->sarg, &o) != 0U ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}
