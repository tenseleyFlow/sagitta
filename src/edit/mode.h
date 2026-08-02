#ifndef SAG_EDIT_MODE_H
#define SAG_EDIT_MODE_H

#include <stdbool.h>

#include "util/base.h"

typedef enum {
    SAG_MODE_L,
    SAG_MODE_W,
    SAG_MODE_B,
    SAG_MODE_H,
    SAG_MODE_I,
    SAG_MODE_E,
    SAG_MODE_F,
    SAG_MODE__N
} Mode;

typedef struct ModeDesc {
    const char *name;
    const char *long_name;
    bool is_unit;
    bool takes_count;
    u8 layer;
} ModeDesc;

extern const ModeDesc sag_modes[SAG_MODE__N];

#endif
