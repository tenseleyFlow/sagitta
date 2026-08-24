#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/plug/internal.h"
#include "ui/cmdcomp.h"

static CmdStatus completion_command(CmdCtx *cx)
{
    (void)cx;
    return YEW_CMD_OK;
}

static const CompItem *completion_find(const Vec_CompItem *items,
                                       const char *text)
{
    u32 i;

    for (i = 0U; i < items->len; i++)
        if (strcmp(items->data[i].text, text) == 0)
            return &items->data[i];
    return NULL;
}

void test_plug_completion_maps_lifecycle_args_and_lists_winners(void)
{
    static const char *const commands[] = {
        "plug.enable", "plug.disable", "plug.reload", "plug.info"
    };
    Ed ed = {0};
    PlugSys sys = {0};
    Plug alpha = {0};
    Plug alpha_shadow = {0};
    Plug zeta = {0};
    Plug *plugins[] = {&zeta, &alpha_shadow, &alpha};
    Vec_CompItem items = {0};
    Arena scratch;
    YewCompQuery query;
    u32 i;

    arena_init(&ed.arena);
    arena_init(&scratch);
    alpha.mf.name_text = "alpha";
    alpha.mf.desc = "alpha plugin";
    alpha.winner = true;
    alpha_shadow.mf.name_text = "alpha";
    alpha_shadow.winner = false;
    zeta.mf.name_text = "zeta";
    zeta.winner = true;
    sys.v = plugins;
    sys.n = (u32)YEW_ARRAY_LEN(plugins);
    ed.plug = &sys;

    yew_cmd_shutdown();
    yew_cmd_init();
    for (i = 0U; i < (u32)YEW_ARRAY_LEN(commands); i++) {
        char line[64];
        int n = snprintf(line, sizeof(line), "%s al", commands[i]);

        YEW_ASSERT(n > 0 && (size_t)n < sizeof(line));
        YEW_ASSERT(yew_comp_query(&ed, line, (size_t)n, (size_t)n,
                                  &scratch, &query));
        YEW_ASSERT_EQ_I64(query.kind, YEW_COMP_PLUGIN);
        YEW_ASSERT_EQ_STR(query.stem, "al");
    }

    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&ed, YEW_COMP_PLUGIN, "", &items), 2U);
    YEW_ASSERT_EQ_U64(items.len, 2U);
    YEW_ASSERT_EQ_STR(items.data[0].text, "zeta");
    YEW_ASSERT_EQ_STR(items.data[1].text, "alpha");
    YEW_ASSERT_NOT_NULL(completion_find(&items, "alpha"));
    YEW_ASSERT_EQ_STR(completion_find(&items, "alpha")->detail,
                      "alpha plugin");
    Vec_CompItem_free(&items);
    yew_cmd_shutdown();
    arena_free_all(&scratch);
    arena_free_all(&ed.arena);
}

void test_plug_completion_hides_unregistered_plugin_commands(void)
{
    Ed ed = {0};
    Vec_CompItem items = {0};
    CmdId id = YEW_CMD_NONE;
    char err[96];

    arena_init(&ed.arena);
    yew_cmd_shutdown();
    yew_cmd_init();
    YEW_ASSERT(yew_cmd_register_plugin("completion", "probe",
                                        completion_command, "probe", &id,
                                        err, sizeof(err)));
    YEW_ASSERT(id.v != 0U);
    (void)yew_comp_enumerate(&ed, YEW_COMP_CMD,
                             "plug.completion.probe", &items);
    YEW_ASSERT_NOT_NULL(completion_find(&items, "plug.completion.probe"));

    YEW_ASSERT(yew_cmd_unregister(id));
    (void)yew_comp_enumerate(&ed, YEW_COMP_CMD,
                             "plug.completion.probe", &items);
    YEW_ASSERT_NULL(completion_find(&items, "plug.completion.probe"));
    Vec_CompItem_free(&items);
    yew_cmd_shutdown();
    arena_free_all(&ed.arena);
}
