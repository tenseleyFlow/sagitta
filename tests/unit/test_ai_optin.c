#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flconf.h"
#include "mod/ai/ai.h"
#include "mod/ai/optin.h"
#include "ui/cmdline.h"
#include "ui/message.h"

typedef struct OptinCommitProbe {
    YewAiOptinBackend backend;
    char scope;
    u32 calls;
    bool fail_persistent;
} OptinCommitProbe;

static bool optin_commit_probe(Ed *ed, YewAiOptinBackend backend,
                               char scope, void *ctx)
{
    OptinCommitProbe *probe = ctx;

    (void)ed;
    probe->backend = backend;
    probe->scope = scope;
    probe->calls++;
    return !probe->fail_persistent || scope == 'o';
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

static u32 optin_count_text(const char *haystack, const char *needle)
{
    u32 count = 0U;
    size_t len = strlen(needle);
    const char *at = haystack;

    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += len;
    }
    return count;
}

static void optin_write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t len = strlen(text);

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, len, file), len);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static char *optin_read_text(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    char *text;

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_END), 0);
    size = ftell(file);
    YEW_ASSERT(size >= 0L);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    text = malloc((size_t)size + 1U);
    YEW_ASSERT_NOT_NULL(text);
    YEW_ASSERT_EQ_U64(fread(text, 1U, (size_t)size, file), (size_t)size);
    text[(size_t)size] = '\0';
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    return text;
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
    Bytebuf first;
    Bytebuf second;

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

    probe.fail_persistent = true;
    YEW_ASSERT(yew_ai_optin_begin_checked(&optin, &ed, optin_commit_probe,
                                          &probe, true));
    optin_answer(&ed, "1");
    optin_answer(&ed, "y");
    optin_answer(&ed, "w");
    YEW_ASSERT(optin.active);
    YEW_ASSERT_EQ_U64(optin.phase, 4U);
    YEW_ASSERT(strstr(optin_message(&ed), "session only") != NULL);
    YEW_ASSERT_EQ_U64(probe.calls, 1U);
    optin_answer(&ed, "y");
    YEW_ASSERT(!optin.active);
    YEW_ASSERT_EQ_U64(probe.calls, 2U);
    YEW_ASSERT_EQ_U64(probe.scope, (u64)'o');
    yew_ed_free(&ed);

    bytebuf_init(&first);
    bytebuf_init(&second);
    YEW_ASSERT(yew_ai_optin_config_merge(
        &first, "let before = 1\nlet after = 2\n",
        sizeof("let before = 1\nlet after = 2\n") - 1U,
        YEW_AI_OPTIN_LOCAL, true));
    bytebuf_push_u8(&first, 0U);
    YEW_ASSERT(strstr((const char *)first.data, "let before = 1") != NULL);
    YEW_ASSERT(strstr((const char *)first.data, "let after = 2") != NULL);
    YEW_ASSERT(strstr((const char *)first.data,
                      "\"ai.default_workspace\": \"allow\"") != NULL);
    YEW_ASSERT_EQ_U64(optin_count_text((const char *)first.data,
                                      "# yew AI opt-in"), 1U);
    YEW_ASSERT_EQ_U64(optin_count_text((const char *)first.data,
                                      "# end yew AI opt-in"), 1U);

    YEW_ASSERT(yew_ai_optin_config_merge(
        &second, (const char *)first.data, first.len - 1U,
        YEW_AI_OPTIN_CLOUD, false));
    bytebuf_push_u8(&second, 0U);
    YEW_ASSERT(strstr((const char *)second.data, "let before = 1") != NULL);
    YEW_ASSERT(strstr((const char *)second.data, "let after = 2") != NULL);
    YEW_ASSERT(strstr((const char *)second.data, "api.anthropic.com") != NULL);
    YEW_ASSERT(strstr((const char *)second.data,
                      "\"ai.default_workspace\": \"allow\"") == NULL);
    YEW_ASSERT_EQ_U64(optin_count_text((const char *)second.data,
                                      "# yew AI opt-in"), 1U);
    YEW_ASSERT_EQ_U64(optin_count_text((const char *)second.data,
                                      "# end yew AI opt-in"), 1U);
    bytebuf_free(&second);
    bytebuf_free(&first);
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
    YewAiOptin default_optin;
    YewEdStartup startup = {0};
    Bytebuf initial;
    char root[] = "/tmp/yew-ai-optin-XXXXXX";
    char config[sizeof(root) + sizeof("/init.fl")];
    char blocker[sizeof(root) + sizeof("/blocker")];
    char blocked_config[sizeof(root) + sizeof("/blocker/init.fl")];
    char state_blocker[sizeof(root) + sizeof("/state-blocker")];
    const char *state_env;
    char *saved_state = NULL;
    char *before;
    char *persisted;
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

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(config, sizeof(config), "%s/init.fl", root);
    bytebuf_init(&initial);
    YEW_ASSERT(yew_ai_optin_config_merge(&initial, "", 0U,
                                          YEW_AI_OPTIN_LOCAL, false));
    bytebuf_push_u8(&initial, 0U);
    optin_write_text(config, (const char *)initial.data);
    bytebuf_free(&initial);
    startup.config_path = config;
    yew_ed_init(&ed);
    yew_config_init(&ed, &startup);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "ai-commands"));
    cx.ed = &ed;
    cx.win = ed.win;
    cx.source = YEW_SRC_TEST;
    cx.count = 1U;

    YEW_ASSERT_EQ_I64(yew_ai_cmd_disable(&cx), YEW_CMD_OK);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &enabled));
    YEW_ASSERT_EQ_U64(enabled.type, YEW_OPT_BOOL);
    YEW_ASSERT(!enabled.as.b);
    persisted = optin_read_text(config);
    YEW_ASSERT(strstr(persisted, "\"ai.enable\": false") != NULL);
    YEW_ASSERT(strstr(persisted, "\"ai.backend\": \"local\"") != NULL);
    free(persisted);
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

    yew_ed_init(&ed);
    yew_config_init(&ed, &startup);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "optin-restart"));
    YEW_ASSERT_EQ_I64(yew_config_load_all(&ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &enabled));
    YEW_ASSERT_EQ_U64(enabled.type, YEW_OPT_BOOL);
    YEW_ASSERT(!enabled.as.b);
    yew_ed_free(&ed);

    (void)snprintf(blocker, sizeof(blocker), "%s/blocker", root);
    (void)snprintf(blocked_config, sizeof(blocked_config),
                   "%s/blocker/init.fl", root);
    optin_write_text(blocker, "not a directory\n");
    startup.config_path = blocked_config;
    yew_ed_init(&ed);
    yew_config_init(&ed, &startup);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "optin-failure"));
    YEW_ASSERT(yew_ai_optin_begin_default_checked(&default_optin, &ed,
                                                   true));
    optin_answer(&ed, "1");
    optin_answer(&ed, "y");
    optin_answer(&ed, "w");
    YEW_ASSERT(default_optin.active);
    YEW_ASSERT_EQ_U64(default_optin.phase, 4U);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &enabled));
    YEW_ASSERT(!enabled.as.b);
    YEW_ASSERT_NULL(yew_ai_backend_selected(&ed));
    persisted = optin_read_text(blocker);
    YEW_ASSERT_EQ_STR(persisted, "not a directory\n");
    free(persisted);
    optin_answer(&ed, "y");
    YEW_ASSERT(!default_optin.active);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &enabled));
    YEW_ASSERT(enabled.as.b);
    YEW_ASSERT_NOT_NULL(yew_ai_backend_selected(&ed));
    YEW_ASSERT_EQ_U64(yew_ai_workspace_grant(&ed), YEW_AI_WS_ALLOW);
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(unlink(blocker), 0);
    startup.config_path = config;
    (void)snprintf(state_blocker, sizeof(state_blocker),
                   "%s/state-blocker", root);
    optin_write_text(state_blocker, "not a directory\n");
    state_env = getenv("XDG_STATE_HOME");
    if (state_env != NULL) {
        saved_state = strdup(state_env);
        YEW_ASSERT_NOT_NULL(saved_state);
    }
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state_blocker, 1), 0);
    before = optin_read_text(config);
    yew_ed_init(&ed);
    yew_config_init(&ed, &startup);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "optin-trust-failure"));
    YEW_ASSERT(yew_ai_optin_begin_default_checked(&default_optin, &ed,
                                                   true));
    optin_answer(&ed, "1");
    optin_answer(&ed, "y");
    optin_answer(&ed, "w");
    YEW_ASSERT(default_optin.active);
    YEW_ASSERT_EQ_U64(default_optin.phase, 4U);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &enabled));
    YEW_ASSERT(!enabled.as.b);
    YEW_ASSERT_NULL(yew_ai_backend_selected(&ed));
    YEW_ASSERT_EQ_U64(yew_ai_workspace_grant(&ed), YEW_AI_WS_UNSET);
    persisted = optin_read_text(config);
    YEW_ASSERT_EQ_STR(persisted, before);
    free(persisted);
    free(before);
    optin_answer(&ed, "n");
    YEW_ASSERT(!default_optin.active);
    yew_ed_free(&ed);
    if (saved_state != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_state, 1), 0);
        free(saved_state);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    YEW_ASSERT_EQ_I64(unlink(state_blocker), 0);
    YEW_ASSERT_EQ_I64(unlink(config), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}
