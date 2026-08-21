#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/context.h"
#include "mod/ai/registry.h"
#include "mod/ai/shadow_ai.h"

typedef struct PrivacyProbe {
    u64 context_builds;
    u64 http_bytes;
    u64 curl_bytes;
    u32 http_starts;
    u32 curl_starts;
    Bytebuf body;
} PrivacyProbe;

static ShadowReq privacy_request(const Ed *ed, u32 seq)
{
    ShadowReq request = {0};
    u64 at = yew_textbuf_len(ed->win->buf->tb);

    request.buf_id = ed->win->buf->id;
    request.buf_gen = ed->win->buf->tb->gen;
    request.pos = BYTEOFF(at);
    request.line = yew_textbuf_line_span(ed->win->buf->tb,
        yew_textbuf_line_of(ed->win->buf->tb, request.pos));
    request.seq = seq;
    request.prov = YEW_SHADOW_AI;
    return request;
}

static void privacy_context_count(void *opaque)
{
    PrivacyProbe *probe = opaque;

    probe->context_builds++;
}

static bool privacy_transport_count(void *opaque, u8 transport,
                                    const u8 *body, u64 len)
{
    PrivacyProbe *probe = opaque;

    if (transport == (u8)YEW_AI_TR_HTTP) {
        probe->http_starts++;
        probe->http_bytes += len;
    } else {
        probe->curl_starts++;
        probe->curl_bytes += len;
    }
    probe->body.len = 0U;
    bytebuf_append(&probe->body, body, (size_t)len);
    return true;
}

static bool privacy_allow_workspace(Ed *ed, const char *root)
{
    (void)ed;
    (void)root;
    return true;
}

static void privacy_enable_backend(Ed *ed, bool cloud)
{
    static const char config[] =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", "
        "model: \"qwen2.5-coder:7b\"})\n"
        "ai.backend(\"cloud\", {kind: \"anthropic\", "
        "url: \"https://api.anthropic.com\", model: \"sonnet\", "
        "transport: \"curl\", key_env: \"ANTHROPIC_API_KEY\"})\n";
    OptVal enabled = {YEW_OPT_BOOL, {.b = true}};
    const char *name = cloud ? "cloud" : "local";
    OptVal backend = {YEW_OPT_STR, {.str = {name, 5U}}};
    const char *error = NULL;

    YEW_ASSERT_EQ_I64(yew_fl_eval(ed, config, sizeof(config) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.backend", 10U,
                           &backend, &error));
    YEW_ASSERT_NULL(error);
    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.enable", 9U,
                           &enabled, &error));
    YEW_ASSERT_NULL(error);
}

static void privacy_probe_install(PrivacyProbe *probe)
{
    (void)memset(probe, 0, sizeof(*probe));
    bytebuf_init(&probe->body);
    yew_ai_workspace_policy_set(privacy_allow_workspace);
    yew_ai_path_policy_set(NULL);
    yew_ai_context_build_test_set(privacy_context_count, probe);
    yew_ai_transport_test_set(privacy_transport_count, probe);
}

static void privacy_probe_remove(PrivacyProbe *probe)
{
    yew_ai_transport_test_set(NULL, NULL);
    yew_ai_context_build_test_set(NULL, NULL);
    yew_ai_workspace_policy_set(NULL);
    yew_ai_path_policy_set(NULL);
    bytebuf_free(&probe->body);
}

void test_ai_privacy_cloud_redaction_transmits_zero_bytes_on_both_transports(void)
{
    static const u8 source[] =
        "line one\nAuthorization: Bearer abcdefghijklmnop\n";
    PrivacyProbe probe;
    Ed ed;
    ShadowReq request;
    AiBackendEntry *entry;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, source, sizeof(source) - 1U,
                                  "cloud-secret.c"));
    privacy_enable_backend(&ed, true);
    privacy_probe_install(&probe);

    request = privacy_request(&ed, 1U);
    YEW_ASSERT(!yew_ai_shadow_test_request(&ed, &request));
    YEW_ASSERT_EQ_U64(probe.context_builds, 1U);
    YEW_ASSERT_EQ_U64(probe.curl_starts, 0U);
    YEW_ASSERT_EQ_U64(probe.curl_bytes, 0U);
    YEW_ASSERT(strstr(ed.msg.text, "line 2 matches 'bearer-token'") != NULL);

    entry = yew_ai_registry_find_mut(&ed.ai->backends, "cloud");
    YEW_ASSERT_NOT_NULL(entry);
    entry->backend.transport = (u8)YEW_AI_TR_HTTP;
    request = privacy_request(&ed, 2U);
    YEW_ASSERT(!yew_ai_shadow_test_request(&ed, &request));
    YEW_ASSERT_EQ_U64(probe.context_builds, 2U);
    YEW_ASSERT_EQ_U64(probe.http_starts, 0U);
    YEW_ASSERT_EQ_U64(probe.http_bytes, 0U);
    YEW_ASSERT_EQ_U64(probe.curl_bytes, 0U);

    privacy_probe_remove(&probe);
    yew_ed_free(&ed);
}

void test_ai_privacy_loopback_elision_reaches_exact_request_body(void)
{
    static const u8 source[] =
        "before sk-0123456789abcdefghijklmnop after";
    static const char golden[] =
        "{\"model\":\"qwen2.5-coder:7b\","
        "\"prompt\":\"before <redacted:openai-key> after\","
        "\"suffix\":\"\","
        "\"system\":\"File: src/redact.c\\nLanguage: c\\n\","
        "\"stream\":true,\"raw\":false,"
        "\"options\":{\"num_predict\":256,\"temperature\":0.01,"
        "\"stop\":[\"\\n\\n\",\"\\n}\",\"```\"]}}";
    PrivacyProbe probe;
    Ed ed;
    ShadowReq request;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, source, sizeof(source) - 1U,
                                  "loopback-secret.c"));
    ed.win->buf->path = "src/redact.c";
    ed.win->buf->lang = "c";
    privacy_enable_backend(&ed, false);
    privacy_probe_install(&probe);

    request = privacy_request(&ed, 1U);
    YEW_ASSERT(yew_ai_shadow_test_request(&ed, &request));
    YEW_ASSERT_EQ_U64(probe.http_starts, 1U);
    YEW_ASSERT_EQ_U64(probe.curl_starts, 0U);
    bytebuf_push_u8(&probe.body, 0U);
    YEW_ASSERT_EQ_STR((const char *)probe.body.data, golden);
    YEW_ASSERT_NOT_NULL(strstr((const char *)probe.body.data,
                               "before <redacted:openai-key> after"));
    YEW_ASSERT_NULL(strstr((const char *)probe.body.data,
                           "sk-0123456789abcdefghijklmnop"));

    privacy_probe_remove(&probe);
    yew_ed_free(&ed);
}

static bool privacy_exclude_every_path(Ed *ed, const char *path)
{
    (void)ed;
    (void)path;
    return true;
}

void test_ai_privacy_excluded_buffer_builds_no_context(void)
{
    PrivacyProbe probe;
    Ed ed;
    ShadowReq request;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"ordinary text", 13U,
                                  ".env.production"));
    ed.win->buf->path = ".env.production";
    privacy_enable_backend(&ed, false);
    privacy_probe_install(&probe);
    yew_ai_path_policy_set(privacy_exclude_every_path);

    request = privacy_request(&ed, 1U);
    YEW_ASSERT(!yew_ai_shadow_test_request(&ed, &request));
    YEW_ASSERT_EQ_U64(probe.context_builds, 0U);
    YEW_ASSERT_EQ_U64(probe.http_starts, 0U);
    YEW_ASSERT_EQ_U64(probe.http_bytes, 0U);

    privacy_probe_remove(&probe);
    yew_ed_free(&ed);
}
