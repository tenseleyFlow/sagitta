#ifndef SAG_EDIT_ED_H
#define SAG_EDIT_ED_H

#include "edit/dispatch.h"
#include "edit/keymap.h"
#include "edit/mode.h"

/* Sprint 14 widens this into the full editor model.  Keeping the Sprint 13
 * state here makes dispatch embeddable without a global editor pointer. */
struct Ed {
    Mode mode;
    Mode prev_unit;
    Keymap mode_keys[SAG_MODE__N];
    Keymap user_keys;
    KeyStack keys;
    Chord chord;
    u32 chord_timeout_ms;
    CmdId last_cmd;
    CmdStatus last_status;
    u64 dispatch_count;
    char dispatch_message[192];
};

#endif
