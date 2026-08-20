#define _POSIX_C_SOURCE 200809L

#include "mod/ai/ai.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/option.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/backend.h"
#include "mod/ai/backend_curl.h"
#include "mod/ai/key.h"
#include "mod/lsp/json.h"
#include "text/piece.h"
#include "ui/message.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

#define YEW_AI_LOG_NAME "[AI Log]"
#define YEW_AI_LOG_MAX (1024U * 1024U)
#define YEW_AI_MODELS_MSG_MAX (64U * 1024U)

typedef enum AiCommandKind {
    YEW_AI_COMMAND_MODELS,
    YEW_AI_COMMAND_PING
} AiCommandKind;

typedef enum AiCommandStage {
    YEW_AI_COMMAND_PROBE,
    YEW_AI_COMMAND_HTTP,
    YEW_AI_COMMAND_CURL,
    YEW_AI_COMMAND_DONE
} AiCommandStage;

struct AiCommandCall {
    Ed *ed;
    AiBackendEntry *entry;
    HttpConn *http;
    Bytebuf body;
    Bytebuf curl_err;
    Bytebuf curl_config;
    AiErr error;
    i64 started_ms;
    i64 retry_after_ms;
    u32 job_id;
    int curl_exit;
    int curl_signal;
    u16 status;
    u8 kind;
    u8 stage;
    bool failed;
    bool json_started;
    bool json_in_string;
    bool json_escape;
    bool json_complete;
    bool json_bad_trailer;
    u32 json_depth;
};

static void backends_render(Ed *ed, bool probe_failed);

static void ai_log(Ed *ed, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;
    size_t len;

    if (ed == NULL || ed->ai == NULL || fmt == NULL)
        return;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1U;
    if (ed->ai->log.len + len + 1U > YEW_AI_LOG_MAX) {
        static const char full[] = "(AI log reached 1 MiB; later entries omitted)\n";

        if (ed->ai->log.len + sizeof(full) - 1U <= YEW_AI_LOG_MAX)
            bytebuf_append(&ed->ai->log, full, sizeof(full) - 1U);
        return;
    }
    bytebuf_append(&ed->ai->log, line, len);
    if (len == 0U || line[len - 1U] != '\n')
        bytebuf_push_u8(&ed->ai->log, (u8)'\n');
}

static bool ai_enabled(Ed *ed)
{
    OptVal value;

    return ed != NULL &&
           yew_opt_get(ed, NULL, NULL, "ai.enable", 9U, &value) &&
           value.type == (u8)YEW_OPT_BOOL && value.as.b;
}

static bool command_guard(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return false;
    if (ai_enabled(cx->ed))
        return true;
    (void)yew_ai_cmd_off(cx);
    return false;
}

static const char *transport_name(u8 transport)
{
    return transport == (u8)YEW_AI_TR_CURL ? "curl" : "http";
}

static const char *error_name(AiErrKind kind)
{
    static const char *const names[] = {
        "ok", "unreachable", "auth", "rate-limit", "model",
        "protocol", "timeout", "tls", "too-large", "no-curl",
        "cancelled"
    };

    return (u32)kind < YEW_ARRAY_LEN(names) ? names[(u32)kind] : "protocol";
}

static i64 option_int(Ed *ed, const char *name, i64 fallback)
{
    OptVal value;

    if (yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name), &value) &&
        value.type == (u8)YEW_OPT_INT)
        return value.as.i;
    return fallback;
}

static AiBackendEntry *only_backend(Ed *ed)
{
    u32 count;

    if (ed == NULL || ed->ai == NULL)
        return NULL;
    count = yew_ai_registry_count(&ed->ai->backends);
    if (count == 0U) {
        yew_msg(ed, YEW_MSG_INFO,
                "no AI backends configured; add ai.backend(...) to init.fl");
        return NULL;
    }
    if (count != 1U) {
        yew_msg(ed, YEW_MSG_INFO,
                "multiple AI backends configured; backend selection lands in Sprint 49");
        return NULL;
    }
    return yew_ai_registry_find_mut(
        &ed->ai->backends,
        yew_ai_registry_at(&ed->ai->backends, 0U)->backend.name);
}

static void command_call_free(AiCommandCall *call)
{
    if (call == NULL)
        return;
    bytebuf_free(&call->body);
    bytebuf_free(&call->curl_err);
    if (call->curl_config.data != NULL)
        yew_memzero(call->curl_config.data, call->curl_config.cap);
    bytebuf_free(&call->curl_config);
    yew_memzero(call, sizeof(*call));
    free(call);
}

void yew_ai_command_cancel(Ed *ed)
{
    AiCommandCall *call;
    YewJob *job;

    if (ed == NULL || ed->ai == NULL || ed->ai->command_call == NULL)
        return;
    call = ed->ai->command_call;
    if (call->http != NULL) {
        yew_http_abort(ed, call->http);
        yew_http_conn_callbacks(call->http, NULL, NULL, NULL);
        yew_http_conn_release(ed, call->http);
        call->http = NULL;
    }
    job = call->job_id != 0U ? yew_job_find(ed, call->job_id) : NULL;
    if (job != NULL) {
        if (job->in_bytes != NULL) {
            yew_memzero((void *)job->in_bytes, (size_t)job->in_len);
            job->in_bytes = NULL;
            job->in_len = 0U;
            job->in_off = 0U;
        }
        if (job->in_fd >= 0) {
            (void)close(job->in_fd);
            job->in_fd = -1;
        }
        job->sink = YEW_SINK_DISCARD;
        job->stream_owner = NULL;
        job->stream_ops = NULL;
        job->stream_destroyed = true;
        if (job->state == YEW_JOB_RUNNING)
            (void)yew_job_signal(ed, job->id, SIGTERM);
    }
    ed->ai->command_call = NULL;
    command_call_free(call);
}

static bool json_space(u8 byte)
{
    return byte == (u8)' ' || byte == (u8)'\t' ||
           byte == (u8)'\r' || byte == (u8)'\n';
}

static void json_boundary_feed(AiCommandCall *call, const u8 *bytes, u64 len)
{
    u64 i;

    for (i = 0U; i < len; i++) {
        u8 byte = bytes[i];

        if (call->json_complete) {
            if (!json_space(byte))
                call->json_bad_trailer = true;
            continue;
        }
        if (!call->json_started) {
            if (json_space(byte))
                continue;
            call->json_started = true;
            if (byte != (u8)'{' && byte != (u8)'[')
                continue;
            call->json_depth = 1U;
            continue;
        }
        if (call->json_in_string) {
            if (call->json_escape) {
                call->json_escape = false;
            } else if (byte == (u8)'\\') {
                call->json_escape = true;
            } else if (byte == (u8)'"') {
                call->json_in_string = false;
            }
            continue;
        }
        if (byte == (u8)'"') {
            call->json_in_string = true;
        } else if (byte == (u8)'{' || byte == (u8)'[') {
            if (call->json_depth != UINT32_MAX)
                call->json_depth++;
        } else if ((byte == (u8)'}' || byte == (u8)']') &&
                   call->json_depth != 0U) {
            call->json_depth--;
            if (call->json_depth == 0U)
                call->json_complete = true;
        }
    }
}

static void http_body(void *ctx, const u8 *bytes, u64 len)
{
    AiCommandCall *call = ctx;

    if (call == NULL || call->failed)
        return;
    if (len > YEW_HTTP_MAX_BODY - (u64)call->body.len) {
        call->failed = true;
        yew_ai_err_format(&call->error, YEW_AI_ERR_TOO_LARGE,
                          &call->entry->backend, 0U, -1, NULL);
        return;
    }
    bytebuf_append(&call->body, bytes, (size_t)len);
    json_boundary_feed(call, bytes, len);
    /* Close-delimited WHOLE responses need a terminator before EOF.  Track
     * JSON structure incrementally so fragmented input remains O(n).  The
     * full parser still supplies the authoritative verdict at completion. */
    if (call->http != NULL && call->http->rx != NULL &&
        call->http->rx->close_delimited && call->json_complete &&
        !call->json_bad_trailer)
        yew_http_conn_mark_stream_done(call->http);
}

static void http_done(void *ctx, const HttpConn *conn)
{
    AiCommandCall *call = ctx;

    if (call == NULL || conn == NULL)
        return;
    call->status = conn->rx != NULL ? conn->rx->status : 0U;
    call->retry_after_ms = conn->rx != NULL ?
                           conn->rx->retry_after_ms : -1;
    if (conn->state == (u8)YEW_HC_DEAD)
        call->failed = true;
    call->stage = (u8)YEW_AI_COMMAND_DONE;
}

static bool curl_feed_out(void *owner, const u8 *bytes, u64 len)
{
    AiCommandCall *call = owner;

    if (call == NULL)
        return false;
    if (len > YEW_HTTP_MAX_BODY - (u64)call->body.len) {
        call->failed = true;
        yew_ai_err_format(&call->error, YEW_AI_ERR_TOO_LARGE,
                          &call->entry->backend, 0U, -1, NULL);
        return false;
    }
    bytebuf_append(&call->body, bytes, (size_t)len);
    return true;
}

static bool curl_feed_err(void *owner, const u8 *bytes, u64 len)
{
    AiCommandCall *call = owner;

    if (call == NULL || len > YEW_JOB_COLLECT_MAX - (u64)call->curl_err.len)
        return false;
    bytebuf_append(&call->curl_err, bytes, (size_t)len);
    return true;
}

static bool curl_finish(void *owner)
{
    return owner != NULL;
}

static u16 curl_status(const Bytebuf *err)
{
    static const char marker[] = "yew-http-status: ";
    u16 status = 0U;
    size_t i;

    if (err == NULL)
        return 0U;
    for (i = 0U; i + sizeof(marker) - 1U + 3U <= err->len; i++) {
        const u8 *p = err->data + i + sizeof(marker) - 1U;

        if (memcmp(err->data + i, marker, sizeof(marker) - 1U) == 0 &&
            p[0] >= (u8)'0' && p[0] <= (u8)'9' &&
            p[1] >= (u8)'0' && p[1] <= (u8)'9' &&
            p[2] >= (u8)'0' && p[2] <= (u8)'9')
            status = (u16)((p[0] - (u8)'0') * 100U +
                           (p[1] - (u8)'0') * 10U +
                           (p[2] - (u8)'0'));
    }
    return status;
}

static void curl_destroy(void *owner)
{
    AiCommandCall *call = owner;
    YewJob *job;

    if (call == NULL || call->ed == NULL)
        return;
    job = yew_job_find(call->ed, call->job_id);
    if (job == NULL) {
        call->failed = true;
        call->error.kind = (u8)YEW_AI_ERR_PROTOCOL;
        call->error.retry_ms = -1;
        (void)snprintf(call->error.msg, sizeof(call->error.msg),
                       "%s curl job disappeared",
                       call->entry->backend.name);
    } else {
        call->curl_exit = job->exit_code;
        call->curl_signal = job->termsig;
        call->status = curl_status(&call->curl_err);
        if (job->state == YEW_JOB_TIMEOUT) {
            call->failed = true;
            yew_ai_err_format(&call->error, YEW_AI_ERR_TIMEOUT,
                              &call->entry->backend, call->status, -1,
                              "configured total timeout");
        } else if (job->state == YEW_JOB_CANCELLED) {
            call->failed = true;
            yew_ai_err_format(&call->error, YEW_AI_ERR_CANCELLED,
                              &call->entry->backend, call->status, -1,
                              NULL);
        } else if (job->stream_failed || job->collect_capped) {
            call->failed = true;
            if (call->error.msg[0] == '\0') {
                call->error.kind = (u8)YEW_AI_ERR_PROTOCOL;
                call->error.retry_ms = -1;
                (void)snprintf(call->error.msg, sizeof(call->error.msg),
                               "%s curl output could not be consumed",
                               call->entry->backend.name);
            }
        } else if (job->state != YEW_JOB_EXITED || job->exit_code != 0) {
            call->failed = true;
        }
    }
    call->stage = (u8)YEW_AI_COMMAND_DONE;
}

static const YewJobStreamOps curl_ops = {
    curl_feed_out,
    curl_finish,
    curl_feed_err,
    curl_finish,
    NULL,
    NULL,
    curl_destroy
};

static bool request_url(Bytebuf *out, const AiBackendEntry *entry,
                        const char *path)
{
    const char *scheme;
    const char *authority;
    const char *slash;
    size_t prefix;

    if (out == NULL || entry == NULL || entry->url_text == NULL ||
        path == NULL || path[0] != '/')
        return false;
    scheme = strstr(entry->url_text, "://");
    if (scheme == NULL)
        return false;
    authority = scheme + 3U;
    slash = strchr(authority, '/');
    prefix = slash != NULL ? (size_t)(slash - entry->url_text) :
                             strlen(entry->url_text);
    bytebuf_append(out, entry->url_text, prefix);
    bytebuf_append(out, path, strlen(path));
    bytebuf_push_u8(out, 0U);
    out->len--;
    return true;
}

static bool auth_prepare(AiCommandCall *call, HttpHdr *headers, u32 *nhdr,
                         AiCurlSecret *curl_secret, char *auth,
                         size_t auth_size, char *key, size_t key_size)
{
    AiBackend *backend = &call->entry->backend;
    size_t key_len;

    *nhdr = 0U;
    curl_secret->kind = YEW_CURL_AUTH_NONE;
    curl_secret->bytes = NULL;
    curl_secret->len = 0U;
    if (backend->kind == (u8)YEW_AI_OLLAMA)
        return true;
    if (!yew_ai_key_cache_get(call->ed, &call->ed->ai->keys, backend,
                              key, key_size, &call->error))
        return false;
    key_len = strlen(key);
    if (backend->kind == (u8)YEW_AI_OPENAI) {
        if (key_len + 8U > auth_size)
            return false;
        (void)snprintf(auth, auth_size, "Bearer %s", key);
        headers[(*nhdr)++] = (HttpHdr){"authorization", auth};
        curl_secret->kind = YEW_CURL_AUTH_BEARER;
    } else {
        headers[(*nhdr)++] =
            (HttpHdr){"anthropic-version", "2023-06-01"};
        headers[(*nhdr)++] = (HttpHdr){"x-api-key", key};
        curl_secret->kind = YEW_CURL_AUTH_X_API_KEY;
    }
    curl_secret->bytes = (const u8 *)key;
    curl_secret->len = key_len;
    return true;
}

static bool start_http(AiCommandCall *call, const char *path)
{
    HttpHdr headers[2];
    HttpReq request = {0};
    AiCurlSecret unused;
    char auth[YEW_AI_KEY_MAX + 8U];
    char key[YEW_AI_KEY_MAX + 1U];
    u32 nhdr;

    (void)memset(auth, 0, sizeof(auth));
    (void)memset(key, 0, sizeof(key));
    if (!auth_prepare(call, headers, &nhdr, &unused, auth, sizeof(auth),
                      key, sizeof(key)))
        goto fail;
    request.method = "GET";
    request.path = path;
    request.hdrs = headers;
    request.nhdr = nhdr;
    request.keepalive = true;
    call->http = yew_http_begin(call->ed, &call->entry->backend.url,
                                &request, &call->error);
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    if (call->http == NULL)
        return false;
    if (nhdr != 0U)
        yew_http_conn_mark_secret(call->http);
    yew_http_conn_callbacks(call->http, http_body, http_done, call);
    call->stage = (u8)YEW_AI_COMMAND_HTTP;
    return true;

fail:
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    return false;
}

static bool start_curl(AiCommandCall *call, const char *path)
{
    HttpHdr headers[2];
    AiCurlSecret secret;
    AiCurlRequest request = {0};
    YewJobSpec spec = {0};
    Bytebuf url;
    char auth[YEW_AI_KEY_MAX + 8U];
    char key[YEW_AI_KEY_MAX + 1U];
    char err[192] = {0};
    u32 nhdr;

    bytebuf_init(&url);
    (void)memset(auth, 0, sizeof(auth));
    (void)memset(key, 0, sizeof(key));
    if (!request_url(&url, call->entry, path) ||
        !auth_prepare(call, headers, &nhdr, &secret, auth, sizeof(auth),
                      key, sizeof(key)))
        goto fail;
    /* The curl builder receives credential bytes only through its separate
     * secret channel.  Keep Anthropic's public version header, but never
     * duplicate either authentication header in the ordinary array. */
    if (secret.kind == YEW_CURL_AUTH_BEARER)
        nhdr = 0U;
    else if (secret.kind == YEW_CURL_AUTH_X_API_KEY)
        nhdr = 1U;
    request.url = (const char *)url.data;
    request.method = "GET";
    request.hdrs = headers;
    request.nhdr = nhdr;
    request.connect_timeout_ms =
        option_int(call->ed, "ai.connect_timeout_ms", 2000);
    request.total_timeout_ms =
        option_int(call->ed, "ai.total_timeout_ms", 120000);
    if (!yew_ai_curl_config(&call->curl_config, &request, &secret,
                            err, sizeof(err))) {
        call->error.kind = (u8)YEW_AI_ERR_PROTOCOL;
        call->error.retry_ms = -1;
        (void)snprintf(call->error.msg, sizeof(call->error.msg), "%s",
                       err[0] != '\0' ? err : "cannot build curl request");
        goto fail;
    }
    spec.argv = (char **)yew_ai_curl_argv();
    spec.sink = YEW_SINK_STREAM;
    spec.in_bytes = call->curl_config.data;
    spec.in_len = (u64)call->curl_config.len;
    spec.timeout_ms = request.total_timeout_ms;
    spec.display = "curl AI request";
    spec.stream_owner = call;
    spec.stream_ops = &curl_ops;
    call->job_id = yew_job_spawn(call->ed, &spec, err, sizeof(err));
    if (call->job_id == 0U) {
        call->error.kind = (u8)YEW_AI_ERR_UNREACHABLE;
        call->error.retry_ms = -1;
        (void)snprintf(call->error.msg, sizeof(call->error.msg), "%s",
                       err[0] != '\0' ? err : "cannot start curl");
        goto fail;
    }
    call->stage = (u8)YEW_AI_COMMAND_CURL;
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    bytebuf_free(&url);
    return true;

fail:
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    bytebuf_free(&url);
    return false;
}

static bool start_transport(AiCommandCall *call)
{
    const AiAdapter *adapter =
        &yew_ai_adapters[call->entry->backend.kind];
    const char *path = adapter->path_models(&call->entry->backend);

    if (path == NULL) {
        call->error.kind = (u8)YEW_AI_ERR_PROTOCOL;
        call->error.retry_ms = -1;
        (void)snprintf(call->error.msg, sizeof(call->error.msg),
                       "%s does not support model listing",
                       call->entry->backend.name);
        return false;
    }
    if (call->entry->backend.transport == (u8)YEW_AI_TR_CURL)
        return start_curl(call, path);
    return start_http(call, path);
}

static void classify_response(AiCommandCall *call, JsonValue *body)
{
    AiErrKind kind;
    const AiAdapter *adapter;
    const char *detail = NULL;
    char curl_detail[96];

    if (call->failed && call->error.msg[0] != '\0')
        return;
    if (call->failed && call->stage == (u8)YEW_AI_COMMAND_DONE &&
        call->job_id != 0U) {
        kind = yew_ai_curl_exit_class(call->curl_exit, call->curl_signal);
        if (kind != YEW_AI_OK) {
            size_t n = call->curl_err.len < sizeof(curl_detail) - 1U ?
                       call->curl_err.len : sizeof(curl_detail) - 1U;

            (void)memcpy(curl_detail, call->curl_err.data, n);
            curl_detail[n] = '\0';
            yew_ai_err_format(&call->error, kind, &call->entry->backend,
                              call->status, -1, curl_detail);
            return;
        }
    }
    if (call->status >= 200U && call->status < 300U) {
        call->failed = false;
        return;
    }
    adapter = &yew_ai_adapters[call->entry->backend.kind];
    kind = adapter->classify(call->status, body);
    if (kind == YEW_AI_OK)
        kind = YEW_AI_ERR_PROTOCOL;
    if (body == NULL)
        detail = "empty or malformed JSON error body";
    yew_ai_err_format(&call->error, kind, &call->entry->backend,
                      call->status, call->retry_after_ms, detail);
    call->failed = true;
}

static bool models_render(Bytebuf *out, const AiCommandCall *call,
                          const JsonValue *root)
{
    const JsonValue *models;
    const char *field;
    u32 i;
    u32 emitted = 0U;

    if (root == NULL)
        return false;
    if (call->entry->backend.kind == (u8)YEW_AI_OLLAMA) {
        models = yew_json_get(root, "models");
        field = "name";
    } else {
        models = yew_json_get(root, "data");
        field = "id";
    }
    if (models == NULL || models->kind != (u8)YEW_JS_ARR)
        return false;
    bytebuf_printf(out, "%s models:", call->entry->backend.name);
    for (i = 0U; i < yew_json_len(models); i++) {
        const JsonValue *item = yew_json_at(models, i);
        const u8 *name;
        size_t separator;
        u32 len = 0U;

        name = yew_json_str(yew_json_get(item, field), &len);
        if (name == NULL)
            continue;
        separator = emitted == 0U ? 1U : 2U;
        if (out->len + separator + (size_t)len > YEW_AI_MODELS_MSG_MAX) {
            size_t ellipsis_len = emitted == 0U ? 4U : 5U;

            if (out->len + ellipsis_len <= YEW_AI_MODELS_MSG_MAX) {
                if (emitted == 0U)
                    bytebuf_append(out, " \xE2\x80\xA6", ellipsis_len);
                else
                    bytebuf_append(out, ", \xE2\x80\xA6", ellipsis_len);
            }
            emitted++;
            break;
        }
        bytebuf_printf(out, "%s%.*s", emitted == 0U ? " " : ", ",
                       (int)len, (const char *)name);
        emitted++;
    }
    if (emitted == 0U)
        bytebuf_append(out, " (none)", 7U);
    return true;
}

static void finish_call(Ed *ed, AiCommandCall *call)
{
    Arena arena;
    JsonErr json_error = {0};
    JsonValue *root = NULL;
    Bytebuf message;
    i64 elapsed;
    AiErrKind kind;
    YewJob *job;

    arena_init(&arena);
    bytebuf_init(&message);
    if (call->body.len != 0U)
        root = yew_json_parse(&arena, call->body.data,
                              (u64)call->body.len, &json_error);
    classify_response(call, root);
    elapsed = yew_now_ms() - call->started_ms;
    if (!call->failed && root == NULL) {
        call->failed = true;
        yew_ai_err_format(
            &call->error, YEW_AI_ERR_PROTOCOL, &call->entry->backend,
            call->status, -1,
            json_error.msg[0] != '\0' ? json_error.msg :
                                         "empty JSON response");
    }
    if (!call->failed && call->kind == (u8)YEW_AI_COMMAND_MODELS &&
        !models_render(&message, call, root)) {
        call->failed = true;
        yew_ai_err_format(&call->error, YEW_AI_ERR_PROTOCOL,
                          &call->entry->backend, call->status, -1,
                          root == NULL && json_error.msg[0] != '\0' ?
                                         json_error.msg :
                                         "model list has the wrong shape");
    }
    if (call->failed) {
        kind = (AiErrKind)call->error.kind;
        yew_ai_cooldown_note(&call->entry->cooldown, kind,
                             call->error.retry_ms, yew_now_ms(),
                             option_int(ed, "ai.backoff_max_ms", 60000));
        if (kind != YEW_AI_ERR_CANCELLED)
            yew_msg(ed, YEW_MSG_WARN, "%s [%s, %lld ms]",
                    call->error.msg, error_name(kind), (long long)elapsed);
        ai_log(ed, "%s %s failed: %s: %s (%lld ms)",
               call->kind == (u8)YEW_AI_COMMAND_MODELS ? "models" : "ping",
               call->entry->backend.name, error_name(kind), call->error.msg,
               (long long)elapsed);
    } else if (call->kind == (u8)YEW_AI_COMMAND_PING) {
        yew_ai_cooldown_note(&call->entry->cooldown, YEW_AI_OK, -1,
                             yew_now_ms(), 60000);
        yew_msg(ed, YEW_MSG_INFO, "%s: ok (%lld ms)",
                call->entry->backend.name, (long long)elapsed);
        ai_log(ed, "ping %s: ok (%lld ms)", call->entry->backend.name,
               (long long)elapsed);
    } else {
        yew_ai_cooldown_note(&call->entry->cooldown, YEW_AI_OK, -1,
                             yew_now_ms(), 60000);
        yew_msg(ed, YEW_MSG_INFO, "%.*s", (int)message.len,
                (const char *)message.data);
        ai_log(ed, "models %s: ok, %llu bytes (%lld ms)",
               call->entry->backend.name,
               (unsigned long long)call->body.len, (long long)elapsed);
    }
    if (call->http != NULL)
        yew_http_conn_release(ed, call->http);
    job = call->job_id != 0U ? yew_job_find(ed, call->job_id) : NULL;
    if (job != NULL && !yew_job_pending(job))
        yew_job_release(ed, job);
    bytebuf_free(&message);
    arena_free_all(&arena);
}

void yew_ai_command_pump(Ed *ed)
{
    AiCommandCall *call;
    char err[192] = {0};
    YewJob *probe_job;

    if (ed == NULL || ed->ai == NULL)
        return;
    call = ed->ai->command_call;
    if (call != NULL && call->stage == (u8)YEW_AI_COMMAND_PROBE) {
        if (!yew_ai_curl_probe(ed, &ed->ai->curl, err, sizeof(err))) {
            if (err[0] == '\0')
                return;
            call->failed = true;
            call->error.kind = (u8)YEW_AI_ERR_NO_CURL;
            call->error.retry_ms = -1;
            (void)snprintf(call->error.msg, sizeof(call->error.msg), "%s",
                           err);
            call->stage = (u8)YEW_AI_COMMAND_DONE;
        } else if (!start_transport(call)) {
            call->failed = true;
            call->stage = (u8)YEW_AI_COMMAND_DONE;
        }
    }
    if (call != NULL && call->stage == (u8)YEW_AI_COMMAND_DONE) {
        finish_call(ed, call);
        ed->ai->command_call = NULL;
        command_call_free(call);
    }
    if (!ed->ai->curl.running && ed->ai->curl.job_id != 0U) {
        probe_job = yew_job_find(ed, ed->ai->curl.job_id);
        if (probe_job == NULL || !yew_job_pending(probe_job)) {
            if (probe_job != NULL)
                yew_job_release(ed, probe_job);
            ed->ai->curl.job_id = 0U;
            if (ed->ai->curl_backends_waiting) {
                ed->ai->curl_backends_waiting = false;
                backends_render(ed, false);
            }
        }
    }
}

static CmdStatus start_command(CmdCtx *cx, AiCommandKind kind)
{
    AiBackendEntry *entry;
    AiCommandCall *call;
    i64 cooldown;
    char err[192] = {0};

    if (!command_guard(cx))
        return YEW_CMD_ERR_STATE;
    if (cx->ed->ai->command_call != NULL) {
        yew_msg(cx->ed, YEW_MSG_INFO, "an AI command request is already running");
        return YEW_CMD_ERR_STATE;
    }
    entry = only_backend(cx->ed);
    if (entry == NULL)
        return YEW_CMD_ERR_STATE;
    cooldown = yew_ai_cooldown_remaining(&entry->cooldown, yew_now_ms());
    if (cooldown > 0) {
        yew_msg(cx->ed, YEW_MSG_INFO, "%s is cooling down for %lld ms",
                entry->backend.name, (long long)cooldown);
        return YEW_CMD_ERR_STATE;
    }
    call = yew_xcalloc(1U, sizeof(*call));
    call->ed = cx->ed;
    call->entry = entry;
    call->kind = (u8)kind;
    call->started_ms = yew_now_ms();
    call->retry_after_ms = -1;
    call->error.retry_ms = -1;
    bytebuf_init(&call->body);
    bytebuf_init(&call->curl_err);
    bytebuf_init(&call->curl_config);
    cx->ed->ai->command_call = call;
    /* A foreground curl request adopts any probe started by :ai backends.
     * Its completion message owns the message line from this point onward. */
    if (entry->backend.transport == (u8)YEW_AI_TR_CURL)
        cx->ed->ai->curl_backends_waiting = false;
    if (entry->backend.transport == (u8)YEW_AI_TR_CURL &&
        !yew_ai_curl_probe(cx->ed, &cx->ed->ai->curl, err, sizeof(err))) {
        if (err[0] != '\0') {
            call->failed = true;
            call->error.kind = (u8)YEW_AI_ERR_NO_CURL;
            (void)snprintf(call->error.msg, sizeof(call->error.msg), "%s",
                           err);
            call->stage = (u8)YEW_AI_COMMAND_DONE;
            yew_ai_command_pump(cx->ed);
            return YEW_CMD_ERR_STATE;
        }
        call->stage = (u8)YEW_AI_COMMAND_PROBE;
    } else if (!start_transport(call)) {
        call->failed = true;
        call->stage = (u8)YEW_AI_COMMAND_DONE;
        yew_ai_command_pump(cx->ed);
        return YEW_CMD_ERR_STATE;
    }
    ai_log(cx->ed, "%s %s: started via %s",
           kind == YEW_AI_COMMAND_MODELS ? "models" : "ping",
           entry->backend.name, transport_name(entry->backend.transport));
    yew_msg(cx->ed, YEW_MSG_INFO, "%s %s request started",
            entry->backend.name,
            kind == YEW_AI_COMMAND_MODELS ? "model-list" : "ping");
    return YEW_CMD_OK;
}

static void backends_render(Ed *ed, bool probe_failed)
{
    Bytebuf out;
    u32 i;
    const char *curl_version;

    curl_version = yew_ai_curl_probe_version(&ed->ai->curl);
    bytebuf_init(&out);
    for (i = 0U; i < yew_ai_registry_count(&ed->ai->backends); i++) {
        const AiBackendEntry *entry =
            yew_ai_registry_at(&ed->ai->backends, i);
        i64 cooldown = yew_ai_cooldown_remaining(&entry->cooldown,
                                                  yew_now_ms());

        if (i != 0U)
            bytebuf_append(&out, "; ", 2U);
        bytebuf_printf(&out, "%s %s", entry->backend.name,
                       transport_name(entry->backend.transport));
        if (entry->backend.transport == (u8)YEW_AI_TR_CURL) {
            if (curl_version != NULL)
                bytebuf_printf(&out, " %s", curl_version);
            else if (probe_failed ||
                     ed->ai->curl.state == YEW_CURL_ABSENT ||
                     ed->ai->curl.state == YEW_CURL_TOO_OLD)
                bytebuf_append(&out, " unavailable", 12U);
            else
                bytebuf_append(&out, " probing", 8U);
        }
        bytebuf_printf(&out, " cooldown=%lldms", (long long)cooldown);
    }
    yew_msg(ed, YEW_MSG_INFO, "%.*s", (int)out.len,
            (const char *)out.data);
    ai_log(ed, "backends: %.*s", (int)out.len,
           (const char *)out.data);
    bytebuf_free(&out);
}

CmdStatus yew_ai_cmd_backends(CmdCtx *cx)
{
    u32 i;
    bool have_curl = false;
    bool probe_failed = false;
    char probe_err[192] = {0};

    if (!command_guard(cx))
        return YEW_CMD_ERR_STATE;
    if (yew_ai_registry_count(&cx->ed->ai->backends) == 0U) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                "no AI backends configured; add ai.backend(...) to init.fl");
        return YEW_CMD_OK;
    }
    for (i = 0U; i < yew_ai_registry_count(&cx->ed->ai->backends); i++) {
        const AiBackendEntry *entry =
            yew_ai_registry_at(&cx->ed->ai->backends, i);

        if (entry->backend.transport == (u8)YEW_AI_TR_CURL)
            have_curl = true;
    }
    if (have_curl &&
        !yew_ai_curl_probe(cx->ed, &cx->ed->ai->curl,
                           probe_err, sizeof(probe_err))) {
        if (cx->ed->ai->curl.running)
            cx->ed->ai->curl_backends_waiting = true;
        else if (probe_err[0] != '\0')
            probe_failed = true;
    }
    backends_render(cx->ed, probe_failed);
    return YEW_CMD_OK;
}

CmdStatus yew_ai_cmd_models(CmdCtx *cx)
{
    return start_command(cx, YEW_AI_COMMAND_MODELS);
}

CmdStatus yew_ai_cmd_ping(CmdCtx *cx)
{
    return start_command(cx, YEW_AI_COMMAND_PING);
}

CmdStatus yew_ai_cmd_log(CmdCtx *cx)
{
    Buffer *log;

    if (!command_guard(cx))
        return YEW_CMD_ERR_STATE;
    log = yew_ws_scratch_find(cx->ed, YEW_AI_LOG_NAME);
    if (log == NULL)
        log = yew_ws_scratch_new(cx->ed, YEW_AI_LOG_NAME,
                                 YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (log == NULL)
        return YEW_CMD_ERR_IO;
    yew_textbuf_delete(log->tb, (Span){0U, yew_textbuf_len(log->tb)});
    yew_textbuf_insert(log->tb, BYTEOFF(0U), cx->ed->ai->log.data,
                       (u64)cx->ed->ai->log.len);
    return yew_ed_show_buffer(cx->ed, log) ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_ai_cmd_reload(CmdCtx *cx)
{
    HttpState *fresh;
    HttpState *old;
    bool *loopback;
    u32 count;
    u32 i;
    AiErr error;
    bool ok = true;

    if (!command_guard(cx))
        return YEW_CMD_ERR_STATE;
    if (cx->ed->ai->command_call != NULL) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                "cannot reload AI backends while a request is running");
        return YEW_CMD_ERR_STATE;
    }
    count = yew_ai_registry_count(&cx->ed->ai->backends);
    loopback = yew_xcalloc(count == 0U ? 1U : count, sizeof(*loopback));
    fresh = yew_http_state_new();
    old = cx->ed->ai->http;
    cx->ed->ai->http = fresh;
    for (i = 0U; i < count; i++) {
        AiBackendEntry *entry = yew_ai_registry_find_mut(
            &cx->ed->ai->backends,
            yew_ai_registry_at(&cx->ed->ai->backends, i)->backend.name);

        loopback[i] = entry->backend.url.loopback;
        if (entry->backend.transport == (u8)YEW_AI_TR_HTTP &&
            !yew_http_register_endpoint(cx->ed, &entry->backend.url,
                                        &error)) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        while (i-- > 0U) {
            AiBackendEntry *entry = yew_ai_registry_find_mut(
                &cx->ed->ai->backends,
                yew_ai_registry_at(&cx->ed->ai->backends, i)->backend.name);

            entry->backend.url.loopback = loopback[i];
        }
        cx->ed->ai->http = old;
        yew_http_state_free(fresh);
        free(loopback);
        yew_msg(cx->ed, YEW_MSG_WARN, "%s", error.msg);
        ai_log(cx->ed, "reload failed: %s", error.msg);
        return YEW_CMD_ERR_IO;
    }
    yew_http_state_free(old);
    yew_ai_key_cache_reload(&cx->ed->ai->keys);
    free(loopback);
    yew_msg(cx->ed, YEW_MSG_INFO, "reloaded %u AI backend%s; key cache cleared",
            (unsigned)count, count == 1U ? "" : "s");
    ai_log(cx->ed, "reload: %u backend%s; key cache cleared",
           (unsigned)count, count == 1U ? "" : "s");
    return YEW_CMD_OK;
}
