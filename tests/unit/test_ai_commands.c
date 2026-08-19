#include "harness.h"

#include "edit/cmd.h"
#include "edit/ed.h"
#include "ui/cmdline.h"
#include "ui/message.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    u32 sprint;
} AiCommandRow;

void test_ai_commands_cross_module_boundary(void)
{
    static const AiCommandRow rows[] = {
        {"ed.ai.backends", 0U},
        {"ed.ai.models", 0U},
        {"ed.ai.ping", 0U},
        {"ed.ai.log", 0U},
        {"ed.ai.reload", 0U},
        {"ed.ai.enable", 50U},
        {"ed.ai.disable", 50U},
        {"ed.ai.stats", 49U}
    };
    Ed ed;
    CmdCtx cx = {0};
    size_t i;

    yew_ed_init(&ed);
    cx.ed = &ed;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        CmdId id = yew_cmd_lookup(rows[i].name, (u32)strlen(rows[i].name));
        const CmdDesc *desc;

        YEW_ASSERT(id.v != 0U);
        desc = yew_cmd_desc(id);
        YEW_ASSERT_NOT_NULL(desc);
#if YEW_WITH_AI
        if (rows[i].sprint != 0U) {
            char sprint[24];
            char diagnostic[96];

            (void)snprintf(sprint, sizeof(sprint), "Sprint %u",
                           rows[i].sprint);
            (void)snprintf(diagnostic, sizeof(diagnostic),
                           "%s lands in %s", rows[i].name, sprint);
            YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) != 0U);
            YEW_ASSERT_NOT_NULL(strstr(desc->help, sprint));
            yew_test_capture_log();
            YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx),
                              YEW_CMD_ERR_DEFERRED);
            YEW_ASSERT(ed.msg.active);
            YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
            YEW_ASSERT_EQ_STR(
                ed.msg.text,
                "AI is off; :ai enable turns it on (Sprint 50)");
            YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR, diagnostic));
        } else {
            YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
            YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx),
                              YEW_CMD_ERR_STATE);
            YEW_ASSERT(ed.msg.active);
            YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
            YEW_ASSERT_EQ_STR(
                ed.msg.text,
                "AI is off; :ai enable turns it on (Sprint 50)");
        }
#else
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
        YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx), YEW_CMD_ERR_STATE);
        YEW_ASSERT(ed.msg.active);
        YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
        YEW_ASSERT_EQ_STR(
            ed.msg.text,
            "this build has no ai module; rebuild with 'make MODULES=\"… ai\"'");
#endif
        yew_msg_clear(&ed);
    }
    yew_ed_free(&ed);
}

void test_ai_open_remains_sprint49_deferred(void)
{
    CmdId id = yew_cmd_lookup("ed.ai.open", 10U);
    const CmdDesc *desc;
    Ed ed;
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    desc = yew_cmd_desc(id);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) != 0U);
    YEW_ASSERT_NOT_NULL(strstr(desc->help, "Sprint 49"));
    yew_ed_init(&ed);
    cx.ed = &ed;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    yew_test_capture_log();
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx), YEW_CMD_ERR_DEFERRED);
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_ERROR, "ed.ai.open lands in Sprint 49"));
#if YEW_WITH_AI
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
    YEW_ASSERT_EQ_STR(ed.msg.text,
                      "AI is off; :ai enable turns it on (Sprint 50)");
#endif

    /* Exercise the real command-line dispatch path.  Its inline error wins
     * drawing priority over ed.msg, so it must preserve the AI off-state
     * guidance while also naming the deliberately deferred sprint. */
    ed.clean = true;
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, "ai.open");
    cx.win = yew_cmdline_target(&ed);
    YEW_ASSERT_EQ_I64(yew_cmdline_cmd_accept(&cx),
                      YEW_CMD_ERR_DEFERRED);
    YEW_ASSERT(ed.cmdline.active);
#if YEW_WITH_AI
    YEW_ASSERT_EQ_STR(
        ed.cmdline.err.msg,
        "AI is off; :ai enable turns it on (Sprint 50); "
        ":ai.open lands in Sprint 49");
#else
    YEW_ASSERT_EQ_STR(ed.cmdline.err.msg,
                      ":ai.open lands in Sprint 49");
#endif
    yew_ed_free(&ed);
}
