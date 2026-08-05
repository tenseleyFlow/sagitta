#include "edit/keys_default.h"

#include <stddef.h>

#include "edit/ed.h"
#include "edit/keys_highlight.h"
#include "util/log.h"

static const BindRow keys_L[] = {
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
    /* Sprint 21 §5.  Ctrl-O/Ctrl-I are the jumplist's two directions;
     * g; / g, walk the changelist. */
    /* Sprint 21 §1: the search surface. */
    /* Sprint 22 §4: panes.  `s`-prefixed chords, keymap DATA — no key
     * calls a pane function directly (DoD 8). */
    {"C-w s", "ed.pane.split_h", 0, NULL},
    {"C-w v", "ed.pane.split_v", 0, NULL},
    {"C-w c", "ed.pane.close", 0, NULL},
    {"C-w <left>", "ed.pane.focus_left", 0, NULL},
    {"C-w <right>", "ed.pane.focus_right", 0, NULL},
    {"C-w <up>", "ed.pane.focus_up", 0, NULL},
    {"C-w <down>", "ed.pane.focus_down", 0, NULL},
    {"C-w w", "ed.pane.focus_next", 0, NULL},
    {"C-w +", "ed.pane.grow", 0, NULL},
    {"C-w -", "ed.pane.shrink", 0, NULL},
    {"m", "ed.mark.set", 0, NULL},
    {"'", "ed.mark.jump", 0, NULL},
    {"/", "ed.search.open", 0, NULL},
    {"?", "ed.search.open_back", 0, NULL},
    {"n", "ed.search.next", 0, NULL},
    {"N", "ed.search.prev", 0, NULL},
    {"*", "ed.search.word_next", 0, NULL},
    {"#", "ed.search.word_prev", 0, NULL},
    {"C-o", "ed.jump.back", 0, NULL},
    {"C-i", "ed.jump.fwd", 0, NULL},
    {"g ;", "ed.change.older", 0, NULL},
    {"g ,", "ed.change.newer", 0, NULL},
    {"G", "ed.move.buf.end", 0, NULL},
    {"s", "ed.file.save", 0, NULL},
    {"q", "ed.quit", 0, NULL},
    {"q !", "ed.quit_force", 0, NULL},
    {"C-z", "ed.suspend", 0, NULL},
    {"C-l", "ed.redraw", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"0", "ed.move.unit.home", 0, NULL},
    {"w", "ed.mode.enter", 0, "W"},
    {"b", "ed.mode.enter", 0, "B"},
    {"h", "ed.mode.enter", 0, "H"},
    {":", "ed.mode.enter", 0, "E"},
    {"e", "ed.mode.enter", 0, "E"},
    {"f", "ed.mode.enter", 0, "F"},
};

static const BindRow keys_W[] = {
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
    {"h", "ed.mode.enter", 0, "H"},
    {":", "ed.mode.enter", 0, "E"},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

static const BindRow keys_B[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"A-<left>", "ed.move.block.match_prev", 0, NULL},
    {"A-<right>", "ed.move.block.match_next", 0, NULL},
    {"A-<up>", "ed.sel.unit.expand", 0, NULL},
    {"A-<down>", "ed.sel.unit.contract", 0, NULL},
    {"h", "ed.mode.enter", 0, "H"},
    {":", "ed.mode.enter", 0, "E"},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

static const BindRow keys_I[] = {
    {"A-h", "ed.mode.enter", 0, "H"},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"<cr>", "ed.edit.insert.newline", 0, NULL},
    {"<tab>", "ed.edit.insert.tab", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
};

static const BindRow keys_E[] = {
    {"<left>", "ed.move.char.prev", 0, NULL},
    {"<right>", "ed.move.char.next", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
    {"C-a", "ed.move.line.home", 0, NULL},
    {"C-e", "ed.move.line.end", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"C-w", "ed.del.word_prev", 0, NULL},
    {"C-u", "ed.del.to_home", 0, NULL},
    {"C-k", "ed.del.to_end", 0, NULL},
    {"<up>", "ed.cmdline.hist_prev", 0, NULL},
    {"<down>", "ed.cmdline.hist_next", 0, NULL},
    {"<tab>", "ed.cmdline.complete_next", 0, NULL},
    {"S-<tab>", "ed.cmdline.complete_prev", 0, NULL},
    {"C-r", "ed.cmdline.insert_register", 0, NULL},
    {"C-v", "ed.cmdline.literal_next", 0, NULL},
    {"C-z", "ed.edit.undo", 0, NULL},
    {"C-y", "ed.edit.redo", 0, NULL},
    {"<cr>", "ed.cmdline.accept", 0, NULL},
    {"<esc>", "ed.cmdline.cancel", 0, NULL},
    {"C-g", "ed.cmdline.cancel", 0, NULL},
};

static const BindRow keys_F[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};

void sag_keys_default_install(Ed *ed)
{
    static const BindRow *const rows[SAG_MODE__N] = {
        keys_L, keys_W, keys_B, NULL, keys_I, keys_E, keys_F,
    };
    static const u32 counts[SAG_MODE__N] = {
        SAG_ARRAY_LEN(keys_L), SAG_ARRAY_LEN(keys_W),
        SAG_ARRAY_LEN(keys_B), 0U,
        SAG_ARRAY_LEN(keys_I), SAG_ARRAY_LEN(keys_E),
        SAG_ARRAY_LEN(keys_F),
    };
    u32 i;

    for (i = 0U; i < SAG_MODE__N; i++) {
        if (!sag_keymap_build(&ed->mode_keys[i], sag_modes[i].name,
                              rows[i], counts[i]))
            SAG_BUG("invalid built-in %s-mode key table", sag_modes[i].name);
    }
    sag_keys_highlight_install(ed, SAG_MODE_L);
    if (!sag_keymap_build(&ed->user_keys, "user", NULL, 0U))
        SAG_BUG("cannot build empty user key table");
    ed->keys.n = 2U;
    ed->keys.l[0] = &ed->mode_keys[ed->mode];
    ed->keys.l[1] = &ed->user_keys;
}
