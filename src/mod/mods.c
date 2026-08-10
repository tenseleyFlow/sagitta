#include "mod/mods.h"

#include <stdio.h>

#ifndef YEW_WITH_LSP
#define YEW_WITH_LSP 0
#endif
#ifndef YEW_WITH_AI
#define YEW_WITH_AI 0
#endif
#ifndef YEW_WITH_FUSS
#define YEW_WITH_FUSS 0
#endif
#ifndef YEW_WITH_PLUGINS
#define YEW_WITH_PLUGINS 0
#endif

bool yew_mod_enabled(YewMod mod)
{
    switch (mod) {
    case YEW_MOD_LSP:
        return YEW_WITH_LSP != 0;
    case YEW_MOD_AI:
        return YEW_WITH_AI != 0;
    case YEW_MOD_FUSS:
        return YEW_WITH_FUSS != 0;
    case YEW_MOD_PLUGINS:
        return YEW_WITH_PLUGINS != 0;
    case YEW_MOD_COUNT:
        break;
    }
    return false;
}

const char *yew_mod_name(YewMod mod)
{
    static const char *const names[YEW_MOD_COUNT] = {
        "lsp", "ai", "fuss", "plugins"
    };

    if ((unsigned int)mod >= (unsigned int)YEW_MOD_COUNT) {
        return "unknown";
    }
    return names[mod];
}

bool yew_mod_require(YewMod mod, char *err, size_t errsz)
{
    const char *name;

    if (yew_mod_enabled(mod)) {
        return true;
    }
    name = yew_mod_name(mod);
    if (err != NULL && errsz != 0U) {
        (void)snprintf(err, errsz,
            "this build has no %s module; rebuild with 'make MODULES=\"… %s\"'",
            name, name);
    }
    return false;
}
