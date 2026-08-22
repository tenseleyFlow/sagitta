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
 * Sprint 52: F mode must remain useful even when runtime/init.fl cannot be
 * loaded.  Keep arrows primary.  The historical h/l navigation aliases are
 * deliberately omitted because the authoritative fuss verb table assigns h
 * to history and l to pull; duplicate sequences are rejected by the keymap
 * builder.
 */
static const BindRow panic_F[] = {
    {"<up>", "ed.git.nav.prev", 0, NULL},
    {"k", "ed.git.nav.prev", 0, NULL},
    {"<down>", "ed.git.nav.next", 0, NULL},
    {"j", "ed.git.nav.next", 0, NULL},
    {"<left>", "ed.git.nav.parent", 0, NULL},
    {"<right>", "ed.git.nav.enter", 0, NULL},
    {"<space>", "ed.git.nav.toggle", 0, NULL},
    {"C-<up>", "ed.git.nav.row_prev", 0, NULL},
    {"C-<down>", "ed.git.nav.row_next", 0, NULL},
    {"<pgup>", "ed.view.page_up", 0, NULL},
    {"<pgdn>", "ed.view.page_down", 0, NULL},
    {"<home>", "ed.move.buf.home", 0, NULL},
    {"<end>", "ed.move.buf.end", 0, NULL},
    {"a", "ed.git.stage", 0, NULL},
    {"u", "ed.git.unstage", 0, NULL},
    {"S", "ed.git.stage.all", 0, NULL},
    {"U", "ed.git.unstage.all", 0, NULL},
    {"m", "ed.git.commit", 0, NULL},
    {"M", "ed.git.commit.amend", 0, NULL},
    {"p", "ed.git.push", 0, NULL},
    {"l", "ed.git.pull", 0, NULL},
    {"f", "ed.git.fetch", 0, NULL},
    {"d", "ed.git.diff", 0, NULL},
    {"s", "ed.git.status", 0, NULL},
    {"w", "ed.git.blame", 0, NULL},
    {"h", "ed.git.history", 0, NULL},
    {"L", "ed.git.reflog", 0, NULL},
    {"c", "ed.git.view", 0, NULL},
    {"b", "ed.git.branch.switch", 0, NULL},
    {"n", "ed.git.branch.create", 0, NULL},
    {"R", "ed.git.branch.delete", 0, NULL},
    {"G", "ed.git.merge", 0, NULL},
    {"O", "ed.git.reset", 0, NULL},
    {"I", "ed.git.rebase.interactive", 0, NULL},
    {"y", "ed.git.cherry_pick", 0, NULL},
    {"v", "ed.git.revert", 0, NULL},
    {"z", "ed.git.stash.push", 0, NULL},
    {"Z", "ed.git.stash.pop", 0, NULL},
    {"t", "ed.git.tag", 0, NULL},
    {"x", "ed.git.discard", 0, NULL},
    {"r", "ed.git.file.delete", 0, NULL},
    {"N", "ed.git.file.rename", 0, NULL},
    {"<cr>", "ed.git.open", 0, NULL},
    {"g", "ed.group.from_dir", 0, NULL},
    {"T", "ed.git.tree.all", 0, NULL},
    {".", "ed.git.tree.hidden", 0, NULL},
    {"/", "ed.git.jump.arm", 0, NULL},
    {"C-r", "ed.git.refresh", 0, NULL},
    {"q", "ed.git.mode.leave", 0, NULL},
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
