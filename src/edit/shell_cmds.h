#ifndef SAG_EDIT_SHELL_CMDS_H
#define SAG_EDIT_SHELL_CMDS_H

#include "edit/cmd.h"

CmdStatus sag_shell_cmd_run(CmdCtx *cx);
CmdStatus sag_shell_cmd_run_bg(CmdCtx *cx);
CmdStatus sag_shell_cmd_read(CmdCtx *cx);
CmdStatus sag_shell_cmd_filter(CmdCtx *cx);
CmdStatus sag_shell_cmd_term(CmdCtx *cx);

CmdStatus sag_job_cmd_list(CmdCtx *cx);
CmdStatus sag_job_cmd_kill(CmdCtx *cx);
CmdStatus sag_job_cmd_kill_force(CmdCtx *cx);
CmdStatus sag_job_cmd_jump(CmdCtx *cx);
CmdStatus sag_job_cmd_clear_finished(CmdCtx *cx);
CmdStatus sag_job_cmd_rerun(CmdCtx *cx);

#endif
