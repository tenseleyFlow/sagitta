#ifndef YEW_EDIT_LSP_CMDS_H
#define YEW_EDIT_LSP_CMDS_H

#include "edit/cmd.h"

CmdStatus yew_lsp_cmd_info(CmdCtx *cx);
CmdStatus yew_lsp_cmd_log(CmdCtx *cx);
CmdStatus yew_lsp_cmd_start(CmdCtx *cx);
CmdStatus yew_lsp_cmd_stop(CmdCtx *cx);
CmdStatus yew_lsp_cmd_diagnostics(CmdCtx *cx);
CmdStatus yew_lsp_cmd_diag_next(CmdCtx *cx);
CmdStatus yew_lsp_cmd_diag_prev(CmdCtx *cx);
CmdStatus yew_lsp_cmd_complete(CmdCtx *cx);
CmdStatus yew_lsp_cmd_hover(CmdCtx *cx);
CmdStatus yew_lsp_cmd_signature(CmdCtx *cx);
CmdStatus yew_lsp_cmd_goto_def(CmdCtx *cx);
CmdStatus yew_lsp_cmd_goto_decl(CmdCtx *cx);
CmdStatus yew_lsp_cmd_goto_type(CmdCtx *cx);
CmdStatus yew_lsp_cmd_goto_impl(CmdCtx *cx);
CmdStatus yew_lsp_cmd_references(CmdCtx *cx);
CmdStatus yew_lsp_cmd_rename(CmdCtx *cx);
CmdStatus yew_lsp_cmd_symbols(CmdCtx *cx);
CmdStatus yew_lsp_cmd_require(CmdCtx *cx);

#endif
