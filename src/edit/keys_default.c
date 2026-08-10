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

void yew_keys_default_install(Ed *ed)
{
    static const BindRow *const rows[YEW_MODE__N] = {
        panic_L, NULL, NULL, NULL, NULL, panic_E, NULL,
    };
    static const u32 counts[YEW_MODE__N] = {
        YEW_ARRAY_LEN(panic_L), 0U, 0U, 0U, 0U,
        YEW_ARRAY_LEN(panic_E), 0U,
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
