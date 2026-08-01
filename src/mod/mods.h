#ifndef SAG_MOD_MODS_H
#define SAG_MOD_MODS_H

#include <stdbool.h>

#include "util/base.h"

typedef enum {
    SAG_MOD_LSP,
    SAG_MOD_AI,
    SAG_MOD_FUSS,
    SAG_MOD_PLUGINS,
    SAG_MOD_COUNT
} SagMod;

bool sag_mod_enabled(SagMod mod);
const char *sag_mod_name(SagMod mod);
bool sag_mod_require(SagMod mod, char *err, size_t errsz);

#endif
