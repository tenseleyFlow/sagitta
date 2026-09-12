/*
 * YEW-F-075 — stripped builds accept module-only config as inert state.
 *
 * Correct behavior: in a MODULES="" build every option owned by an excluded
 * module rejects through yew_mod_require's canonical module error.
 *
 * Baseline failure: ai.enable, lsp.open_in, and git.ascii_glyphs remain in
 * the core option table and set successfully; plugin options disappear and
 * return only "unknown option".  The default build reports this reproducer
 * as inapplicable while the required MODULES="" audit lane exercises it.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/mods.h"

static bool rejected_canonically(Ed *ed, const char *name, OptVal value,
                                 const char *module, char *detail,
                                 size_t detail_cap)
{
    const char *err = NULL;
    bool accepted = yew_opt_set(ed, YEW_OPT_GLOBAL, name,
                                (u32)strlen(name), &value, &err);
    bool canonical = !accepted && err != NULL && strstr(err, "this build has no") != NULL &&
                     strstr(err, module) != NULL;

    if (!canonical)
        (void)snprintf(detail, detail_cap, "%s=%s", name,
                       accepted ? "accepted" : err == NULL ? "no error" : err);
    return canonical;
}

bool test_yew_f_075(char *why, size_t why_cap)
{
    OptVal yes = {(u8)YEW_OPT_BOOL, {.b = true}};
    OptVal tab = {(u8)YEW_OPT_STR, {.str = {"tab", 3U}}};
    Ed ed;
    char detail[192] = "";
    bool ai;
    bool lsp;
    bool fuss;
    bool plugins;

    if (yew_mod_enabled(YEW_MOD_AI) || yew_mod_enabled(YEW_MOD_LSP) ||
        yew_mod_enabled(YEW_MOD_FUSS) || yew_mod_enabled(YEW_MOD_PLUGINS)) {
        (void)snprintf(why, why_cap,
                       "reproducer requires the MODULES=empty audit lane");
        return false;
    }
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        yew_ed_free(&ed);
        return false;
    }
    ai = rejected_canonically(&ed, "ai.enable", yes, "ai",
                              detail, sizeof(detail));
    lsp = rejected_canonically(&ed, "lsp.open_in", tab, "lsp",
                               detail, sizeof(detail));
    fuss = rejected_canonically(&ed, "git.ascii_glyphs", yes, "fuss",
                                detail, sizeof(detail));
    plugins = rejected_canonically(&ed, "plug.verify_on_load", yes,
                                   "plugins", detail, sizeof(detail));
    yew_ed_free(&ed);
    if (!ai || !lsp || !fuss || !plugins)
        (void)snprintf(why, why_cap,
                       "module option boundary ai=%u lsp=%u fuss=%u "
                       "plugins=%u; %s",
                       ai ? 1U : 0U, lsp ? 1U : 0U, fuss ? 1U : 0U,
                       plugins ? 1U : 0U, detail);
    return ai && lsp && fuss && plugins;
}
