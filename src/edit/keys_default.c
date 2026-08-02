#include "edit/keys_default.h"

#include <stddef.h>

#include "edit/ed.h"
#include "util/log.h"

static const BindRow keys_L[] = {
    {"<left>", "ed.move.line.home", 0, NULL},
    {"<right>", "ed.move.line.end", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"A-<left>", "ed.move.line.first_nonblank", 0, NULL},
    {"A-<right>", "ed.move.line.last_nonblank", 0, NULL},
    {"A-<up>", "ed.view.half_page_up", 0, NULL},
    {"A-<down>", "ed.view.half_page_down", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
    {"<pgup>", "ed.view.page_up", 0, NULL},
    {"<pgdn>", "ed.view.page_down", 0, NULL},
    {"i", "ed.mode.enter", 0, "I"},
    {"a", "ed.edit.insert.after", 0, NULL},
    {"o", "ed.edit.line.open_below", 0, NULL},
    {"O", "ed.edit.line.open_above", 0, NULL},
    {"x", "ed.edit.delete.grapheme", 0, NULL},
    {"d d", "ed.edit.line.delete", 0, NULL},
    {"u", "ed.edit.undo", 0, NULL},
    {"C-r", "ed.edit.redo", 0, NULL},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"g g", "ed.move.buf.home", 0, NULL},
    {"G", "ed.move.buf.end", 0, NULL},
    {"s", "ed.file.save", 0, NULL},
    {"q", "ed.quit", 0, NULL},
    {"q !", "ed.quit_force", 0, NULL},
    {"C-z", "ed.suspend", 0, NULL},
    {"C-l", "ed.redraw", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"0", "ed.move.line.home", 0, NULL},
    {"w", "ed.mode.enter", 0, "W"},
    {"b", "ed.mode.enter", 0, "B"},
    {"h", "ed.mode.enter", 0, "H"},
    {"e", "ed.mode.enter", 0, "E"},
    {"f", "ed.mode.enter", 0, "F"},
};

static const BindRow keys_W[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

static const BindRow keys_B[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

static const BindRow keys_H[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

static const BindRow keys_I[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"<cr>", "ed.edit.insert.newline", 0, NULL},
    {"<tab>", "ed.edit.insert.tab", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"<left>", "ed.move.char.prev", 0, NULL},
    {"<right>", "ed.move.char.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
};

static const BindRow keys_E[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

static const BindRow keys_F[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

void sag_keys_default_install(Ed *ed)
{
    static const BindRow *const rows[SAG_MODE__N] = {
        keys_L, keys_W, keys_B, keys_H, keys_I, keys_E, keys_F,
    };
    static const u32 counts[SAG_MODE__N] = {
        SAG_ARRAY_LEN(keys_L), SAG_ARRAY_LEN(keys_W),
        SAG_ARRAY_LEN(keys_B), SAG_ARRAY_LEN(keys_H),
        SAG_ARRAY_LEN(keys_I), SAG_ARRAY_LEN(keys_E),
        SAG_ARRAY_LEN(keys_F),
    };
    u32 i;

    for (i = 0U; i < SAG_MODE__N; i++) {
        if (!sag_keymap_build(&ed->mode_keys[i], sag_modes[i].name,
                              rows[i], counts[i]))
            SAG_BUG("invalid built-in %s-mode key table", sag_modes[i].name);
    }
    if (!sag_keymap_build(&ed->user_keys, "user", NULL, 0U))
        SAG_BUG("cannot build empty user key table");
    ed->keys.n = 2U;
    ed->keys.l[0] = &ed->mode_keys[ed->mode];
    ed->keys.l[1] = &ed->user_keys;
}
