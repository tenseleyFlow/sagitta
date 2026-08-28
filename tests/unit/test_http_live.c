#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/backend.h"
#include "mod/ai/http.h"

#ifndef YEW_TEST_FAKEHTTP
#define YEW_TEST_FAKEHTTP "build/tests/helpers/fakehttp"
#endif

typedef struct LiveCapture {
    Bytebuf body;
    HttpConn *conn;
    Ed *ed;
    bool close_complete;
    bool done;
    bool release_on_done;
} LiveCapture;

static void body_capture(void *ctx, const u8 *bytes, u64 len)
{
    LiveCapture *capture = ctx;

    bytebuf_append(&capture->body, bytes, (size_t)len);
    if (capture->close_complete && capture->body.len == 5U)
        yew_http_conn_mark_stream_done(capture->conn);
}

static void done_capture(void *ctx, const HttpConn *conn)
{
    LiveCapture *capture = ctx;

    capture->done = true;
    if (capture->release_on_done) {
        YEW_ASSERT(conn == capture->conn);
        yew_http_conn_release(capture->ed, capture->conn);
        capture->conn = NULL;
    }
}

static pid_t server_start(const char *mode, u16 *port)
{
    int output[2];
    pid_t pid;
    FILE *stream;
    unsigned value = 0U;

    YEW_ASSERT_EQ_I64(pipe(output), 0);
    pid = fork();
    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output[1]);
        execl(YEW_TEST_FAKEHTTP, YEW_TEST_FAKEHTTP, mode, (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    stream = fdopen(output[0], "r");
    YEW_ASSERT_NOT_NULL(stream);
    YEW_ASSERT_EQ_I64(fscanf(stream, "%u", &value), 1);
    YEW_ASSERT(value > 0U && value <= 65535U);
    YEW_ASSERT_EQ_I64(fclose(stream), 0);
    *port = (u16)value;
    return pid;
}

static void server_wait(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    YEW_ASSERT(WIFEXITED(status));
}

static void pause_ms(long milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static void drive(Ed *ed, const LiveCapture *capture, i64 ceiling_ms)
{
    i64 started = yew_now_ms();
    bool poll_ok = true;
    bool within_deadline = true;

    while (!capture->done) {
        struct pollfd fds[YEW_HTTP_POOL_MAX];
        u32 nfds = 0U;
        int timeout = (int)yew_http_deadline(ed, yew_now_ms());
        int polled;

        yew_http_collect_fds(ed, fds, &nfds);
        if (timeout < 0 || timeout > 50)
            timeout = 50;
        polled = poll(fds, nfds, timeout);
        if (polled < 0 && errno != EINTR) {
            poll_ok = false;
            break;
        }
        yew_http_pump(ed, fds, nfds);
        if (yew_now_ms() - started >= ceiling_ms) {
            within_deadline = false;
            break;
        }
    }
    YEW_ASSERT(poll_ok);
    YEW_ASSERT(within_deadline);
    yew_http_pump(ed, NULL, 0U);
}

static void set_int(Ed *ed, const char *name, i64 value)
{
    OptVal option = {YEW_OPT_INT, {.i = value}};
    const char *err = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                           &option, &err));
}

static HttpConn *request_start_host(Ed *ed, const char *host, u16 port,
                                    bool keepalive, AiErr *err,
                                    LiveCapture *capture)
{
    HttpUrl url = {host, port, "/", true};
    HttpReq request = {"POST", "/", NULL, 0U, NULL, 0U, keepalive};
    HttpConn *conn = yew_http_begin(ed, &url, &request, err);

    YEW_ASSERT_NOT_NULL(conn);
    capture->conn = conn;
    capture->ed = ed;
    capture->done = false;
    yew_http_conn_callbacks(conn, body_capture, done_capture, capture);
    return conn;
}

static HttpConn *request_start(Ed *ed, u16 port, bool keepalive,
                               AiErr *err, LiveCapture *capture)
{
    return request_start_host(ed, "127.0.0.1", port, keepalive, err,
                              capture);
}

static void capture_init(LiveCapture *capture)
{
    (void)memset(capture, 0, sizeof(*capture));
    bytebuf_init(&capture->body);
}

static void capture_drop(LiveCapture *capture)
{
    bytebuf_free(&capture->body);
}

static void run_shape(const char *mode, bool close_complete,
                      bool release_in_callback)
{
    Ed ed;
    AiErr err;
    LiveCapture capture;
    HttpConn *conn;
    u16 port;
    pid_t server = server_start(mode, &port);

    yew_ed_init(&ed);
    capture_init(&capture);
    capture.close_complete = close_complete;
    capture.release_on_done = release_in_callback;
    conn = request_start(&ed, port, false, &err, &capture);
    drive(&ed, &capture, 3000);
    YEW_ASSERT(capture.done);
    YEW_ASSERT_EQ_U64(capture.body.len, 5U);
    YEW_ASSERT_EQ_MEM(capture.body.data, "hello", 5U);
    if (!release_in_callback) {
        YEW_ASSERT_EQ_U64(conn->state, YEW_HC_DONE);
        yew_http_conn_release(&ed, conn);
    } else {
        YEW_ASSERT_NULL(capture.conn);
    }
    capture_drop(&capture);
    yew_ed_free(&ed);
    server_wait(server);
}

static void run_truncated_close(void)
{
    Ed ed;
    AiErr err;
    LiveCapture capture;
    HttpConn *conn;
    u16 port;
    pid_t server = server_start("midclose", &port);

    yew_ed_init(&ed);
    capture_init(&capture);
    conn = request_start(&ed, port, false, &err, &capture);
    drive(&ed, &capture, 3000);
    YEW_ASSERT(capture.done);
    YEW_ASSERT_EQ_U64(conn->state, YEW_HC_DEAD);
    YEW_ASSERT_EQ_U64(err.kind, YEW_AI_ERR_PROTOCOL);
    YEW_ASSERT_EQ_STR(err.msg, "response ended early");
    YEW_ASSERT_EQ_U64(capture.body.len, 3U);
    YEW_ASSERT_EQ_MEM(capture.body.data, "hel", 3U);
    yew_http_conn_release(&ed, conn);
    capture_drop(&capture);
    yew_ed_free(&ed);
    server_wait(server);
}

void test_http_live_body_shapes(void)
{
    run_shape("byte", false, false);
    run_shape("giant", false, true);
    run_shape("chunk", false, false);
    run_shape("close", true, false);
    run_truncated_close();
}

void test_http_live_timeout_classes(void)
{
    static const struct {
        const char *mode;
        const char *option;
        const char *message;
    } cases[] = {
        {"delay", "ai.first_byte_timeout_ms", "HTTP first-byte timeout"},
        {"idle", "ai.stream_idle_timeout_ms", "HTTP stream-idle timeout"},
        {"delay", "ai.total_timeout_ms", "HTTP total timeout"}
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        Ed ed;
        AiErr err;
        LiveCapture capture;
        HttpConn *conn;
        u16 port;
        pid_t server = server_start(cases[i].mode, &port);

        yew_ed_init(&ed);
        capture_init(&capture);
        set_int(&ed, cases[i].option, 25);
        conn = request_start(&ed, port, false, &err, &capture);
        drive(&ed, &capture, 1000);
        YEW_ASSERT_EQ_U64(conn->state, YEW_HC_DEAD);
        YEW_ASSERT_EQ_U64(err.kind, YEW_AI_ERR_TIMEOUT);
        YEW_ASSERT_EQ_STR(err.msg, cases[i].message);
        yew_http_conn_release(&ed, conn);
        capture_drop(&capture);
        yew_ed_free(&ed);
        server_wait(server);
    }
}

void test_http_live_pool_retry_and_fresh_failure(void)
{
    Ed ed;
    AiErr first_err;
    AiErr second_err;
    LiveCapture first;
    LiveCapture second;
    HttpConn *first_conn;
    HttpConn *second_conn;
    u16 port;
    pid_t server = server_start("stale", &port);

    yew_ed_init(&ed);
    capture_init(&first);
    capture_init(&second);
    first_conn = request_start(&ed, port, true, &first_err, &first);
    drive(&ed, &first, 3000);
    YEW_ASSERT_EQ_U64(first_conn->state, YEW_HC_DONE);
    YEW_ASSERT(first_conn->reusable);
    yew_http_conn_release(&ed, first_conn);
    pause_ms(100);
    second_conn = request_start(&ed, port, false, &second_err, &second);
    drive(&ed, &second, 3000);
    YEW_ASSERT_EQ_U64(second_conn->state, YEW_HC_DONE);
    YEW_ASSERT(second_conn->retried);
    YEW_ASSERT_EQ_U64(second.body.len, 3U);
    YEW_ASSERT_EQ_MEM(second.body.data, "two", 3U);
    yew_http_conn_release(&ed, second_conn);
    server_wait(server);

    second_conn = request_start(&ed, port, false, &second_err, &second);
    drive(&ed, &second, 1000);
    YEW_ASSERT_EQ_U64(second_conn->state, YEW_HC_DEAD);
    YEW_ASSERT(!second_conn->retried);
    yew_http_conn_release(&ed, second_conn);
    capture_drop(&second);
    capture_drop(&first);
    yew_ed_free(&ed);
}

void test_http_live_resolver_and_address_fallback(void)
{
    Ed ed;
    AiErr err;
    LiveCapture capture;
    HttpConn *conn;
    HttpUrl literal = {"127.0.0.1", 9U, "/", true};
    HttpUrl alias;
    u64 resolvers;
    u64 sockets;
    u16 port;
    pid_t server = server_start("byte", &port);

    yew_ed_init(&ed);
    capture_init(&capture);
    resolvers = yew_http_resolver_call_count();
    YEW_ASSERT(yew_http_register_endpoint(&ed, &literal, &err));
    YEW_ASSERT_EQ_U64(yew_http_resolver_call_count(), resolvers);

    alias.host = "fallback.test";
    alias.port = port;
    alias.path = "/";
    alias.loopback = false;
    YEW_ASSERT(yew_http_register_address(&ed, &alias, "127.0.0.2", &err));
    YEW_ASSERT(yew_http_register_address(&ed, &alias, "127.0.0.1", &err));
    sockets = yew_http_socket_call_count();
    conn = request_start_host(&ed, alias.host, port, false, &err, &capture);
    drive(&ed, &capture, 3000);
    YEW_ASSERT_EQ_U64(conn->state, YEW_HC_DONE);
    YEW_ASSERT_EQ_U64(yew_http_socket_call_count(), sockets + 2U);
    YEW_ASSERT_EQ_U64(yew_http_resolver_call_count(), resolvers);
    YEW_ASSERT_EQ_U64(capture.body.len, 5U);
    YEW_ASSERT_EQ_MEM(capture.body.data, "hello", 5U);
    yew_http_conn_release(&ed, conn);
    capture_drop(&capture);
    yew_ed_free(&ed);
    server_wait(server);
}
