/*
 * YEW-F-075 — stripped builds accept module-only config as inert state.
 *
 * Every module-owned descriptor remains discoverable in all build profiles.
 * Writes succeed when its module exists and otherwise reject through
 * yew_mod_require's canonical module error.  The inventory check prevents a
 * sibling key from escaping the same boundary without an owner.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/mods.h"

static u8 expected_module(const char *name)
{
    if (strncmp(name, "ai.", 3U) == 0)
        return (u8)YEW_OPT_MODULE_AI;
    if (strncmp(name, "plug.", 5U) == 0)
        return (u8)YEW_OPT_MODULE_PLUGINS;
    if (strcmp(name, "lsp.open_in") == 0)
        return (u8)YEW_OPT_MODULE_LSP;
    if (strcmp(name, "git.ascii_glyphs") == 0)
        return (u8)YEW_OPT_MODULE_FUSS;
    return (u8)YEW_OPT_MODULE_CORE;
}

bool test_yew_f_075(char *why, size_t why_cap)
{
    Ed ed;
    u8 seen = 0U;
    u32 i;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        yew_ed_free(&ed);
        return false;
    }
    for (i = 0U; i < yew_opts_len; i++) {
        const OptDesc *desc = &yew_opts[i];
        const char *err = NULL;
        char canonical[192] = "";
        u8 expected = expected_module(desc->name);
        bool accepted;
        YewMod module;

        if (desc->module != expected) {
            (void)snprintf(why, why_cap,
                           "%s owner=%u expected=%u", desc->name,
                           (unsigned int)desc->module,
                           (unsigned int)expected);
            yew_ed_free(&ed);
            return false;
        }
        if (expected == (u8)YEW_OPT_MODULE_CORE)
            continue;
        seen = (u8)(seen | (u8)(1U << expected));
        module = (YewMod)(expected - 1U);
        accepted = yew_opt_set(&ed, desc->scope, desc->name,
                               (u32)strlen(desc->name), &desc->dflt, &err);
        if (yew_mod_enabled(module)) {
            if (!accepted || err != NULL) {
                (void)snprintf(why, why_cap,
                               "%s rejected with enabled module: %s",
                               desc->name, err == NULL ? "no error" : err);
                yew_ed_free(&ed);
                return false;
            }
        } else {
            (void)yew_mod_require(module, canonical, sizeof(canonical));
            if (accepted || err == NULL || strcmp(err, canonical) != 0) {
                (void)snprintf(why, why_cap,
                               "%s=%s", desc->name,
                               accepted ? "accepted" :
                               err == NULL ? "no error" : err);
                yew_ed_free(&ed);
                return false;
            }
        }
    }
    yew_ed_free(&ed);
    if (seen != (u8)0x1eU) {
        (void)snprintf(why, why_cap,
                       "module option inventory mask=%u expected=30",
                       (unsigned int)seen);
        return false;
    }
    return true;
}
