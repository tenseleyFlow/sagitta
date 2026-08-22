#ifndef YEW_UI_GROUPFROMDIR_H
#define YEW_UI_GROUPFROMDIR_H
#include "edit/cmd.h"

typedef struct Ed Ed;

enum {
    YEW_GROUP_MAX_MEMBERS = 200
};

typedef struct GroupFromDirOpts {
    bool recursive;
    bool respect_ignore;
    u32 max_members;
    const char *label;
} GroupFromDirOpts;

u32 yew_group_from_dir(Ed *ed, const char *dir, const GroupFromDirOpts *opts);
CmdStatus yew_group_cmd_from_dir(CmdCtx *cx);
#endif
