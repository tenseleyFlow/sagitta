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
 * reserved for type-to-jump, so every printable action lives under C-g.
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
    {"C-g a", "ed.git.stage", 0, NULL},
    {"C-g u", "ed.git.unstage", 0, NULL},
    {"C-g S", "ed.git.stage.all", 0, NULL},
    {"C-g U", "ed.git.unstage.all", 0, NULL},
    {"C-g m", "ed.git.commit", 0, NULL},
    {"C-g M", "ed.git.commit.amend", 0, NULL},
    {"C-g p", "ed.git.push", 0, NULL},
    {"C-g l", "ed.git.pull", 0, NULL},
    {"C-g f", "ed.git.fetch", 0, NULL},
    {"C-g d", "ed.git.diff", 0, NULL},
    {"C-g D", "ed.git.diff.view", 0, NULL},
    {"C-g s", "ed.git.status", 0, NULL},
    {"C-g w", "ed.git.blame", 0, NULL},
    {"C-g h", "ed.git.history", 0, NULL},
    {"C-g L", "ed.git.reflog", 0, NULL},
    {"C-g c", "ed.git.view", 0, NULL},
    {"C-g b", "ed.git.branch.switch", 0, NULL},
    {"C-g n", "ed.git.branch.create", 0, NULL},
    {"C-g R", "ed.git.branch.delete", 0, NULL},
    {"C-g G", "ed.git.merge", 0, NULL},
    {"C-g O", "ed.git.reset", 0, NULL},
    {"C-g I", "ed.git.rebase.interactive", 0, NULL},
    {"C-g y", "ed.git.cherry_pick", 0, NULL},
    {"C-g v", "ed.git.revert", 0, NULL},
    {"C-g z", "ed.git.stash.push", 0, NULL},
    {"C-g Z", "ed.git.stash.pop", 0, NULL},
    {"C-g t", "ed.git.tag", 0, NULL},
    {"C-g x", "ed.git.discard", 0, NULL},
    {"C-g r", "ed.git.file.delete", 0, NULL},
    {"C-g N", "ed.git.file.rename", 0, NULL},
    {"<cr>", "ed.git.open", 0, NULL},
    {"C-g g", "ed.group.from_dir", 0, NULL},
    {"C-g T", "ed.git.tree.all", 0, NULL},
    {"C-g .", "ed.git.tree.hidden", 0, NULL},
    {"C-g ?", "ed.ui.message_expand", 0, NULL},
    {"C-r", "ed.git.refresh", 0, NULL},
    {"C-g q", "ed.git.mode.leave", 0, NULL},
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
