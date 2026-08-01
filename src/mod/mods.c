#include "mod/mods.h"

#include <stdio.h>

#ifndef SAG_WITH_LSP
#define SAG_WITH_LSP 0
#endif
#ifndef SAG_WITH_AI
#define SAG_WITH_AI 0
#endif
#ifndef SAG_WITH_FUSS
#define SAG_WITH_FUSS 0
#endif
#ifndef SAG_WITH_PLUGINS
#define SAG_WITH_PLUGINS 0
#endif

bool sag_mod_enabled(SagMod mod)
{
    switch (mod) {
    case SAG_MOD_LSP:
        return SAG_WITH_LSP != 0;
    case SAG_MOD_AI:
        return SAG_WITH_AI != 0;
    case SAG_MOD_FUSS:
        return SAG_WITH_FUSS != 0;
    case SAG_MOD_PLUGINS:
        return SAG_WITH_PLUGINS != 0;
    case SAG_MOD_COUNT:
        break;
    }
    return false;
}

const char *sag_mod_name(SagMod mod)
{
    static const char *const names[SAG_MOD_COUNT] = {
        "lsp", "ai", "fuss", "plugins"
    };

    if ((unsigned int)mod >= (unsigned int)SAG_MOD_COUNT) {
        return "unknown";
    }
    return names[mod];
}

bool sag_mod_require(SagMod mod, char *err, size_t errsz)
{
    const char *name;

    if (sag_mod_enabled(mod)) {
        return true;
    }
    name = sag_mod_name(mod);
    if (err != NULL && errsz != 0U) {
        (void)snprintf(err, errsz,
            "this build has no %s module; rebuild with 'make MODULES=\"… %s\"'",
            name, name);
    }
    return false;
}
