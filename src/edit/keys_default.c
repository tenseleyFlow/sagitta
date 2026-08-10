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

void sag_keys_default_install(Ed *ed)
{
    static const BindRow *const rows[SAG_MODE__N] = {
        panic_L, NULL, NULL, NULL, NULL, panic_E, NULL,
    };
    static const u32 counts[SAG_MODE__N] = {
        SAG_ARRAY_LEN(panic_L), 0U, 0U, 0U, 0U,
        SAG_ARRAY_LEN(panic_E), 0U,
    };
    u32 i;

    for (i = 0U; i < SAG_MODE__N; i++) {
        if (!sag_keymap_build(&ed->mode_keys[i], sag_modes[i].name,
                              rows[i], counts[i]))
            SAG_BUG("invalid panic %s-mode key table", sag_modes[i].name);
    }
    /* H motions remain unit-dependent and are rebuilt on each H entry. */
    sag_keys_highlight_install(ed, SAG_MODE_L);
    if (!sag_keymap_build(&ed->user_keys, "user", NULL, 0U))
        SAG_BUG("cannot build empty user key table");
    ed->keys.n = 2U;
    ed->keys.l[0] = &ed->mode_keys[ed->mode];
    ed->keys.l[1] = &ed->user_keys;
}
