#include "edit/shadow_cmds.h"

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/shadow.h"

static CmdStatus accept_status(bool accepted)
{
    return accepted ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_shadow_cmd_accept_word(CmdCtx *cx)
{
    return accept_status(cx != NULL &&
                         yew_shadow_accept_word(cx->ed, cx->win, false));
}

CmdStatus yew_shadow_cmd_accept_word_alt(CmdCtx *cx)
{
    return accept_status(cx != NULL &&
                         yew_shadow_accept_word(cx->ed, cx->win, true));
}

CmdStatus yew_shadow_cmd_accept_line(CmdCtx *cx)
{
    return accept_status(cx != NULL &&
                         yew_shadow_accept_line(cx->ed, cx->win));
}

CmdStatus yew_shadow_cmd_accept_all(CmdCtx *cx)
{
    return accept_status(cx != NULL &&
                         yew_shadow_accept_all(cx->ed, cx->win));
}

CmdStatus yew_shadow_cmd_dismiss(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    if (cx->win->shadow.live) {
        yew_shadow_dismiss(cx->ed, cx->win);
        return YEW_CMD_OK;
    }
    return yew_mode_escape(cx->ed);
}
