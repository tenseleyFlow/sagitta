#include "harness.h"

#include "edit/cmd.h"
#include "edit/ed.h"
#include "mod/mods.h"
#include "ui/message.h"

#include <stdio.h>
#include <string.h>

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

void test_lsp_restart_crosses_module_boundary(void)
{
    Ed ed;
    CmdCtx cx = {0};
    CmdId restart;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    restart = yew_cmd_lookup("ed.lsp.restart", 14U);
    YEW_ASSERT(restart.v != 0U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
#if YEW_WITH_LSP
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, restart, &cx), YEW_CMD_OK);
#else
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, restart, &cx), YEW_CMD_ERR_STATE);
#endif
    YEW_ASSERT(ed.msg.active);
#if YEW_WITH_LSP
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
    YEW_ASSERT_EQ_STR(ed.msg.text,
                      "no LSP server configured for this buffer");
#else
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT_EQ_STR(
        ed.msg.text,
        "this build has no lsp module; rebuild with 'make MODULES=\"… lsp\"'");
#endif
    yew_ed_free(&ed);
}

void test_git_commands_cross_module_boundary(void)
{
#if !YEW_WITH_FUSS
    static const char absent[] =
        "this build has no fuss module; rebuild with 'make MODULES=\"… fuss\"'";
    CmdCtx cx = {0};
#endif
    Ed ed;
    u32 seen = 0U;
    u32 i;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
#if !YEW_WITH_FUSS
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
#endif
    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);

        if (desc == NULL || strncmp(desc->name, "ed.git.", 7U) != 0)
            continue;
        seen++;
#if YEW_WITH_FUSS
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
#else
        {
            CmdId id = yew_cmd_lookup(desc->name, (u32)strlen(desc->name));

            YEW_ASSERT(id.v != 0U);
            YEW_ASSERT_EQ_I64(yew_cmd_invoke(id, &cx), YEW_CMD_ERR_STATE);
            YEW_ASSERT(ed.msg.active);
            YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
            YEW_ASSERT_EQ_STR(ed.msg.text, absent);
        }
#endif
    }
    YEW_ASSERT(seen >= 40U);
    yew_ed_free(&ed);
}
