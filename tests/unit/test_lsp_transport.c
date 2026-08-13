/* Sprint 45: real pipe/poll integration for the module-neutral framed sink. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "mod/lsp/jsonrpc.h"

typedef struct TransportCapture {
    Bytebuf bodies;
    u32 messages;
    bool destroyed;
} TransportCapture;

typedef struct TransportOwner {
    RpcConn conn;
    TransportCapture *capture;
} TransportOwner;

static void capture_msg(void *ctx, const u8 *body, u64 len)
{
    TransportCapture *capture = ctx;

    bytebuf_append(&capture->bodies, body, (size_t)len);
    capture->messages++;
}

static bool transport_feed(void *opaque, const u8 *bytes, u64 len)
{
    TransportOwner *owner = opaque;

    (void)yew_rpcrx_feed(&owner->conn.rx, bytes, len, capture_msg,
                         owner->capture);
    return owner->conn.rx.state != YEW_RPCRX_DEAD;
}

static bool transport_finish(void *opaque)
{
    TransportOwner *owner = opaque;
    RpcRx *rx = &owner->conn.rx;

    if (rx->state == YEW_RPCRX_DEAD)
        return false;
    if (rx->state == YEW_RPCRX_HDR && rx->line.len == 0U &&
        !rx->pending_cr && !rx->have_len)
        return true;
    rx->state = YEW_RPCRX_DEAD;
    (void)snprintf(rx->err, sizeof(rx->err), "connection closed mid-frame");
    return false;
}

static u64 transport_tx_view(void *opaque, const u8 **bytes)
{
    TransportOwner *owner = opaque;
    RpcTx *tx = &owner->conn.tx;

    if (tx->sent >= (u64)tx->pending.len) {
        *bytes = NULL;
        return 0U;
    }
    *bytes = tx->pending.data + (size_t)tx->sent;
    return (u64)tx->pending.len - tx->sent;
}

static void transport_tx_consume(void *opaque, u64 len)
{
    TransportOwner *owner = opaque;

    yew_rpctx_consume(&owner->conn.tx, len);
}

static void transport_destroy(void *opaque)
{
    TransportOwner *owner = opaque;

    yew_rpc_conn_free(&owner->conn);
    owner->capture->destroyed = true;
    free(owner);
}

static const YewJobFramedOps transport_ops = {
    transport_feed, transport_finish, transport_tx_view,
    transport_tx_consume, transport_destroy
};

static bool run_framed(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    for (;;) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;
        YewJob *job = yew_job_find(ed, id);

        if (job == NULL)
            return false;
        if (job->drained)
            return true;
        yew_job_collect_fds(ed, pfd, &n);
        (void)poll(pfd, (nfds_t)n, 20);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_settle(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
}

static u32 spawn_fakelsp(Ed *ed, const char *mode, TransportCapture *capture,
                         char *err, size_t errsz)
{
    static const u8 request[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"é\"}";
    TransportOwner *owner = yew_xcalloc(1U, sizeof(*owner));
    char *argv[3];
    YewJobSpec spec = {0};
    u32 id;

    yew_rpc_conn_init(&owner->conn);
    owner->capture = capture;
    yew_rpctx_send(&owner->conn.tx, request, sizeof(request) - 1U);
    argv[0] = (char *)"build/tests/helpers/fakelsp";
    argv[1] = (char *)mode;
    argv[2] = NULL;
    spec.argv = argv;
    spec.sink = YEW_SINK_FRAMED;
    spec.framed_owner = owner;
    spec.framed_ops = &transport_ops;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        transport_destroy(owner); /* ownership transfers only on success */
    return id;
}

void test_lsp_transport_one_byte_and_stderr_isolation(void)
{
    static const char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"é\"}";
    TransportCapture capture = {0};
    Ed ed;
    char err[160] = {0};
    u32 id;
    YewJob *job;

    bytebuf_init(&capture.bodies);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    id = spawn_fakelsp(&ed, "onebyte", &capture, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT_EQ_STR(err, "");
    YEW_ASSERT(run_framed(&ed, id));
    job = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT(!job->framed_failed);
    YEW_ASSERT_EQ_U64(capture.messages, 1U);
    YEW_ASSERT_EQ_MEM(capture.bodies.data, body, sizeof(body) - 1U);
    YEW_ASSERT_EQ_MEM(job->framed_err.data, "fake-lsp stderr\n", 16U);
    YEW_ASSERT(capture.destroyed);
    yew_ed_free(&ed);
    bytebuf_free(&capture.bodies);
}

void test_lsp_transport_reports_midframe_exit(void)
{
    TransportCapture capture = {0};
    Ed ed;
    char err[160] = {0};
    u32 id;
    YewJob *job;

    bytebuf_init(&capture.bodies);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    id = spawn_fakelsp(&ed, "mid", &capture, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_framed(&ed, id));
    job = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT(job->framed_failed);
    YEW_ASSERT_EQ_U64(capture.messages, 0U);
    YEW_ASSERT(capture.destroyed);
    yew_ed_free(&ed);
    bytebuf_free(&capture.bodies);
}

void test_lsp_transport_bypasses_text_safe_prefix(void)
{
    TransportCapture capture = {0};
    Ed ed;
    char err[160] = {0};
    u32 id;

    /* The echoed body ends in a complete JSON object but includes a
     * multibyte codepoint.  Applying yew_job_safe_prefix to each one-byte
     * read would retain protocol bytes, corrupt Content-Length accounting,
     * and this message would never complete. */
    bytebuf_init(&capture.bodies);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    id = spawn_fakelsp(&ed, "onebyte", &capture, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_framed(&ed, id));
    YEW_ASSERT_EQ_U64(capture.messages, 1U);
    yew_ed_free(&ed);
    bytebuf_free(&capture.bodies);
}
