#include "harness.h"

#include "mod/mods.h"

#include <stdio.h>

void test_mod_require_message(void)
{
    static const bool expected_enabled[YEW_MOD_COUNT] = {
        YEW_WITH_LSP != 0,
        YEW_WITH_AI != 0,
        YEW_WITH_FUSS != 0,
        YEW_WITH_PLUGINS != 0
    };
    YewMod mod;

    for (mod = YEW_MOD_LSP; mod < YEW_MOD_COUNT; mod++) {
        char err[160] = {0};
        bool result = yew_mod_require(mod, err, sizeof(err));

        YEW_ASSERT(yew_mod_enabled(mod) == expected_enabled[mod]);
        YEW_ASSERT(result == expected_enabled[mod]);
        if (expected_enabled[mod]) {
            YEW_ASSERT_EQ_STR(err, "");
        } else {
            char expected[160];

            (void)snprintf(expected, sizeof(expected),
                "this build has no %s module; rebuild with 'make MODULES=\"… %s\"'",
                yew_mod_name(mod), yew_mod_name(mod));
            YEW_ASSERT_EQ_STR(err, expected);
        }
    }
}
