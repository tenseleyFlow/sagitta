#include "edit/keys_default.h"

#include <stddef.h>

#include "edit/ed.h"
#include "util/log.h"

static const BindRow keys_L[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"g g", "ed.move.buf.home", 0, NULL},
    {"q", "ed.quit", 0, NULL},
    {"q !", "ed.quit_force", 0, NULL},
    {"n", "ed.nop", 0, NULL},
};

static const BindRow keys_W[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"n", "ed.nop", 0, NULL},
};

static const BindRow keys_B[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"n", "ed.nop", 0, NULL},
};

static const BindRow keys_H[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"n", "ed.nop", 0, NULL},
};

static const BindRow keys_I[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"n", "ed.nop", 0, NULL},
};

static const BindRow keys_E[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"n", "ed.nop", 0, NULL},
};

static const BindRow keys_F[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"n", "ed.nop", 0, NULL},
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
