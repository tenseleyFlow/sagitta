#include "edit/keys_highlight.h"

#include <stddef.h>

#include "edit/ed.h"
#include "util/log.h"

static const BindRow keys_H_L[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"A-<left>", "ed.move.unit.home_alt", 0, NULL},
    {"A-<right>", "ed.move.unit.end_alt", 0, NULL},
    {"A-<up>", "ed.move.unit.prev_alt", 0, NULL},
    {"A-<down>", "ed.move.unit.next_alt", 0, NULL},
    {"<home>", "ed.move.unit.home", 0, NULL},
    {"<end>", "ed.move.unit.end", 0, NULL},
};

static const BindRow keys_H_W[] = {
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"A-<left>", "ed.move.unit.prev_alt", 0, NULL},
    {"A-<right>", "ed.move.unit.next_alt", 0, NULL},
    {"C-<left>", "ed.move.word.sub_prev", 0, NULL},
    {"C-<right>", "ed.move.word.sub_next", 0, NULL},
    {"<home>", "ed.move.unit.home", 0, NULL},
    {"<end>", "ed.move.unit.end", 0, NULL},
};

static const BindRow keys_H_B[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"A-<left>", "ed.move.block.match_prev", 0, NULL},
    {"A-<right>", "ed.move.block.match_next", 0, NULL},
    {"A-<up>", "ed.sel.unit.expand", 0, NULL},
    {"A-<down>", "ed.sel.unit.contract", 0, NULL},
};

static const BindRow keys_H_C[] = {
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
};

static const BindRow keys_H[] = {
    {":", "ed.mode.enter", 0, "E"},
    {"y", "ed.sel.yank", 0, NULL},
    {"d", "ed.sel.delete", 0, NULL},
    {"<del>", "ed.sel.delete", 0, NULL},
    {"c", "ed.sel.change", 0, NULL},
    {"U", "ed.sel.case_upper", 0, NULL},
    {"u", "ed.sel.case_lower", 0, NULL},
    {"~", "ed.sel.case_toggle", 0, NULL},
    {">", "ed.sel.indent", 0, NULL},
    {"<lt>", "ed.sel.dedent", 0, NULL},
    {"A-<lt>", "ed.sel.shift_left", 0, NULL},
    {"A->", "ed.sel.shift_right", 0, NULL},
    {"J", "ed.sel.join", 0, NULL},
    {"r", "ed.sel.replace_char", 0, NULL},
    {"I", "ed.edit.rect.insert", 0, NULL},
    {"A", "ed.edit.rect.append", 0, NULL},
    {"o", "ed.sel.swap_ends", 0, NULL},
    {"v c", "ed.sel.kind", 0, "c"},
    {"v l", "ed.sel.kind", 0, "l"},
    {"v r", "ed.sel.kind", 0, "r"},
    {"<cr>", "ed.cursor.lift.lines", 0, NULL},
    {"m", "ed.cursor.lift.matches", 0, NULL},
    {"e", "ed.cursor.lift.ends", 0, NULL},
    {"C-A-<up>", "ed.cursor.add.above", 0, NULL},
    {"C-A-<down>", "ed.cursor.add.below", 0, NULL},
    {"C-A-<bs>", "ed.cursor.drop", 0, NULL},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

void sag_keys_highlight_install(Ed *ed, Mode unit)
{
    BindRow rows[SAG_ARRAY_LEN(keys_H_L) + SAG_ARRAY_LEN(keys_H)];
    const BindRow *motion = keys_H_L;
    u32 motion_count = SAG_ARRAY_LEN(keys_H_L);
    u32 i;

    if (ed == NULL)
        SAG_BUG("cannot install H bindings without an editor");
    switch (unit) {
    case SAG_MODE_L:
        motion = keys_H_L;
        motion_count = SAG_ARRAY_LEN(keys_H_L);
        break;
    case SAG_MODE_W:
        motion = keys_H_W;
        motion_count = SAG_ARRAY_LEN(keys_H_W);
        break;
    case SAG_MODE_B:
        motion = keys_H_B;
        motion_count = SAG_ARRAY_LEN(keys_H_B);
        break;
    case SAG_MODE_I:
        motion = keys_H_C;
        motion_count = SAG_ARRAY_LEN(keys_H_C);
        break;
    case SAG_MODE_H:
    case SAG_MODE_E:
    case SAG_MODE_F:
    case SAG_MODE__N:
        SAG_BUG("invalid H unit mode");
    }
    for (i = 0U; i < motion_count; i++)
        rows[i] = motion[i];
    for (i = 0U; i < SAG_ARRAY_LEN(keys_H); i++)
        rows[motion_count + i] = keys_H[i];
    if (!sag_keymap_build(&ed->mode_keys[SAG_MODE_H], "H", rows,
                          motion_count + SAG_ARRAY_LEN(keys_H)))
        SAG_BUG("invalid built-in H-mode key table");
}
