#ifndef YEW_MOD_MODS_H
#define YEW_MOD_MODS_H

#include <stdbool.h>

#include "util/base.h"

typedef enum {
    YEW_MOD_LSP,
    YEW_MOD_AI,
    YEW_MOD_FUSS,
    YEW_MOD_PLUGINS,
    YEW_MOD_COUNT
} YewMod;

bool yew_mod_enabled(YewMod mod);
const char *yew_mod_name(YewMod mod);
bool yew_mod_require(YewMod mod, char *err, size_t errsz);

#endif
