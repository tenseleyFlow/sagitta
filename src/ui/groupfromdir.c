#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ui/groupfromdir.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#if YEW_WITH_FUSS
#include "mod/git/fussmode.h"
#endif
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/tabs.h"
#include "util/log.h"
#include "ws/walk.h"

static char *group_path_join(const char *root, const char *relative)
{
    size_t root_len;
    size_t relative_len;
    bool slash;
    char *path;

    if (root == NULL || relative == NULL || relative[0] == '\0')
        return NULL;
    root_len = strlen(root);
    relative_len = strlen(relative);
    slash = root_len != 0U && root[root_len - 1U] != '/';
    if (root_len > SIZE_MAX - relative_len - (slash ? 2U : 1U))
        return NULL;
    path = yew_xmalloc(root_len + relative_len + (slash ? 2U : 1U));
    if (root_len != 0U)
        (void)memcpy(path, root, root_len);
    if (slash)
        path[root_len++] = '/';
    (void)memcpy(path + root_len, relative, relative_len + 1U);
    return path;
}

static bool group_picker_open(Ed *ed, const char *root,
                              const FileList *files)
{
    size_t i;

    if (!yew_gp_show(ed, root))
        return false;
    for (i = 0U; i < files->paths.len; i++) {
        char *path = group_path_join(root, files->paths.data[i]);

        if (path != NULL) {
            yew_gp_preselect(path);
            yew_xfree(path);
        }
    }
    return true;
}

static u32 group_open_members(Ed *ed, const char *root,
                              const GroupFromDirOpts *opts,
                              const FileList *files)
{
    int members[YEW_GROUP_MAX_MEMBERS];
    u32 gid;
    size_t i;
    int n;

    gid = yew_group_create(ed, root, opts->label);
    if (gid == 0U)
        return 0U;
    for (i = 0U; i < files->paths.len; i++) {
        char *path = group_path_join(root, files->paths.data[i]);
        int idx;
        bool opened = false;

        if (path == NULL)
            continue;
        idx = yew_tab_find_by_path(ed, path);
        if (idx < 0) {
            idx = yew_tab_open(ed, path);
            opened = idx >= 0;
        }
        yew_xfree(path);
        if (idx < 0)
            continue;
        /* yew_group_add_member removes an adopted tab from its old group
         * before appending it here, preserving both ordinal sequences. */
        yew_group_add_member(ed, gid, idx);
        if (opened)
            yew_tab_defer(ed, idx);
    }
    n = yew_group_members(ed, gid, members,
                          (int)YEW_ARRAY_LEN(members));
    if (n == 0) {
        yew_group_dissolve(ed, gid);
        yew_msg(ed, YEW_MSG_ERROR, "could not open files from %s", root);
        return 0U;
    }
    /* Switching is the hydration boundary: all new members remain
     * deferred except the first, which costs exactly one file read. */
    yew_tab_switch(ed, members[0]);
    yew_group_note_position(ed);
    return gid;
}

u32 yew_group_from_dir(Ed *ed, const char *dir,
                       const GroupFromDirOpts *input)
{
    GroupFromDirOpts opts = {
        true, true, YEW_GROUP_MAX_MEMBERS, NULL
    };
    WalkOpts walk_opts = {0};
    FileList files;
    WalkState *walk;
    char *root;
    u32 gid = 0U;

    if (ed == NULL || dir == NULL || dir[0] == '\0')
        return 0U;
    if (input != NULL)
        opts = *input;
    if (opts.max_members == 0U ||
        opts.max_members > YEW_GROUP_MAX_MEMBERS)
        opts.max_members = YEW_GROUP_MAX_MEMBERS;

    root = yew_xrealpath(dir);
    if (root == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "cannot open directory %s", dir);
        return 0U;
    }
    walk_opts.follow_symlinks = false;
    walk_opts.use_gitignore = opts.respect_ignore;
    walk_opts.include_dirs = false;
    walk_opts.max_depth = opts.recursive ? 0U : 1U;
    walk_opts.max_entries = (u64)opts.max_members + 1U;
    yew_filelist_init(&files);
    walk = yew_walk_begin(root, &walk_opts, &files);
    if (walk == NULL) {
        yew_filelist_free(&files);
        yew_msg(ed, YEW_MSG_ERROR, "cannot scan directory %s", root);
        yew_xfree(root);
        return 0U;
    }
    (void)yew_walk_step(walk, 0);
    yew_walk_end(walk);

    if (files.n_files == 0U) {
        yew_msg(ed, YEW_MSG_INFO, "no files in %s", root);
    } else if (files.truncated ||
               files.paths.len > (size_t)opts.max_members) {
        if (!group_picker_open(ed, root, &files))
            yew_msg(ed, YEW_MSG_ERROR, "cannot open group picker");
    } else {
        gid = group_open_members(ed, root, &opts, &files);
    }
    yew_filelist_free(&files);
    yew_xfree(root);
    return gid;
}

CmdStatus yew_group_cmd_from_dir(CmdCtx *cx)
{
    GroupFromDirOpts opts = {
        true, true, YEW_GROUP_MAX_MEMBERS, NULL
    };
    char *selected = NULL;
    const char *dir;
    u32 gid;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    dir = cx->sarg;
    if (dir == NULL || cx->sarg_len == 0U) {
#if YEW_WITH_FUSS
        selected = yew_fuss_selected_directory(cx);
#endif
        dir = selected;
    }
    if (dir == NULL || dir[0] == '\0') {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "select a directory to open as a group");
        yew_xfree(selected);
        return YEW_CMD_ERR_ARG;
    }
    gid = yew_group_from_dir(cx->ed, dir, &opts);
    yew_xfree(selected);
    if (gid != 0U || yew_gp_active())
        return YEW_CMD_OK;
    return YEW_CMD_ERR_STATE;
}
