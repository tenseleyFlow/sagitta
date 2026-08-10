#ifndef YEW_EDIT_MODE_H
#define YEW_EDIT_MODE_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "util/base.h"

typedef struct Ed Ed;

typedef enum {
    YEW_MODE_L,
    YEW_MODE_W,
    YEW_MODE_B,
    YEW_MODE_H,
    YEW_MODE_I,
    YEW_MODE_E,
    YEW_MODE_F,
    YEW_MODE__N
} Mode;

typedef struct ModeDesc {
    const char *name;
    const char *long_name;
    bool is_unit;
    bool takes_count;
    u8 layer;
} ModeDesc;

extern const ModeDesc yew_modes[YEW_MODE__N];

CmdStatus yew_mode_enter(Ed *ed, Mode mode);
CmdStatus yew_mode_enter_highlight(Ed *ed, Mode unit, bool sticky);
CmdStatus yew_mode_escape(Ed *ed);

#endif
