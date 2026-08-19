#include "harness.h"

#include <string.h>

#include "mod/ai/http.h"

static HttpUrl request_url(const char *host, u16 port, const char *path)
{
    HttpUrl url;

    url.host = host;
    url.port = port;
    url.path = path;
    url.loopback = true;
    return url;
}

static void assert_request(const HttpUrl *url, const HttpReq *req,
                           const u8 *want, size_t want_len)
{
    Bytebuf out;
    char err[128];

    bytebuf_init(&out);
    YEW_ASSERT(yew_http_req_build(&out, url, req, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(out.len, want_len);
    YEW_ASSERT_EQ_MEM(out.data, want, want_len);
    bytebuf_free(&out);
}

void test_http_req_golden(void)
{
    static const u8 body[] = "{\"model\":\"qwen\"}";
    static const HttpHdr headers[] = {
        {"Authorization", "Bearer secret"},
        {"X-Trace", "fixed"}
    };
    static const u8 want[] =
        "POST /v1/chat/completions HTTP/1.1\r\n"
        "Host: 127.0.0.1:11434\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 16\r\n"
        "Connection: keep-alive\r\n"
        "Authorization: Bearer secret\r\n"
        "X-Trace: fixed\r\n"
        "\r\n"
        "{\"model\":\"qwen\"}";
    HttpUrl url = request_url("127.0.0.1", 11434U, "/unused");
    HttpReq req;

    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.hdrs = headers;
    req.nhdr = YEW_ARRAY_LEN(headers);
    req.body = body;
    req.blen = sizeof(body) - 1U;
    req.keepalive = true;
    assert_request(&url, &req, want, sizeof(want) - 1U);
}

void test_http_req_adapter_goldens(void)
{
    static const u8 body[] = "{}";
    static const u8 ollama[] =
        "POST /api/generate HTTP/1.1\r\n"
        "Host: localhost:11434\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "{}";
    static const HttpHdr anthropic_headers[] = {
        {"x-api-key", "secret"},
        {"anthropic-version", "2023-06-01"}
    };
    static const u8 anthropic[] =
        "POST /v1/messages HTTP/1.1\r\n"
        "Host: api.anthropic.com\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "x-api-key: secret\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "\r\n"
        "{}";
    HttpUrl url;
    HttpReq req;

    url = request_url("localhost", 11434U, "/api/generate");
    req.method = "POST";
    req.path = NULL;
    req.hdrs = NULL;
    req.nhdr = 0U;
    req.body = body;
    req.blen = sizeof(body) - 1U;
    req.keepalive = true;
    assert_request(&url, &req, ollama, sizeof(ollama) - 1U);

    url = request_url("api.anthropic.com", 80U, "/v1/messages");
    req.hdrs = anthropic_headers;
    req.nhdr = YEW_ARRAY_LEN(anthropic_headers);
    req.keepalive = false;
    assert_request(&url, &req, anthropic, sizeof(anthropic) - 1U);
}

void test_http_req_host_and_binary_body(void)
{
    static const u8 body[] = {0U, 0xffU, (u8)'x'};
    static const u8 prefix[] =
        "POST /x HTTP/1.1\r\n"
        "Host: [::1]:8080\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n";
    HttpUrl url = request_url("::1", 8080U, "/x");
    static const HttpHdr empty_header[] = {{"X-Empty", ""}};
    static const u8 empty_want[] =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "X-Empty: \r\n"
        "\r\n";
    HttpReq req;
    Bytebuf want;

    req.method = "POST";
    req.path = NULL;
    req.hdrs = NULL;
    req.nhdr = 0U;
    req.body = body;
    req.blen = sizeof(body);
    req.keepalive = false;
    bytebuf_init(&want);
    bytebuf_append(&want, prefix, sizeof(prefix) - 1U);
    bytebuf_append(&want, body, sizeof(body));
    assert_request(&url, &req, want.data, want.len);
    bytebuf_free(&want);

    url = request_url("localhost", 80U, "/");
    req.method = "GET";
    req.hdrs = empty_header;
    req.nhdr = YEW_ARRAY_LEN(empty_header);
    req.body = NULL;
    req.blen = 0U;
    assert_request(&url, &req, empty_want, sizeof(empty_want) - 1U);
}

static void assert_bad_header(const char *name, const char *value)
{
    HttpUrl url = request_url("localhost", 80U, "/");
    HttpHdr header;
    HttpReq req;
    Bytebuf out;
    char err[128];

    header.name = name;
    header.value = value;
    req.method = "GET";
    req.path = "/";
    req.hdrs = &header;
    req.nhdr = 1U;
    req.body = NULL;
    req.blen = 0U;
    req.keepalive = false;
    bytebuf_init(&out);
    bytebuf_append(&out, "sentinel", 8U);
    YEW_ASSERT(!yew_http_req_build(&out, &url, &req, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, "bad http header");
    YEW_ASSERT_EQ_U64(out.len, 8U);
    YEW_ASSERT_EQ_MEM(out.data, "sentinel", 8U);
    bytebuf_free(&out);
}

void test_http_req_injection_rejected(void)
{
    HttpUrl url = request_url("localhost", 80U, "/");
    HttpReq req;
    Bytebuf out;
    char err[128];

    assert_bad_header("X-Test\rBad", "x");
    assert_bad_header("X-Test\nBad", "x");
    assert_bad_header("X Test", "x");
    assert_bad_header("X-Test", "x\rBad: yes");
    assert_bad_header("X-Test", "x\nBad: yes");
    assert_bad_header("", "x");
    assert_bad_header(NULL, "x");
    assert_bad_header("X-Test", NULL);

    req.method = "GE\nT";
    req.path = "/";
    req.hdrs = NULL;
    req.nhdr = 0U;
    req.body = NULL;
    req.blen = 0U;
    req.keepalive = false;
    bytebuf_init(&out);
    YEW_ASSERT(!yew_http_req_build(&out, &url, &req, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, "bad http request");
    req.method = "GET";
    req.path = "/x\r\nInjected: yes";
    YEW_ASSERT(!yew_http_req_build(&out, &url, &req, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(out.len, 0U);
    bytebuf_free(&out);
}
