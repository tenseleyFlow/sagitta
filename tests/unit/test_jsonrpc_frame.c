#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/lsp/jsonrpc.h"

typedef struct {
    Bytebuf joined;
    u32 count;
} FrameSeen;

static void remember_frame(void *ctx, const u8 *body, u64 n)
{
    FrameSeen *seen = ctx;

    bytebuf_append(&seen->joined, body, (size_t)n);
    bytebuf_push_u8(&seen->joined, 0U);
    seen->count++;
}

static void make_frame(Bytebuf *wire, const char *headers, const char *body)
{
    bytebuf_printf(wire, "Content-Length: %zu\r\n%s\r\n%s", strlen(body),
                   headers, body);
}

static void make_frame_n(Bytebuf *wire, const char *headers, const u8 *body,
                         size_t n)
{
    bytebuf_printf(wire, "Content-Length: %zu\r\n%s\r\n", n, headers);
    bytebuf_append(wire, body, n);
}

static void remember_expected(Bytebuf *expected, const u8 *body, size_t n)
{
    bytebuf_append(expected, body, n);
    bytebuf_push_u8(expected, 0U);
}

static void assert_dead(const char *wire, const char *error)
{
    RpcRx rx;

    yew_rpcrx_init(&rx);
    (void)yew_rpcrx_feed(&rx, (const u8 *)wire, strlen(wire), NULL, NULL);
    YEW_ASSERT_EQ_U64(rx.state, YEW_RPCRX_DEAD);
    YEW_ASSERT_EQ_STR(rx.err, error);
    yew_rpcrx_free(&rx);
}

void test_jsonrpc_frame_every_split(void)
{
    static const char body1[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"one\",\"params\":{}}";
    static const char body2[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"caf\xC3\xA9\"}";
    Bytebuf wire;
    Bytebuf expected;
    Bytebuf body;
    size_t split;
    u32 i;

    bytebuf_init(&wire);
    bytebuf_init(&expected);
    make_frame(&wire,
               "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n",
               body1);
    remember_expected(&expected, (const u8 *)body1, strlen(body1));
    bytebuf_printf(&wire, "X-Test: yes\r\ncontent-length: %zu\r\n\r\n%s",
                   strlen(body2), body2);
    remember_expected(&expected, (const u8 *)body2, strlen(body2));
    for (i = 0U; i < 9U; i++) {
        bytebuf_init(&body);
        bytebuf_printf(&body,
                       "{\"jsonrpc\":\"2.0\",\"method\":\"method-%u\"}", i);
        make_frame_n(&wire, "", body.data, body.len);
        remember_expected(&expected, body.data, body.len);
        bytebuf_free(&body);
    }
    bytebuf_init(&body);
    bytebuf_append(&body,
                   "{\"jsonrpc\":\"2.0\",\"method\":\"pad\",\"params\":\"",
                   sizeof("{\"jsonrpc\":\"2.0\",\"method\":\"pad\",\"params\":\"") - 1U);
    for (i = 0U; i < 40000U; i++)
        bytebuf_push_u8(&body, (u8)'a');
    bytebuf_append(&body, "\"}", 2U);
    make_frame_n(&wire, "", body.data, body.len);
    remember_expected(&expected, body.data, body.len);
    bytebuf_free(&body);
    YEW_ASSERT(wire.len > 40000U);
    for (split = 0U; split <= wire.len; split++) {
        RpcRx rx;
        FrameSeen seen;

        yew_rpcrx_init(&rx);
        bytebuf_init(&seen.joined);
        seen.count = 0U;
        (void)yew_rpcrx_feed(&rx, wire.data, split, remember_frame, &seen);
        (void)yew_rpcrx_feed(&rx, wire.data + split,
                             (u64)(wire.len - split), remember_frame, &seen);
        YEW_ASSERT_EQ_U64(rx.state, YEW_RPCRX_HDR);
        YEW_ASSERT_EQ_U64(seen.count, 12U);
        YEW_ASSERT_EQ_U64(rx.msgs_in, 12U);
        YEW_ASSERT_EQ_U64(rx.bytes_in, wire.len);
        YEW_ASSERT_EQ_U64(seen.joined.len, expected.len);
        YEW_ASSERT_EQ_MEM(seen.joined.data, expected.data, expected.len);
        bytebuf_free(&seen.joined);
        yew_rpcrx_free(&rx);
    }
    bytebuf_free(&expected);
    bytebuf_free(&wire);
}

void test_jsonrpc_frame_one_byte_chunks(void)
{
    const char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":\"\xE2\x98\x83\"}";
    Bytebuf wire;
    RpcRx rx;
    FrameSeen seen;
    size_t i;

    bytebuf_init(&wire);
    make_frame(&wire, "", body);
    yew_rpcrx_init(&rx);
    bytebuf_init(&seen.joined);
    seen.count = 0U;
    for (i = 0U; i < wire.len; i++)
        (void)yew_rpcrx_feed(&rx, wire.data + i, 1U, remember_frame, &seen);
    YEW_ASSERT_EQ_U64(seen.count, 1U);
    YEW_ASSERT_EQ_MEM(seen.joined.data, body, strlen(body));
    YEW_ASSERT_EQ_U64(rx.state, YEW_RPCRX_HDR);
    bytebuf_free(&seen.joined);
    yew_rpcrx_free(&rx);
    bytebuf_free(&wire);
}

void test_jsonrpc_frame_errors(void)
{
    char huge[4300];
    RpcRx rx;
    Bytebuf wire;
    u32 i;

    assert_dead("X: y\r\n\r\n", "frame has no Content-Length");
    assert_dead("Content-Length: nope\r\n\r\n", "bad Content-Length 'nope'");
    assert_dead("Content-Length: 67108865\r\n\r\n",
                "bad Content-Length '67108865'");
    assert_dead("Content-Length: 1\rX", "header line must end with CRLF");

    (void)memset(huge, 'a', sizeof huge);
    yew_rpcrx_init(&rx);
    (void)yew_rpcrx_feed(&rx, (const u8 *)huge, sizeof huge, NULL, NULL);
    YEW_ASSERT_EQ_STR(rx.err, "header line too long");
    yew_rpcrx_free(&rx);

    bytebuf_init(&wire);
    for (i = 0U; i < 33U; i++)
        bytebuf_printf(&wire, "X-%u: yes\r\n", i);
    bytebuf_append(&wire, "Content-Length: 2\r\n\r\n{}",
                   sizeof("Content-Length: 2\r\n\r\n{}") - 1U);
    yew_rpcrx_init(&rx);
    (void)yew_rpcrx_feed(&rx, wire.data, wire.len, NULL, NULL);
    YEW_ASSERT_EQ_STR(rx.err, "too many headers");
    yew_rpcrx_free(&rx);
    bytebuf_free(&wire);
}

void test_jsonrpc_frame_rejects_invalid_body(void)
{
    static const u8 bad_utf8[] = {
        'C','o','n','t','e','n','t','-','L','e','n','g','t','h',':',' ','1',
        '\r','\n','\r','\n',0xff
    };
    RpcRx rx;

    yew_rpcrx_init(&rx);
    (void)yew_rpcrx_feed(&rx, bad_utf8, sizeof bad_utf8, NULL, NULL);
    YEW_ASSERT_EQ_STR(rx.err, "body is not UTF-8 at offset 0");
    yew_rpcrx_free(&rx);
    assert_dead("Content-Length: 1\r\n\r\n5",
                "frame is not a JSON-RPC object");
    assert_dead("Content-Length: 0\r\n\r\n",
                "frame is not a JSON-RPC object");
}

void test_jsonrpc_frame_tx_and_eof(void)
{
    RpcConn c;
    const u8 *view;
    u64 n;

    yew_rpc_conn_init(&c);
    yew_rpctx_send(&c.tx, (const u8 *)"{}", 2U);
    n = yew_rpc_tx_view(&c, &view);
    YEW_ASSERT_EQ_U64(n, 23U);
    YEW_ASSERT_EQ_MEM(view, "Content-Length: 2\r\n\r\n{}", 23U);
    yew_rpc_tx_consume(&c, 5U);
    YEW_ASSERT_EQ_U64(yew_rpc_tx_view(&c, &view), 18U);
    yew_rpc_tx_consume(&c, 100U);
    YEW_ASSERT_EQ_U64(yew_rpc_tx_view(&c, &view), 0U);
    YEW_ASSERT(yew_rpc_finish_stdout(&c));
    YEW_ASSERT(yew_rpc_feed_stdout(&c, (const u8 *)"Content-Length: 2\r\n", 19U));
    YEW_ASSERT(!yew_rpc_finish_stdout(&c));
    YEW_ASSERT_EQ_STR(c.rx.err, "unexpected EOF in JSON-RPC frame");
    yew_rpc_conn_free(&c);
}
