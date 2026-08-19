/* Sprint 48: deterministic curl --config transport material. */

#include "mod/ai/backend_curl.h"

#include <signal.h>
#include <stdio.h>
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

static bool curl_quoted(Bytebuf *out, const u8 *bytes, size_t len,
                        char *err, size_t errsz)
{
    size_t i;

    bytebuf_push_u8(out, (u8)'"');
    for (i = 0U; i < len; i++) {
        u8 c = bytes[i];

        if (c < 0x20U) {
            curl_error(err, errsz,
                       "curl config value contains a control byte");
            return false;
        }
        if (c == (u8)'\\' || c == (u8)'"')
            bytebuf_push_u8(out, (u8)'\\');
        bytebuf_push_u8(out, c);
    }
    bytebuf_push_u8(out, (u8)'"');
    return true;
}

static bool curl_directive(Bytebuf *out, const char *name,
                           const u8 *value, size_t len,
                           char *err, size_t errsz)
{
    bytebuf_append(out, name, strlen(name));
    bytebuf_append(out, " = ", 3U);
    if (!curl_quoted(out, value, len, err, errsz))
        return false;
    bytebuf_push_u8(out, (u8)'\n');
    return true;
}

static bool curl_header(Bytebuf *out, const HttpHdr *h,
                        char *err, size_t errsz)
{
    Bytebuf value;
    bool ok;

    if (h->name == NULL || h->name[0] == '\0' || h->value == NULL) {
        curl_error(err, errsz, "curl header is missing a name or value");
        return false;
    }
    bytebuf_init(&value);
    bytebuf_append(&value, h->name, strlen(h->name));
    bytebuf_append(&value, ": ", 2U);
    bytebuf_append(&value, h->value, strlen(h->value));
    ok = curl_directive(out, "header", value.data, value.len, err, errsz);
    yew_memzero(value.data, value.len);
    bytebuf_free(&value);
    return ok;
}

bool yew_ai_curl_config(Bytebuf *out, const AiCurlRequest *req,
                        char *err, size_t errsz)
{
    Bytebuf tmp;
    u32 i;
    size_t at;
    bool ok = false;

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

    bytebuf_init(&tmp);
    if (!curl_directive(&tmp, "url", (const u8 *)req->url,
                        strlen(req->url), err, errsz) ||
        !curl_directive(&tmp, "request", (const u8 *)req->method,
                        strlen(req->method), err, errsz))
        goto done;
    for (i = 0U; i < req->nhdr; i++) {
        if (!curl_header(&tmp, &req->hdrs[i], err, errsz))
            goto done;
    }
    if (!curl_directive(&tmp, "data-binary", req->body,
                        (size_t)req->blen, err, errsz))
        goto done;
    {
        static const char connect_timeout[] = "connect-timeout = 2\n";
        static const char max_time[] = "max-time = 120\n";
        static const char write_out[] =
            "write-out = \"%{stderr}yew-http-status: %{http_code}\\n\"\n";

        bytebuf_append(&tmp, connect_timeout, sizeof(connect_timeout) - 1U);
        bytebuf_append(&tmp, max_time, sizeof(max_time) - 1U);
        bytebuf_append(&tmp, write_out, sizeof(write_out) - 1U);
    }

    /* Only line separators introduced above may be controls.  Quoted input
     * was checked byte-by-byte before it could reach this buffer. */
    for (at = 0U; at < tmp.len; at++) {
        if (tmp.data[at] < 0x20U && tmp.data[at] != (u8)'\n')
            YEW_BUG("curl config assembly emitted control byte 0x%02x",
                    (unsigned)tmp.data[at]);
    }
    bytebuf_append(out, tmp.data, tmp.len);
    ok = true;

done:
    yew_memzero(tmp.data, tmp.len);
    bytebuf_free(&tmp);
    return ok;
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
    if (probe == NULL)
        return;
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
