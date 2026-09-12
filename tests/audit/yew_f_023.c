/*
 * YEW-F-023 — plugin commands cannot enter the recorder's CMDWORD space.
 *
 * Correct behavior: Sprint 58 F14 q8 requires a plugin-registered command
 * to be recordable and replayable, with its CMDWORD checked for collisions
 * against core commands.
 *
 * Baseline failure: plugin registration always creates an unrecordable
 * descriptor with a NULL word.  It consequently accepts a plugin command
 * whose local word is the core recorder word "up".
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/cmd.h"

static CmdStatus plugin_noop(CmdCtx *cx)
{
    (void)cx;
    return YEW_CMD_OK;
}

bool test_yew_f_023(char *why, size_t why_cap)
{
    char error[128];
    CmdId pulse = YEW_CMD_NONE;
    CmdId collision = YEW_CMD_NONE;
    CmdId core_up;
    const CmdDesc *desc;
    bool pulse_registered;
    bool collision_registered;
    bool recordable;
    bool named;

    yew_cmd_init();
    core_up = yew_cmd_by_word("up", 2U);
    pulse_registered = yew_cmd_register_plugin(
        "auditrec", "pulse", plugin_noop, "Audit recorder command",
        &pulse, error, sizeof(error));
    desc = pulse_registered ? yew_cmd_desc(pulse) : NULL;
    recordable = desc != NULL &&
                 (desc->flags & YEW_CMD_RECORDABLE) != 0U;
    named = desc != NULL && desc->word != NULL &&
            strcmp(desc->word, "pulse") == 0;
    collision_registered = yew_cmd_register_plugin(
        "auditrec", "up", plugin_noop, "Audit collision command",
        &collision, error, sizeof(error));

    if (collision_registered)
        (void)yew_cmd_unregister(collision);
    if (pulse_registered)
        (void)yew_cmd_unregister(pulse);

    if (core_up.v == 0U || !recordable || !named || collision_registered)
        (void)snprintf(why, why_cap,
                       "core-up=%u recordable=%u word=%u collision=%u",
                       core_up.v != 0U ? 1U : 0U,
                       recordable ? 1U : 0U, named ? 1U : 0U,
                       collision_registered ? 1U : 0U);
    return core_up.v != 0U && recordable && named &&
           !collision_registered;
}
