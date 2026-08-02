#include "edit/mode.h"

#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "util/log.h"

const ModeDesc sag_modes[SAG_MODE__N] = {
    [SAG_MODE_L] = {"L", "line", true, true, SAG_MODE_L},
    [SAG_MODE_W] = {"W", "word", true, true, SAG_MODE_W},
    [SAG_MODE_B] = {"B", "block", true, true, SAG_MODE_B},
    [SAG_MODE_H] = {"H", "highlight", false, true, SAG_MODE_H},
    [SAG_MODE_I] = {"I", "insert", false, false, SAG_MODE_I},
    [SAG_MODE_E] = {"E", "execute", false, false, SAG_MODE_E},
    [SAG_MODE_F] = {"F", "fuss", false, false, SAG_MODE_F},
};

static const char *mode_sprint(Mode mode)
{
    switch (mode) {
    case SAG_MODE_W:
    case SAG_MODE_B:
        return "16";
    case SAG_MODE_H:
        return "17";
    case SAG_MODE_E:
        return "18";
    case SAG_MODE_F:
        return "52";
    case SAG_MODE_L:
    case SAG_MODE_I:
    case SAG_MODE__N:
        break;
    }
    return NULL;
}

CmdStatus sag_mode_enter(Ed *ed, Mode mode)
{
    const char *sprint;
    size_t i;

    if (ed == NULL || mode >= SAG_MODE__N)
        return SAG_CMD_ERR_ARG;
    sprint = mode_sprint(mode);
    if (sprint != NULL) {
        sag_msg(ed, SAG_MSG_ERROR,
                "%s mode lands in Sprint %s", sag_modes[mode].name, sprint);
        sag_log(SAG_LOG_ERROR,
                "command not implemented yet: %s mode lands in Sprint %s",
                sag_modes[mode].name, sprint);
        return SAG_CMD_ERR_DEFERRED;
    }
    if (ed->mode == SAG_MODE_I && mode != SAG_MODE_I) {
        sag_ed_insert_barrier(ed);
        if (ed->win != NULL && ed->win->buf != NULL &&
            ed->win->buf->tb != NULL) {
            for (i = 0U; i < ed->win->cs.curs.len; i++)
                sag_cursor_clamp(ed->win->buf->tb,
                                 &ed->win->cs.curs.data[i]);
        }
    }
    if (sag_modes[mode].is_unit)
        ed->prev_unit = mode;
    sag_dispatch_set_mode(ed, mode);
    ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_mode_escape(Ed *ed)
{
    if (ed == NULL)
        return SAG_CMD_ERR_ARG;
    ed->chord.n = 0U;
    ed->chord.layer = -1;
    ed->chord.count = 0U;
    ed->chord.count_given = false;
    ed->chord.deadline = 0;
    if (ed->prompt != SAG_PROMPT_NONE) {
        ed->prompt = SAG_PROMPT_NONE;
        sag_msg_clear(ed);
    }
    return sag_mode_enter(ed, SAG_MODE_L);
}
