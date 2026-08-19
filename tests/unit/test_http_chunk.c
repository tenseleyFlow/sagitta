#include "harness.h"

#include <string.h>

#include "mod/ai/http.h"
#include "util/buf.h"

typedef struct ChunkCapture {
    Bytebuf body;
} ChunkCapture;

static void chunk_capture(void *ctx, const u8 *bytes, u64 n)
{
    ChunkCapture *capture = ctx;

    bytebuf_append(&capture->body, bytes, (size_t)n);
}

static void feed_chunk_size(const char *size, u8 state, u64 want,
                            const char *error)
{
    static const char prefix[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    HttpRx rx;
    Bytebuf response;

    bytebuf_init(&response);
    bytebuf_append(&response, prefix, sizeof(prefix) - 1U);
    bytebuf_append(&response, size, strlen(size));
    bytebuf_append(&response, "\r\n", 2U);
    if (state == YEW_HX_DONE)
        bytebuf_append(&response, "\r\n", 2U);
    yew_http_rx_init(&rx);
    yew_http_rx_feed(&rx, response.data, response.len, false, NULL, NULL);
    YEW_ASSERT_EQ_U64(rx.state, state);
    YEW_ASSERT_EQ_U64(rx.want, want);
    YEW_ASSERT_EQ_STR(rx.err, error);
    bytebuf_free(&rx.line);
    bytebuf_free(&response);
}

void test_http_chunk_size_table(void)
{
    feed_chunk_size("a3", YEW_HX_CHUNK_DATA, 0xa3U, "");
    feed_chunk_size("A3", YEW_HX_CHUNK_DATA, 0xa3U, "");
    feed_chunk_size("a3;foo=\"9\"", YEW_HX_CHUNK_DATA, 0xa3U, "");
    feed_chunk_size("0", YEW_HX_DONE, 0U, "");
    feed_chunk_size("1000000", YEW_HX_CHUNK_DATA, YEW_HTTP_MAX_CHUNK, "");
    feed_chunk_size("", YEW_HX_DEAD, 0U, "bad chunk size");
    feed_chunk_size("g", YEW_HX_DEAD, 0U, "bad chunk size");
    feed_chunk_size("1000001", YEW_HX_DEAD, 0U, "bad chunk size");
    feed_chunk_size("10000000000000000", YEW_HX_DEAD, 0U,
                    "bad chunk size");
}

static void assert_chunked_split(const u8 *response, size_t response_len,
                                 size_t split)
{
    static const u8 body[] = {(u8)'A', 0U, 0xffU, (u8)'B', (u8)'z'};
    HttpRx rx;
    ChunkCapture capture;

    yew_http_rx_init(&rx);
    bytebuf_init(&capture.body);
    yew_http_rx_feed(&rx, response, split, false, chunk_capture, &capture);
    yew_http_rx_feed(&rx, response + split, response_len - split, true,
                     chunk_capture, &capture);
    YEW_ASSERT_EQ_U64(rx.state, YEW_HX_DONE);
    YEW_ASSERT_EQ_U64(capture.body.len, sizeof(body));
    YEW_ASSERT_EQ_MEM(capture.body.data, body, sizeof(body));
    bytebuf_free(&capture.body);
    bytebuf_free(&rx.line);
}

void test_http_chunk_boundaries(void)
{
    static const u8 response[] = {
        'H','T','T','P','/','1','.','1',' ','2','0','0',' ','O','K','\r','\n',
        'T','r','a','n','s','f','e','r','-','E','n','c','o','d','i','n','g',':',
        ' ','c','h','u','n','k','e','d','\r','\n','\r','\n',
        '4','\r','\n','A',0U,0xffU,'B','\r','\n',
        '1','\r','\n','z','\r','\n','0','\r','\n',
        'X','-','T','r','a','i','l','e','r',':',' ','x','\r','\n','\r','\n'
    };
    HttpRx rx;
    ChunkCapture capture;
    size_t split;
    size_t i;

    for (split = 0U; split <= sizeof(response); split++)
        assert_chunked_split(response, sizeof(response), split);

    yew_http_rx_init(&rx);
    bytebuf_init(&capture.body);
    for (i = 0U; i < sizeof(response); i++)
        yew_http_rx_feed(&rx, response + i, 1U, false, chunk_capture,
                         &capture);
    yew_http_rx_feed(&rx, NULL, 0U, true, chunk_capture, &capture);
    YEW_ASSERT_EQ_U64(rx.state, YEW_HX_DONE);
    YEW_ASSERT_EQ_MEM(capture.body.data, "A\0\xff" "Bz", 5U);
    bytebuf_free(&capture.body);
    bytebuf_free(&rx.line);
}
