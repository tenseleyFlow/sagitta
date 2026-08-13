#ifndef YEW_EDIT_LSP_CMDS_H
#define YEW_EDIT_LSP_CMDS_H

#include "edit/cmd.h"

CmdStatus yew_lsp_cmd_info(CmdCtx *cx);
CmdStatus yew_lsp_cmd_log(CmdCtx *cx);
CmdStatus yew_lsp_cmd_start(CmdCtx *cx);
CmdStatus yew_lsp_cmd_require(CmdCtx *cx);

#endif
