#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/backend_curl.h"
#include "mod/ai/context.h"
#include "mod/ai/http.h"
#include "mod/ai/registry.h"
#include "mod/ai/shadow_ai.h"
#include "ui/message.h"

static bool policy_deny_workspace(Ed *ed, const char *root)
{
    (void)ed;
    (void)root;
    return false;
}

static bool policy_exclude_path(Ed *ed, const char *path)
{
    (void)ed;
    (void)path;
    return true;
}

static bool policy_redact(Ed *ed, const AiCtx *context, RedactHit *hit)
{
    (void)ed;
    (void)context;
    if (hit != NULL) {
        hit->rule = "fixture";
        hit->line_1based = 1U;
        hit->len = 1U;
    }
    return true;
}

static bool policy_set_bool(Ed *ed, const char *name, bool value)
{
    OptVal option = {YEW_OPT_BOOL, {.b = value}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static bool policy_set_str(Ed *ed, const char *name, const char *value)
{
    OptVal option = {YEW_OPT_STR,
                     {.str = {value, (u32)strlen(value)}}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static bool policy_define(Ed *ed, bool curl_transport)
{
    const char *source_http =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"openai\", transport: \"http\", "
        "url: \"http://127.0.0.1:9/v1\", model: \"test\"})\n";
    const char *source_curl =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"openai\", transport: \"curl\", "
        "url: \"https://127.0.0.1:443/v1\", model: \"test\"})\n";
    const char *source = curl_transport ? source_curl : source_http;

    return yew_fl_eval(ed, source, (u32)strlen(source)) == YEW_CMD_OK;
}

static void policy_fire(Ed *ed)
{
    ed->now_ms += 1000;
    yew_shadow_arm(ed, ed->win);
    ed->now_ms += 350;
    yew_timers_fire(&ed->timers, ed, ed->now_ms);
}

static int policy_child(void)
{
    char status[512];
    Ed ed;
    AiBackendEntry *entry;
    u64 sockets;

    if (setenv("YEW_AI_MOCK", "1", 1) != 0 ||
        unsetenv("YEW_SHADOW_TEST") != 0)
        return 10;
    yew_ai_shadow_init(NULL);
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, NULL, 0U, "ai-shadow-policy"))
        return 11;
    if (!policy_set_str(&ed, "shadow.providers", "ai"))
        return 12;

    yew_shadow_stats_format(&ed, status, sizeof(status));
    if (strstr(status, "ai ready") == NULL ||
        strstr(status, "ai — (Sprint 49)") != NULL)
        return 13;
    sockets = yew_http_socket_call_count();

    /* Row 1: the default-off profile silently declines before socket(2). */
    policy_fire(&ed);
    if (ed.msg.active || ed.ai->call.active ||
        yew_http_socket_call_count() != sockets)
        return 14;

    if (!policy_set_bool(&ed, "ai.enable", true))
        return 15;
    /* Row 2a: no selected backend. */
    policy_fire(&ed);
    if (ed.msg.active || ed.ai->call.active ||
        yew_http_socket_call_count() != sockets)
        return 16;

    if (!policy_define(&ed, false) ||
        !policy_set_str(&ed, "ai.backend", "local"))
        return 17;
    entry = yew_ai_registry_find_mut(&ed.ai->backends, "local");
    if (entry == NULL)
        return 18;

    /* Row 2b: a selected backend in cooldown. */
    entry->cooldown.until_ms = yew_now_ms() + 60000;
    policy_fire(&ed);
    if (ed.msg.active || ed.ai->call.active ||
        yew_http_socket_call_count() != sockets)
        return 19;
    entry->cooldown.until_ms = 0;

    /* Rows 3–5: trust, excluded path, and the pre-prompt redaction gate. */
    yew_ai_workspace_policy_set(policy_deny_workspace);
    policy_fire(&ed);
    yew_ai_workspace_policy_set(NULL);
    if (ed.msg.active || ed.ai->call.active ||
        yew_http_socket_call_count() != sockets)
        return 20;
    yew_ai_path_policy_set(policy_exclude_path);
    policy_fire(&ed);
    yew_ai_path_policy_set(NULL);
    if (ed.msg.active || ed.ai->call.active ||
        yew_http_socket_call_count() != sockets)
        return 21;
    entry->backend.url.loopback = false;
    yew_ai_redact_hook_set(policy_redact);
    policy_fire(&ed);
    yew_ai_redact_hook_set(NULL);
    entry->backend.url.loopback = true;
    if (!ed.msg.active || strstr(ed.msg.text, "line 1 matches 'fixture'") == NULL ||
        ed.ai->call.active ||
        yew_http_socket_call_count() != sockets)
        return 22;
    yew_msg_clear(&ed);

    /* Row 6: a non-newer generation cannot replace the global call. */
    ed.ai->call.active = true;
    ed.ai->call.seq = UINT32_MAX;
    ed.ai->call.buf_id = ed.win->buf->id;
    policy_fire(&ed);
    ed.ai->call.active = false;
    if (ed.msg.active || yew_http_socket_call_count() != sockets)
        return 23;

    /* Row 7: a failed curl probe warns once, then declines silently. */
    if (!policy_define(&ed, true))
        return 24;
    ed.ai->curl.state = YEW_CURL_ABSENT;
    policy_fire(&ed);
    if (!ed.msg.active || !ed.ai->curl_probe_messaged ||
        strstr(ed.msg.text, "curl") == NULL)
        return 25;
    yew_msg_clear(&ed);
    policy_fire(&ed);
    if (ed.msg.active || yew_http_socket_call_count() != sockets)
        return 26;

    yew_ed_free(&ed);
    return 0;
}

void test_ai_shadow_provider_declines_silently_and_starts_no_socket(void)
{
    pid_t pid = fork();
    int status = 0;

    YEW_ASSERT(pid >= 0);
    if (pid == 0)
        _exit(policy_child());
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
}
