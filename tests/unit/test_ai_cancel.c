#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/shadow.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/http.h"
#include "mod/ai/shadow_ai.h"

#ifndef YEW_TEST_FAKEHTTP
#define YEW_TEST_FAKEHTTP "build/tests/helpers/fakehttp"
#endif

static void cancel_call_init(Ed *ed, AiCall *call)
{
    (void)memset(call, 0, sizeof(*call));
    call->ed = ed;
    call->active = true;
    call->live = true;
    call->t_sent = -1;
    call->t_first_token = -1;
    call->t_done = -1;
    call->retry_after_ms = -1;
    arena_init(&call->arena);
    bytebuf_init(&call->raw);
    bytebuf_init(&call->text);
    bytebuf_init(&call->body);
    bytebuf_init(&call->response);
    bytebuf_init(&call->curl_config);
    bytebuf_init(&call->curl_err);
    yew_ai_adapter_state_init(&call->adapter);
}

static pid_t cancel_server_start(const char *mode, u16 *port)
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

static void cancel_server_stop(pid_t pid)
{
    int status = 0;
    pid_t waited;

    YEW_ASSERT(kill(pid, SIGTERM) == 0 || errno == ESRCH);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, pid);
    YEW_ASSERT(WIFEXITED(status) ||
               (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM));
}

void test_ai_cancel_http_never_pools_the_connection(void)
{
    static const u8 body[] = "{}";
    HttpHdr headers[] = {{"content-type", "application/json"}};
    HttpReq request = {"POST", "/", headers, 1U,
                       body, sizeof(body) - 1U, true};
    Ed ed;
    AiCall *call;
    HttpUrl url;
    AiErr error = {0};
    u16 port;
    HttpConn *next;
    pid_t server = cancel_server_start("stale", &port);

    yew_ed_init(&ed);
    call = &ed.ai->call;
    cancel_call_init(&ed, call);
    url = (HttpUrl){"127.0.0.1", port, "/", true};
    call->conn = yew_http_begin(&ed, &url, &request, &error);
    YEW_ASSERT_NOT_NULL(call->conn);
    YEW_ASSERT(call->conn->fd >= 0);
    yew_ai_call_abort(&ed, call, YEW_AI_ERR_CANCELLED);

    YEW_ASSERT(!call->active);
    YEW_ASSERT_NULL(call->conn);
    YEW_ASSERT_EQ_I64(yew_http_deadline(&ed, yew_now_ms()), -1);

    next = yew_http_begin(&ed, &url, &request, &error);
    YEW_ASSERT_NOT_NULL(next);
    YEW_ASSERT(!next->from_pool);
    yew_http_abort(&ed, next);
    yew_http_conn_release(&ed, next);
    cancel_server_stop(server);
    yew_ed_free(&ed);
}

/* Guard A: even with provider cancellation stubbed to a no-op, the shared
 * shadow generation floor rejects a response that was already queued. */
void test_ai_cancel_guard_sequence_floor_rejects_late_delivery(void)
{
    Ed ed;
    ShadowSug suggestion = {0};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "ai-sequence-guard"));
    suggestion.seq = 4U;
    suggestion.prov = (u8)YEW_SHADOW_AI;
    suggestion.buf_id = ed.win->buf->id;
    suggestion.buf_gen = ed.win->buf->tb->gen;
    suggestion.pos = BYTEOFF(0U);
    suggestion.text = (const u8 *)"late";
    suggestion.len = 4U;
    ed.win->shadow.seq_min[YEW_SHADOW_AI] = 5U;
    ed.win->shadow.seq_next[YEW_SHADOW_AI] = 6U;

    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.dropped_stale, 1U);
    yew_ed_free(&ed);
}

/* Guard B: even with the shared sequence floor held unchanged, transport
 * cancellation destroys the pending bytes and the pump cannot emit them. */
void test_ai_cancel_guard_transport_abort_discards_pending_bytes(void)
{
    Ed ed;
    AiCall *call;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "ai-transport-guard"));
    call = &ed.ai->call;
    cancel_call_init(&ed, call);
    call->seq = 1U;
    call->buf_id = ed.win->buf->id;
    call->buf_gen = ed.win->buf->tb->gen;
    call->pos = BYTEOFF(0U);
    bytebuf_append(&call->raw, "queued", 6U);
    call->dirty = true;
    ed.win->shadow.seq_next[YEW_SHADOW_AI] = 2U;

    yew_ai_call_abort(&ed, call, YEW_AI_ERR_CANCELLED);
    YEW_ASSERT_EQ_U64(ed.win->shadow.seq_min[YEW_SHADOW_AI], 0U);
    yew_ai_shadow_pump(&ed);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT(!ed.ai->call.active);
    yew_ed_free(&ed);
}

void test_ai_cancel_curl_detaches_then_reaps_without_callbacks(void)
{
    char *argv[] = {(char *)"/bin/sh", (char *)"-c",
                    (char *)"trap '' TERM; sleep 30", NULL};
    YewJobSpec spec = {0};
    char error[192] = {0};
    Ed ed;
    AiCall *call;
    YewJob *job;
    u32 id;
    i64 started;

    yew_ed_init(&ed);
    call = &ed.ai->call;
    cancel_call_init(&ed, call);
    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    id = yew_job_spawn(&ed, &spec, error, sizeof(error));
    YEW_ASSERT(id != 0U);
    call->job = id;
    job = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(job);

    yew_ai_call_abort(&ed, call, YEW_AI_ERR_CANCELLED);
    YEW_ASSERT(!call->active);
    YEW_ASSERT_EQ_U64(ed.ai->nretired_jobs, 1U);
    YEW_ASSERT_EQ_U64(ed.ai->retired_jobs[0], id);
    YEW_ASSERT_EQ_U64(job->sink, YEW_SINK_DISCARD);
    YEW_ASSERT_NULL(job->stream_owner);
    YEW_ASSERT_NULL(job->stream_ops);
    YEW_ASSERT(job->stream_destroyed);
    YEW_ASSERT_EQ_I64(job->in_fd, -1);
    YEW_ASSERT_EQ_I64(job->out_fd, -1);
    YEW_ASSERT_EQ_I64(job->err_fd, -1);
    YEW_ASSERT_EQ_I64(job->exec_fd, -1);
    YEW_ASSERT(job->kill_at_ms > 0);

    yew_job_tick(&ed, job->kill_at_ms);
    started = yew_now_ms();
    while (yew_job_find(&ed, id) != NULL &&
           yew_now_ms() - started < 3000) {
        (void)poll(NULL, 0U, 5);
        yew_job_reap(&ed);
        yew_job_settle(&ed);
        yew_ai_shadow_pump(&ed);
    }
    YEW_ASSERT_NULL(yew_job_find(&ed, id));
    YEW_ASSERT_EQ_U64(ed.ai->nretired_jobs, 0U);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
    yew_ed_free(&ed);
}

void test_ai_cancel_curl_reaps_five_hundred_cycles(void)
{
    char *argv[] = {(char *)"/bin/sh", (char *)"-c",
                    (char *)"sleep 30", NULL};
    YewJobSpec spec = {0};
    char error[192];
    Ed ed;
    u32 cycle;

    yew_ed_init(&ed);
    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    for (cycle = 0U; cycle < 500U; cycle++) {
        AiCall *call = &ed.ai->call;
        u32 id;
        i64 started;

        (void)memset(error, 0, sizeof(error));
        cancel_call_init(&ed, call);
        id = yew_job_spawn(&ed, &spec, error, sizeof(error));
        YEW_ASSERT(id != 0U);
        call->job = id;
        yew_ai_call_abort(&ed, call, YEW_AI_ERR_CANCELLED);
        started = yew_now_ms();
        while (yew_job_find(&ed, id) != NULL &&
               yew_now_ms() - started < 3000) {
            (void)poll(NULL, 0U, 1);
            yew_job_reap(&ed);
            yew_job_settle(&ed);
            yew_ai_shadow_pump(&ed);
        }
        YEW_ASSERT_NULL(yew_job_find(&ed, id));
        YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
        YEW_ASSERT_EQ_U64(ed.ai->nretired_jobs, 0U);
    }
    yew_ed_free(&ed);
}
