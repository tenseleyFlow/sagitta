/* Sprint 43: provider preference, cycling, commands, and stats. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "edit/shadow.h"

static void arbitration_fixture(Ed *ed)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, NULL, 0U, "shadow-arbitration"));
}

static void arbitration_deliver(Ed *ed, u8 prov, const char *text)
{
    ShadowSug suggestion = {0};
    u32 len = (u32)strlen(text);

    suggestion.seq = ed->win->shadow.seq_next[prov]++;
    suggestion.prov = prov;
    suggestion.buf_id = ed->win->buf->id;
    suggestion.buf_gen = ed->win->buf->tb->gen;
    suggestion.pos = BYTEOFF(0U);
    suggestion.text = (const u8 *)text;
    suggestion.len = len;
    yew_shadow_deliver(ed, &suggestion);
}

static void arbitration_deliver_order(Ed *ed, const u8 order[3])
{
    static const char *const text[YEW_SHADOW_NPROV] = {
        "index-answer", "lsp-answer", "ai-answer",
    };
    u32 i;

    for (i = 0U; i < 3U; i++)
        arbitration_deliver(ed, order[i], text[order[i]]);
}

void test_shadow_stats_names_every_unregistered_provider_sprint(void)
{
    Ed ed;
    CmdCtx command = {0};
    CmdId stats;

    arbitration_fixture(&ed);
    stats = yew_cmd_lookup("ed.shadow.stats", 15U);
    YEW_ASSERT(stats.v != 0U);
    command.source = YEW_SRC_TEST;
    command.count = 1U;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, stats, &command), YEW_CMD_OK);
    YEW_ASSERT(strstr(ed.msg.text, "index — (Sprint 44)") != NULL);
    YEW_ASSERT(strstr(ed.msg.text, "lsp — (Sprint 47)") != NULL);
    YEW_ASSERT(strstr(ed.msg.text, "ai — (Sprint 49)") != NULL);
    YEW_ASSERT(strstr(ed.msg.text, "requests=0 delivered=0") != NULL);
    yew_ed_free(&ed);
}

void test_shadow_arbitration_is_independent_of_delivery_order(void)
{
    static const u8 forward[] = {
        YEW_SHADOW_INDEX, YEW_SHADOW_LSP, YEW_SHADOW_AI,
    };
    static const u8 reverse[] = {
        YEW_SHADOW_AI, YEW_SHADOW_LSP, YEW_SHADOW_INDEX,
    };
    Ed first;
    Ed second;
    u32 i;

    arbitration_fixture(&first);
    arbitration_fixture(&second);
    arbitration_deliver_order(&first, forward);
    arbitration_deliver_order(&second, reverse);

    YEW_ASSERT(first.win->shadow.live);
    YEW_ASSERT(second.win->shadow.live);
    YEW_ASSERT_EQ_U64(first.win->shadow.selected, YEW_SHADOW_INDEX);
    YEW_ASSERT_EQ_U64(second.win->shadow.selected, YEW_SHADOW_INDEX);
    YEW_ASSERT_EQ_U64(first.win->shadow.sug.prov, YEW_SHADOW_INDEX);
    YEW_ASSERT_EQ_U64(second.win->shadow.sug.prov, YEW_SHADOW_INDEX);
    YEW_ASSERT_EQ_U64(first.win->shadow.sug.len,
                      second.win->shadow.sug.len);
    YEW_ASSERT_EQ_MEM(first.win->shadow.sug.text,
                      second.win->shadow.sug.text,
                      first.win->shadow.sug.len);
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        YEW_ASSERT(first.win->shadow.answers[i].live);
        YEW_ASSERT(second.win->shadow.answers[i].live);
    }
    YEW_ASSERT_EQ_U64(first.shadow_stats.delivered, 3U);
    YEW_ASSERT_EQ_U64(second.shadow_stats.delivered, 3U);
    yew_ed_free(&second);
    yew_ed_free(&first);
}

void test_shadow_next_prev_cycle_in_configured_provider_order(void)
{
    static const u8 reverse[] = {
        YEW_SHADOW_AI, YEW_SHADOW_LSP, YEW_SHADOW_INDEX,
    };
    Ed ed;

    arbitration_fixture(&ed);
    arbitration_deliver_order(&ed, reverse);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_INDEX);
    YEW_ASSERT(yew_shadow_next(&ed, ed.win));
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_LSP);
    YEW_ASSERT(yew_shadow_next(&ed, ed.win));
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_AI);
    YEW_ASSERT(yew_shadow_next(&ed, ed.win));
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_INDEX);
    YEW_ASSERT(yew_shadow_prev(&ed, ed.win));
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_AI);
    YEW_ASSERT(ed.win->shadow.selected_by_user);

    arbitration_deliver(&ed, YEW_SHADOW_LSP, "new-lsp-answer");
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_AI);
    YEW_ASSERT(yew_shadow_prev(&ed, ed.win));
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_LSP);
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, "new-lsp-answer", 14U);
    yew_ed_free(&ed);
}

void test_shadow_preference_option_controls_default_selection(void)
{
    static const u8 forward[] = {
        YEW_SHADOW_INDEX, YEW_SHADOW_LSP, YEW_SHADOW_AI,
    };
    OptVal order = {YEW_OPT_STR, {.str = {"ai index lsp", 12U}}};
    const char *error = NULL;
    Ed ed;

    arbitration_fixture(&ed);
    YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_SCOPE_DECLARED,
                           "shadow.providers", 16U, &order, &error));
    arbitration_deliver_order(&ed, forward);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_AI);
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, "ai-answer", 9U);
    yew_ed_free(&ed);
}

void test_shadow_toggle_command_updates_option_and_dismisses(void)
{
    Ed ed;
    CmdCtx command = {0};
    CmdId toggle;
    OptVal value;

    arbitration_fixture(&ed);
    arbitration_deliver(&ed, YEW_SHADOW_INDEX, "answer");
    toggle = yew_cmd_lookup("ed.shadow.toggle", 16U);
    YEW_ASSERT(toggle.v != 0U);
    command.source = YEW_SRC_TEST;
    command.count = 1U;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, toggle, &command), YEW_CMD_OK);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "shadow.enable", 13U,
                           &value));
    YEW_ASSERT(!value.as.b);
    YEW_ASSERT_EQ_STR(ed.msg.text, "shadow text disabled");

    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, toggle, &command), YEW_CMD_OK);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "shadow.enable", 13U,
                           &value));
    YEW_ASSERT(value.as.b);
    YEW_ASSERT_EQ_STR(ed.msg.text, "shadow text enabled");
    yew_ed_free(&ed);
}
