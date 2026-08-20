#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/backend.h"
#include "mod/ai/http.h"
#include "util/sort.h"

enum {
    AI_HTTP_SAMPLES = 1001,
    AI_HTTP_CYCLES = 500,
    AI_HTTP_TIMEOUT_MS = 5000,
    AI_HTTP_BUILD_P99_BUDGET_NS = 100000,
    AI_HTTP_PARSE_P99_BUDGET_NS = 100000
};

typedef struct Timing {
    const char *name;
    u64 median_ns;
    u64 p99_ns;
    u64 maximum_ns;
    u64 baseline_median_ns;
    u64 baseline_p99_ns;
    u64 budget_p99_ns;
} Timing;

typedef struct Capture {
    u64 bytes;
    u64 checksum;
    bool done;
} Capture;

static const u8 response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 11\r\n"
    "Content-Type: application/json\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"ok\":true}";

static const u8 response_close[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 11\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"ok\":true}";

static const u8 request_body[] = "{\"prompt\":\"alpha\"}";
static const HttpHdr request_headers[] = {
    {"X-Yew-Test", "deterministic"},
    {"X-Yew-Mode", "performance"}
};

static const u8 request_golden[] =
    "POST /api/generate HTTP/1.1\r\n"
    "Host: 127.0.0.1:11434\r\n"
    "User-Agent: yew/1.0.0\r\n"
    "Accept: application/json\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 18\r\n"
    "Connection: keep-alive\r\n"
    "X-Yew-Test: deterministic\r\n"
    "X-Yew-Mode: performance\r\n"
    "\r\n"
    "{\"prompt\":\"alpha\"}";

static volatile u64 ai_http_sink;

static void fail(const char *message)
{
    (void)fprintf(stderr, "perf_ai_http: %s\n", message);
    exit(2);
}

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        fail("clock_gettime failed");
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    const u64 a = *(const u64 *)left;
    const u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void summarize(u64 samples[AI_HTTP_SAMPLES], Timing *out)
{
    yew_sort_stable(samples, AI_HTTP_SAMPLES, sizeof(*samples), cmp_u64,
                    NULL);
    out->median_ns = samples[AI_HTTP_SAMPLES / 2U];
    out->p99_ns = samples[(AI_HTTP_SAMPLES * 99U) / 100U];
    out->maximum_ns = samples[AI_HTTP_SAMPLES - 1U];
}

static void capture_body(void *ctx, const u8 *bytes, u64 len)
{
    Capture *capture = ctx;
    u64 i;

    capture->bytes += len;
    for (i = 0U; i < len; i++)
        capture->checksum = capture->checksum * 33U + bytes[i];
}

static void capture_done(void *ctx, const HttpConn *conn)
{
    Capture *capture = ctx;

    capture->done = true;
    ai_http_sink ^= (u64)conn->rx->status + capture->checksum;
}

static bool measure_request_build(Timing *out)
{
    const HttpUrl url = {"127.0.0.1", 11434U, "/api/generate", true};
    const HttpReq request = {
        "POST", "/api/generate", request_headers,
        YEW_ARRAY_LEN(request_headers), request_body,
        sizeof(request_body) - 1U, true
    };
    u64 samples[AI_HTTP_SAMPLES];
    u32 i;

    for (i = 0U; i < AI_HTTP_SAMPLES; i++) {
        Bytebuf built;
        char err[128];
        u64 started;

        bytebuf_init(&built);
        started = now_ns();
        if (!yew_http_req_build(&built, &url, &request, err, sizeof(err))) {
            bytebuf_free(&built);
            return false;
        }
        samples[i] = now_ns() - started;
        if (built.len != sizeof(request_golden) - 1U ||
            memcmp(built.data, request_golden, built.len) != 0) {
            bytebuf_free(&built);
            return false;
        }
        ai_http_sink ^= built.data[i % built.len];
        bytebuf_free(&built);
    }
    summarize(samples, out);
    return true;
}

static bool measure_response_parse(Timing *out)
{
    u64 samples[AI_HTTP_SAMPLES];
    u32 i;

    for (i = 0U; i < AI_HTTP_SAMPLES; i++) {
        HttpRx rx;
        Capture capture = {0U, 0U, false};
        u64 offset = 0U;
        u64 started = now_ns();

        yew_http_rx_init(&rx);
        while (offset < sizeof(response) - 1U) {
            u64 span = 1U + ((offset + i) % 17U);
            u64 remaining = sizeof(response) - 1U - offset;
            u64 consumed;

            if (span > remaining)
                span = remaining;
            consumed = yew_http_rx_feed(&rx, response + offset, span,
                                         false, capture_body, &capture);
            if (consumed != span)
                break;
            offset += consumed;
        }
        samples[i] = now_ns() - started;
        if (offset != sizeof(response) - 1U ||
            rx.state != (u8)YEW_HX_DONE || rx.status != 200U ||
            capture.bytes != 11U) {
            bytebuf_free(&rx.line);
            return false;
        }
        ai_http_sink ^= capture.checksum;
        bytebuf_free(&rx.line);
    }
    summarize(samples, out);
    return true;
}

static bool write_all(int fd, const u8 *bytes, size_t len)
{
    size_t offset = 0U;

    while (offset < len) {
        ssize_t written = write(fd, bytes + offset, len - offset);

        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool read_request(int fd)
{
    u8 tail[4] = {0U, 0U, 0U, 0U};
    u64 total = 0U;

    while (total < 8192U) {
        u8 byte;
        ssize_t got = read(fd, &byte, 1U);

        if (got == 1) {
            tail[0] = tail[1];
            tail[1] = tail[2];
            tail[2] = tail[3];
            tail[3] = byte;
            total++;
            if (memcmp(tail, "\r\n\r\n", 4U) == 0)
                return true;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        return false;
    }
    return false;
}

static int server_run(int listener)
{
    int one = 1;
    u32 i;

    for (i = 0U; i <= AI_HTTP_CYCLES; i++) {
        const u8 *reply = i == AI_HTTP_CYCLES ? response : response_close;
        size_t reply_len = i == AI_HTTP_CYCLES ? sizeof(response) - 1U :
                                                 sizeof(response_close) - 1U;
        int client;

        do {
            client = accept(listener, NULL, NULL);
        } while (client < 0 && errno == EINTR);
        if (client < 0)
            return 10;
        if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one,
                       sizeof(one)) != 0) {
            (void)close(client);
            return 11;
        }
        if (!read_request(client) ||
            !write_all(client, reply, reply_len)) {
            (void)close(client);
            return 12;
        }
        if (close(client) != 0)
            return 13;
    }
    return 0;
}

static bool server_start(u16 *port, pid_t *child_out)
{
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int listener;
    int one = 1;
    pid_t child;

    *child_out = -1;
    listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return false;
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0U;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) !=
            0 ||
        bind(listener, (const struct sockaddr *)&address, sizeof(address)) !=
            0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_len) !=
            0) {
        (void)close(listener);
        return false;
    }
    *port = ntohs(address.sin_port);
    child = fork();
    if (child < 0) {
        (void)close(listener);
        return false;
    }
    if (child == 0) {
        int status = server_run(listener);

        (void)close(listener);
        _exit(status);
    }
    *child_out = child;
    /* The child has its own descriptor table after fork.  The parent must
     * retain no listener while its fd accounting gate is active. */
    (void)close(listener);
    return true;
}

static int fd_count(void)
{
#if defined(__linux__)
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;

    if (dir == NULL)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        char *end;
        long value;

        errno = 0;
        value = strtol(entry->d_name, &end, 10);
        if (errno == 0 && entry->d_name[0] != '\0' && *end == '\0' &&
            value >= 0)
            count++;
    }
    if (closedir(dir) != 0)
        return -1;
    return count;
#else
    return -1;
#endif
}

static bool set_keepalive(Ed *ed, i64 milliseconds)
{
    OptVal value = {YEW_OPT_INT, {.i = milliseconds}};
    const char *err = NULL;

    if (!yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.keepalive_ms", 15U, &value,
                     &err)) {
        (void)fprintf(stderr, "perf_ai_http: %s\n",
                      err == NULL ? "could not set keepalive" : err);
        return false;
    }
    return true;
}

static bool drive(Ed *ed, HttpConn *conn)
{
    i64 started = yew_now_ms();

    while (conn->state != (u8)YEW_HC_DONE &&
           conn->state != (u8)YEW_HC_DEAD) {
        struct pollfd fds[YEW_HTTP_POOL_MAX];
        u32 nfds = 0U;
        int polled;

        yew_http_collect_fds(ed, fds, &nfds);
        polled = poll(fds, (nfds_t)nfds, 100);
        if (polled < 0 && errno != EINTR) {
            (void)fprintf(stderr, "perf_ai_http: poll failed\n");
            return false;
        }
        yew_http_pump(ed, fds, nfds);
        if (yew_now_ms() - started >= AI_HTTP_TIMEOUT_MS) {
            (void)fprintf(stderr, "perf_ai_http: request timed out\n");
            return false;
        }
    }
    yew_http_pump(ed, NULL, 0U);
    return true;
}

static bool wait_until(i64 deadline_ms)
{
    for (;;) {
        i64 now = yew_now_ms();
        i64 remaining;
        int delay;

        if (now >= deadline_ms)
            return true;
        remaining = deadline_ms - now;
        delay = remaining > 1000 ? 1000 : (int)remaining;
        if (poll(NULL, 0U, delay) == 0)
            continue;
        if (errno != EINTR)
            return false;
    }
}

static pid_t waitpid_intr(pid_t child, int *status, int options)
{
    pid_t waited;

    do {
        waited = waitpid(child, status, options);
    } while (waited < 0 && errno == EINTR);
    return waited;
}

static bool reap_until(pid_t child, int *status, i64 deadline_ms)
{
    for (;;) {
        pid_t waited = waitpid_intr(child, status, WNOHANG);

        if (waited == child)
            return true;
        if (waited < 0 || yew_now_ms() >= deadline_ms)
            return false;
        if (!wait_until(yew_now_ms() + 1))
            return false;
    }
}

static bool run_resource_cycles(void)
{
    const HttpReq request = {"POST", "/", NULL, 0U, NULL, 0U, false};
    const HttpReq pooled_request = {"POST", "/", NULL, 0U, NULL, 0U, true};
    Ed ed;
    HttpUrl url;
    AiErr err;
    HttpConn *active = NULL;
    pid_t server = -1;
    u16 port = 0U;
    int before = fd_count();
    int live_after = -1;
    int final_after;
    int status = 0;
    bool ed_ready = false;
    bool server_reaped = false;
    bool ok = false;
    u32 i;

    yew_ed_init(&ed);
    ed_ready = true;
    if (!server_start(&port, &server)) {
        (void)fprintf(stderr, "perf_ai_http: server setup failed\n");
        goto cleanup;
    }
    if (!set_keepalive(&ed, 1))
        goto cleanup;
    url.host = "127.0.0.1";
    url.port = port;
    url.path = "/";
    url.loopback = true;
    for (i = 0U; i < AI_HTTP_CYCLES; i++) {
        Capture capture = {0U, 0U, false};

        active = yew_http_begin(&ed, &url, &request, &err);
        if (active == NULL) {
            (void)fprintf(stderr, "perf_ai_http: %s\n", err.msg);
            goto cleanup;
        }
        yew_http_conn_callbacks(active, capture_body, capture_done, &capture);
        if (!drive(&ed, active))
            goto cleanup;
        if (!capture.done || active->state != (u8)YEW_HC_DONE ||
            active->rx->status != 200U || capture.bytes != 11U) {
            (void)fprintf(stderr,
                          "perf_ai_http: request/response invariant failed\n");
            goto cleanup;
        }
        yew_http_conn_release(&ed, active);
        active = NULL;
    }
    {
        Capture capture = {0U, 0U, false};

        active = yew_http_begin(&ed, &url, &pooled_request, &err);
        if (active == NULL) {
            (void)fprintf(stderr, "perf_ai_http: %s\n", err.msg);
            goto cleanup;
        }
        yew_http_conn_callbacks(active, capture_body, capture_done, &capture);
        if (!drive(&ed, active))
            goto cleanup;
        if (!capture.done || active->state != (u8)YEW_HC_DONE ||
            !active->reusable || capture.bytes != 11U) {
            (void)fprintf(stderr,
                          "perf_ai_http: pooled request invariant failed\n");
            goto cleanup;
        }
        yew_http_conn_release(&ed, active);
        active = NULL;
    }
    if (!wait_until(yew_now_ms() + 5)) {
        (void)fprintf(stderr, "perf_ai_http: idle wait failed\n");
        goto cleanup;
    }
    yew_http_pump(&ed, NULL, 0U);
    if (!reap_until(server, &status, yew_now_ms() + 1000)) {
        (void)fprintf(stderr, "perf_ai_http: fake server did not exit\n");
        goto cleanup;
    }
    server_reaped = true;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void)fprintf(stderr, "perf_ai_http: fake server failed\n");
        goto cleanup;
    }
    /* The child has been reaped and the parent closed its listener before
     * the first request.  Any remaining descriptor is therefore owned by
     * the live editor, proving idle expiry independently of teardown. */
    live_after = fd_count();
    if (before >= 0 && live_after != before) {
        (void)fprintf(stderr, "perf_ai_http: fd delta %d (%d -> %d)\n",
                      live_after - before, before, live_after);
        goto cleanup;
    }
    ok = true;

cleanup:
    if (ed_ready)
        yew_ed_free(&ed);
    if (server > 0 && !server_reaped) {
        pid_t waited = waitpid_intr(server, &status, WNOHANG);

        if (waited == 0) {
            if (kill(server, SIGTERM) != 0 && errno != ESRCH)
                ok = false;
            waited = waitpid_intr(server, &status, 0);
        }
        if (waited != server)
            ok = false;
        server_reaped = waited == server;
    }
    final_after = fd_count();
    if (before >= 0 && final_after != before) {
        (void)fprintf(stderr,
                      "perf_ai_http: post-teardown fd delta %d (%d -> %d)\n",
                      final_after - before, before, final_after);
        ok = false;
    }
    if (!ok)
        return false;
    (void)printf("ai.http.cycles=%u fd_delta=%d ok\n", AI_HTTP_CYCLES,
                 before < 0 ? 0 : live_after - before);
    return true;
}

static bool load_baselines(Timing *rows, size_t count)
{
    FILE *fp = fopen("tests/perf/baselines/ai_http.txt", "r");
    char line[160];
    size_t i;

    if (fp == NULL)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char name[64];
        unsigned long long median;
        unsigned long long p99;

        if (sscanf(line, "%63s %llu %llu", name, &median, &p99) != 3)
            continue;
        for (i = 0U; i < count; i++) {
            if (strcmp(rows[i].name, name) == 0) {
                rows[i].baseline_median_ns = (u64)median;
                rows[i].baseline_p99_ns = (u64)p99;
            }
        }
    }
    if (ferror(fp) || fclose(fp) != 0)
        return false;
    for (i = 0U; i < count; i++) {
        if (rows[i].baseline_median_ns == 0U ||
            rows[i].baseline_p99_ns == 0U)
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    Timing rows[] = {
        {"request_build", 0U, 0U, 0U, 0U, 0U,
         AI_HTTP_BUILD_P99_BUDGET_NS},
        {"response_parse", 0U, 0U, 0U, 0U, 0U,
         AI_HTTP_PARSE_P99_BUDGET_NS}
    };
    bool cycles_only = argc == 2 && strcmp(argv[1], "--cycles-only") == 0;
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;
    int result = 0;
    size_t i;

    if (argc > 2 || (argc == 2 && !cycles_only && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure|--cycles-only]\n",
                      argv[0]);
        return 2;
    }
    if (!cycles_only) {
        if (!measure_request_build(&rows[0]))
            fail("request build invariant failed");
        if (!measure_response_parse(&rows[1]))
            fail("response parse invariant failed");
        if (!measure && !load_baselines(rows, YEW_ARRAY_LEN(rows)))
            fail("missing or invalid baseline");
        for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
            bool regression = rows[i].p99_ns > rows[i].budget_p99_ns;

            (void)printf("ai.http.%s median_ns=%llu p99_ns=%llu "
                         "max_ns=%llu budget_ns=%llu%s\n", rows[i].name,
                         (unsigned long long)rows[i].median_ns,
                         (unsigned long long)rows[i].p99_ns,
                         (unsigned long long)rows[i].maximum_ns,
                         (unsigned long long)rows[i].budget_p99_ns,
                         regression ? " REGRESSION" : " ok");
            if (measure)
                (void)printf("%s %llu %llu\n", rows[i].name,
                             (unsigned long long)rows[i].median_ns,
                             (unsigned long long)rows[i].p99_ns);
            if (regression)
                result = 1;
        }
    }
    if (!run_resource_cycles())
        result = 1;
    return result;
}
