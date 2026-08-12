#include "edit/shadow_cmds.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/option.h"
#include "edit/shadow.h"
#include "ui/message.h"

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

CmdStatus yew_shadow_cmd_next(CmdCtx *cx)
{
    return accept_status(cx != NULL &&
                         yew_shadow_next(cx->ed, cx->win));
}

CmdStatus yew_shadow_cmd_prev(CmdCtx *cx)
{
    return accept_status(cx != NULL &&
                         yew_shadow_prev(cx->ed, cx->win));
}

CmdStatus yew_shadow_cmd_toggle(CmdCtx *cx)
{
    const OptProvider *provider;
    OptVal current;
    OptVal next;
    const char *error = NULL;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    provider = yew_opt_provider(cx->ed);
    if (!provider->get(cx->ed, "shadow.enable", 13U, &current) ||
        current.type != (u8)YEW_OPT_BOOL)
        return YEW_CMD_ERR_STATE;
    next = (OptVal){YEW_OPT_BOOL, {.b = !current.as.b}};
    if (!provider->set(cx->ed, "shadow.enable", 13U, &next, &error)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s",
                error == NULL ? "could not toggle shadow text" : error);
        return YEW_CMD_ERR_STATE;
    }
    if (next.as.b && cx->ed->win != NULL)
        yew_shadow_arm(cx->ed, cx->ed->win);
    yew_msg(cx->ed, YEW_MSG_INFO, "shadow text %s",
            next.as.b ? "enabled" : "disabled");
    return YEW_CMD_OK;
}

CmdStatus yew_shadow_cmd_stats(CmdCtx *cx)
{
    char status[512];

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_shadow_stats_format(cx->ed, status, sizeof(status));
    yew_msg(cx->ed, YEW_MSG_INFO, "%s", status);
    return YEW_CMD_OK;
}
