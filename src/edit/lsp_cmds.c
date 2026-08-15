#include "edit/lsp_cmds.h"

#include "mod/lsp/lsp.h"
#include "ui/win.h"

static CmdStatus status_of(bool ok)
{
    return ok ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_lsp_cmd_info(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_info(cx->ed));
}

CmdStatus yew_lsp_cmd_log(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_log(cx->ed));
}

CmdStatus yew_lsp_cmd_start(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_start(cx->ed, cx->win->buf));
}

CmdStatus yew_lsp_cmd_stop(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_stop(cx->ed, cx->win->buf));
}

CmdStatus yew_lsp_cmd_diagnostics(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_diagnostics(cx->ed));
}

CmdStatus yew_lsp_cmd_diag_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_diag_step(cx->ed, cx->win, true));
}

CmdStatus yew_lsp_cmd_diag_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_diag_step(cx->ed, cx->win, false));
}

CmdStatus yew_lsp_cmd_require(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    return status_of(yew_lsp_require(cx->ed));
}
