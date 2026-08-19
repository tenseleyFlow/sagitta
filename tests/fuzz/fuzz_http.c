#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "mod/ai/http.h"
#include "util/buf.h"

typedef struct BodySeen {
    u64 bytes;
    u64 hash;
    bool valid;
} BodySeen;

typedef struct HttpResult {
    u8 state;
    u8 ver_minor;
    bool pending_cr;
    bool have_len;
    bool bad_len;
    bool conflicting_len;
    bool chunked;
    bool close_delimited;
    bool no_body;
    u16 nhdr;
    u16 status;
    u64 want;
    u64 got;
    u64 body_total;
    u64 body_hash;
    u64 consumed;
    i64 retry_after_ms;
    size_t line_len;
    u64 line_hash;
    char ctype[64];
    char err[96];
} HttpResult;

static u64 hash_bytes(u64 hash, const u8 *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static void body_seen(void *ctx, const u8 *bytes, u64 len)
{
    BodySeen *seen = ctx;

    if (!seen->valid)
        return;
    if (len > YEW_HTTP_MAX_BODY - seen->bytes || len > (u64)SIZE_MAX) {
        seen->valid = false;
        return;
    }
    seen->hash = hash_bytes(seen->hash, bytes, (size_t)len);
    seen->bytes += len;
}

static size_t next_chunk(const u8 *data, size_t len, size_t at,
                         unsigned schedule)
{
    size_t chunk;

    if (schedule == 0U)
        return len - at;
    if (schedule == 1U)
        return 1U;
    chunk = 1U + (size_t)(data[at] & 31U);
    if (chunk > len - at)
        chunk = len - at;
    return chunk;
}

static bool valid_state(u8 state)
{
    return state <= YEW_HX_DEAD;
}

static bool run_http(const u8 *data, size_t len, unsigned schedule,
                     HttpResult *result, char *why, size_t why_cap)
{
    HttpRx rx;
    BodySeen seen;
    size_t at = 0U;

    (void)memset(&seen, 0, sizeof(seen));
    seen.hash = UINT64_C(1469598103934665603);
    seen.valid = true;
    yew_http_rx_init(&rx);
    while (at < len && rx.state != YEW_HX_DONE &&
           rx.state != YEW_HX_DEAD) {
        size_t chunk = next_chunk(data, len, at, schedule);
        u64 consumed;

        consumed = yew_http_rx_feed(&rx, data + at, (u64)chunk,
                                    at + chunk == len, body_seen, &seen);
        if (consumed > (u64)chunk) {
            bytebuf_free(&rx.line);
            return fail(why, why_cap,
                        "HTTP parser consumed beyond the supplied input");
        }
        at += (size_t)consumed;
        if (consumed < (u64)chunk && rx.state != YEW_HX_DONE &&
            rx.state != YEW_HX_DEAD) {
            bytebuf_free(&rx.line);
            return fail(why, why_cap,
                        "HTTP parser stopped before a terminal state");
        }
    }
    if (len == 0U)
        yew_http_rx_feed(&rx, NULL, 0U, true, body_seen, &seen);
    if (!seen.valid) {
        bytebuf_free(&rx.line);
        return fail(why, why_cap, "HTTP parser emitted an oversized body");
    }
    if (!valid_state(rx.state)) {
        bytebuf_free(&rx.line);
        return fail(why, why_cap, "HTTP parser returned an invalid state");
    }
    if (rx.state != YEW_HX_DONE && rx.state != YEW_HX_DEAD) {
        bytebuf_free(&rx.line);
        return fail(why, why_cap, "HTTP parser was not terminal at EOF");
    }
    if (rx.state == YEW_HX_DEAD && rx.err[0] == '\0') {
        bytebuf_free(&rx.line);
        return fail(why, why_cap,
                    "dead HTTP parser state has no diagnostic");
    }
    if (rx.line.len > YEW_HTTP_MAX_HDRLINE ||
        rx.nhdr > YEW_HTTP_MAX_HDRS ||
        rx.body_total > YEW_HTTP_MAX_BODY ||
        seen.bytes != rx.body_total) {
        bytebuf_free(&rx.line);
        return fail(why, why_cap, "HTTP parser exceeded a resource cap");
    }
    if (rx.have_len && !rx.chunked && rx.body_total > rx.want) {
        bytebuf_free(&rx.line);
        return fail(why, why_cap,
                    "HTTP parser emitted more than Content-Length");
    }
    if (rx.state == YEW_HX_DONE && rx.have_len && !rx.chunked &&
        !rx.no_body &&
        rx.body_total != rx.want) {
        bytebuf_free(&rx.line);
        return fail(why, why_cap,
                    "HTTP parser completed before Content-Length");
    }

    (void)memset(result, 0, sizeof(*result));
    result->state = rx.state;
    result->ver_minor = rx.ver_minor;
    result->pending_cr = rx.pending_cr;
    result->have_len = rx.have_len;
    result->bad_len = rx.bad_len;
    result->conflicting_len = rx.conflicting_len;
    result->chunked = rx.chunked;
    result->close_delimited = rx.close_delimited;
    result->no_body = rx.no_body;
    result->nhdr = rx.nhdr;
    result->status = rx.status;
    result->want = rx.want;
    result->got = rx.got;
    result->body_total = rx.body_total;
    result->body_hash = seen.hash;
    result->consumed = (u64)at;
    result->retry_after_ms = rx.retry_after_ms;
    result->line_len = rx.line.len;
    result->line_hash = hash_bytes(UINT64_C(1469598103934665603),
                                   rx.line.data, rx.line.len);
    (void)memcpy(result->ctype, rx.ctype, sizeof(result->ctype));
    (void)memcpy(result->err, rx.err, sizeof(result->err));
    bytebuf_free(&rx.line);
    return true;
}

static bool same_result(const HttpResult *a, const HttpResult *b)
{
    return a->state == b->state &&
           a->ver_minor == b->ver_minor &&
           a->pending_cr == b->pending_cr &&
           a->have_len == b->have_len &&
           a->bad_len == b->bad_len &&
           a->conflicting_len == b->conflicting_len &&
           a->chunked == b->chunked &&
           a->close_delimited == b->close_delimited &&
           a->no_body == b->no_body &&
           a->nhdr == b->nhdr &&
           a->status == b->status &&
           a->want == b->want &&
           a->got == b->got &&
           a->body_total == b->body_total &&
           a->body_hash == b->body_hash &&
           a->consumed == b->consumed &&
           a->retry_after_ms == b->retry_after_ms &&
           a->line_len == b->line_len &&
           a->line_hash == b->line_hash &&
           memcmp(a->ctype, b->ctype, sizeof(a->ctype)) == 0 &&
           memcmp(a->err, b->err, sizeof(a->err)) == 0;
}

static bool check_raw_stream(const u8 *data, size_t len,
                             char *why, size_t why_cap)
{
    HttpResult whole;
    HttpResult bytewise;
    HttpResult varied;

    if (!run_http(data, len, 0U, &whole, why, why_cap) ||
        !run_http(data, len, 1U, &bytewise, why, why_cap) ||
        !run_http(data, len, 2U, &varied, why, why_cap))
        return false;
    if (!same_result(&whole, &bytewise) ||
        !same_result(&whole, &varied))
        return fail(why, why_cap,
                    "HTTP result depends on input chunk boundaries");
    return true;
}

static bool check_valid_body(const u8 *data, size_t len,
                             char *why, size_t why_cap)
{
    Bytebuf response;
    HttpResult result;
    unsigned schedule;
    bool ok = true;
    u64 expected = hash_bytes(UINT64_C(1469598103934665603), data, len);

    bytebuf_init(&response);
    bytebuf_printf(&response,
                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
                   len);
    bytebuf_append(&response, data, len);
    for (schedule = 0U; schedule < 3U; schedule++) {
        if (!run_http(response.data, response.len, schedule,
                      &result, why, why_cap)) {
            ok = false;
            break;
        }
        if (result.state != YEW_HX_DONE || result.status != 200U ||
            result.body_total != (u64)len || result.body_hash != expected) {
            ok = fail(why, why_cap,
                      "valid Content-Length body was not preserved");
            break;
        }
    }
    bytebuf_free(&response);
    return ok;
}

static bool check_valid_no_body(char *why, size_t why_cap)
{
    static const char *const responses[] = {
        "HTTP/1.1 204 No Content\r\nContent-Length: 99\r\n\r\nNEXT",
        "HTTP/1.1 304 Not Modified\r\n\r\nNEXT"
    };
    static const size_t consumed[] = {
        sizeof("HTTP/1.1 204 No Content\r\nContent-Length: 99\r\n\r\n") - 1U,
        sizeof("HTTP/1.1 304 Not Modified\r\n\r\n") - 1U
    };
    size_t i;
    unsigned schedule;

    for (i = 0U; i < YEW_ARRAY_LEN(responses); i++) {
        for (schedule = 0U; schedule < 3U; schedule++) {
            HttpResult result;

            if (!run_http((const u8 *)responses[i], strlen(responses[i]),
                          schedule, &result, why, why_cap))
                return false;
            if (result.state != YEW_HX_DONE || !result.no_body ||
                result.body_total != 0U || result.consumed != consumed[i])
                return fail(why, why_cap,
                            "valid no-body response consumed trailing bytes");
        }
    }
    return true;
}

static bool check_input(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    return check_raw_stream(data, len, why, why_cap) &&
           check_valid_body(data, len, why, why_cap) &&
           check_valid_no_body(why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_http", NULL, check_input);
}
