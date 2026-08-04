#include "edit/mode.h"

#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keys_highlight.h"
#include "edit/motion.h"
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
    case SAG_MODE_F:
        return "52";
    case SAG_MODE_L:
    case SAG_MODE_W:
    case SAG_MODE_B:
    case SAG_MODE_H:
    case SAG_MODE_I:
    case SAG_MODE_E:
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
    if (mode == SAG_MODE_H) {
        Mode unit = ed->mode == SAG_MODE_I ? SAG_MODE_I : ed->prev_unit;

        return sag_mode_enter_highlight(ed, unit, false);
    }
    if (mode == SAG_MODE_E) {
        const char *seed = NULL;

        if (ed->mode == SAG_MODE_I)
            sag_ed_insert_barrier(ed);
        if (ed->mode == SAG_MODE_H && ed->win != NULL &&
            ed->win->cs.curs.len != 0U &&
            ed->win->cs.primary < ed->win->cs.curs.len &&
            ed->win->cs.curs.data[ed->win->cs.primary].anchor.v !=
                ed->win->cs.curs.data[ed->win->cs.primary].pos.v)
            seed = "'<,'>";
        sag_cmdline_open(ed, SAG_PROMPT_CMD, seed);
        return SAG_CMD_OK;
    }
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
    ed->footer_dirty = true;
    return SAG_CMD_OK;
}

CmdStatus sag_mode_enter_highlight(Ed *ed, Mode unit, bool sticky)
{
    const UnitOps *ops;

    if (ed == NULL || ed->win == NULL)
        return SAG_CMD_ERR_STATE;
    if (unit == SAG_MODE_I)
        ops = &sag_unit_char;
    else
        ops = sag_unit_of_mode(unit);
    if (ops == NULL)
        return SAG_CMD_ERR_ARG;
    if (ed->mode == SAG_MODE_I) {
        size_t i;

        sag_ed_insert_barrier(ed);
        if (ed->win->buf != NULL && ed->win->buf->tb != NULL) {
            for (i = 0U; i < ed->win->cs.curs.len; i++)
                sag_cursor_clamp(ed->win->buf->tb,
                                 &ed->win->cs.curs.data[i]);
        }
    }
    ed->win->h.from = unit;
    ed->win->h.unit = ops;
    ed->win->h.kind = SAG_SEL_CHAR;
    ed->win->h.sticky = sticky;
    sag_keys_highlight_install(ed, unit);
    sag_dispatch_set_mode(ed, SAG_MODE_H);
    ed->footer_dirty = true;
    return SAG_CMD_OK;
}

CmdStatus sag_mode_escape(Ed *ed)
{
    size_t i;

    if (ed == NULL)
        return SAG_CMD_ERR_ARG;
    ed->chord.n = 0U;
    ed->chord.layer = -1;
    ed->chord.count = 0U;
    ed->chord.count_given = false;
    ed->chord.deadline = 0;
    if (ed->cmdline.active) {
        sag_cmdline_close(ed, false);
        return SAG_CMD_OK;
    }
    if (ed->prompt != SAG_PROMPT_NONE) {
        ed->prompt = SAG_PROMPT_NONE;
        sag_msg_clear(ed);
    }
    if (ed->mode == SAG_MODE_H && ed->win != NULL &&
        ed->win->cs.curs.len != 0U) {
        for (i = 0U; i < ed->win->cs.curs.len; i++)
            ed->win->cs.curs.data[i].anchor =
                ed->win->cs.curs.data[i].pos;
        sag_cset_remove_all_but_primary(&ed->win->cs);
        sag_selstack_clear(ed->win);
        sag_ed_damage_document(ed);
    }
    return sag_mode_enter(ed, SAG_MODE_L);
}
