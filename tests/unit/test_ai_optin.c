#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai.h"
#include "mod/ai/optin.h"
#include "ui/cmdline.h"
#include "ui/message.h"

typedef struct OptinCommitProbe {
    YewAiOptinBackend backend;
    char scope;
    u32 calls;
} OptinCommitProbe;

static bool optin_commit_probe(Ed *ed, YewAiOptinBackend backend,
                               char scope, void *ctx)
{
    OptinCommitProbe *probe = ctx;

    (void)ed;
    probe->backend = backend;
    probe->scope = scope;
    probe->calls++;
    return true;
}

static const char *optin_message(const Ed *ed)
{
    return ed->msg.full == NULL ? ed->msg.text : ed->msg.full;
}

static void optin_answer(Ed *ed, const char *answer)
{
    if (answer != NULL && answer[0] != '\0')
        yew_cmdline_paste(ed, (const u8 *)answer, strlen(answer));
    yew_cmdline_close(ed, true);
}

void test_ai_optin_local_and_cloud_confirmations_are_distinct(void)
{
    Ed ed;
    YewAiOptin optin;
    OptinCommitProbe probe = {0};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "optin"));
    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    YEW_ASSERT(strstr(optin_message(&ed), "Choice [1/2/3/n]") != NULL);
    optin_answer(&ed, "1");
    YEW_ASSERT_EQ_U64(optin.phase, 2U);
    YEW_ASSERT(strstr(optin_message(&ed), "127.0.0.1:11434") != NULL);
    optin_answer(&ed, "");
    YEW_ASSERT(!optin.active);
    YEW_ASSERT_EQ_U64(probe.calls, 0U);

    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    optin_answer(&ed, "1");
    optin_answer(&ed, "y");
    YEW_ASSERT_EQ_U64(optin.phase, 3U);
    YEW_ASSERT(strstr(optin_message(&ed), "Choice [w/a/o]") != NULL);
    optin_answer(&ed, "w");
    YEW_ASSERT_EQ_U64(probe.calls, 1U);
    YEW_ASSERT_EQ_U64(probe.backend, YEW_AI_OPTIN_LOCAL);
    YEW_ASSERT_EQ_U64(probe.scope, 'w');

    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    optin_answer(&ed, "2");
    YEW_ASSERT(strstr(optin_message(&ed), "literal") == NULL);
    YEW_ASSERT(strstr(optin_message(&ed), "word 'send'") != NULL);
    optin_answer(&ed, "y");
    YEW_ASSERT(!optin.active);
    YEW_ASSERT_EQ_U64(probe.calls, 1U);

    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    optin_answer(&ed, "2");
    optin_answer(&ed, "send");
    optin_answer(&ed, "o");
    YEW_ASSERT_EQ_U64(probe.calls, 2U);
    YEW_ASSERT_EQ_U64(probe.backend, YEW_AI_OPTIN_CLOUD);
    YEW_ASSERT_EQ_U64(probe.scope, 'o');
    yew_ed_free(&ed);
}

void test_ai_optin_escape_and_no_tty_never_commit(void)
{
    Ed ed;
    YewAiOptin optin;
    OptinCommitProbe probe = {0};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "optin-cancel"));
    YEW_ASSERT(!yew_ai_optin_begin_checked(&optin, &ed,
                                           optin_commit_probe, &probe,
                                           false));
    YEW_ASSERT_EQ_STR(ed.msg.text, yew_ai_optin_no_tty_message());
    YEW_ASSERT(!ed.cmdline.active);
    YEW_ASSERT_EQ_U64(probe.calls, 0U);

    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    yew_cmdline_close(&ed, false);
    YEW_ASSERT(!optin.active);
    YEW_ASSERT_EQ_U64(probe.calls, 0U);

    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    optin_answer(&ed, "2");
    yew_cmdline_close(&ed, false);
    YEW_ASSERT(!optin.active);
    YEW_ASSERT_EQ_U64(probe.calls, 0U);

    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    optin_answer(&ed, "1");
    optin_answer(&ed, "y");
    yew_cmdline_close(&ed, false);
    YEW_ASSERT(!optin.active);
    YEW_ASSERT_EQ_U64(probe.calls, 0U);
    yew_ed_free(&ed);
}

void test_ai_privacy_commands_are_live(void)
{
    static const struct {
        const char *name;
        u8 arity;
    } rows[] = {
        {"ed.ai.enable", YEW_ARITY_NONE},
        {"ed.ai.disable", YEW_ARITY_NONE},
        {"ed.ai.forget", YEW_ARITY_NONE},
        {"ed.ai.privacy", YEW_ARITY_NONE},
        {"ed.ai.preset", YEW_ARITY_STR},
        {"ed.ai.status", YEW_ARITY_NONE}
    };
    Ed ed;
    CmdCtx cx = {0};
    OptVal enabled;
    size_t i;

    for (i = 0U; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CmdId id = yew_cmd_lookup(rows[i].name,
                                  (u32)strlen(rows[i].name));
        const CmdDesc *desc;

        YEW_ASSERT(id.v != 0U);
        desc = yew_cmd_desc(id);
        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT_EQ_U64(desc->arity, rows[i].arity);
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
    }

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "ai-commands"));
    cx.ed = &ed;
    cx.win = ed.win;
    cx.source = YEW_SRC_TEST;
    cx.count = 1U;

    YEW_ASSERT_EQ_I64(yew_ai_cmd_disable(&cx), YEW_CMD_OK);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &enabled));
    YEW_ASSERT_EQ_U64(enabled.type, YEW_OPT_BOOL);
    YEW_ASSERT(!enabled.as.b);
    YEW_ASSERT_EQ_I64(yew_ai_cmd_enable(&cx), YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_STR(ed.msg.text, yew_ai_optin_no_tty_message());
    yew_msg_clear(&ed);

    cx.sarg = "local";
    cx.sarg_len = 5U;
    YEW_ASSERT_EQ_I64(yew_ai_cmd_preset(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_ai_cmd_status(&cx), YEW_CMD_OK);
    YEW_ASSERT(strstr(optin_message(&ed), "backend=local") != NULL);
    YEW_ASSERT_EQ_I64(yew_ai_cmd_privacy(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_STR(yew_buf_label(ed.win->buf), "[AI Privacy]");
    yew_ed_free(&ed);
}
