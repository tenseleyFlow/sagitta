#include "edit/shadow_cmds.h"

#include <string.h>

#include "edit/ed.h"
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

/*
 * Sprint 57.19: Alt+Right in Insert mode, in the shape of
 * `yew_cmdline_cmd_ghost_accept` -- accept the suggestion when one is
 * there, and be the ordinary motion when it is not, so the key is never
 * dead under the finger.
 *
 * "A suggestion is showing" is the shadow's OWN drawn predicate, the one
 * yew_shadow_draw tests: live, and not suppressed by an open completion
 * menu.  It is not a guess, and it is not "did accept_word succeed" --
 * a stale ghost's accept fails after dismissing and reporting, and the
 * user should not also be moved for having pressed the key.
 *
 * The name deliberately stays in the `ed.shadow.` namespace.  Naming it
 * `ed.move.*` would be fatal: yew_ed_invoke dismisses the ghost before
 * dispatching anything whose name begins "ed.move.", so the accepting
 * half could never run.
 *
 * Both halves go back through yew_ed_invoke rather than calling the
 * command function, so the recordable command that actually ran is the
 * one a macro captures -- this dispatcher is INTERNAL plumbing and has
 * no motion word of its own.
 */
CmdStatus yew_shadow_cmd_accept_or_word(CmdCtx *cx)
{
    bool showing;
    CmdId id;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    showing = cx->win->shadow.live && !cx->win->shadow.suppressed;
    id = showing ? yew_cmd_lookup("ed.shadow.accept_word", 21U)
                 : yew_cmd_lookup("ed.move.word.next", 17U);
    if (id.v == 0U)
        YEW_BUG("shadow accept-or-word: target command is missing");
    return yew_ed_invoke(cx->ed, id, cx);
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
    CmdId escape;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    if (cx->win->shadow.live) {
        yew_shadow_dismiss(cx->ed, cx->win);
        return YEW_CMD_OK;
    }
    escape = yew_cmd_lookup("ed.mode.escape", 14U);
    if (escape.v == 0U)
        YEW_BUG("shadow dismiss: mode escape command is missing");
    return yew_ed_invoke(cx->ed, escape, cx);
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
