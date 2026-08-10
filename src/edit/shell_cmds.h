#ifndef YEW_EDIT_SHELL_CMDS_H
#define YEW_EDIT_SHELL_CMDS_H

#include "edit/cmd.h"

CmdStatus yew_shell_cmd_run(CmdCtx *cx);
CmdStatus yew_shell_cmd_run_bg(CmdCtx *cx);
CmdStatus yew_shell_cmd_read(CmdCtx *cx);
CmdStatus yew_shell_cmd_filter(CmdCtx *cx);
CmdStatus yew_shell_cmd_term(CmdCtx *cx);

CmdStatus yew_job_cmd_list(CmdCtx *cx);
CmdStatus yew_job_cmd_kill(CmdCtx *cx);
CmdStatus yew_job_cmd_kill_force(CmdCtx *cx);
CmdStatus yew_job_cmd_jump(CmdCtx *cx);
CmdStatus yew_job_cmd_clear_finished(CmdCtx *cx);
CmdStatus yew_job_cmd_rerun(CmdCtx *cx);

#endif
