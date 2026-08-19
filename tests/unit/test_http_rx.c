#include "harness.h"

#include <string.h>

#include "mod/ai/http.h"
#include "util/buf.h"

typedef struct RxCapture {
    Bytebuf body;
} RxCapture;

typedef struct RxCase {
    Bytebuf response;
    Bytebuf body;
    const char *ctype;
    const char *error;
    i64 retry_ms;
    u16 status;
    u8 state;
} RxCase;

static void capture_body(void *ctx, const u8 *bytes, u64 n)
{
    RxCapture *capture = ctx;

    bytebuf_append(&capture->body, bytes, (size_t)n);
}

static void rx_case_init(RxCase *test, u16 status, u8 state,
                         const char *error)
{
    bytebuf_init(&test->response);
    bytebuf_init(&test->body);
    test->ctype = "";
    test->error = error;
    test->retry_ms = -1;
    test->status = status;
    test->state = state;
}

static void rx_case_free(RxCase *test)
{
    bytebuf_free(&test->response);
    bytebuf_free(&test->body);
}

static void append_pattern(Bytebuf *body, size_t n, u8 salt)
{
    size_t i;

    for (i = 0U; i < n; i++)
        bytebuf_push_u8(body, (u8)(i * 37U + salt));
}

static void assert_split_expectation(const RxCase *test)
{
    size_t split;

    for (split = 0U; split <= test->response.len; split++) {
        HttpRx rx;
        RxCapture capture;

        yew_http_rx_init(&rx);
        bytebuf_init(&capture.body);
        yew_http_rx_feed(&rx, test->response.data, split, false,
                         capture_body, &capture);
        yew_http_rx_feed(&rx, test->response.data + split,
                         test->response.len - split, true, capture_body,
                         &capture);
        YEW_ASSERT_EQ_U64(rx.state, test->state);
        YEW_ASSERT_EQ_U64(rx.status, test->status);
        YEW_ASSERT_EQ_U64(capture.body.len, test->body.len);
        YEW_ASSERT_EQ_MEM(capture.body.data, test->body.data,
                          test->body.len);
        YEW_ASSERT_EQ_STR(rx.ctype, test->ctype);
        YEW_ASSERT_EQ_I64(rx.retry_after_ms, test->retry_ms);
        YEW_ASSERT_EQ_STR(rx.err, test->error);
        bytebuf_free(&capture.body);
        bytebuf_free(&rx.line);
    }
}

static void assert_split_case(const u8 *response, size_t response_len,
                              u16 status, const u8 *body, size_t body_len,
                              const char *ctype, i64 retry_ms)
{
    size_t split;

    for (split = 0U; split <= response_len; split++) {
        HttpRx rx;
        RxCapture capture;

        yew_http_rx_init(&rx);
        bytebuf_init(&capture.body);
        yew_http_rx_feed(&rx, response, split, false, capture_body,
                         &capture);
        yew_http_rx_feed(&rx, response + split, response_len - split, true,
                         capture_body, &capture);
        YEW_ASSERT_EQ_U64(rx.state, YEW_HX_DONE);
        YEW_ASSERT_EQ_U64(rx.status, status);
        YEW_ASSERT_EQ_U64(capture.body.len, body_len);
        YEW_ASSERT_EQ_MEM(capture.body.data, body, body_len);
        YEW_ASSERT_EQ_STR(rx.ctype, ctype);
        YEW_ASSERT_EQ_I64(rx.retry_after_ms, retry_ms);
        bytebuf_free(&capture.body);
        bytebuf_free(&rx.line);
    }
}

void test_http_rx_framing_and_splits(void)
{
    static const char length[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Type: Application/JSON; charset=utf-8\r\n\r\nhello";
    static const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
        "5;token=1\r\nhello\r\n0\r\nX-Trailer: yes\r\n\r\n";
    static const char interim[] =
        "HTTP/1.1 103 Early Hints\r\nLink: </model>\r\n\r\n"
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok";
    static const char no_content[] =
        "HTTP/1.1 204 No Content\r\nContent-Length: 99\r\n\r\n";
    static const char not_modified[] =
        "HTTP/1.1 304 Not Modified\r\n\r\n";
    static const char by_close[] =
        "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nclose body";
    static const char unauthorized[] =
        "HTTP/1.1 401 Nope\r\nContent-Length: 14\r\n\r\n{\"error\":true}";
    static const char retry_delta[] =
        "HTTP/1.1 429 Slow\r\nContent-Length: 0\r\nRetry-After: 21\r\n\r\n";
    static const char retry_date[] =
        "HTTP/1.1 429 Slow\r\nContent-Length: 0\r\n"
        "Retry-After: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n";
    static const char duplicate_equal[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
        "content-length:\t3 \r\n\r\nabc";
    static const char chunked_over_len[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n"
        "Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n"
        "2\r\nok\r\n0\r\n\r\n";

    assert_split_case((const u8 *)length, sizeof(length) - 1U, 200U,
                      (const u8 *)"hello", 5U, "application/json", -1);
    assert_split_case((const u8 *)chunked, sizeof(chunked) - 1U, 200U,
                      (const u8 *)"hello", 5U, "", -1);
    assert_split_case((const u8 *)interim, sizeof(interim) - 1U, 200U,
                      (const u8 *)"ok", 2U, "", -1);
    assert_split_case((const u8 *)no_content, sizeof(no_content) - 1U,
                      204U, NULL, 0U, "", -1);
    assert_split_case((const u8 *)not_modified, sizeof(not_modified) - 1U,
                      304U, NULL, 0U, "", -1);
    assert_split_case((const u8 *)by_close, sizeof(by_close) - 1U, 200U,
                      (const u8 *)"close body", 10U, "text/plain", -1);
    assert_split_case((const u8 *)unauthorized, sizeof(unauthorized) - 1U,
                      401U, (const u8 *)"{\"error\":true}", 14U, "", -1);
    assert_split_case((const u8 *)retry_delta, sizeof(retry_delta) - 1U,
                      429U, NULL, 0U, "", 21000);
    assert_split_case((const u8 *)retry_date, sizeof(retry_date) - 1U,
                      429U, NULL, 0U, "", 0);
    assert_split_case((const u8 *)duplicate_equal,
                      sizeof(duplicate_equal) - 1U, 200U,
                      (const u8 *)"abc", 3U, "", -1);
    assert_split_case((const u8 *)chunked_over_len,
                      sizeof(chunked_over_len) - 1U, 200U,
                      (const u8 *)"ok", 2U, "", -1);
}

void test_http_rx_large_split_corpus(void)
{
    RxCase cases[20];
    u64 split_cases = 0U;
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        rx_case_init(&cases[i], 200U, YEW_HX_DONE, "");

    append_pattern(&cases[0].body, 8000U, 1U);
    cases[0].ctype = "application/json";
    bytebuf_printf(&cases[0].response,
                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                   "Content-Type: Application/JSON; charset=utf-8\r\n\r\n",
                   cases[0].body.len);
    bytebuf_append(&cases[0].response, cases[0].body.data,
                   cases[0].body.len);

    append_pattern(&cases[1].body, 8000U, 2U);
    bytebuf_append(&cases[1].response,
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                   sizeof("HTTP/1.1 200 OK\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n") - 1U);
    bytebuf_printf(&cases[1].response, "%zx\r\n", cases[1].body.len);
    bytebuf_append(&cases[1].response, cases[1].body.data,
                   cases[1].body.len);
    bytebuf_append(&cases[1].response, "\r\n0\r\n\r\n",
                   sizeof("\r\n0\r\n\r\n") - 1U);

    append_pattern(&cases[2].body, 8000U, 3U);
    bytebuf_append(&cases[2].response,
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                   sizeof("HTTP/1.1 200 OK\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n") - 1U);
    bytebuf_printf(&cases[2].response, "%zx;token=\"9\"\r\n",
                   cases[2].body.len);
    bytebuf_append(&cases[2].response, cases[2].body.data,
                   cases[2].body.len);
    bytebuf_append(&cases[2].response, "\r\n0\r\n\r\n",
                   sizeof("\r\n0\r\n\r\n") - 1U);

    append_pattern(&cases[3].body, 8000U, 4U);
    bytebuf_append(&cases[3].response,
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                   sizeof("HTTP/1.1 200 OK\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n") - 1U);
    bytebuf_printf(&cases[3].response, "%zx\r\n", cases[3].body.len);
    bytebuf_append(&cases[3].response, cases[3].body.data,
                   cases[3].body.len);
    bytebuf_append(&cases[3].response,
                   "\r\n0\r\nX-Usage: 17\r\nX-Model: local\r\n\r\n",
                   sizeof("\r\n0\r\nX-Usage: 17\r\n"
                          "X-Model: local\r\n\r\n") - 1U);

    append_pattern(&cases[4].body, 8000U, 5U);
    bytebuf_printf(&cases[4].response,
                   "HTTP/1.1 100 Continue\r\nX-Interim: yes\r\n\r\n"
                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
                   cases[4].body.len);
    bytebuf_append(&cases[4].response, cases[4].body.data,
                   cases[4].body.len);

    cases[5].status = 204U;
    bytebuf_append(&cases[5].response,
                   "HTTP/1.1 204 No Content\r\nContent-Length: 99\r\n\r\n",
                   sizeof("HTTP/1.1 204 No Content\r\n"
                          "Content-Length: 99\r\n\r\n") - 1U);

    cases[6].status = 304U;
    bytebuf_append(&cases[6].response,
                   "HTTP/1.1 304 Not Modified\r\n\r\n",
                   sizeof("HTTP/1.1 304 Not Modified\r\n\r\n") - 1U);

    append_pattern(&cases[7].body, 8000U, 8U);
    cases[7].ctype = "application/octet-stream";
    bytebuf_append(&cases[7].response,
                   "HTTP/1.0 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n\r\n",
                   sizeof("HTTP/1.0 200 OK\r\n"
                          "Content-Type: application/octet-stream\r\n\r\n") -
                       1U);
    bytebuf_append(&cases[7].response, cases[7].body.data,
                   cases[7].body.len);

    cases[8].status = 401U;
    bytebuf_append(&cases[8].body, "{\"error\":\"",
                   sizeof("{\"error\":\"") - 1U);
    for (i = 0U; i < 7988U; i++)
        bytebuf_push_u8(&cases[8].body, (u8)'x');
    bytebuf_append(&cases[8].body, "\"}", 2U);
    bytebuf_printf(&cases[8].response,
                   "HTTP/1.1 401 Unauthorized\r\nContent-Length: %zu\r\n\r\n",
                   cases[8].body.len);
    bytebuf_append(&cases[8].response, cases[8].body.data,
                   cases[8].body.len);

    cases[9].status = 429U;
    cases[9].retry_ms = 21000;
    bytebuf_append(&cases[9].response,
                   "HTTP/1.1 429 Slow\r\nContent-Length: 0\r\n"
                   "Retry-After: 21\r\n\r\n",
                   sizeof("HTTP/1.1 429 Slow\r\nContent-Length: 0\r\n"
                          "Retry-After: 21\r\n\r\n") - 1U);

    cases[10].status = 429U;
    cases[10].retry_ms = 0;
    bytebuf_append(&cases[10].response,
                   "HTTP/1.1 429 Slow\r\nContent-Length: 0\r\n"
                   "Retry-After: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n",
                   sizeof("HTTP/1.1 429 Slow\r\nContent-Length: 0\r\n"
                          "Retry-After: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
                          "\r\n") - 1U);

    cases[11].state = YEW_HX_DEAD;
    cases[11].error = "conflicting Content-Length";
    bytebuf_append(&cases[11].response,
                   "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
                   "Content-Length: 2\r\n\r\nx",
                   sizeof("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
                          "Content-Length: 2\r\n\r\nx") - 1U);

    cases[12].state = YEW_HX_DEAD;
    cases[12].error = "obsolete header folding";
    bytebuf_append(&cases[12].response,
                   "HTTP/1.1 200 OK\r\n X-Folded: bad\r\n\r\n",
                   sizeof("HTTP/1.1 200 OK\r\n X-Folded: bad\r\n\r\n") - 1U);

    cases[13].state = YEW_HX_DEAD;
    cases[13].error = "header line too long";
    bytebuf_append(&cases[13].response, "HTTP/1.1 200 OK\r\nX: ",
                   sizeof("HTTP/1.1 200 OK\r\nX: ") - 1U);
    for (i = 0U; i < YEW_HTTP_MAX_HDRLINE - 2U; i++)
        bytebuf_push_u8(&cases[13].response, (u8)'x');
    bytebuf_append(&cases[13].response, "\r\n\r\n", 4U);

    cases[14].state = YEW_HX_DEAD;
    cases[14].error = "too many headers";
    bytebuf_append(&cases[14].response, "HTTP/1.1 200 OK\r\n",
                   sizeof("HTTP/1.1 200 OK\r\n") - 1U);
    for (i = 0U; i < YEW_HTTP_MAX_HDRS + 1U; i++)
        bytebuf_printf(&cases[14].response, "X-%u: x\r\n", i);
    bytebuf_append(&cases[14].response, "\r\n", 2U);

    cases[15].state = YEW_HX_DEAD;
    cases[15].error = "response too large";
    bytebuf_append(&cases[15].response,
                   "HTTP/1.1 200 OK\r\nContent-Length: 67108865\r\n\r\n",
                   sizeof("HTTP/1.1 200 OK\r\n"
                          "Content-Length: 67108865\r\n\r\n") - 1U);

    bytebuf_append(&cases[16].body, "same", 4U);
    bytebuf_append(&cases[16].response,
                   "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                   "content-length:\t4 \r\n\r\nsame",
                   sizeof("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
                          "content-length:\t4 \r\n\r\nsame") - 1U);

    bytebuf_append(&cases[17].response,
                   "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
                   sizeof("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") -
                       1U);

    cases[18].status = 0U;
    cases[18].state = YEW_HX_DEAD;
    cases[18].error = "bad status line";
    bytebuf_append(&cases[18].response, "HTTP/2 200 OK\r\n\r\n",
                   sizeof("HTTP/2 200 OK\r\n\r\n") - 1U);

    cases[19].state = YEW_HX_DEAD;
    cases[19].error = "response ended early";
    bytebuf_append(&cases[19].body, "abc", 3U);
    bytebuf_append(&cases[19].response,
                   "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc",
                   sizeof("HTTP/1.1 200 OK\r\n"
                          "Content-Length: 4\r\n\r\nabc") - 1U);

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        split_cases += cases[i].response.len + 1U;
        assert_split_expectation(&cases[i]);
        rx_case_free(&cases[i]);
    }
    YEW_ASSERT(split_cases >= 60000U);
}

static void assert_rx_error(const u8 *response, size_t response_len,
                            const char *error)
{
    HttpRx rx;
    RxCapture capture;

    yew_http_rx_init(&rx);
    bytebuf_init(&capture.body);
    yew_http_rx_feed(&rx, response, response_len, true, capture_body,
                     &capture);
    YEW_ASSERT_EQ_U64(rx.state, YEW_HX_DEAD);
    YEW_ASSERT_EQ_STR(rx.err, error);
    bytebuf_free(&capture.body);
    bytebuf_free(&rx.line);
}

void test_http_rx_rejections(void)
{
    static const char bad_status[] = "HTTP/2 200 OK\r\n\r\n";
    static const char conflict[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
        "Content-Length: 2\r\n\r\nx";
    static const char bad_length[] =
        "HTTP/1.1 200 OK\r\nContent-Length: +1\r\n\r\nx";
    static const char obs_fold[] =
        "HTTP/1.1 200 OK\r\n X: folded\r\n\r\n";
    static const char truncated[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc";
    static const char bare_lf[] = "HTTP/1.1 200 OK\n\n";
    static const u8 nul_header[] =
        "HTTP/1.1 200 OK\r\nX-Bad: x\0y\r\n\r\n";

    assert_rx_error((const u8 *)bad_status, sizeof(bad_status) - 1U,
                    "bad status line");
    assert_rx_error((const u8 *)conflict, sizeof(conflict) - 1U,
                    "conflicting Content-Length");
    assert_rx_error((const u8 *)bad_length, sizeof(bad_length) - 1U,
                    "bad Content-Length");
    assert_rx_error((const u8 *)obs_fold, sizeof(obs_fold) - 1U,
                    "obsolete header folding");
    assert_rx_error((const u8 *)truncated, sizeof(truncated) - 1U,
                    "response ended early");
    assert_rx_error((const u8 *)bare_lf, sizeof(bare_lf) - 1U,
                    "bad line ending");
    assert_rx_error(nul_header, sizeof(nul_header) - 1U, "bad header");
}

static void count_body(void *ctx, const u8 *bytes, u64 n)
{
    u64 *count = ctx;

    (void)bytes;
    *count += n;
}

void test_http_rx_caps(void)
{
    static u8 block[1024U * 1024U];
    Bytebuf response;
    HttpRx rx;
    u64 count = 0U;
    u32 i;

    bytebuf_init(&response);
    bytebuf_append(&response, "HTTP/1.1 200 OK\r\nX: ",
                   sizeof("HTTP/1.1 200 OK\r\nX: ") - 1U);
    for (i = 0U; i < YEW_HTTP_MAX_HDRLINE; i++)
        bytebuf_push_u8(&response, (u8)'x');
    bytebuf_append(&response, "\r\n\r\n", 4U);
    assert_rx_error(response.data, response.len, "header line too long");
    bytebuf_free(&response);

    bytebuf_init(&response);
    bytebuf_append(&response, "HTTP/1.1 200 OK\r\n", 17U);
    for (i = 0U; i < YEW_HTTP_MAX_HDRS + 1U; i++)
        bytebuf_printf(&response, "X-%u: x\r\n", i);
    bytebuf_append(&response, "\r\n", 2U);
    assert_rx_error(response.data, response.len, "too many headers");
    bytebuf_free(&response);

    assert_rx_error(
        (const u8 *)"HTTP/1.1 200 OK\r\nContent-Length: 67108865\r\n\r\n",
        sizeof("HTTP/1.1 200 OK\r\nContent-Length: 67108865\r\n\r\n") -
            1U,
        "response too large");

    yew_http_rx_init(&rx);
    yew_http_rx_feed(
        &rx, (const u8 *)"HTTP/1.1 200 OK\r\nContent-Length: 67108864\r\n\r\n",
        sizeof("HTTP/1.1 200 OK\r\nContent-Length: 67108864\r\n\r\n") -
            1U,
        false, count_body, &count);
    for (i = 0U; i < 64U; i++)
        yew_http_rx_feed(&rx, block, sizeof(block), false, count_body, &count);
    YEW_ASSERT_EQ_U64(rx.state, YEW_HX_DONE);
    YEW_ASSERT_EQ_U64(count, YEW_HTTP_MAX_BODY);
    bytebuf_free(&rx.line);

    count = 0U;
    yew_http_rx_init(&rx);
    yew_http_rx_feed(&rx, (const u8 *)"HTTP/1.1 200 OK\r\n\r\n",
                     sizeof("HTTP/1.1 200 OK\r\n\r\n") - 1U, false,
                     count_body, &count);
    for (i = 0U; i < 64U; i++)
        yew_http_rx_feed(&rx, block, sizeof(block), false, count_body, &count);
    yew_http_rx_feed(&rx, block, 1U, false, count_body, &count);
    YEW_ASSERT_EQ_U64(rx.state, YEW_HX_DEAD);
    YEW_ASSERT_EQ_STR(rx.err, "response too large");
    YEW_ASSERT_EQ_U64(count, YEW_HTTP_MAX_BODY);
    bytebuf_free(&rx.line);
}
