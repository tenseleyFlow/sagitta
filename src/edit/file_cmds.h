#ifndef YEW_EDIT_FILE_CMDS_H
#define YEW_EDIT_FILE_CMDS_H

#include <stdbool.h>

#include "edit/cmd.h"

CmdStatus yew_file_cmd_save(Ed *ed, bool force);
CmdStatus yew_file_cmd_save_current(CmdCtx *cx);
CmdStatus yew_file_cmd_write(CmdCtx *cx);
CmdStatus yew_file_cmd_write_quit(CmdCtx *cx);
CmdStatus yew_file_cmd_new(CmdCtx *cx);
CmdStatus yew_file_cmd_reload(CmdCtx *cx);
CmdStatus yew_file_cmd_quit(CmdCtx *cx);
CmdStatus yew_file_cmd_quit_force(CmdCtx *cx);
CmdStatus yew_file_cmd_suspend(CmdCtx *cx);
CmdStatus yew_file_cmd_redraw(CmdCtx *cx);
CmdStatus yew_file_cmd_buf_open(CmdCtx *cx);
CmdStatus yew_file_cmd_buf_close(CmdCtx *cx);

#endif
