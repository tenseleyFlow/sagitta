#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "mod/lsp/client.h"
#include "ui/message.h"

typedef struct GateFix {
    Ed ed;
    LspServerCfg cfg;
    struct sigaction saved_sigpipe;
    const char *args[3];
    char helper[4096];
    char path[256];
    char marker[256];
} GateFix;

typedef struct GateCase {
    const char *command;
    u32 cap;
    const char *message;
} GateCase;

static void gate_helper_path(char *out, size_t cap)
{
    const char *program = yew_test_program_path();
    const char *slash = strrchr(program, '/');
    int n;

    if (slash != NULL)
        n = snprintf(out, cap, "%.*s/tests/helpers/fakelsp",
                     (int)(slash - program), program);
    else
        n = snprintf(out, cap, "./tests/helpers/fakelsp");
    YEW_ASSERT(n >= 0 && (size_t)n < cap);
}

static void gate_fix_init(GateFix *f, const char *mode)
{
    char tmp[] = "/tmp/yew-lsp-gate-XXXXXX";
    char marker[] = "/tmp/yew-lsp-gate-marker-XXXXXX";
    struct sigaction ignored;
    int fd;

    (void)memset(f, 0, sizeof(*f));
    (void)memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    YEW_ASSERT_EQ_I64(sigemptyset(&ignored.sa_mask), 0);
    YEW_ASSERT_EQ_I64(sigaction(SIGPIPE, &ignored, &f->saved_sigpipe), 0);
    gate_helper_path(f->helper, sizeof(f->helper));
    fd = mkstemp(tmp);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, "int value;\n", 11U), 11);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    (void)snprintf(f->path, sizeof(f->path), "%s", tmp);
    fd = mkstemp(marker);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_I64(unlink(marker), 0);
    (void)snprintf(f->marker, sizeof(f->marker), "%s", marker);
    f->args[0] = mode;
    f->args[1] = f->marker;
    f->cfg.id = "fakelsp";
    f->cfg.lang = "c";
    f->cfg.cmd = f->helper;
    f->cfg.args = f->args;
    f->cfg.init_timeout_ms = 1000;
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open(&f->ed, f->path) == YEW_LOAD_OK);
    f->ed.buffer.lang = "c";
    f->ed.now_ms = yew_now_ms();
    YEW_ASSERT(yew_lsp_client_start_cfg(&f->ed, &f->ed.buffer, &f->cfg));
}

static void gate_pump_once(Ed *ed)
{
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    u32 n = 0U;
    i64 now;

    yew_job_collect_fds(ed, pfd, &n);
    (void)poll(pfd, (nfds_t)n, 10);
    now = yew_now_ms();
    ed->now_ms = now;
    yew_job_pump(ed, pfd, n);
    yew_job_reap(ed);
    yew_job_tick(ed, now);
    yew_job_settle(ed);
    yew_lsp_client_pump(ed);
}

static LspServer *gate_server(GateFix *f)
{
    LspDoc *doc = yew_lsp_doc_for_buffer(&f->ed, &f->ed.buffer);

    if (doc == NULL)
        YEW_BUG("LSP gate fixture lost its document");
    return yew_lsp_server_for_doc(&f->ed, doc);
}

static bool gate_wait_ready(GateFix *f, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (gate_server(f)->state == YEW_LSP_READY &&
            gate_server(f)->rpc.tx.pending.len == 0U)
            return true;
        gate_pump_once(&f->ed);
    }
    return false;
}

static bool gate_wait_capability_clear(GateFix *f, u32 cap,
                                       i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (!yew_lsp_has(gate_server(f), cap) &&
            gate_server(f)->rpc.npending == 0U &&
            gate_server(f)->rpc.tx.pending.len == 0U)
            return true;
        gate_pump_once(&f->ed);
    }
    return false;
}

static void gate_fix_free(GateFix *f)
{
    LspServer *server = gate_server(f);
    i64 start;
    bool drained = false;

    yew_lsp_client_stop(&f->ed, server, true);
    start = yew_now_ms();
    while (yew_now_ms() - start <= 2000) {
        YewJob *job = yew_job_find(&f->ed, server->job);

        if (job != NULL && job->drained) {
            drained = true;
            break;
        }
        gate_pump_once(&f->ed);
    }
    YEW_ASSERT(drained);
    yew_ed_free(&f->ed);
    YEW_ASSERT_EQ_I64(sigaction(SIGPIPE, &f->saved_sigpipe, NULL), 0);
    YEW_ASSERT_EQ_I64(unlink(f->path), 0);
    errno = 0;
    YEW_ASSERT(unlink(f->marker) == 0 || errno == ENOENT);
}

static CmdStatus gate_invoke(GateFix *f, const char *name)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_cmd_invoke(id, &cx);
}

void test_lsp_gate_all_feature_commands_message_once_without_round_trip(void)
{
    static const GateCase cases[] = {
        {"ed.lsp.goto_def", YEW_LSPC_DEFINITION,
         "fakelsp does not support definition"},
        {"ed.lsp.goto_decl", YEW_LSPC_DECLARATION,
         "fakelsp does not support declaration"},
        {"ed.lsp.goto_type", YEW_LSPC_TYPE_DEFINITION,
         "fakelsp does not support type definition"},
        {"ed.lsp.goto_impl", YEW_LSPC_IMPLEMENTATION,
         "fakelsp does not support implementation"},
        {"ed.lsp.references", YEW_LSPC_REFERENCES,
         "fakelsp does not support references"},
        {"ed.lsp.hover", YEW_LSPC_HOVER,
         "fakelsp does not support hover"},
        {"ed.lsp.signature", YEW_LSPC_SIGNATURE,
         "fakelsp does not support signature help"},
        {"ed.lsp.rename", YEW_LSPC_RENAME,
         "fakelsp does not support rename"},
        {"ed.lsp.symbols", YEW_LSPC_DOCUMENT_SYMBOL,
         "fakelsp does not support document symbols"}
    };
    GateFix f;
    LspServer *server;
    size_t i;

    gate_fix_init(&f, "session-utf8");
    YEW_ASSERT(gate_wait_ready(&f, 2000));
    server = gate_server(&f);
    server->caps.bits = 0U;
    server->missing_warned = 0U;
    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        u64 next_id = server->rpc.next_id;
        u16 npending = server->rpc.npending;
        size_t tx_len = server->rpc.tx.pending.len;

        YEW_ASSERT_EQ_U64(gate_invoke(&f, cases[i].command),
                          YEW_CMD_ERR_STATE);
        YEW_ASSERT(f.ed.msg.active);
        YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_INFO);
        YEW_ASSERT_EQ_STR(f.ed.msg.text, cases[i].message);
        YEW_ASSERT((server->missing_warned & cases[i].cap) != 0U);
        YEW_ASSERT_EQ_U64(server->rpc.next_id, next_id);
        YEW_ASSERT_EQ_U64(server->rpc.npending, npending);
        YEW_ASSERT_EQ_U64(server->rpc.tx.pending.len, tx_len);

        yew_msg_clear(&f.ed);
        YEW_ASSERT_EQ_U64(gate_invoke(&f, cases[i].command),
                          YEW_CMD_ERR_STATE);
        YEW_ASSERT(!f.ed.msg.active);
        YEW_ASSERT_EQ_U64(server->rpc.next_id, next_id);
        YEW_ASSERT_EQ_U64(server->rpc.npending, npending);
        YEW_ASSERT_EQ_U64(server->rpc.tx.pending.len, tx_len);
    }
    gate_fix_free(&f);
}

void test_lsp_gate_method_not_found_blocks_second_command_round_trip(void)
{
    GateFix f;
    LspServer *server;
    u64 next_id;
    u16 npending;
    size_t tx_len;

    gate_fix_init(&f, "session-definition-missing");
    YEW_ASSERT(gate_wait_ready(&f, 2000));
    server = gate_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DEFINITION));
    YEW_ASSERT_EQ_U64(gate_invoke(&f, "ed.lsp.goto_def"), YEW_CMD_OK);
    YEW_ASSERT(gate_wait_capability_clear(&f, YEW_LSPC_DEFINITION, 2000));
    YEW_ASSERT(!f.ed.msg.active);

    next_id = server->rpc.next_id;
    npending = server->rpc.npending;
    tx_len = server->rpc.tx.pending.len;
    YEW_ASSERT_EQ_U64(gate_invoke(&f, "ed.lsp.goto_def"),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "fakelsp does not support definition");
    YEW_ASSERT_EQ_U64(server->rpc.next_id, next_id);
    YEW_ASSERT_EQ_U64(server->rpc.npending, npending);
    YEW_ASSERT_EQ_U64(server->rpc.tx.pending.len, tx_len);

    gate_fix_free(&f);
}
