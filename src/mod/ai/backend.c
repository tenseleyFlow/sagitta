#include "mod/ai/backend.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

struct AiBlockState {
    i64 index;
    bool text;
};

static bool header_name_eq(const char *left, const char *right)
{
    unsigned char a;
    unsigned char b;

    if (left == NULL || right == NULL)
        return false;
    for (;;) {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z')
            a = (unsigned char)(a + ((unsigned char)'a' -
                                     (unsigned char)'A'));
        if (b >= (unsigned char)'A' && b <= (unsigned char)'Z')
            b = (unsigned char)(b + ((unsigned char)'a' -
                                     (unsigned char)'A'));
        if (a != b)
            return false;
        if (a == '\0')
            return true;
    }
}

static const char *auth_header_name(const char *name)
{
    if (header_name_eq(name, "authorization") ||
        header_name_eq(name, "proxy-authorization"))
        return "authorization";
    if (header_name_eq(name, "x-api-key") ||
        header_name_eq(name, "api-key") ||
        header_name_eq(name, "x-auth-token"))
        return "x-api-key";
    return NULL;
}

static bool log_secret_header(const AiSecretHeader *secret)
{
    const char *name;

    if (secret->kind == YEW_AI_SECRET_BEARER)
        name = "authorization";
    else if (secret->kind == YEW_AI_SECRET_X_API_KEY)
        name = "x-api-key";
    else
        return false;
    yew_log(YEW_LOG_DEBUG, "%s: <redacted %u bytes>", name,
            (unsigned)secret->len);
    return true;
}

bool yew_ai_log_headers(const HttpHdr *hdrs, u32 nhdr,
                        const AiSecretHeader *secret)
{
    bool valid = true;
    bool have_secret = secret != NULL && secret->kind != YEW_AI_SECRET_NONE;
    u32 secret_index = have_secret ? secret->index : nhdr;
    u32 i;

    if ((nhdr != 0U && hdrs == NULL) || secret_index > nhdr)
        return false;
    for (i = 0U; i <= nhdr; i++) {
        const char *masked;

        if (have_secret && i == secret_index) {
            if (!log_secret_header(secret))
                valid = false;
        }
        if (i == nhdr)
            break;
        if (hdrs[i].name == NULL || hdrs[i].value == NULL) {
            valid = false;
            continue;
        }
        masked = auth_header_name(hdrs[i].name);
        if (masked != NULL) {
            yew_log(YEW_LOG_DEBUG, "%s: <redacted>", masked);
            valid = false;
        } else {
            yew_log(YEW_LOG_DEBUG, "%s: %s", hdrs[i].name,
                    hdrs[i].value);
        }
    }
    return valid;
}

static void json_text(JsonW *w, const AiText *text)
{
    yew_jsonw_str(w, text->bytes, text->len);
}

static void json_stops(JsonW *w, const AiPrompt *prompt)
{
    u32 i;

    yew_jsonw_arr(w);
    for (i = 0U; i < prompt->nstops; i++)
        json_text(w, &prompt->stops[i]);
    yew_jsonw_arr_end(w);
}

static void json_message(JsonW *w, const char *role, const AiText *content)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "role");
    yew_jsonw_cstr(w, role);
    yew_jsonw_key(w, "content");
    json_text(w, content);
    yew_jsonw_obj_end(w);
}

static void json_messages(JsonW *w, const AiPrompt *prompt)
{
    yew_jsonw_arr(w);
    if (prompt->system.len != 0U)
        json_message(w, "system", &prompt->system);
    json_message(w, "user", &prompt->prefix);
    yew_jsonw_arr_end(w);
}

static void ollama_options(JsonW *w, const AiBackend *backend,
                           const AiPrompt *prompt)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "num_predict");
    yew_jsonw_int(w, backend->max_tokens);
    yew_jsonw_key(w, "temperature");
    yew_jsonw_real(w, backend->temperature);
    yew_jsonw_key(w, "stop");
    json_stops(w, prompt);
    yew_jsonw_obj_end(w);
}

static void ollama_build(JsonW *w, const AiBackend *backend,
                         const AiPrompt *prompt)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "model");
    yew_jsonw_cstr(w, backend->model);
    if (backend->fim) {
        yew_jsonw_key(w, "prompt");
        json_text(w, &prompt->prefix);
        yew_jsonw_key(w, "suffix");
        json_text(w, &prompt->suffix);
        if (prompt->system.len != 0U) {
            yew_jsonw_key(w, "system");
            json_text(w, &prompt->system);
        }
    } else {
        yew_jsonw_key(w, "messages");
        json_messages(w, prompt);
    }
    yew_jsonw_key(w, "stream");
    yew_jsonw_bool(w, backend->stream);
    yew_jsonw_key(w, "options");
    ollama_options(w, backend, prompt);
    yew_jsonw_obj_end(w);
}

static void openai_build(JsonW *w, const AiBackend *backend,
                         const AiPrompt *prompt)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "model");
    yew_jsonw_cstr(w, backend->model);
    yew_jsonw_key(w, "messages");
    json_messages(w, prompt);
    yew_jsonw_key(w, "stream");
    yew_jsonw_bool(w, backend->stream);
    yew_jsonw_key(w, "max_tokens");
    yew_jsonw_int(w, backend->max_tokens);
    yew_jsonw_key(w, "temperature");
    yew_jsonw_real(w, backend->temperature);
    yew_jsonw_key(w, "stop");
    json_stops(w, prompt);
    yew_jsonw_obj_end(w);
}

static void anthropic_build(JsonW *w, const AiBackend *backend,
                            const AiPrompt *prompt)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "model");
    yew_jsonw_cstr(w, backend->model);
    if (prompt->system.len != 0U) {
        yew_jsonw_key(w, "system");
        json_text(w, &prompt->system);
    }
    yew_jsonw_key(w, "messages");
    yew_jsonw_arr(w);
    json_message(w, "user", &prompt->prefix);
    yew_jsonw_arr_end(w);
    yew_jsonw_key(w, "max_tokens");
    yew_jsonw_int(w, backend->max_tokens);
    yew_jsonw_key(w, "stream");
    yew_jsonw_bool(w, backend->stream);
    yew_jsonw_key(w, "temperature");
    yew_jsonw_real(w, backend->temperature);
    yew_jsonw_key(w, "stop_sequences");
    json_stops(w, prompt);
    yew_jsonw_obj_end(w);
}

static const char *ollama_path_gen(const AiBackend *backend)
{
    return backend->fim ? "/api/generate" : "/api/chat";
}

static const char *ollama_path_models(const AiBackend *backend)
{
    (void)backend;
    return "/api/tags";
}

static const char *openai_path_gen(const AiBackend *backend)
{
    (void)backend;
    return "/v1/chat/completions";
}

static const char *cloud_path_models(const AiBackend *backend)
{
    (void)backend;
    return "/v1/models";
}

static const char *anthropic_path_gen(const AiBackend *backend)
{
    (void)backend;
    return "/v1/messages";
}

static bool json_string(const JsonValue *value, const u8 **text, u32 *len)
{
    u32 n = 0U;
    const u8 *bytes = yew_json_str(value, &n);

    if (text != NULL)
        *text = bytes != NULL && n != 0U ? bytes : NULL;
    if (len != NULL)
        *len = bytes != NULL ? n : 0U;
    return bytes != NULL && n != 0U;
}

static bool ollama_delta(const JsonValue *value, const u8 **text, u32 *len)
{
    const JsonValue *message;

    if (json_string(yew_json_get(value, "response"), text, len))
        return true;
    message = yew_json_get(value, "message");
    return json_string(yew_json_get(message, "content"), text, len);
}

static bool openai_delta(const JsonValue *value, const u8 **text, u32 *len)
{
    const JsonValue *choice = yew_json_at(yew_json_get(value, "choices"),
                                          0U);

    return json_string(yew_json_get(yew_json_get(choice, "delta"),
                                    "content"), text, len);
}

static bool anthropic_delta(const JsonValue *value, const u8 **text,
                            u32 *len)
{
    const JsonValue *delta;

    if (!yew_json_streq(yew_json_get(value, "type"),
                        "content_block_delta"))
        return false;
    delta = yew_json_get(value, "delta");
    if (!yew_json_streq(yew_json_get(delta, "type"), "text_delta"))
        return false;
    return json_string(yew_json_get(delta, "text"), text, len);
}

void yew_ai_adapter_state_init(AiAdapterState *state)
{
    memset(state, 0, sizeof(*state));
    state->input_tokens = -1;
    state->output_tokens = -1;
}

void yew_ai_adapter_state_free(AiAdapterState *state)
{
    yew_xfree(state->blocks);
    yew_ai_adapter_state_init(state);
}

void yew_ai_err_format(AiErr *out, AiErrKind kind,
                       const AiBackend *backend, u16 status,
                       i64 retry_ms, const char *detail)
{
    const char *name = backend != NULL && backend->name != NULL ?
                       backend->name : "AI backend";
    const char *host = backend != NULL && backend->url.host != NULL ?
                       backend->url.host : "configured host";
    const char *model = backend != NULL && backend->model != NULL ?
                        backend->model : "configured model";
    const char *key_env = backend != NULL && backend->key_env != NULL ?
                          backend->key_env : "API key";
    const char *why = detail != NULL && detail[0] != '\0' ? detail :
                      "no additional detail";
    unsigned port = backend != NULL ? (unsigned)backend->url.port : 0U;
    long long seconds;

    memset(out, 0, sizeof(*out));
    out->kind = (u8)kind;
    out->retry_ms = retry_ms;
    switch (kind) {
    case YEW_AI_ERR_UNREACHABLE:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "%s is not answering at http://%s:%u (%s) — is "
                       "the server running?", name, host, port, why);
        break;
    case YEW_AI_ERR_AUTH:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "%s rejected the API key (HTTP %u). Check $%s, "
                       "then :ai reload.", name,
                       status == 0U ? 401U : (unsigned)status, key_env);
        break;
    case YEW_AI_ERR_RATELIMIT:
        if (retry_ms >= 0) {
            seconds = ((long long)retry_ms + 999LL) / 1000LL;
            (void)snprintf(out->msg, sizeof(out->msg),
                           "%s is rate limiting (HTTP %u); retrying is "
                           "possible in %lld s.", name,
                           status == 0U ? 429U : (unsigned)status, seconds);
        } else {
            (void)snprintf(out->msg, sizeof(out->msg),
                           "%s is rate limiting (HTTP %u); wait before "
                           "retrying.", name,
                           status == 0U ? 429U : (unsigned)status);
        }
        break;
    case YEW_AI_ERR_MODEL:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "model '%s' is not available on %s. :ai models "
                       "lists what is.", model, name);
        break;
    case YEW_AI_ERR_PROTOCOL:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "%s returned a response yew could not parse (%s). "
                       ":ai log has the detail.", name, why);
        break;
    case YEW_AI_ERR_TIMEOUT:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "%s did not answer within %s.", name, why);
        break;
    case YEW_AI_ERR_TLS:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "TLS handshake with %s failed (%s). Check the "
                       "system CA store.", host, why);
        break;
    case YEW_AI_ERR_TOO_LARGE:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "%s response exceeded 64 MiB and was dropped.",
                       name);
        break;
    case YEW_AI_ERR_NO_CURL:
        (void)snprintf(out->msg, sizeof(out->msg),
                       "cloud AI backends need curl, which is not in "
                       "$PATH. Install curl, or run a local model: "
                       ":ai backend local (see :help ai-local).");
        break;
    case YEW_AI_ERR_CANCELLED:
    case YEW_AI_OK:
        break;
    }
}

void yew_ai_cooldown_init(AiCooldown *cooldown)
{
    memset(cooldown, 0, sizeof(*cooldown));
}

static i64 cooldown_backoff(u32 consecutive, i64 backoff_max_ms)
{
    i64 delay;
    u32 i;

    if (backoff_max_ms <= 0)
        return 0;
    delay = backoff_max_ms < 1000 ? backoff_max_ms : 1000;
    for (i = 1U; i < consecutive && delay < backoff_max_ms; i++) {
        if (delay > backoff_max_ms / 2) {
            delay = backoff_max_ms;
        } else {
            delay *= 2;
        }
    }
    return delay;
}

void yew_ai_cooldown_note(AiCooldown *cooldown, AiErrKind kind,
                          i64 retry_after_ms, i64 now_ms,
                          i64 backoff_max_ms)
{
    i64 delay;

    if (kind != YEW_AI_ERR_RATELIMIT && kind != YEW_AI_ERR_UNREACHABLE) {
        yew_ai_cooldown_init(cooldown);
        return;
    }
    if (cooldown->consecutive != UINT32_MAX)
        cooldown->consecutive++;
    delay = cooldown_backoff(cooldown->consecutive, backoff_max_ms);
    if (retry_after_ms > delay)
        delay = retry_after_ms;
    if (now_ms < 0)
        now_ms = 0;
    cooldown->until_ms = delay > INT64_MAX - now_ms ?
                         INT64_MAX : now_ms + delay;
}

i64 yew_ai_cooldown_remaining(const AiCooldown *cooldown, i64 now_ms)
{
    if (now_ms < 0)
        now_ms = 0;
    if (cooldown->until_ms <= now_ms)
        return 0;
    return cooldown->until_ms - now_ms;
}

static void copy_json_string(char *out, size_t cap, const JsonValue *value)
{
    u32 len = 0U;
    const u8 *bytes = yew_json_str(value, &len);
    size_t n;

    if (cap == 0U)
        return;
    if (bytes == NULL) {
        out[0] = '\0';
        return;
    }
    n = len < cap - 1U ? len : cap - 1U;
    memcpy(out, bytes, n);
    out[n] = '\0';
}

static bool event_type_mismatch(const AiEvent *event,
                                const JsonValue *value)
{
    u32 json_len = 0U;
    const u8 *json_type;
    bool mismatch;

    if (event == NULL || event->tlen == 0U)
        return false;
    json_type = yew_json_str(yew_json_get(value, "type"), &json_len);
    mismatch = json_type == NULL || json_len != event->tlen ||
               memcmp(json_type, event->type, event->tlen) != 0;
    if (mismatch) {
        yew_log(YEW_LOG_DEBUG,
                "AI SSE event/type mismatch: event='%.*s', JSON type='%.*s'",
                (int)event->tlen, (const char *)event->type,
                (int)json_len,
                json_type == NULL ? "" : (const char *)json_type);
    }
    return mismatch;
}

static void generic_consume(bool (*delta)(const JsonValue *, const u8 **,
                                          u32 *),
                            AiAdapterState *state, const AiEvent *event,
                            const JsonValue *value, AiAdapterEvent *out)
{
    memset(out, 0, sizeof(*out));
    out->terminal = event != NULL && event->terminator;
    state->terminal = state->terminal || out->terminal;
    if (value != NULL)
        out->has_text = delta(value, &out->text, &out->len);
}

static void ollama_consume(AiAdapterState *state, const AiEvent *event,
                           const JsonValue *value, AiAdapterEvent *out)
{
    generic_consume(ollama_delta, state, event, value, out);
}

static void openai_consume(AiAdapterState *state, const AiEvent *event,
                           const JsonValue *value, AiAdapterEvent *out)
{
    generic_consume(openai_delta, state, event, value, out);
}

static void blocks_clear(AiAdapterState *state)
{
    state->nblocks = 0U;
}

static AiBlockState *block_find(AiAdapterState *state, i64 index)
{
    u32 i;

    for (i = 0U; i < state->nblocks; i++) {
        if (state->blocks[i].index == index)
            return &state->blocks[i];
    }
    return NULL;
}

static AiBlockState *block_put(AiAdapterState *state, i64 index)
{
    AiBlockState *block = block_find(state, index);
    u32 cap;

    if (block != NULL)
        return block;
    if (state->nblocks == state->block_cap) {
        cap = state->block_cap == 0U ? 8U : state->block_cap * 2U;
        state->blocks = yew_xreallocarray(state->blocks, cap,
                                          sizeof(*state->blocks));
        state->block_cap = cap;
    }
    block = &state->blocks[state->nblocks++];
    block->index = index;
    block->text = false;
    return block;
}

static bool block_index(const JsonValue *value, i64 *index)
{
    const JsonValue *field = yew_json_get(value, "index");

    if (field == NULL || field->kind != YEW_JS_INT || field->i < 0)
        return false;
    *index = field->i;
    return true;
}

static void block_remove(AiAdapterState *state, i64 index)
{
    u32 i;

    for (i = 0U; i < state->nblocks; i++) {
        if (state->blocks[i].index == index) {
            state->blocks[i] = state->blocks[state->nblocks - 1U];
            state->nblocks--;
            return;
        }
    }
}

static void anthropic_message_start(AiAdapterState *state,
                                    const JsonValue *value)
{
    const JsonValue *message = yew_json_get(value, "message");

    blocks_clear(state);
    state->terminal = false;
    state->input_tokens = yew_json_int(
        yew_json_path(message, "usage.input_tokens"), -1);
    state->output_tokens = -1;
    state->stop_reason[0] = '\0';
    state->stop_sequence[0] = '\0';
    copy_json_string(state->message_id, sizeof(state->message_id),
                     yew_json_get(message, "id"));
    copy_json_string(state->model, sizeof(state->model),
                     yew_json_get(message, "model"));
}

static void anthropic_message_delta(AiAdapterState *state,
                                    const JsonValue *value)
{
    const JsonValue *delta = yew_json_get(value, "delta");

    copy_json_string(state->stop_reason, sizeof(state->stop_reason),
                     yew_json_get(delta, "stop_reason"));
    copy_json_string(state->stop_sequence, sizeof(state->stop_sequence),
                     yew_json_get(delta, "stop_sequence"));
    state->output_tokens = yew_json_int(
        yew_json_path(value, "usage.output_tokens"),
        state->output_tokens);
}

static void anthropic_consume(AiAdapterState *state, const AiEvent *event,
                              const JsonValue *value, AiAdapterEvent *out)
{
    const JsonValue *type;
    i64 index;
    AiBlockState *block;

    memset(out, 0, sizeof(*out));
    if (value == NULL) {
        out->terminal = event != NULL && event->terminator;
        state->terminal = state->terminal || out->terminal;
        return;
    }
    type = yew_json_get(value, "type");
    if (yew_json_streq(type, "message_start"))
        anthropic_message_start(state, value);
    out->event_type_mismatch = event_type_mismatch(event, value);
    if (out->event_type_mismatch)
        state->event_type_mismatches++;
    if (yew_json_streq(type, "content_block_start")) {
        if (block_index(value, &index)) {
            block = block_put(state, index);
            block->text = yew_json_streq(
                yew_json_path(value, "content_block.type"), "text");
        }
    } else if (yew_json_streq(type, "content_block_delta")) {
        if (block_index(value, &index)) {
            block = block_find(state, index);
            if (block != NULL && block->text) {
                out->has_text = anthropic_delta(value, &out->text,
                                                 &out->len);
            }
        }
    } else if (yew_json_streq(type, "content_block_stop")) {
        if (block_index(value, &index))
            block_remove(state, index);
    } else if (yew_json_streq(type, "message_delta")) {
        anthropic_message_delta(state, value);
    } else if (yew_json_streq(type, "message_stop")) {
        out->terminal = true;
    }
    if (event != NULL && event->terminator)
        out->terminal = true;
    state->terminal = state->terminal || out->terminal;
}

static u8 ascii_lower(u8 byte)
{
    if (byte >= (u8)'A' && byte <= (u8)'Z')
        return (u8)(byte + ((u8)'a' - (u8)'A'));
    return byte;
}

static bool text_contains(const u8 *text, u32 len, const char *needle)
{
    size_t nlen = strlen(needle);
    u32 i;
    size_t j;

    if (text == NULL || nlen == 0U || nlen > len)
        return false;
    for (i = 0U; i <= len - (u32)nlen; i++) {
        for (j = 0U; j < nlen; j++) {
            if (ascii_lower(text[i + (u32)j]) !=
                ascii_lower((u8)needle[j]))
                break;
        }
        if (j == nlen)
            return true;
    }
    return false;
}

static bool value_contains(const JsonValue *value, const char *needle)
{
    u32 len;
    const u8 *text = yew_json_str(value, &len);

    return text_contains(text, len, needle);
}

static bool model_error(const JsonValue *message, const JsonValue *code)
{
    return value_contains(code, "model_not_found") ||
           (value_contains(message, "model") &&
            (value_contains(message, "not found") ||
             value_contains(message, "not available")));
}

static AiErrKind status_class(u16 status)
{
    if (status == 401U || status == 403U)
        return YEW_AI_ERR_AUTH;
    if (status == 413U)
        return YEW_AI_ERR_TOO_LARGE;
    if (status == 429U)
        return YEW_AI_ERR_RATELIMIT;
    if (status < 200U || status >= 300U)
        return YEW_AI_ERR_PROTOCOL;
    return YEW_AI_OK;
}

static bool status_preempts_body(u16 status, AiErrKind *kind)
{
    *kind = status_class(status);
    return *kind == YEW_AI_ERR_AUTH || *kind == YEW_AI_ERR_RATELIMIT ||
           *kind == YEW_AI_ERR_TOO_LARGE || status < 200U ||
           (status >= 300U && status < 400U) || status >= 500U;
}

static AiErrKind ollama_classify(u16 status, const JsonValue *body)
{
    const JsonValue *message = yew_json_get(body, "error");
    AiErrKind by_status;

    if (status_preempts_body(status, &by_status))
        return by_status;
    if (model_error(message, NULL))
        return YEW_AI_ERR_MODEL;
    if (message != NULL)
        return YEW_AI_ERR_PROTOCOL;
    return status_class(status);
}

static AiErrKind openai_classify(u16 status, const JsonValue *body)
{
    const JsonValue *error = yew_json_get(body, "error");
    const JsonValue *message = yew_json_get(error, "message");
    const JsonValue *type = yew_json_get(error, "type");
    const JsonValue *code = yew_json_get(error, "code");
    AiErrKind by_status;

    if (status_preempts_body(status, &by_status))
        return by_status;
    if (model_error(message, code))
        return YEW_AI_ERR_MODEL;
    if (value_contains(type, "auth") || value_contains(code, "api_key"))
        return YEW_AI_ERR_AUTH;
    if (value_contains(type, "rate_limit") ||
        value_contains(code, "rate_limit"))
        return YEW_AI_ERR_RATELIMIT;
    if (error != NULL)
        return YEW_AI_ERR_PROTOCOL;
    return status_class(status);
}

static AiErrKind anthropic_classify(u16 status, const JsonValue *body)
{
    const JsonValue *error = yew_json_get(body, "error");
    const JsonValue *type = yew_json_get(error, "type");
    const JsonValue *message = yew_json_get(error, "message");
    AiErrKind by_status;

    if (status == 529U && value_contains(type, "overloaded"))
        return YEW_AI_ERR_RATELIMIT;
    if (status_preempts_body(status, &by_status))
        return by_status;
    if (model_error(message, type))
        return YEW_AI_ERR_MODEL;
    if (value_contains(type, "authentication"))
        return YEW_AI_ERR_AUTH;
    if (value_contains(type, "overloaded") ||
        value_contains(type, "rate_limit"))
        return YEW_AI_ERR_RATELIMIT;
    if (error != NULL || yew_json_streq(yew_json_get(body, "type"), "error"))
        return YEW_AI_ERR_PROTOCOL;
    return status_class(status);
}

const AiAdapter yew_ai_adapters[YEW_AI_NKIND] = {
    {
        "ollama", ollama_build, ollama_path_gen, ollama_path_models,
        YEW_AISTREAM_NDJSON, ollama_delta, ollama_consume, ollama_classify
    },
    {
        "openai", openai_build, openai_path_gen, cloud_path_models,
        YEW_AISTREAM_SSE, openai_delta, openai_consume, openai_classify
    },
    {
        "anthropic", anthropic_build, anthropic_path_gen, cloud_path_models,
        YEW_AISTREAM_SSE, anthropic_delta, anthropic_consume,
        anthropic_classify
    }
};
