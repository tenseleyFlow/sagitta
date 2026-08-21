#define _POSIX_C_SOURCE 200809L

#include "mod/ai/shadow_ai.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/backend_curl.h"
#include "mod/ai/key.h"
#include "mod/ai/prompt.h"
#include "mod/ai/registry.h"
#include "mod/ai/stats.h"
#include "mod/lsp/json.h"
#include "ui/message.h"

#define AI_LOG_MAX (1024U * 1024U)

static AiWorkspaceAllowedFn workspace_allowed;
static AiPathExcludedFn path_excluded;

static i64 ai_option_int(Ed *ed, const char *name, i64 fallback)
{
    OptVal value;

    if (yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name), &value) &&
        value.type == (u8)YEW_OPT_INT)
        return value.as.i;
    return fallback;
}

static bool ai_option_bool(Ed *ed, const char *name, bool fallback)
{
    OptVal value;

    if (yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name), &value) &&
        value.type == (u8)YEW_OPT_BOOL)
        return value.as.b;
    return fallback;
}

static const char *ai_option_str(Ed *ed, const char *name)
{
    OptVal value;

    if (yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name), &value) &&
        (value.type == (u8)YEW_OPT_STR ||
         value.type == (u8)YEW_OPT_ENUM))
        return value.as.str.s;
    return "";
}

static void ai_shadow_log(Ed *ed, const char *fmt, ...)
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
    if (ed->ai->log.len + len + 1U > AI_LOG_MAX)
        return;
    bytebuf_append(&ed->ai->log, line, len);
    if (len == 0U || line[len - 1U] != '\n')
        bytebuf_push_u8(&ed->ai->log, (u8)'\n');
}

void yew_ai_workspace_policy_set(AiWorkspaceAllowedFn allowed)
{
    workspace_allowed = allowed;
}

void yew_ai_path_policy_set(AiPathExcludedFn excluded)
{
    path_excluded = excluded;
}

static void ai_call_init(AiCall *call, Ed *ed)
{
    (void)memset(call, 0, sizeof(*call));
    call->ed = ed;
    call->t_sent = -1;
    call->t_first_token = -1;
    call->t_done = -1;
    call->retry_after_ms = -1;
    arena_init(&call->arena);
    bytebuf_init(&call->raw);
    bytebuf_init(&call->text);
    bytebuf_init(&call->body);
    bytebuf_init(&call->response);
    bytebuf_init(&call->curl_config);
    bytebuf_init(&call->curl_err);
    yew_ai_adapter_state_init(&call->adapter);
}

static void ai_call_reset(AiCall *call)
{
    if (call == NULL)
        return;
    yew_ai_adapter_state_free(&call->adapter);
    yew_ai_stream_free(&call->stream);
    arena_free_all(&call->arena);
    bytebuf_free(&call->raw);
    bytebuf_free(&call->text);
    bytebuf_free(&call->body);
    bytebuf_free(&call->response);
    if (call->curl_config.data != NULL)
        yew_memzero(call->curl_config.data, call->curl_config.cap);
    bytebuf_free(&call->curl_config);
    bytebuf_free(&call->curl_err);
    (void)memset(call, 0, sizeof(*call));
}

static void retire_job(Ed *ed, u32 id)
{
    AiState *state;
    u32 i;

    if (ed == NULL || ed->ai == NULL || id == 0U)
        return;
    state = ed->ai;
    for (i = 0U; i < state->nretired_jobs; i++)
        if (state->retired_jobs[i] == id)
            return;
    if (state->nretired_jobs == YEW_JOB_MAX)
        YEW_BUG("AI retired-job table overflow");
    state->retired_jobs[state->nretired_jobs++] = id;
}

static void detach_curl_job(Ed *ed, AiCall *call)
{
    YewJob *job;

    if (call->job == 0U)
        return;
    job = yew_job_find(ed, call->job);
    if (job == NULL) {
        call->job = 0U;
        return;
    }
    if (job->state == YEW_JOB_RUNNING)
        (void)yew_job_signal(ed, job->id, SIGTERM);
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
    if (job->out_fd >= 0) {
        (void)close(job->out_fd);
        job->out_fd = -1;
    }
    if (job->err_fd >= 0) {
        (void)close(job->err_fd);
        job->err_fd = -1;
    }
    if (job->exec_fd >= 0) {
        (void)close(job->exec_fd);
        job->exec_fd = -1;
    }
    job->sink = YEW_SINK_DISCARD;
    job->stream_owner = NULL;
    job->stream_ops = NULL;
    job->stream_destroyed = true;
    retire_job(ed, job->id);
    call->job = 0U;
}

void yew_ai_call_abort(Ed *ed, AiCall *call, AiErrKind why)
{
    char backend[128];

    if (ed == NULL || ed->ai == NULL || call == NULL || !call->active)
        return;
    (void)snprintf(backend, sizeof(backend), "%s",
                   call->backend.name == NULL ? "" : call->backend.name);
    if (call->conn != NULL) {
        yew_http_abort(ed, call->conn);
        yew_http_conn_callbacks(call->conn, NULL, NULL, NULL);
        yew_http_conn_release(ed, call->conn);
        call->conn = NULL;
    }
    if (call->job != 0U)
        detach_curl_job(ed, call);
    if (why == YEW_AI_ERR_CANCELLED) {
        yew_ai_stats_cancel(ed, backend);
        ai_shadow_log(ed, "request %s cancelled", backend);
    }
    ai_call_reset(call);
}

static void ai_retired_pump(Ed *ed)
{
    AiState *state = ed->ai;
    u32 i = 0U;

    while (i < state->nretired_jobs) {
        YewJob *job = yew_job_find(ed, state->retired_jobs[i]);

        if (job != NULL && yew_job_pending(job)) {
            i++;
            continue;
        }
        if (job != NULL)
            yew_job_release(ed, job);
        (void)memmove(&state->retired_jobs[i],
                      &state->retired_jobs[i + 1U],
                      (size_t)(state->nretired_jobs - i - 1U) *
                          sizeof(state->retired_jobs[0]));
        state->nretired_jobs--;
    }
}

static void ai_stream_event(void *ctx, const AiEvent *event)
{
    AiCall *call = ctx;
    const AiAdapter *adapter;
    AiAdapterEvent result;
    Arena arena;
    JsonErr json_error = {0};
    JsonValue *root = NULL;

    if (call == NULL || !call->active || event == NULL || call->failed)
        return;
    adapter = &yew_ai_adapters[call->backend.kind];
    arena_init(&arena);
    if (event->data != NULL && event->dlen != 0U) {
        root = yew_json_parse(&arena, event->data, event->dlen, &json_error);
        if (root == NULL) {
            call->failed = true;
            yew_ai_err_format(&call->error, YEW_AI_ERR_PROTOCOL,
                              &call->backend, call->status, -1,
                              json_error.msg);
            arena_free_all(&arena);
            return;
        }
    }
    adapter->consume(&call->adapter, event, root, &result);
    if (result.has_text && result.len != 0U) {
        if (result.len > YEW_HTTP_MAX_BODY - call->raw.len) {
            call->failed = true;
            yew_ai_err_format(&call->error, YEW_AI_ERR_TOO_LARGE,
                              &call->backend, call->status, -1, NULL);
        } else {
            bytebuf_append(&call->raw, result.text, result.len);
            call->dirty = true;
            call->ntokens++;
            if (call->t_first_token < 0)
                call->t_first_token = yew_now_ms();
        }
    }
    if (result.terminal || event->terminator) {
        call->terminal = true;
        if (call->conn != NULL)
            yew_http_conn_mark_stream_done(call->conn);
    }
    arena_free_all(&arena);
}

static void ai_feed(AiCall *call, const u8 *bytes, u64 len, bool eof)
{
    if (call == NULL || !call->active || call->failed)
        return;
    if (len > YEW_HTTP_MAX_BODY - call->response.len) {
        call->failed = true;
        yew_ai_err_format(&call->error, YEW_AI_ERR_TOO_LARGE,
                          &call->backend, call->status, -1, NULL);
        return;
    }
    if (len != 0U)
        bytebuf_append(&call->response, bytes, (size_t)len);
    yew_ai_stream_feed(&call->stream, bytes, len, eof, ai_stream_event,
                       call);
    if (call->stream.err[0] != '\0') {
        call->failed = true;
        yew_ai_err_format(&call->error, YEW_AI_ERR_PROTOCOL,
                          &call->backend, call->status, -1,
                          call->stream.err);
    }
}

static void ai_http_body(void *ctx, const u8 *bytes, u64 len)
{
    AiCall *call = ctx;

    if (call == NULL || call->conn == NULL || call->conn->rx == NULL)
        return;
    call->status = call->conn->rx->status;
    if (call->status >= 200U && call->status < 300U)
        ai_feed(call, bytes, len, false);
    else if (len <= YEW_HTTP_MAX_BODY - call->response.len)
        bytebuf_append(&call->response, bytes, (size_t)len);
}

static void ai_http_done(void *ctx, const HttpConn *conn)
{
    AiCall *call = ctx;

    if (call == NULL || !call->active || conn == NULL)
        return;
    call->status = conn->rx == NULL ? 0U : conn->rx->status;
    call->retry_after_ms = conn->rx == NULL ? -1 :
                           conn->rx->retry_after_ms;
    if (call->status >= 200U && call->status < 300U)
        ai_feed(call, NULL, 0U, true);
    if (conn->state == (u8)YEW_HC_DEAD && !call->failed) {
        call->failed = true;
        if (call->error.kind == (u8)YEW_AI_OK)
            yew_ai_err_format(&call->error, YEW_AI_ERR_UNREACHABLE,
                              &call->backend, call->status, -1,
                              "connection closed");
    }
    call->transport_done = true;
    call->live = false;
    call->t_done = yew_now_ms();
}

static bool ai_curl_feed_out(void *owner, const u8 *bytes, u64 len)
{
    AiCall *call = owner;

    if (call == NULL || !call->active)
        return false;
    ai_feed(call, bytes, len, false);
    return !call->failed;
}

static bool ai_curl_feed_err(void *owner, const u8 *bytes, u64 len)
{
    AiCall *call = owner;

    if (call == NULL || !call->active ||
        len > YEW_JOB_COLLECT_MAX - call->curl_err.len)
        return false;
    bytebuf_append(&call->curl_err, bytes, (size_t)len);
    return true;
}

static bool ai_curl_finish(void *owner)
{
    return owner != NULL;
}

static u16 ai_curl_status(const Bytebuf *err)
{
    static const char marker[] = "yew-http-status: ";
    size_t i;
    u16 status = 0U;

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

static void ai_curl_destroy(void *owner)
{
    AiCall *call = owner;
    YewJob *job;

    if (call == NULL || !call->active || call->ed == NULL)
        return;
    job = yew_job_find(call->ed, call->job);
    call->status = ai_curl_status(&call->curl_err);
    if (!call->failed)
        ai_feed(call, NULL, 0U, true);
    if (job == NULL || job->stream_failed || job->collect_capped ||
        job->state == YEW_JOB_TIMEOUT ||
        job->state == YEW_JOB_EXECFAIL ||
        (job->state == YEW_JOB_EXITED && job->exit_code != 0) ||
        job->state == YEW_JOB_SIGNALED) {
        call->failed = true;
        if (call->error.kind == (u8)YEW_AI_OK)
            yew_ai_err_format(&call->error,
                              job != NULL && job->state == YEW_JOB_TIMEOUT ?
                                  YEW_AI_ERR_TIMEOUT :
                                  YEW_AI_ERR_UNREACHABLE,
                              &call->backend, call->status, -1,
                              "curl request failed");
    }
    call->transport_done = true;
    call->live = false;
    call->t_done = yew_now_ms();
}

static const YewJobStreamOps ai_curl_ops = {
    ai_curl_feed_out,
    ai_curl_finish,
    ai_curl_feed_err,
    ai_curl_finish,
    NULL,
    NULL,
    ai_curl_destroy
};

static bool ai_auth(Ed *ed, AiCall *call, HttpHdr *headers, u32 *nhdr,
                    AiCurlSecret *secret, char *auth, size_t auth_size,
                    char *key, size_t key_size)
{
    size_t key_len;

    *nhdr = 0U;
    headers[(*nhdr)++] = (HttpHdr){"content-type", "application/json"};
    secret->kind = YEW_CURL_AUTH_NONE;
    secret->bytes = NULL;
    secret->len = 0U;
    if (call->backend.kind == (u8)YEW_AI_OLLAMA)
        return true;
    if (!yew_ai_key_cache_get(ed, &ed->ai->keys, &call->backend,
                              key, key_size, &call->error))
        return false;
    key_len = strlen(key);
    if (call->backend.kind == (u8)YEW_AI_OPENAI) {
        if (key_len + 8U > auth_size)
            return false;
        (void)snprintf(auth, auth_size, "Bearer %s", key);
        headers[(*nhdr)++] = (HttpHdr){"authorization", auth};
        secret->kind = YEW_CURL_AUTH_BEARER;
    } else {
        headers[(*nhdr)++] =
            (HttpHdr){"anthropic-version", "2023-06-01"};
        headers[(*nhdr)++] = (HttpHdr){"x-api-key", key};
        secret->kind = YEW_CURL_AUTH_X_API_KEY;
    }
    secret->bytes = (const u8 *)key;
    secret->len = key_len;
    return true;
}

static bool ai_request_url(Bytebuf *out, const AiBackendEntry *entry,
                           const char *path)
{
    const char *scheme;
    const char *authority;
    const char *slash;
    size_t prefix;

    scheme = strstr(entry->url_text, "://");
    if (scheme == NULL || path == NULL || path[0] != '/')
        return false;
    authority = scheme + 3U;
    slash = strchr(authority, '/');
    prefix = slash == NULL ? strlen(entry->url_text) :
                             (size_t)(slash - entry->url_text);
    bytebuf_append(out, entry->url_text, prefix);
    bytebuf_append(out, path, strlen(path));
    bytebuf_push_u8(out, 0U);
    out->len--;
    return true;
}

static bool ai_start_http(Ed *ed, AiCall *call, const char *path)
{
    HttpHdr headers[3];
    HttpReq request = {0};
    AiCurlSecret unused;
    char auth[YEW_AI_KEY_MAX + 8U];
    char key[YEW_AI_KEY_MAX + 1U];
    u32 nhdr;
    bool secret;

    (void)memset(auth, 0, sizeof(auth));
    (void)memset(key, 0, sizeof(key));
    if (!ai_auth(ed, call, headers, &nhdr, &unused, auth, sizeof(auth),
                 key, sizeof(key)))
        goto fail;
    request.method = "POST";
    request.path = path;
    request.hdrs = headers;
    request.nhdr = nhdr;
    request.body = call->body.data;
    request.blen = call->body.len;
    request.keepalive = true;
    secret = call->backend.kind != (u8)YEW_AI_OLLAMA;
    call->conn = yew_http_begin(ed, &call->backend.url, &request,
                                &call->error);
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    if (call->conn == NULL)
        return false;
    if (secret)
        yew_http_conn_mark_secret(call->conn);
    yew_http_conn_callbacks(call->conn, ai_http_body, ai_http_done, call);
    call->t_sent = yew_now_ms();
    return true;

fail:
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    return false;
}

static bool ai_start_curl(Ed *ed, AiCall *call, const char *path)
{
    HttpHdr auth_headers[3];
    HttpHdr public_headers[2];
    AiCurlSecret secret;
    AiCurlRequest request = {0};
    YewJobSpec spec = {0};
    Bytebuf url;
    char auth[YEW_AI_KEY_MAX + 8U];
    char key[YEW_AI_KEY_MAX + 1U];
    char err[192] = {0};
    u32 nhdr;
    u32 npublic = 1U;

    bytebuf_init(&url);
    (void)memset(auth, 0, sizeof(auth));
    (void)memset(key, 0, sizeof(key));
    if (!ai_request_url(&url, call->entry, path) ||
        !ai_auth(ed, call, auth_headers, &nhdr, &secret, auth,
                 sizeof(auth), key, sizeof(key)))
        goto fail;
    public_headers[0] = auth_headers[0];
    if (call->backend.kind == (u8)YEW_AI_ANTHROPIC)
        public_headers[npublic++] = auth_headers[1];
    request.url = (const char *)url.data;
    request.method = "POST";
    request.hdrs = public_headers;
    request.nhdr = npublic;
    request.body = call->body.data;
    request.blen = call->body.len;
    request.connect_timeout_ms =
        ai_option_int(ed, "ai.connect_timeout_ms", 2000);
    request.total_timeout_ms =
        ai_option_int(ed, "ai.total_timeout_ms", 120000);
    if (!yew_ai_curl_config(&call->curl_config, &request, &secret,
                            err, sizeof(err))) {
        yew_ai_err_format(&call->error, YEW_AI_ERR_PROTOCOL,
                          &call->backend, 0U, -1, err);
        goto fail;
    }
    spec.argv = (char **)yew_ai_curl_argv();
    spec.sink = YEW_SINK_STREAM;
    spec.in_bytes = call->curl_config.data;
    spec.in_len = call->curl_config.len;
    spec.timeout_ms = request.total_timeout_ms;
    spec.display = "curl AI completion";
    spec.stream_owner = call;
    spec.stream_ops = &ai_curl_ops;
    call->job = yew_job_spawn(ed, &spec, err, sizeof(err));
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    bytebuf_free(&url);
    if (call->job == 0U) {
        yew_ai_err_format(&call->error, YEW_AI_ERR_UNREACHABLE,
                          &call->backend, 0U, -1, err);
        return false;
    }
    call->t_sent = yew_now_ms();
    return true;

fail:
    yew_memzero(auth, sizeof(auth));
    yew_memzero(key, sizeof(key));
    bytebuf_free(&url);
    return false;
}

static void ai_effective_backend(Ed *ed, const AiBackend *source,
                                 AiBackend *out)
{
    const char *fim = ai_option_str(ed, "ai.fim");

    *out = *source;
    out->stream = true;
    out->max_tokens = ai_option_int(ed, "ai.max_tokens", 256);
    out->temperature =
        (double)ai_option_int(ed, "ai.temperature", 10) / 1000.0;
    if (strcmp(fim, "on") == 0)
        out->fim_policy = (u8)YEW_AI_FIM_ON;
    else if (strcmp(fim, "off") == 0)
        out->fim_policy = (u8)YEW_AI_FIM_OFF;
    else
        out->fim_policy = (u8)YEW_AI_FIM_AUTO;
    out->fim = yew_ai_template_of(out) == YEW_AI_TPL_FIM;
}

static bool ai_shadow_request(Ed *ed, const ShadowReq *request)
{
    AiState *state;
    AiCall *call;
    const AiBackendEntry *selected;
    AiBackendEntry *entry;
    const AiAdapter *adapter;
    JsonW writer;
    const char *path;
    char probe_error[192] = {0};
    bool started;

    if (ed == NULL || ed->ai == NULL || request == NULL || ed->win == NULL ||
        ed->win->buf == NULL || !ai_option_bool(ed, "ai.enable", false))
        return false;
    state = ed->ai;
    selected = yew_ai_backend_selected(ed);
    if (selected == NULL ||
        yew_ai_cooldown_remaining(&selected->cooldown, yew_now_ms()) > 0) {
        yew_ai_stats_decline(ed, selected == NULL ? "" :
                                               selected->backend.name);
        return false;
    }
    if (workspace_allowed != NULL &&
        !workspace_allowed(ed, yew_ws_root(ed))) {
        yew_ai_stats_decline(ed, selected->backend.name);
        return false;
    }
    path = ed->win->buf->path == NULL ? "" : ed->win->buf->path;
    if (path_excluded != NULL && path_excluded(ed, path)) {
        yew_ai_stats_decline(ed, selected->backend.name);
        return false;
    }
    call = &state->call;
    if (call->active) {
        if (request->seq <= call->seq) {
            yew_ai_stats_decline(ed, selected->backend.name);
            return false;
        }
        yew_ai_call_abort(ed, call, YEW_AI_ERR_CANCELLED);
    }
    if (selected->backend.transport == (u8)YEW_AI_TR_CURL &&
        !yew_ai_curl_probe(ed, &state->curl, probe_error,
                           sizeof(probe_error))) {
        if (probe_error[0] != '\0' && !state->curl_probe_messaged) {
            yew_msg(ed, YEW_MSG_WARN, "%s", probe_error);
            state->curl_probe_messaged = true;
        }
        yew_ai_stats_decline(ed, selected->backend.name);
        return false;
    }
    entry = yew_ai_registry_find_mut(&state->backends,
                                     selected->backend.name);
    if (entry == NULL)
        return false;
    ai_call_init(call, ed);
    call->active = true;
    call->live = true;
    call->entry = entry;
    ai_effective_backend(ed, &entry->backend, &call->backend);
    call->seq = request->seq;
    call->buf_id = request->buf_id;
    call->buf_gen = request->buf_gen;
    call->pos = request->pos;
    call->t_armed = ed->now_ms;
    if (!yew_ai_context_build(ed, ed->win, request, &call->arena,
                              &call->context, &call->error)) {
        yew_ai_stats_decline(ed, call->backend.name);
        ai_call_reset(call);
        return false;
    }
    yew_jsonw_init(&writer, &call->body);
    yew_ai_prompt_build(&writer, &call->backend, &call->context);
    adapter = &yew_ai_adapters[call->backend.kind];
    yew_ai_stream_init(&call->stream, adapter->stream_mode);
    started = call->backend.transport == (u8)YEW_AI_TR_HTTP ?
                  ai_start_http(ed, call, adapter->path_gen(&call->backend)) :
                  ai_start_curl(ed, call, adapter->path_gen(&call->backend));
    if (!started) {
        yew_ai_stats_error(ed, call->backend.name);
        if (call->error.msg[0] != '\0')
            yew_msg(ed, YEW_MSG_WARN, "%s", call->error.msg);
        ai_call_reset(call);
        return false;
    }
    yew_ai_stats_request(ed, call->backend.name);
    ai_shadow_log(ed,
                  "request %s model=%s template=%s context=%u transport=%s",
                  call->backend.name, call->backend.model,
                  call->backend.fim ? "fim" : "chat",
                  call->context.plen + call->context.slen,
                  call->backend.transport == (u8)YEW_AI_TR_CURL ?
                      "curl" : "http");
    return true;
}

static void ai_shadow_cancel(Ed *ed, u32 buf_id, u32 up_to)
{
    AiCall *call;

    if (ed == NULL || ed->ai == NULL)
        return;
    call = &ed->ai->call;
    if (!call->active || call->buf_id != buf_id || call->seq > up_to)
        return;
    yew_ai_call_abort(ed, call, YEW_AI_ERR_CANCELLED);
}

static const ShadowProvider yew_shadow_ai = {
    "ai", YEW_SHADOW_AI, 350U, ai_shadow_request, ai_shadow_cancel,
};

void yew_ai_shadow_init(Ed *ed)
{
    static bool installed;
    const char *test_provider = getenv("YEW_SHADOW_TEST");

    (void)ed;
    if (installed ||
        (test_provider != NULL && strcmp(test_provider, "1") == 0))
        return;
    yew_shadow_register(&yew_shadow_ai);
    installed = true;
}

static void ai_clear_ghost(Ed *ed, const AiCall *call)
{
    ShadowSug suggestion = {0};

    suggestion.seq = call->seq;
    suggestion.prov = (u8)YEW_SHADOW_AI;
    suggestion.buf_id = call->buf_id;
    suggestion.buf_gen = call->buf_gen;
    suggestion.pos = call->pos;
    yew_shadow_deliver(ed, &suggestion);
}

static bool ai_shadow_frame(Ed *ed, AiCall *call)
{
    ShadowSug suggestion = {0};
    u32 max_lines;
    i64 frame_ms;
    i64 now;

    if (!call->active || !call->dirty)
        return false;
    now = yew_now_ms();
    frame_ms = ai_option_int(ed, "ai.frame_ms", 33);
    if (call->delivered != 0U && ed->ai->last_deliver_ms >= 0 &&
        now - ed->ai->last_deliver_ms < frame_ms)
        return false;
    call->text.len = 0U;
    bytebuf_append(&call->text, call->raw.data, call->raw.len);
    max_lines = (u32)ai_option_int(ed, "ai.max_lines", 8);
    call->text.len = yew_ai_response_trim(call->text.data,
                                           (u32)call->text.len,
                                           &call->context, max_lines);
    call->dirty = false;
    if (call->text.len <= call->delivered)
        return false;
    suggestion.seq = call->seq;
    suggestion.prov = (u8)YEW_SHADOW_AI;
    suggestion.buf_id = call->buf_id;
    suggestion.buf_gen = call->buf_gen;
    suggestion.pos = call->pos;
    suggestion.text = call->text.data;
    suggestion.len = (u32)call->text.len;
    yew_shadow_deliver(ed, &suggestion);
    ed->ai->last_deliver_ms = now;
    call->delivered = (u32)call->text.len;
    if (!call->counted_delivery) {
        call->counted_delivery = true;
        yew_ai_stats_delivery(ed, call->backend.name, call->text.len);
    }
    ed->ai->suggestion_seq = call->seq;
    (void)snprintf(ed->ai->suggestion_backend,
                   sizeof(ed->ai->suggestion_backend), "%s",
                   call->backend.name);
    return true;
}

static AiErrKind ai_classify(AiCall *call)
{
    const AiAdapter *adapter = &yew_ai_adapters[call->backend.kind];
    Arena arena;
    JsonErr error = {0};
    JsonValue *root = NULL;
    AiErrKind kind;

    if (call->error.kind != (u8)YEW_AI_OK)
        return (AiErrKind)call->error.kind;
    arena_init(&arena);
    if (call->status >= 200U && call->status < 300U) {
        kind = YEW_AI_OK;
    } else {
        if (call->response.len != 0U)
            root = yew_json_parse(&arena, call->response.data,
                                  call->response.len, &error);
        kind = adapter->classify(call->status, root);
    }
    if (kind == YEW_AI_OK && call->failed)
        kind = YEW_AI_ERR_PROTOCOL;
    if (kind == YEW_AI_OK && (!call->terminal || call->stream.err[0] != '\0'))
        kind = YEW_AI_ERR_PROTOCOL;
    if (kind != YEW_AI_OK)
        yew_ai_err_format(&call->error, kind, &call->backend, call->status,
                          call->retry_after_ms,
                          call->stream.err[0] != '\0' ? call->stream.err :
                          (error.msg[0] != '\0' ? error.msg :
                                                   "response ended early"));
    arena_free_all(&arena);
    return kind;
}

static void ai_finish(Ed *ed, AiCall *call)
{
    AiErrKind kind = ai_classify(call);
    char backend[128];
    i64 first_ms = call->t_first_token < 0 ? -1 :
                   call->t_first_token - call->t_armed;
    i64 total_ms = call->t_done < 0 ? -1 : call->t_done - call->t_armed;
    bool clear = kind != YEW_AI_OK && call->counted_delivery;

    (void)snprintf(backend, sizeof(backend), "%s", call->backend.name);
    if (call->conn != NULL) {
        yew_http_conn_callbacks(call->conn, NULL, NULL, NULL);
        yew_http_conn_release(ed, call->conn);
        call->conn = NULL;
    }
    if (call->job != 0U) {
        YewJob *job = yew_job_find(ed, call->job);

        if (job != NULL && !yew_job_pending(job))
            yew_job_release(ed, job);
        call->job = 0U;
    }
    if (kind == YEW_AI_OK) {
        yew_ai_cooldown_note(&call->entry->cooldown, YEW_AI_OK, -1,
                             yew_now_ms(), 60000);
    } else {
        yew_ai_cooldown_note(&call->entry->cooldown, kind,
                             call->error.retry_ms, yew_now_ms(),
                             ai_option_int(ed, "ai.backoff_max_ms", 60000));
        yew_ai_stats_error(ed, backend);
        if (kind != YEW_AI_ERR_CANCELLED)
            yew_msg(ed, YEW_MSG_WARN, "%s", call->error.msg);
    }
    yew_ai_stats_finish(ed, backend, first_ms,
                        call->adapter.input_tokens,
                        call->adapter.output_tokens);
    ai_shadow_log(ed,
                  "request %s first-token=%lld total=%lld tokens=%lld/%lld class=%u",
                  backend, (long long)first_ms, (long long)total_ms,
                  (long long)call->adapter.input_tokens,
                  (long long)call->adapter.output_tokens, (unsigned)kind);
    if (clear)
        ai_clear_ghost(ed, call);
    ai_call_reset(call);
}

void yew_ai_shadow_pump(Ed *ed)
{
    AiCall *call;

    if (ed == NULL || ed->ai == NULL)
        return;
    ai_retired_pump(ed);
    call = &ed->ai->call;
    if (!call->active)
        return;
    if (call->abort_pending) {
        yew_ai_call_abort(ed, call, YEW_AI_ERR_PROTOCOL);
        return;
    }
    (void)ai_shadow_frame(ed, call);
    if (!call->transport_done)
        return;
    if (call->dirty)
        return;
    ai_finish(ed, call);
}

i64 yew_ai_shadow_deadline(const Ed *ed, i64 now_ms)
{
    const AiCall *call;
    i64 frame_ms;
    i64 due;

    if (ed == NULL || ed->ai == NULL)
        return -1;
    call = &ed->ai->call;
    if (!call->active)
        return -1;
    if (call->transport_done && !call->dirty)
        return 0;
    if (!call->dirty || call->delivered == 0U)
        return call->dirty ? 0 : -1;
    frame_ms = ai_option_int((Ed *)ed, "ai.frame_ms", 33);
    due = ed->ai->last_deliver_ms + frame_ms;
    return due <= now_ms ? 0 : due - now_ms;
}

void yew_ai_shadow_free(Ed *ed)
{
    if (ed == NULL || ed->ai == NULL)
        return;
    if (ed->ai->call.active)
        yew_ai_call_abort(ed, &ed->ai->call, YEW_AI_ERR_CANCELLED);
}

void yew_ai_shadow_accept_note(Ed *ed, u32 seq, u8 kind, u64 bytes)
{
    if (ed == NULL || ed->ai == NULL ||
        ed->ai->suggestion_seq != seq ||
        ed->ai->suggestion_backend[0] == '\0')
        return;
    yew_ai_stats_accept(ed, ed->ai->suggestion_backend, kind, bytes);
    if (kind == 2U) {
        ed->ai->suggestion_seq = 0U;
        ed->ai->suggestion_backend[0] = '\0';
    }
}

void yew_ai_shadow_dismiss_note(Ed *ed, u32 seq)
{
    if (ed == NULL || ed->ai == NULL ||
        ed->ai->suggestion_seq != seq ||
        ed->ai->suggestion_backend[0] == '\0')
        return;
    yew_ai_stats_dismiss(ed, ed->ai->suggestion_backend);
    ed->ai->suggestion_seq = 0U;
    ed->ai->suggestion_backend[0] = '\0';
}
