#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "mod/ai/http.h"

static void assert_bad_url(const char *url, const char *want)
{
    Arena arena;
    HttpUrl parsed;
    char err[256];

    arena_init(&arena);
    (void)memset(&parsed, 0, sizeof(parsed));
    YEW_ASSERT(!yew_http_url_parse(&arena, url, &parsed, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, want);
    arena_free_all(&arena);
}

void test_http_url_parse(void)
{
    Arena arena;
    HttpUrl parsed;
    char err[256];

    arena_init(&arena);
    YEW_ASSERT(yew_http_url_parse(
        &arena, "http://127.0.0.1:11434/api/generate", &parsed, err,
        sizeof(err)));
    YEW_ASSERT_EQ_STR(parsed.host, "127.0.0.1");
    YEW_ASSERT_EQ_U64(parsed.port, 11434U);
    YEW_ASSERT_EQ_STR(parsed.path, "/api/generate");
    YEW_ASSERT(parsed.loopback);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_http_url_parse(&arena, "http://localhost:11434", &parsed,
                                   err, sizeof(err)));
    YEW_ASSERT_EQ_STR(parsed.host, "localhost");
    YEW_ASSERT_EQ_U64(parsed.port, 11434U);
    YEW_ASSERT_EQ_STR(parsed.path, "/");
    YEW_ASSERT(parsed.loopback);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_http_url_parse(&arena, "http://[::1]:11434/x", &parsed,
                                   err, sizeof(err)));
    YEW_ASSERT_EQ_STR(parsed.host, "::1");
    YEW_ASSERT_EQ_U64(parsed.port, 11434U);
    YEW_ASSERT_EQ_STR(parsed.path, "/x");
    YEW_ASSERT(parsed.loopback);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_http_url_parse(&arena, "http://box.lan:8080/v1", &parsed,
                                   err, sizeof(err)));
    YEW_ASSERT_EQ_STR(parsed.host, "box.lan");
    YEW_ASSERT_EQ_U64(parsed.port, 8080U);
    YEW_ASSERT_EQ_STR(parsed.path, "/v1");
    YEW_ASSERT(!parsed.loopback);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_http_url_parse(&arena, "http://example.test?q=1", &parsed,
                                   err, sizeof(err)));
    YEW_ASSERT_EQ_U64(parsed.port, 80U);
    YEW_ASSERT_EQ_STR(parsed.path, "/?q=1");
    arena_free_all(&arena);
}

void test_http_url_rejections(void)
{
    static const char tls[] =
        "yew has no TLS: https backends run through curl.\n"
        "set transport: \"curl\" on this backend, or use a http:// endpoint.";

    assert_bad_url("https://api.anthropic.com/v1/messages", tls);
    assert_bad_url("http://u:p@host/", "bad url: userinfo is not allowed");
    assert_bad_url("unix:/run/ollama.sock",
                   "unix socket endpoints are not a yew 1.0 feature");
    assert_bad_url("http:///x", "bad url");
    assert_bad_url("http://host:0/", "bad url");
    assert_bad_url("http://host:65536/", "bad url");
    assert_bad_url("http://host:no/", "bad url");
    assert_bad_url("http://::1/x", "bad url");
    assert_bad_url("http://[not-ipv6]/x", "bad url");
    assert_bad_url("http://host/x#fragment", "bad url");
    assert_bad_url("ftp://host/x", "bad url");

    YEW_ASSERT_EQ_I64(setenv("YEW_AI_MOCK", "1", 1), 0);
    assert_bad_url("http://example.test/v1",
                   "YEW_AI_MOCK permits loopback endpoints only");
    {
        Arena arena;
        HttpUrl parsed;
        char err[128];

        arena_init(&arena);
        YEW_ASSERT(yew_http_url_parse(&arena, "http://127.0.0.1:9/",
                                       &parsed, err, sizeof(err)));
        YEW_ASSERT(parsed.loopback);
        arena_free_all(&arena);
    }
    YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_MOCK"), 0);
}
