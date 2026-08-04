#ifndef SAG_EDIT_FILE_CMDS_H
#define SAG_EDIT_FILE_CMDS_H

#include <stdbool.h>

#include "edit/cmd.h"

CmdStatus sag_file_cmd_save(Ed *ed, bool force);
CmdStatus sag_file_cmd_save_current(CmdCtx *cx);
CmdStatus sag_file_cmd_write(CmdCtx *cx);
CmdStatus sag_file_cmd_write_quit(CmdCtx *cx);
CmdStatus sag_file_cmd_new(CmdCtx *cx);
CmdStatus sag_file_cmd_reload(CmdCtx *cx);
CmdStatus sag_file_cmd_quit(CmdCtx *cx);
CmdStatus sag_file_cmd_quit_force(CmdCtx *cx);
CmdStatus sag_file_cmd_suspend(CmdCtx *cx);
CmdStatus sag_file_cmd_redraw(CmdCtx *cx);

#endif
