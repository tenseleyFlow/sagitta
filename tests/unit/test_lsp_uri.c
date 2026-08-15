#include "harness.h"

#include <string.h>

#include "mod/lsp/client.h"

static void round_trip(const u8 *path, u32 len)
{
    Bytebuf uri;
    Bytebuf decoded;

    bytebuf_init(&uri);
    bytebuf_init(&decoded);
    yew_lsp_uri_of_path(&uri, path, len);
    YEW_ASSERT(yew_lsp_path_of_uri(&decoded, uri.data, (u32)uri.len));
    YEW_ASSERT_EQ_U64(decoded.len, len);
    YEW_ASSERT_EQ_MEM(decoded.data, path, len);
    bytebuf_free(&decoded);
    bytebuf_free(&uri);
}

void test_lsp_uri_round_trips_path_bytes(void)
{
    static const char *const paths[] = {
        "/", "/tmp/a", "/tmp/a b", "/tmp/a#b", "/tmp/a?b",
        "/tmp/a%b", "/tmp/a+b", "/tmp/a:b", "/tmp/a;b",
        "/tmp/a=b", "/tmp/a@b", "/tmp/a&b", "/tmp/a'b",
        "/tmp/a(b", "/tmp/a)b", "/tmp/a,b", "/tmp/a!b",
        "/tmp/a$b", "/tmp/a*b", "/tmp/a\nb", "/tmp/~ok",
        "/tmp/-._", "/tmp/caf\xC3\xA9", "/tmp/\xF0\x9F\x8C\xB2",
        "/tmp/two/slashes"
    };
    static const u8 invalid[] = {'/', 't', 'm', 'p', '/', 0xff, 'x'};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(paths); i++)
        round_trip((const u8 *)paths[i], (u32)strlen(paths[i]));
    round_trip(invalid, (u32)sizeof(invalid));
}

void test_lsp_uri_encoding_is_canonical(void)
{
    static const u8 path[] = "/a b/#?%\n\xC3\xA9\xFF";
    static const char expected[] =
        "file:///a%20b/%23%3F%25%0A%C3%A9%FF";
    Bytebuf uri;
    Bytebuf decoded;

    bytebuf_init(&uri);
    bytebuf_init(&decoded);
    yew_lsp_uri_of_path(&uri, path, (u32)(sizeof(path) - 1U));
    YEW_ASSERT_EQ_U64(uri.len, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_MEM(uri.data, expected, sizeof(expected) - 1U);
    YEW_ASSERT(yew_lsp_path_of_uri(
        &decoded, (const u8 *)"file:///a%2fb%2Fc", 17U));
    YEW_ASSERT_EQ_MEM(decoded.data, "/a/b/c", 6U);
    bytebuf_free(&decoded);
    bytebuf_free(&uri);
}

void test_lsp_uri_refuses_non_file_and_bad_escapes(void)
{
    static const char *const bad[] = {
        "untitled:/x", "jdt://x", "http:///x", "file://host/x",
        "file:///x%", "file:///x%0", "file:///x%GG", "file://"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(bad); i++) {
        Bytebuf out;

        bytebuf_init(&out);
        YEW_ASSERT(!yew_lsp_path_of_uri(
            &out, (const u8 *)bad[i], (u32)strlen(bad[i])));
        bytebuf_free(&out);
    }
}
