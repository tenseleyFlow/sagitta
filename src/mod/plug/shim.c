#include "mod/plug/plug.h"

#include <stdio.h>

#include "edit/ed.h"
#include "ui/message.h"
#include "util/base.h"

static CmdStatus plug_cmd_unavailable(CmdCtx *cx)
{
    if (cx != NULL && cx->ed != NULL)
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "built without plugin support (MODULES=plugins)");
    return YEW_CMD_ERR_STATE;
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
    (void)fputs(yew_plug_module_error, stderr);
    return YEW_EXIT_ERR;
}
