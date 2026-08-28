#include "mod/plug/plug.h"
#include "mod/plug/pkg.h"

#include <stdio.h>

#include "edit/ed.h"
#include "mod/mods.h"
#include "ui/message.h"
#include "util/base.h"

static CmdStatus plug_cmd_unavailable(CmdCtx *cx)
{
    char err[160];

    if (cx != NULL && cx->ed != NULL &&
        !yew_mod_require(YEW_MOD_PLUGINS, err, sizeof(err)))
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", err);
    return YEW_CMD_ERR_STATE;
}

static int plug_main_unavailable(void)
{
    char err[160];

    if (!yew_mod_require(YEW_MOD_PLUGINS, err, sizeof(err)))
        (void)fprintf(stderr, "%s\n", err);
    return YEW_EXIT_ERR;
}

CmdStatus yew_plug_cmd_list(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_enable(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_disable(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_reload(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_info(CmdCtx *cx) { return plug_cmd_unavailable(cx); }

int yew_plug_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return plug_main_unavailable();
}

int yew_pkg_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return plug_main_unavailable();
}
