#include "edit/mode.h"

const ModeDesc sag_modes[SAG_MODE__N] = {
    [SAG_MODE_L] = {"L", "line", true, true, SAG_MODE_L},
    [SAG_MODE_W] = {"W", "word", true, true, SAG_MODE_W},
    [SAG_MODE_B] = {"B", "block", true, true, SAG_MODE_B},
    [SAG_MODE_H] = {"H", "highlight", false, true, SAG_MODE_H},
    [SAG_MODE_I] = {"I", "insert", false, false, SAG_MODE_I},
    [SAG_MODE_E] = {"E", "execute", false, false, SAG_MODE_E},
    [SAG_MODE_F] = {"F", "fuss", false, false, SAG_MODE_F},
};
