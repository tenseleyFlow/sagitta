#include "edit/mode.h"

#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keys_highlight.h"
#include "edit/motion.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "util/log.h"

static void mode_transition(Ed *ed, Mode mode)
{
    Mode old = ed->mode;

    if (old == mode)
        return;
    yew_fl_hook_mode(ed, FL_EV_MODE_LEAVE, yew_modes[old].name);
    yew_dispatch_set_mode(ed, mode);
    yew_fl_hook_mode(ed, FL_EV_MODE_ENTER, yew_modes[mode].name);
}

const ModeDesc yew_modes[YEW_MODE__N] = {
    [YEW_MODE_L] = {"L", "line", true, true, YEW_MODE_L},
    [YEW_MODE_W] = {"W", "word", true, true, YEW_MODE_W},
    [YEW_MODE_B] = {"B", "block", true, true, YEW_MODE_B},
    [YEW_MODE_H] = {"H", "highlight", false, true, YEW_MODE_H},
    [YEW_MODE_I] = {"I", "insert", false, false, YEW_MODE_I},
    [YEW_MODE_E] = {"E", "execute", false, false, YEW_MODE_E},
    [YEW_MODE_F] = {"F", "fuss", false, false, YEW_MODE_F},
};

static const char *mode_sprint(Mode mode)
{
    switch (mode) {
    case YEW_MODE_F:
        return "52";
    case YEW_MODE_L:
    case YEW_MODE_W:
    case YEW_MODE_B:
    case YEW_MODE_H:
    case YEW_MODE_I:
    case YEW_MODE_E:
    case YEW_MODE__N:
        break;
    }
    return NULL;
}

CmdStatus yew_mode_enter(Ed *ed, Mode mode)
{
    const char *sprint;
    size_t i;

    if (ed == NULL || mode >= YEW_MODE__N)
        return YEW_CMD_ERR_ARG;
    if (mode == YEW_MODE_H) {
        Mode unit = ed->mode == YEW_MODE_I ? YEW_MODE_I : ed->prev_unit;

        return yew_mode_enter_highlight(ed, unit, false);
    }
    if (ed->mode != mode && ed->win != NULL)
        yew_shadow_dismiss(ed, ed->win);
    if (mode == YEW_MODE_E) {
        const char *seed = NULL;
        if (ed->mode == YEW_MODE_I)
            yew_ed_insert_barrier(ed);
        if (ed->mode == YEW_MODE_H && ed->win != NULL &&
            ed->win->cs.curs.len != 0U &&
            ed->win->cs.primary < ed->win->cs.curs.len &&
            ed->win->cs.curs.data[ed->win->cs.primary].anchor.v !=
                ed->win->cs.curs.data[ed->win->cs.primary].pos.v)
            seed = "'<,'>";
        yew_cmdline_open(ed, YEW_PROMPT_CMD, seed);
        return YEW_CMD_OK;
    }
    sprint = mode_sprint(mode);
    if (sprint != NULL) {
        yew_msg(ed, YEW_MSG_ERROR,
                "%s mode lands in Sprint %s", yew_modes[mode].name, sprint);
        yew_log(YEW_LOG_ERROR,
                "command not implemented yet: %s mode lands in Sprint %s",
                yew_modes[mode].name, sprint);
        return YEW_CMD_ERR_DEFERRED;
    }
    if (ed->mode == YEW_MODE_I && mode != YEW_MODE_I) {
        yew_ed_insert_barrier(ed);
        if (ed->win != NULL && ed->win->buf != NULL &&
            ed->win->buf->tb != NULL) {
            for (i = 0U; i < ed->win->cs.curs.len; i++)
                yew_cursor_clamp(ed->win->buf->tb,
                                 &ed->win->cs.curs.data[i]);
        }
    }
    if (yew_modes[mode].is_unit)
        ed->prev_unit = mode;
    mode_transition(ed, mode);
    ed->footer_dirty = true;
    return YEW_CMD_OK;
}

CmdStatus yew_mode_enter_highlight(Ed *ed, Mode unit, bool sticky)
{
    const UnitOps *ops;

    if (ed == NULL || ed->win == NULL)
        return YEW_CMD_ERR_STATE;
    if (ed->mode != YEW_MODE_H)
        yew_shadow_dismiss(ed, ed->win);
    if (unit == YEW_MODE_I)
        ops = &yew_unit_char;
    else
        ops = yew_unit_of_mode(unit);
    if (ops == NULL)
        return YEW_CMD_ERR_ARG;
    if (ed->mode == YEW_MODE_I) {
        size_t i;

        yew_ed_insert_barrier(ed);
        if (ed->win->buf != NULL && ed->win->buf->tb != NULL) {
            for (i = 0U; i < ed->win->cs.curs.len; i++)
                yew_cursor_clamp(ed->win->buf->tb,
                                 &ed->win->cs.curs.data[i]);
        }
    }
    ed->win->h.from = unit;
    ed->win->h.unit = ops;
    ed->win->h.kind = YEW_SEL_CHAR;
    ed->win->h.sticky = sticky;
    yew_keys_highlight_install(ed, unit);
    mode_transition(ed, YEW_MODE_H);
    ed->footer_dirty = true;
    return YEW_CMD_OK;
}

CmdStatus yew_mode_escape(Ed *ed)
{
    size_t i;

    if (ed == NULL)
        return YEW_CMD_ERR_ARG;
    ed->chord.n = 0U;
    ed->chord.layer = -1;
    ed->chord.count = 0U;
    ed->chord.count_given = false;
    ed->chord.deadline = 0;
    if (ed->cmdline.active) {
        yew_cmdline_close(ed, false);
        return YEW_CMD_OK;
    }
    if (ed->prompt != YEW_PROMPT_NONE) {
        ed->prompt = YEW_PROMPT_NONE;
        yew_msg_clear(ed);
    }
    if (ed->mode == YEW_MODE_H && ed->win != NULL &&
        ed->win->cs.curs.len != 0U) {
        for (i = 0U; i < ed->win->cs.curs.len; i++)
            ed->win->cs.curs.data[i].anchor =
                ed->win->cs.curs.data[i].pos;
        yew_cset_remove_all_but_primary(&ed->win->cs);
        yew_selstack_clear(ed->win);
        yew_ed_damage_document(ed);
    }
    return yew_mode_enter(ed, YEW_MODE_L);
}
