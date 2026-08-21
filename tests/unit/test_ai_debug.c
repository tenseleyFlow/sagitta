#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/debug.h"
#include "mod/ai/shadow_ai.h"

static bool log_contains(const Bytebuf *log, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0U)
        return true;
    if (nlen > log->len)
        return false;
    for (i = 0U; i <= log->len - nlen; i++) {
        if (memcmp(log->data + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static u32 log_count(const Bytebuf *log, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;
    u32 count = 0U;

    if (nlen == 0U || nlen > log->len)
        return 0U;
    for (i = 0U; i <= log->len - nlen; i++) {
        if (memcmp(log->data + i, needle, nlen) == 0)
            count++;
    }
    return count;
}

static void set_debug_bodies(Ed *ed, bool enabled)
{
    const char *error = NULL;
    OptVal value = {YEW_OPT_BOOL, {.b = enabled}};

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.debug_bodies", 15U,
                           &value, &error));
    YEW_ASSERT_NULL(error);
}

static bool debug_transport_start(void *opaque, u8 transport,
                                  const u8 *body, u64 len)
{
    u32 *starts = opaque;

    (void)transport;
    (void)body;
    (void)len;
    (*starts)++;
    return true;
}

static ShadowReq debug_request(const Ed *ed, u32 seq)
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

static void debug_set_str(Ed *ed, const char *name, const char *value)
{
    OptVal option = {YEW_OPT_STR,
                     {.str = {value, (u32)strlen(value)}}};
    const char *error = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                           &option, &error));
    YEW_ASSERT_NULL(error);
}

static void debug_set_bool(Ed *ed, const char *name, bool value)
{
    OptVal option = {YEW_OPT_BOOL, {.b = value}};
    const char *error = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                           &option, &error));
    YEW_ASSERT_NULL(error);
}

static void debug_cycle(Ed *ed, const char *name, const char *kind,
                        const char *transport, const char *key_env,
                        const char *completion, u32 *starts)
{
    char config[1024];
    ShadowReq request;
    AiCall *call;
    int n;

    n = snprintf(config, sizeof(config),
                 "import ai\n"
                 "ai.backend(\"%s\", {kind: \"%s\", "
                 "transport: \"%s\", url: \"http://127.0.0.1:9/v1\", "
                 "model: \"model-marker-s50\", key_env: \"%s\"})\n",
                 name, kind, transport, key_env);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(config));
    YEW_ASSERT_EQ_I64(yew_fl_eval(ed, config, (u32)n), YEW_CMD_OK);
    debug_set_str(ed, "ai.backend", name);
    debug_set_bool(ed, "ai.enable", true);
    yew_ai_workspace_session_set(ed, YEW_AI_WS_ALLOW);
    yew_ai_transport_test_set(debug_transport_start, starts);

    request = debug_request(ed, 1U);
    YEW_ASSERT(yew_ai_shadow_test_request(ed, &request));
    YEW_ASSERT_EQ_U64(*starts, 1U);
    call = &ed->ai->call;
    YEW_ASSERT(call->active);
    bytebuf_append(&call->raw, completion, strlen(completion));
    call->status = 200U;
    call->terminal = true;
    call->transport_done = true;
    call->live = false;
    call->t_first_token = call->t_armed + 2;
    call->t_done = call->t_armed + 4;
    call->adapter.input_tokens = 11;
    call->adapter.output_tokens = 3;
    yew_ai_shadow_pump(ed);
    YEW_ASSERT(!ed->ai->call.active);
    yew_ai_transport_test_set(NULL, NULL);
}

static void debug_editor(Ed *ed, const char *prompt)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, (const u8 *)prompt, strlen(prompt),
                                  "debug-cycle.c"));
    ed->win->buf->path = "src/debug-cycle.c";
    ed->win->buf->lang = "c";
}

void test_ai_debug_bodies_require_both_privacy_gates(void)
{
    static const u8 request[] = "request-unique-marker-s50";
    static const u8 completion[] = "completion-unique-marker-s50";
    const char *old = getenv("YEW_AI_DEBUG");
    char *saved = old != NULL ? strdup(old) : NULL;
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "1", 1), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    YEW_ASSERT(!log_contains(&ed.ai->log, (const char *)request));

    set_debug_bodies(&ed, true);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_DEBUG"), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    YEW_ASSERT(!log_contains(&ed.ai->log, (const char *)request));

    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "true", 1), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    YEW_ASSERT(!log_contains(&ed.ai->log, (const char *)request));

    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "1", 1), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    yew_ai_debug_body(&ed, "completion", completion,
                      sizeof(completion) - 1U);
    YEW_ASSERT(log_contains(&ed.ai->log, (const char *)request));
    YEW_ASSERT(log_contains(&ed.ai->log, (const char *)completion));
    YEW_ASSERT_EQ_U64(log_count(&ed.ai->log,
                                "WARN: AI debug body logging is enabled"),
                      1U);

    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_DEBUG"), 0);
    }
    yew_ed_free(&ed);
}

void test_ai_debug_normal_cycle_logs_metadata_without_bodies(void)
{
    static const char prompt[] = "prompt-unique-marker-s50";
    static const char completion[] = "completion-unique-marker-s50";
    const char *old_debug = getenv("YEW_AI_DEBUG");
    char *saved_debug = old_debug != NULL ? strdup(old_debug) : NULL;
    u32 starts = 0U;
    Ed ed;

    YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_DEBUG"), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_AI_DEBUG_KEY", "secret-value-s50", 1),
                      0);
    debug_editor(&ed, prompt);
    set_debug_bodies(&ed, true);
    debug_cycle(&ed, "debug-openai", "openai", "http",
                "YEW_TEST_AI_DEBUG_KEY", completion, &starts);

    YEW_ASSERT(log_contains(&ed.ai->log, "model=model-marker-s50"));
    YEW_ASSERT(log_contains(&ed.ai->log, "context="));
    YEW_ASSERT(log_contains(&ed.ai->log, "body="));
    YEW_ASSERT(log_contains(&ed.ai->log, "transport=http"));
    YEW_ASSERT(log_contains(&ed.ai->log,
                            "first-token=2 total=4 response=28 "
                            "tokens=11/3 class=0"));
    YEW_ASSERT(!log_contains(&ed.ai->log, prompt));
    YEW_ASSERT(!log_contains(&ed.ai->log, completion));
    YEW_ASSERT(!log_contains(&ed.ai->log, "debug request body"));
    YEW_ASSERT(!log_contains(&ed.ai->log, "<redacted"));

    yew_ed_free(&ed);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_TEST_AI_DEBUG_KEY"), 0);
    if (saved_debug != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", saved_debug, 1), 0);
        free(saved_debug);
    }
}

void test_ai_debug_dual_gate_cycle_logs_bodies_and_redacted_keys(void)
{
    static const char prompt[] = "prompt-unique-marker-s50";
    static const char completion[] = "completion-unique-marker-s50";
    static const char key[] = "secret-value-s50";
    const char *old_debug = getenv("YEW_AI_DEBUG");
    const char *old_level = getenv("YEW_LOG_LEVEL");
    char *saved_debug = old_debug != NULL ? strdup(old_debug) : NULL;
    char *saved_level = old_level != NULL ? strdup(old_level) : NULL;
    u32 starts = 0U;
    Ed ed;

    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "1", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_LOG_LEVEL", "debug", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_AI_DEBUG_KEY", key, 1), 0);
    yew_test_capture_log();
    debug_editor(&ed, prompt);
    set_debug_bodies(&ed, true);
    debug_cycle(&ed, "debug-openai", "openai", "http",
                "YEW_TEST_AI_DEBUG_KEY", completion, &starts);

    YEW_ASSERT(log_contains(&ed.ai->log, prompt));
    YEW_ASSERT(log_contains(&ed.ai->log, completion));
    YEW_ASSERT_EQ_U64(log_count(&ed.ai->log,
                                "WARN: AI debug body logging is enabled"),
                      1U);
    YEW_ASSERT(log_contains(&ed.ai->log,
                            "authorization: <redacted 16 bytes>"));
    YEW_ASSERT(!log_contains(&ed.ai->log, key));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_DEBUG,
                    "authorization: <redacted 16 bytes>"));
    YEW_ASSERT(!yew_test_log_contains(YEW_LOG_DEBUG, key));
    yew_ed_free(&ed);

    starts = 0U;
    debug_editor(&ed, prompt);
    set_debug_bodies(&ed, true);
    debug_cycle(&ed, "debug-anthropic", "anthropic", "curl",
                "YEW_TEST_AI_DEBUG_KEY", completion, &starts);
    YEW_ASSERT(log_contains(&ed.ai->log,
                            "x-api-key: <redacted 16 bytes>"));
    YEW_ASSERT(!log_contains(&ed.ai->log, key));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_DEBUG,
                                    "x-api-key: <redacted 16 bytes>"));
    YEW_ASSERT(!yew_test_log_contains(YEW_LOG_DEBUG, key));
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(unsetenv("YEW_TEST_AI_DEBUG_KEY"), 0);
    if (saved_debug != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", saved_debug, 1), 0);
        free(saved_debug);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_DEBUG"), 0);
    }
    if (saved_level != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_LOG_LEVEL", saved_level, 1), 0);
        free(saved_level);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_LOG_LEVEL"), 0);
    }
}
