#include "edit/file_cmds.h"

#include "edit/ed.h"
#include "ui/viewport.h"

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

CmdStatus sag_file_cmd_write(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->sarg != NULL && cx->sarg[0] != '\0')
        return sag_ed_file_write_to(cx->ed, cx->sarg, cx->bang);
    return sag_file_cmd_save(cx->ed, cx->bang);
}

CmdStatus sag_file_cmd_write_quit(CmdCtx *cx)
{
    CmdStatus status = sag_file_cmd_write(cx);

    if (status != SAG_CMD_OK)
        return status;
    return sag_ed_request_quit(cx->ed, false);
}

CmdStatus sag_file_cmd_new(CmdCtx *cx)
{
    SagLoadErr load;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (sag_buf_dirty(&cx->ed->buffer) && !cx->bang) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "buffer has unsaved changes (use :new!)");
        return SAG_CMD_ERR_STATE;
    }
    if (cx->sarg == NULL || cx->sarg[0] == '\0')
        return sag_ed_open_scratch(cx->ed) ? SAG_CMD_OK : SAG_CMD_ERR_IO;
    load = sag_ed_open(cx->ed, cx->sarg);
    return load == SAG_LOAD_OK || load == SAG_LOAD_ENOENT ? SAG_CMD_OK :
                                                           SAG_CMD_ERR_IO;
}

CmdStatus sag_file_cmd_reload(CmdCtx *cx)
{
    SagLoadErr load;
    const char *path;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    path = sag_ed_doc(cx->ed)->path;
    if (path == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "buffer has no file name");
        return SAG_CMD_ERR_STATE;
    }
    if (sag_buf_dirty(&cx->ed->buffer) && !cx->bang) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "buffer has unsaved changes (use :reload!)");
        return SAG_CMD_ERR_STATE;
    }
    load = sag_ed_open(cx->ed, path);
    return load == SAG_LOAD_OK ? SAG_CMD_OK : SAG_CMD_ERR_IO;
}

CmdStatus sag_file_cmd_quit(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_ed_request_quit(cx->ed, cx->bang);
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
    if (cx->ed->win != NULL)
        sag_vp_invalidate(cx->ed->win);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}
