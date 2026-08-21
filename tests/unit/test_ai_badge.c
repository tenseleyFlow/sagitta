#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"

static void badge_set(Ed *ed, const char *name, OptVal value)
{
    const char *error = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                           &value, &error));
    YEW_ASSERT_NULL(error);
}

static bool badge_try_set(Ed *ed, const char *name, OptVal value,
                          const char **error)
{
    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &value, error);
}

static void badge_select(Ed *ed, const char *name)
{
    OptVal value = {YEW_OPT_STR,
                    {.str = {name, (u32)strlen(name)}}};

    badge_set(ed, "ai.backend", value);
}

static void badge_define_backends(Ed *ed)
{
    static const char source[] =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", model: \"qwen\"})\n"
        "ai.backend(\"remote\", {kind: \"anthropic\", "
        "url: \"https://api.anthropic.com\", model: \"sonnet\", "
        "transport: \"curl\", key_env: \"ANTHROPIC_API_KEY\"})\n";

    YEW_ASSERT_EQ_I64(yew_fl_eval(ed, source, sizeof(source) - 1U),
                      YEW_CMD_OK);
}

void test_ai_badge_states_hosts_and_priority(void)
{
    Ed ed;
    char out[128];
    u8 priority = 0U;
    OptVal enabled = {YEW_OPT_BOOL, {.b = true}};
    OptVal host_max = {YEW_OPT_INT, {.i = 8}};

    yew_ed_init(&ed);
    yew_ai_workspace_session_set(&ed, YEW_AI_WS_ALLOW);
    YEW_ASSERT(!yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    badge_set(&ed, "ai.enable", enabled);
    YEW_ASSERT(!yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    badge_define_backends(&ed);

    badge_select(&ed, "local");
    yew_ai_workspace_session_set(&ed, YEW_AI_WS_DENY);
    YEW_ASSERT(!yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    yew_ai_workspace_session_set(&ed, YEW_AI_WS_ALLOW);
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI]");
    YEW_ASSERT_EQ_U64(priority, 5U);

    ed.ai->call.active = true;
    ed.ai->call.entry = (AiBackendEntry *)yew_ai_backend_at(&ed, 0U);
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI~]");
    ed.ai->call.active = false;
    ed.ai->call.entry = NULL;

    yew_ai_status_note(&ed, YEW_AI_ERR_AUTH);
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI!]");
    yew_ai_status_note(&ed, YEW_AI_ERR_CANCELLED);
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI!]");
    yew_ai_status_clear(&ed);

    badge_select(&ed, "remote");
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI->api.anthropic.com]");
    YEW_ASSERT_EQ_U64(priority, 2U);
    badge_set(&ed, "ai.badge_host_max", host_max);
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI->…pic.com]");
    yew_ai_status_note(&ed, YEW_AI_ERR_TIMEOUT);
    YEW_ASSERT(yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    YEW_ASSERT_EQ_STR(out, "[AI->…pic.com!]");
    yew_ed_free(&ed);
}

void test_ai_badge_remote_cannot_be_hidden(void)
{
    Ed ed;
    const char *error = NULL;
    OptVal off = {YEW_OPT_STR, {.str = {"off", 3U}}};
    OptVal on = {YEW_OPT_STR, {.str = {"on", 2U}}};
    OptVal remote = {YEW_OPT_STR, {.str = {"remote", 6U}}};
    char out[32];
    u8 priority = 0U;

    yew_ed_init(&ed);
    badge_define_backends(&ed);
    badge_select(&ed, "remote");
    YEW_ASSERT(!badge_try_set(&ed, "ai.badge", off, &error));
    YEW_ASSERT_EQ_STR(error,
                      "ai.badge=off is not allowed with a remote AI backend");

    badge_select(&ed, "local");
    badge_set(&ed, "ai.badge", off);
    YEW_ASSERT(!yew_ai_status_badge(&ed, out, sizeof(out), &priority));
    error = NULL;
    YEW_ASSERT(!badge_try_set(&ed, "ai.backend", remote, &error));
    YEW_ASSERT_EQ_STR(error, "remote AI backends require ai.badge=on");
    badge_set(&ed, "ai.badge", on);
    badge_select(&ed, "remote");
    yew_ed_free(&ed);
}
