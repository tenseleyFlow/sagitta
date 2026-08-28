/* Sprint 48: deterministic curl --config transport material. */

#include "mod/ai/backend_curl.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/job.h"
#include "util/log.h"

static char *const curl_argv[] = {
    (char *)"curl",
    (char *)"-sS",
    (char *)"--no-buffer",
    (char *)"--config",
    (char *)"-",
    NULL
};

static char *curl_probe_argv[] = {
    (char *)"curl",
    (char *)"--version",
    NULL
};

static const char curl_absent[] =
    "cloud AI backends need curl, which is not in $PATH. Install curl, or "
    "run a local model: :ai backend local (see :help ai-local).";

static void curl_error(char *err, size_t errsz, const char *message)
{
    if (err != NULL && errsz != 0U)
        (void)snprintf(err, errsz, "%s", message);
}

char *const *yew_ai_curl_argv(void)
{
    return curl_argv;
}

static bool curl_size_add(size_t *total, size_t add, char *err, size_t errsz)
{
    if (add > SIZE_MAX - *total) {
        curl_error(err, errsz, "curl request is too large");
        return false;
    }
    *total += add;
    return true;
}

static bool curl_piece_measure(const u8 *bytes, size_t len, size_t *total,
                               char *err, size_t errsz)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        u8 c = bytes[i];

        if (c < 0x20U) {
            curl_error(err, errsz,
                       "curl config value contains a control byte");
            return false;
        }
        if (!curl_size_add(total,
                           c == (u8)'\\' || c == (u8)'"' ? 2U : 1U,
                           err, errsz))
            return false;
    }
    return true;
}

static void curl_quoted_piece(Bytebuf *out, const u8 *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        if (bytes[i] == (u8)'\\' || bytes[i] == (u8)'"')
            bytebuf_push_u8(out, (u8)'\\');
        bytebuf_push_u8(out, bytes[i]);
    }
}

static void curl_directive(Bytebuf *out, const char *name,
                           const u8 *value, size_t len)
{
    bytebuf_append(out, name, strlen(name));
    bytebuf_append(out, " = \"", 4U);
    curl_quoted_piece(out, value, len);
    bytebuf_append(out, "\"\n", 2U);
}

static bool curl_ascii_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        unsigned char ac = (unsigned char)*a++;
        unsigned char bc = (unsigned char)*b++;

        if (ac >= (unsigned char)'A' && ac <= (unsigned char)'Z')
            ac = (unsigned char)(ac - (unsigned char)'A' +
                                 (unsigned char)'a');
        if (bc >= (unsigned char)'A' && bc <= (unsigned char)'Z')
            bc = (unsigned char)(bc - (unsigned char)'A' +
                                 (unsigned char)'a');
        if (ac != bc)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool curl_auth_header(const char *name)
{
    return curl_ascii_eq(name, "authorization") ||
           curl_ascii_eq(name, "x-api-key");
}

static bool curl_header_measure(const HttpHdr *h, size_t *total,
                                char *err, size_t errsz)
{
    size_t nlen;
    size_t vlen;

    if (h->name == NULL || h->name[0] == '\0' || h->value == NULL) {
        curl_error(err, errsz, "curl header is missing a name or value");
        return false;
    }
    if (curl_auth_header(h->name)) {
        curl_error(err, errsz,
                   "authorization headers must use the secret argument");
        return false;
    }
    nlen = strlen(h->name);
    vlen = strlen(h->value);
    if (!curl_size_add(total, strlen("header = \""), err, errsz) ||
        !curl_piece_measure((const u8 *)h->name, nlen, total, err, errsz) ||
        !curl_size_add(total, 2U, err, errsz) ||
        !curl_piece_measure((const u8 *)h->value, vlen, total, err, errsz) ||
        !curl_size_add(total, 2U, err, errsz))
        return false;
    return true;
}

static void curl_header(Bytebuf *out, const char *name,
                        const u8 *value, size_t len, const char *prefix)
{
    bytebuf_append(out, "header = \"", 10U);
    curl_quoted_piece(out, (const u8 *)name, strlen(name));
    bytebuf_append(out, ": ", 2U);
    if (prefix != NULL)
        bytebuf_append(out, prefix, strlen(prefix));
    curl_quoted_piece(out, value, len);
    bytebuf_append(out, "\"\n", 2U);
}

static bool curl_directive_measure(const char *name, const u8 *value,
                                   size_t len, size_t *total,
                                   char *err, size_t errsz)
{
    return curl_size_add(total, strlen(name) + strlen(" = \""),
                         err, errsz) &&
           curl_piece_measure(value, len, total, err, errsz) &&
           curl_size_add(total, strlen("\"\n"), err, errsz);
}

static bool curl_secret_measure(const AiCurlSecret *secret, size_t *total,
                                char *err, size_t errsz)
{
    const char *name;
    const char *prefix;

    if (secret == NULL || secret->kind == YEW_CURL_AUTH_NONE)
        return true;
    if (secret->bytes == NULL || secret->len == 0U) {
        curl_error(err, errsz, "curl secret is incomplete");
        return false;
    }
    if (secret->kind == YEW_CURL_AUTH_BEARER) {
        name = "Authorization";
        prefix = "Bearer ";
    } else if (secret->kind == YEW_CURL_AUTH_X_API_KEY) {
        name = "x-api-key";
        prefix = "";
    } else {
        curl_error(err, errsz, "unknown curl authentication kind");
        return false;
    }
    if (!curl_size_add(total, strlen("header = \""), err, errsz) ||
        !curl_piece_measure((const u8 *)name, strlen(name), total,
                            err, errsz) ||
        !curl_size_add(total, 2U + strlen(prefix), err, errsz) ||
        !curl_piece_measure(secret->bytes, secret->len, total, err, errsz) ||
        !curl_size_add(total, strlen("\"\n"), err, errsz))
        return false;
    return true;
}

static bool curl_timeout_line(char *out, size_t outsz, const char *name,
                              i64 configured_ms, i64 fallback_ms,
                              char *err, size_t errsz)
{
    i64 ms = configured_ms > 0 ? configured_ms : fallback_ms;
    i64 seconds = ms / 1000;
    unsigned fraction = (unsigned)(ms % 1000);
    char digits[4];
    size_t nfrac = 3U;
    int n;

    n = snprintf(digits, sizeof(digits), "%03u", fraction);
    if (n != 3) {
        curl_error(err, errsz, "curl timeout is too large");
        return false;
    }
    while (nfrac != 0U && digits[nfrac - 1U] == '0')
        nfrac--;
    if (nfrac == 0U)
        n = snprintf(out, outsz, "%s = %lld\n", name,
                     (long long)seconds);
    else
        n = snprintf(out, outsz, "%s = %lld.%.*s\n", name,
                     (long long)seconds, (int)nfrac, digits);
    if (n < 0 || (size_t)n >= outsz) {
        curl_error(err, errsz, "curl timeout is too large");
        return false;
    }
    return true;
}

static void curl_secure_reserve(Bytebuf *buf, size_t need)
{
    size_t cap;
    u8 *data;

    if (buf->cap >= need)
        return;
    cap = buf->cap != 0U ? buf->cap : 64U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    data = yew_xmalloc(cap);
    if (buf->len != 0U)
        (void)memcpy(data, buf->data, buf->len);
    if (buf->data != NULL) {
        yew_memzero(buf->data, buf->cap);
        yew_xfree(buf->data);
    }
    buf->data = data;
    buf->cap = cap;
}

bool yew_ai_curl_config(Bytebuf *out, const AiCurlRequest *req,
                        const AiCurlSecret *secret, char *err, size_t errsz)
{
    Bytebuf tmp;
    u32 i;
    size_t at;
    size_t total = 0U;
    size_t final_len;
    const char *secret_name = NULL;
    const char *secret_prefix = NULL;
    u8 *stable;
    char connect_timeout[64];
    char max_time[64];
    static const char write_out[] =
        "write-out = \"%{stderr}yew-http-status: %{http_code}\\n\"\n";

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (out == NULL || req == NULL || req->url == NULL ||
        req->url[0] == '\0' || req->method == NULL ||
        req->method[0] == '\0' ||
        (req->nhdr != 0U && req->hdrs == NULL) ||
        (req->blen != 0U && req->body == NULL)) {
        curl_error(err, errsz, "incomplete curl request");
        return false;
    }
    if (req->nhdr > YEW_HTTP_MAX_HDRS) {
        curl_error(err, errsz, "too many curl request headers");
        return false;
    }
    if (req->blen > (u64)SIZE_MAX) {
        curl_error(err, errsz, "curl request body is too large");
        return false;
    }
    if (!curl_timeout_line(connect_timeout, sizeof(connect_timeout),
                           "connect-timeout", req->connect_timeout_ms,
                           2000, err, errsz) ||
        !curl_timeout_line(max_time, sizeof(max_time), "max-time",
                           req->total_timeout_ms, 120000, err, errsz))
        return false;
    if (!curl_directive_measure("url", (const u8 *)req->url,
                                strlen(req->url), &total, err, errsz) ||
        !curl_directive_measure("request", (const u8 *)req->method,
                                strlen(req->method), &total, err, errsz))
        return false;
    for (i = 0U; i < req->nhdr; i++)
        if (!curl_header_measure(&req->hdrs[i], &total, err, errsz))
            return false;
    if (!curl_secret_measure(secret, &total, err, errsz) ||
        !curl_directive_measure("data-binary", req->body,
                                (size_t)req->blen, &total, err, errsz) ||
        !curl_size_add(&total, strlen(connect_timeout), err, errsz) ||
        !curl_size_add(&total, strlen(max_time), err, errsz) ||
        !curl_size_add(&total, sizeof(write_out) - 1U, err, errsz) ||
        total > SIZE_MAX - out->len) {
        if (total > SIZE_MAX - out->len)
            curl_error(err, errsz, "curl request is too large");
        return false;
    }
    final_len = out->len + total;

    bytebuf_init(&tmp);
    bytebuf_reserve(&tmp, total);
    curl_secure_reserve(out, final_len);
    curl_directive(&tmp, "url", (const u8 *)req->url, strlen(req->url));
    curl_directive(&tmp, "request", (const u8 *)req->method,
                   strlen(req->method));
    for (i = 0U; i < req->nhdr; i++)
        curl_header(&tmp, req->hdrs[i].name,
                    (const u8 *)req->hdrs[i].value,
                    strlen(req->hdrs[i].value), NULL);
    stable = tmp.data;
    if (secret != NULL && secret->kind != YEW_CURL_AUTH_NONE) {
        if (secret->kind == YEW_CURL_AUTH_BEARER) {
            secret_name = "Authorization";
            secret_prefix = "Bearer ";
        } else {
            secret_name = "x-api-key";
            secret_prefix = "";
        }
        curl_header(&tmp, secret_name, secret->bytes, secret->len,
                    secret_prefix);
    }
    curl_directive(&tmp, "data-binary", req->body, (size_t)req->blen);
    bytebuf_append(&tmp, connect_timeout, strlen(connect_timeout));
    bytebuf_append(&tmp, max_time, strlen(max_time));
    bytebuf_append(&tmp, write_out, sizeof(write_out) - 1U);
    if (tmp.data != stable || tmp.len != total)
        YEW_BUG("curl config changed allocation after secret insertion");

    /* Only line separators introduced above may be controls.  Quoted input
     * was checked byte-by-byte before it could reach this buffer. */
    for (at = 0U; at < tmp.len; at++) {
        if (tmp.data[at] < 0x20U && tmp.data[at] != (u8)'\n')
            YEW_BUG("curl config assembly emitted control byte 0x%02x",
                    (unsigned)tmp.data[at]);
    }
    bytebuf_append(out, tmp.data, tmp.len);
    if (out->len != final_len)
        YEW_BUG("curl config output length mismatch");
    yew_memzero(tmp.data, tmp.cap);
    bytebuf_free(&tmp);
    return true;
}

AiErrKind yew_ai_curl_exit_class(int exit_code, int termsig)
{
    if (termsig == SIGTERM || exit_code == 130)
        return YEW_AI_ERR_CANCELLED;
    if (termsig != 0)
        return YEW_AI_ERR_PROTOCOL;
    switch (exit_code) {
    case 0:
        return YEW_AI_OK;
    case 6:
    case 7:
        return YEW_AI_ERR_UNREACHABLE;
    case 28:
        return YEW_AI_ERR_TIMEOUT;
    case 35:
    case 60:
        return YEW_AI_ERR_TLS;
    case 2:
    case 22:
    default:
        return YEW_AI_ERR_PROTOCOL;
    }
}

static bool curl_probe_feed_out(void *owner, const u8 *bytes, u64 len)
{
    AiCurlProbe *probe = owner;

    if (len > 4096U - (u64)probe->out.len)
        return false;
    bytebuf_append(&probe->out, bytes, (size_t)len);
    return true;
}

static bool curl_probe_finish(void *owner)
{
    (void)owner;
    return true;
}

static bool curl_probe_feed_err(void *owner, const u8 *bytes, u64 len)
{
    AiCurlProbe *probe = owner;

    if (len > 4096U - (u64)probe->err.len)
        return false;
    bytebuf_append(&probe->err, bytes, (size_t)len);
    return true;
}

static bool curl_parse_component(const u8 **at, const u8 *end, u32 *out)
{
    u32 n = 0U;
    bool any = false;

    while (*at < end && **at >= (u8)'0' && **at <= (u8)'9') {
        u32 digit = (u32)(**at - (u8)'0');

        if (n > (UINT32_MAX - digit) / 10U)
            return false;
        n = n * 10U + digit;
        (*at)++;
        any = true;
    }
    *out = n;
    return any;
}

static bool curl_version_parse(AiCurlProbe *probe)
{
    const u8 prefix[] = "curl ";
    const u8 *at = probe->out.data;
    const u8 *end = at + probe->out.len;

    if (probe->out.len < sizeof(prefix) - 1U ||
        memcmp(at, prefix, sizeof(prefix) - 1U) != 0)
        return false;
    at += sizeof(prefix) - 1U;
    if (!curl_parse_component(&at, end, &probe->major) || at == end ||
        *at++ != (u8)'.' ||
        !curl_parse_component(&at, end, &probe->minor))
        return false;
    probe->patch = 0U;
    if (at < end && *at == (u8)'.') {
        at++;
        if (!curl_parse_component(&at, end, &probe->patch))
            return false;
    }
    if (at < end && *at != (u8)' ' && *at != (u8)'\r' && *at != (u8)'\n')
        return false;
    (void)snprintf(probe->version, sizeof(probe->version), "curl %u.%u.%u",
                   probe->major, probe->minor, probe->patch);
    return true;
}

static void curl_probe_destroy(void *owner)
{
    AiCurlProbe *probe = owner;
    YewJob *job = yew_job_find(probe->ed, probe->job_id);

    probe->running = false;
    if (job == NULL || job->state == YEW_JOB_EXECFAIL ||
        job->state != YEW_JOB_EXITED || job->exit_code != 0 ||
        job->stream_failed || !curl_version_parse(probe)) {
        probe->state = YEW_CURL_ABSENT;
        return;
    }
    if (probe->major < 7U ||
        (probe->major == 7U && probe->minor < 63U)) {
        probe->state = YEW_CURL_TOO_OLD;
        return;
    }
    probe->state = YEW_CURL_OK;
}

static const YewJobStreamOps curl_probe_ops = {
    curl_probe_feed_out,
    curl_probe_finish,
    curl_probe_feed_err,
    curl_probe_finish,
    NULL,
    NULL,
    curl_probe_destroy
};

void yew_ai_curl_probe_init(AiCurlProbe *probe)
{
    if (probe == NULL)
        return;
    (void)memset(probe, 0, sizeof(*probe));
    bytebuf_init(&probe->out);
    bytebuf_init(&probe->err);
}

void yew_ai_curl_probe_free(AiCurlProbe *probe)
{
    YewJob *job;

    if (probe == NULL)
        return;
    job = probe->running && probe->ed != NULL ?
          yew_job_find(probe->ed, probe->job_id) : NULL;
    if (job != NULL) {
        /* Editor teardown frees AI state before the generic job table.
         * Detach the callback first so that table teardown cannot call into
         * this probe after its buffers have been released. */
        job->stream_owner = NULL;
        job->stream_ops = NULL;
        job->stream_destroyed = true;
        (void)yew_job_signal(probe->ed, probe->job_id, SIGTERM);
    }
    bytebuf_free(&probe->out);
    bytebuf_free(&probe->err);
    (void)memset(probe, 0, sizeof(*probe));
}

static bool curl_probe_cached(const AiCurlProbe *probe, char *err,
                              size_t errsz)
{
    if (probe->state == YEW_CURL_OK)
        return true;
    if (probe->state == YEW_CURL_ABSENT) {
        curl_error(err, errsz, curl_absent);
    } else if (probe->state == YEW_CURL_TOO_OLD && err != NULL && errsz != 0U) {
        (void)snprintf(
            err, errsz,
            "curl %u.%u is too old for the AI transport (need >= 7.63 for "
            "--write-out %%{stderr}). Upgrade curl, or use a local http:// "
            "backend.",
            probe->major, probe->minor);
    }
    return false;
}

bool yew_ai_curl_probe(Ed *ed, AiCurlProbe *probe, char *err, size_t errsz)
{
    YewJobSpec spec = {0};
    u32 id;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (ed == NULL || probe == NULL) {
        curl_error(err, errsz, "invalid curl probe");
        return false;
    }
    if (probe->state != YEW_CURL_UNKNOWN)
        return curl_probe_cached(probe, err, errsz);
    if (probe->running)
        return false;
    probe->ed = ed;
    spec.argv = curl_probe_argv;
    spec.sink = YEW_SINK_STREAM;
    spec.timeout_ms = 2000;
    spec.display = "curl --version";
    spec.stream_owner = probe;
    spec.stream_ops = &curl_probe_ops;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return false;
    probe->job_id = id;
    probe->probes++;
    probe->running = true;
    return false;
}

const char *yew_ai_curl_probe_version(const AiCurlProbe *probe)
{
    if (probe == NULL || probe->state != YEW_CURL_OK)
        return NULL;
    return probe->version;
}
