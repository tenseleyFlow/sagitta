#include "edit/file_cmds.h"

#include "edit/ed.h"

CmdStatus sag_file_cmd_save(Ed *ed, bool force)
{
    if (ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_file_save(ed, force);
}

CmdStatus sag_file_cmd_save_current(CmdCtx *cx)
{
    if (cx == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_file_cmd_save(cx->ed, false);
}

CmdStatus sag_file_cmd_quit(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_request_quit(cx->ed, false);
}

CmdStatus sag_file_cmd_quit_force(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_request_quit(cx->ed, true);
}

CmdStatus sag_file_cmd_suspend(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    sag_tty_suspend(&cx->ed->tty);
    cx->ed->layout_dirty = true;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_file_cmd_redraw(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}
