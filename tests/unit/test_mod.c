#include "harness.h"

#include "mod/mods.h"

#include <stdio.h>

void test_mod_require_message(void)
{
    static const bool expected_enabled[SAG_MOD_COUNT] = {
        SAG_WITH_LSP != 0,
        SAG_WITH_AI != 0,
        SAG_WITH_FUSS != 0,
        SAG_WITH_PLUGINS != 0
    };
    SagMod mod;

    for (mod = SAG_MOD_LSP; mod < SAG_MOD_COUNT; mod++) {
        char err[160] = {0};
        bool result = sag_mod_require(mod, err, sizeof(err));

        SAG_ASSERT(sag_mod_enabled(mod) == expected_enabled[mod]);
        SAG_ASSERT(result == expected_enabled[mod]);
        if (expected_enabled[mod]) {
            SAG_ASSERT_EQ_STR(err, "");
        } else {
            char expected[160];

            (void)snprintf(expected, sizeof(expected),
                "this build has no %s module; rebuild with 'make MODULES=\"… %s\"'",
                sag_mod_name(mod), sag_mod_name(mod));
            SAG_ASSERT_EQ_STR(err, expected);
        }
    }
}
