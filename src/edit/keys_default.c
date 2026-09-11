#include "edit/keys_default.h"

#include <stddef.h>

#include "edit/ed.h"
#include "edit/keys_highlight.h"
#include "util/log.h"

/*
 * The permanent floor beneath runtime/init.fl.  These six rows are enough
 * to leave the editor, enter and submit a command, or abandon that command
 * when the shipped or user configuration is broken.  The L-mode escape is
 * deliberately a nop: runtime/init.fl installs the normal mode-escape row.
 */
static const BindRow panic_L[] = {
    {"<esc>", "ed.nop", 0, NULL},
    {"q", "ed.quit", 0, NULL},
    {"q !", "ed.quit_force", 0, NULL},
    {":", "ed.mode.enter", 0, "E"},
};

static const BindRow panic_E[] = {
    {"<cr>", "ed.cmdline.accept", 0, NULL},
    {"<esc>", "ed.cmdline.cancel", 0, NULL},
};

/*
 * F mode must remain useful even when runtime/init.fl cannot be loaded.
 * Arrows and structural keys stay direct.  Bare printable filename bytes are
 * reserved for type-to-jump, so every printable action uses Alt.
 */
static const BindRow panic_F[] = {
    {"<up>", "ed.git.nav.prev", 0, NULL},
    {"<down>", "ed.git.nav.next", 0, NULL},
    {"<left>", "ed.git.nav.parent", 0, NULL},
    {"<right>", "ed.git.nav.enter", 0, NULL},
    {"<space>", "ed.git.view", 0, NULL},
    {"C-w s", "ed.git.open_split_h", 0, NULL},
    {"C-w v", "ed.git.open_split_v", 0, NULL},
    {"C-<up>", "ed.git.nav.row_prev", 0, NULL},
    {"C-<down>", "ed.git.nav.row_next", 0, NULL},
    {"<pgup>", "ed.view.page_up", 0, NULL},
    {"<pgdn>", "ed.view.page_down", 0, NULL},
    {"<home>", "ed.move.buf.home", 0, NULL},
    {"<end>", "ed.move.buf.end", 0, NULL},
    {"A-a", "ed.git.stage", 0, NULL},
    {"A-u", "ed.git.unstage", 0, NULL},
    {"A-S", "ed.git.stage.all", 0, NULL},
    {"A-U", "ed.git.unstage.all", 0, NULL},
    {"A-m", "ed.git.commit", 0, NULL},
    {"A-M", "ed.git.commit.amend", 0, NULL},
    {"A-p", "ed.git.push", 0, NULL},
    {"A-l", "ed.git.pull", 0, NULL},
    {"A-f", "ed.git.fetch", 0, NULL},
    {"A-d", "ed.git.diff", 0, NULL},
    {"A-D", "ed.git.diff.view", 0, NULL},
    {"A-s", "ed.git.status", 0, NULL},
    {"A-w", "ed.git.blame", 0, NULL},
    {"A-h", "ed.git.history", 0, NULL},
    {"A-L", "ed.git.reflog", 0, NULL},
    {"A-c", "ed.git.view", 0, NULL},
    {"A-b", "ed.git.branch.switch", 0, NULL},
    {"A-n", "ed.git.branch.create", 0, NULL},
    {"A-R", "ed.git.branch.delete", 0, NULL},
    {"A-G", "ed.git.merge", 0, NULL},
    {"A-O", "ed.git.reset", 0, NULL},
    {"A-I", "ed.git.rebase.interactive", 0, NULL},
    {"A-y", "ed.git.cherry_pick", 0, NULL},
    {"A-v", "ed.git.revert", 0, NULL},
    {"A-z", "ed.git.stash.push", 0, NULL},
    {"A-Z", "ed.git.stash.pop", 0, NULL},
    {"A-t", "ed.git.tag", 0, NULL},
    {"A-x", "ed.git.discard", 0, NULL},
    {"A-r", "ed.git.file.delete", 0, NULL},
    {"A-N", "ed.git.file.rename", 0, NULL},
    {"<cr>", "ed.git.open", 0, NULL},
    {"A-g", "ed.group.from_dir", 0, NULL},
    {"A-T", "ed.git.tree.all", 0, NULL},
    {"A-.", "ed.git.tree.hidden", 0, NULL},
    {"C-S-/", "ed.git.actions", 0, NULL},
    {"C-?", "ed.git.actions", 0, NULL},
    {"C-_", "ed.git.actions", 0, NULL},
    {"C-r", "ed.git.refresh", 0, NULL},
    {"A-q", "ed.git.mode.leave", 0, NULL},
    {"<esc>", "ed.git.mode.leave", 0, NULL},
};

void yew_keys_default_install(Ed *ed)
{
    static const BindRow *const rows[YEW_MODE__N] = {
        panic_L, NULL, NULL, NULL, NULL, panic_E, panic_F,
    };
    static const u32 counts[YEW_MODE__N] = {
        YEW_ARRAY_LEN(panic_L), 0U, 0U, 0U, 0U,
        YEW_ARRAY_LEN(panic_E), YEW_ARRAY_LEN(panic_F),
    };
    u32 i;

    for (i = 0U; i < YEW_MODE__N; i++) {
        if (!yew_keymap_build(&ed->mode_keys[i], yew_modes[i].name,
                              rows[i], counts[i]))
            YEW_BUG("invalid panic %s-mode key table", yew_modes[i].name);
    }
    /* H motions remain unit-dependent and are rebuilt on each H entry. */
    yew_keys_highlight_install(ed, YEW_MODE_L);
    if (!yew_keymap_build(&ed->user_keys, "user", NULL, 0U))
        YEW_BUG("cannot build empty user key table");
    ed->keys.n = 2U;
    ed->keys.l[0] = &ed->mode_keys[ed->mode];
    ed->keys.l[1] = &ed->user_keys;
}
