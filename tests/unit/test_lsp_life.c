#define _XOPEN_SOURCE 700

#include "harness.h"

#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mod/lsp/client.h"
#include "mod/lsp/lsp.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "util/arena.h"

static JsonValue *parse(Arena *arena, const char *json)
{
    JsonErr err;
    JsonValue *v = yew_json_parse(arena, (const u8 *)json,
                                  (u64)strlen(json), &err);

    YEW_ASSERT_NOT_NULL(v);
    return v;
}

void test_lsp_lifecycle_requires_sync_and_negotiates_ready(void)
{
    LspServer server;
    Arena arena;

    yew_lsp_server_init(&server, 7U, yew_lsp_default_cfg("c"),
                        strdup("/tmp"));
    YEW_ASSERT_EQ_U64(server.state, YEW_LSP_SPAWNING);
    server.state = YEW_LSP_INITIALIZING;
    arena_init(&arena);
    YEW_ASSERT(yew_lsp_server_initialized(&server, parse(&arena,
        "{\"capabilities\":{\"textDocumentSync\":2,"
        "\"positionEncoding\":\"utf-8\",\"hoverProvider\":true}}")));
    YEW_ASSERT_EQ_U64(server.state, YEW_LSP_READY);
    YEW_ASSERT_EQ_U64(server.pos_enc, YEW_POSENC_UTF8);
    YEW_ASSERT(yew_lsp_has(&server, YEW_LSPC_HOVER));
    arena_free_all(&arena);
    yew_lsp_server_dispose(&server);

    yew_lsp_server_init(&server, 8U, yew_lsp_default_cfg("go"),
                        strdup("/tmp"));
    server.state = YEW_LSP_INITIALIZING;
    arena_init(&arena);
    YEW_ASSERT(!yew_lsp_server_initialized(&server, parse(&arena,
        "{\"capabilities\":{\"hoverProvider\":true}}")));
    YEW_ASSERT_EQ_U64(server.state, YEW_LSP_DEAD);
    arena_free_all(&arena);
    yew_lsp_server_dispose(&server);
}

void test_lsp_lifecycle_backoff_and_giveup_messages(void)
{
    static const i64 delay[] = {250, 1000, 4000, 16000};
    static const char *const messages[] = {
        "", "clangd restarted", "clangd restarted (3)",
        "clangd restarted (4)"
    };
    LspServer server;
    i64 now = 1000;
    size_t i;

    yew_lsp_server_init(&server, 1U, yew_lsp_default_cfg("c"),
                        strdup("/tmp"));
    for (i = 0U; i < 4U; i++) {
        Bytebuf message;

        bytebuf_init(&message);
        YEW_ASSERT(yew_lsp_server_crashed(
            &server, now, "clangd failed", &message));
        YEW_ASSERT_EQ_I64(server.next_try_ms, now + delay[i]);
        YEW_ASSERT_EQ_U64(message.len, strlen(messages[i]));
        YEW_ASSERT_EQ_MEM(message.data, messages[i], message.len);
        bytebuf_free(&message);
        now += 100;
    }
    {
        Bytebuf message;

        bytebuf_init(&message);
        YEW_ASSERT(!yew_lsp_server_crashed(
            &server, now, "fatal: no compile_commands.json", &message));
        bytebuf_push_u8(&message, 0U);
        YEW_ASSERT(server.gave_up);
        YEW_ASSERT(strstr((const char *)message.data,
            "crashed 5 times in 5 minutes") != NULL);
        YEW_ASSERT(strstr((const char *)message.data,
            "fatal: no compile_commands.json") != NULL);
        YEW_ASSERT(strstr((const char *)message.data,
            ":lsp.restart") != NULL);
        bytebuf_free(&message);
    }
    yew_lsp_server_restart_reset(&server);
    YEW_ASSERT_EQ_U64(server.restarts, 0U);
    YEW_ASSERT(!server.gave_up);
    yew_lsp_server_dispose(&server);
}

void test_lsp_lifecycle_restart_window_resets(void)
{
    LspServer server;
    Bytebuf message;

    yew_lsp_server_init(&server, 1U, yew_lsp_default_cfg("go"),
                        strdup("/tmp"));
    bytebuf_init(&message);
    YEW_ASSERT(yew_lsp_server_crashed(&server, 1, "one", &message));
    YEW_ASSERT_EQ_U64(server.restarts, 1U);
    message.len = 0U;
    YEW_ASSERT(yew_lsp_server_crashed(&server, 300001, "two", &message));
    YEW_ASSERT_EQ_U64(server.restarts, 1U);
    YEW_ASSERT_EQ_I64(server.next_try_ms, 300251);
    bytebuf_free(&message);
    yew_lsp_server_dispose(&server);
}

void test_lsp_root_resolution_uses_nearest_marker(void)
{
    char tmp[] = "/tmp/yew-lsp-root-XXXXXX";
    char project[256];
    char source[256];
    char file[256];
    char marker[256];
    char *root;
    int fd;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    (void)snprintf(project, sizeof(project), "%s/project", tmp);
    (void)snprintf(source, sizeof(source), "%s/project/src", tmp);
    (void)snprintf(file, sizeof(file), "%s/project/src/main.c", tmp);
    (void)snprintf(marker, sizeof(marker), "%s/project/.clangd", tmp);
    YEW_ASSERT_EQ_I64(mkdir(project, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(source, 0700), 0);
    fd = open(marker, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    root = yew_lsp_resolve_root(yew_lsp_default_cfg("c"), file, tmp);
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_STR(root, project);
    free(root);
    YEW_ASSERT_EQ_I64(unlink(marker), 0);
    YEW_ASSERT_EQ_I64(rmdir(source), 0);
    YEW_ASSERT_EQ_I64(rmdir(project), 0);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
}

void test_lsp_default_server_table_is_complete(void)
{
    static const char *const languages[] = {
        "c", "cpp", "objc", "rust", "python", "go", "js", "ts",
        "jsx", "tsx", "fortran", "sh"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(languages); i++) {
        const LspServerCfg *cfg = yew_lsp_default_cfg(languages[i]);

        YEW_ASSERT_NOT_NULL(cfg);
        YEW_ASSERT_NOT_NULL(cfg->cmd);
        YEW_ASSERT_NOT_NULL(cfg->roots);
        YEW_ASSERT_EQ_I64(cfg->init_timeout_ms, YEW_RPC_INIT_TIMEOUT_MS);
    }
    YEW_ASSERT_NULL(yew_lsp_default_cfg("fletch"));
    YEW_ASSERT_NULL(yew_lsp_default_cfg("unknown"));
}

typedef struct StaleSeen { u32 calls; } StaleSeen;

static void stale_callback(Ed *ed, void *ctx, const JsonValue *result,
                           const JsonValue *error)
{
    StaleSeen *seen = ctx;
    (void)ed;
    (void)result;
    (void)error;
    seen->calls++;
}

void test_lsp_lifecycle_drops_stale_response_before_callback(void)
{
    Ed ed;
    Buffer buffer;
    Buffer *bufs[1];
    LspServer server;
    RpcPending pending;
    StaleSeen seen;
    Arena arena;
    JsonValue *response;
    u64 id;
    char json[80];

    (void)memset(&ed, 0, sizeof(ed));
    (void)memset(&buffer, 0, sizeof(buffer));
    (void)memset(&server, 0, sizeof(server));
    (void)memset(&pending, 0, sizeof(pending));
    (void)memset(&seen, 0, sizeof(seen));
    buffer.id = 9U;
    buffer.tb = yew_textbuf_from_bytes((const u8 *)"old", 3U);
    bufs[0] = &buffer;
    ed.model_ready = true;
    ed.ws.bufs = bufs;
    ed.ws.nbufs = 1U;
    server.owner = &ed;
    yew_rpc_conn_init(&server.rpc);
    server.rpc_live = true;
    pending.buf_id = buffer.id;
    pending.gen = buffer.tb->gen + 1U;
    pending.cb = stale_callback;
    pending.ctx = &seen;
    id = yew_rpc_call(&server.rpc, "textDocument/hover",
                      (const u8 *)"{}", 2U, &pending);
    (void)snprintf(json, sizeof(json),
                   "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{}}",
                   (unsigned long long)id);
    arena_init(&arena);
    response = parse(&arena, json);
    YEW_ASSERT(!yew_lsp_dispatch_response(&server, response));
    YEW_ASSERT_EQ_U64(seen.calls, 0U);
    YEW_ASSERT_EQ_U64(server.dropped_stale, 1U);
    YEW_ASSERT_NULL(yew_rpc_pending(&server.rpc, id));
    arena_free_all(&arena);
    yew_rpc_conn_free(&server.rpc);
    server.rpc_live = false;
    yew_textbuf_free(buffer.tb);
}

typedef struct LifeFix {
    Ed ed;
    LspServerCfg cfg;
    const char *args[2];
    char helper[4096];
    char path[256];
} LifeFix;

static void helper_path(char *out, size_t cap)
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

static void life_fix_init(LifeFix *f, const char *mode, i32 timeout_ms)
{
    char tmp[] = "/tmp/yew-lsp-life-XXXXXX";
    int fd;

    (void)memset(f, 0, sizeof(*f));
    helper_path(f->helper, sizeof(f->helper));
    fd = mkstemp(tmp);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, "int x;\n", 7U), 7);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    (void)snprintf(f->path, sizeof(f->path), "%s", tmp);
    f->args[0] = mode;
    f->args[1] = NULL;
    f->cfg.id = "fakelsp";
    f->cfg.lang = "c";
    f->cfg.cmd = f->helper;
    f->cfg.args = f->args;
    f->cfg.roots = NULL;
    f->cfg.init_timeout_ms = timeout_ms;
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open(&f->ed, f->path) == YEW_LOAD_OK);
    f->ed.buffer.lang = "c";
    f->ed.now_ms = yew_now_ms();
    YEW_ASSERT(yew_lsp_client_start_cfg(&f->ed, &f->ed.buffer, &f->cfg));
}

static void life_fix_free(LifeFix *f)
{
    yew_ed_free(&f->ed);
    YEW_ASSERT_EQ_I64(unlink(f->path), 0);
}

static void life_pump_once(Ed *ed)
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

static LspServer *life_server(LifeFix *f)
{
    LspDoc *doc = yew_lsp_doc_for_buffer(&f->ed, &f->ed.buffer);

    YEW_ASSERT_NOT_NULL(doc);
    return yew_lsp_server_for_doc(&f->ed, doc);
}

static bool wait_state(LifeFix *f, u8 state, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (life_server(f)->state == state)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_drained(LifeFix *f, i64 timeout_ms)
{
    LspServer *server = life_server(f);
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        YewJob *job = yew_job_find(&f->ed, server->job);

        if (job != NULL && job->drained)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_server_drained(LifeFix *f, LspServer *server,
                                i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        YewJob *job = yew_job_find(&f->ed, server->job);

        if (job != NULL && job->drained)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

void test_lsp_lifecycle_fakelsp_handshake_queue_and_shutdown(void)
{
    LifeFix f;
    LspServer *server;
    LspDoc *doc;
    YewJob *job;
    pid_t pgid;
    pid_t pid;
    static const char transcript[] =
        "seq:initialize\nseq:initialized\nseq:didOpen\n"
        "seq:shutdown\nseq:exit\n";

    life_fix_init(&f, "session-utf8", 1000);
    server = life_server(&f);
    doc = yew_lsp_doc_for_buffer(&f.ed, &f.ed.buffer);
    YEW_ASSERT_EQ_U64(server->state, YEW_LSP_INITIALIZING);
    YEW_ASSERT(!doc->open);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    YEW_ASSERT(doc->open);
    YEW_ASSERT_EQ_U64(server->pos_enc, YEW_POSENC_UTF8);
    job = yew_job_find(&f.ed, server->job);
    YEW_ASSERT_NOT_NULL(job);
    pgid = job->pgid;
    pid = job->pid;
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT_EQ_U64(server->state, YEW_LSP_SHUTTING_DOWN);
    YEW_ASSERT(wait_drained(&f, 2000));
    job = yew_job_find(&f.ed, server->job);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT_EQ_U64(job->state, YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(job->exit_code, 0);
    YEW_ASSERT_EQ_U64(job->framed_err.len, sizeof(transcript) - 1U);
    YEW_ASSERT_EQ_MEM(job->framed_err.data, transcript,
                      sizeof(transcript) - 1U);
    errno = 0;
    YEW_ASSERT(kill(-pgid, 0) == -1 && errno == ESRCH);
    errno = 0;
    YEW_ASSERT(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    life_fix_free(&f);
}

void test_lsp_lifecycle_fakelsp_position_encoding_variants(void)
{
    static const struct { const char *mode; u8 encoding; bool warn; } cases[] = {
        {"session-utf8", YEW_POSENC_UTF8, false},
        {"session-utf16", YEW_POSENC_UTF16, false},
        {"session-absent", YEW_POSENC_UTF16, false},
        {"session-garbage", YEW_POSENC_UTF16, true}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        LifeFix f;
        LspServer *server;

        life_fix_init(&f, cases[i].mode, 1000);
        YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
        server = life_server(&f);
        YEW_ASSERT_EQ_U64(server->pos_enc, cases[i].encoding);
        if (cases[i].warn) {
            YEW_ASSERT(f.ed.msg.active);
            YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_WARN);
            YEW_ASSERT(strstr(f.ed.msg.text,
                              "unknown positionEncoding; using utf-16") !=
                       NULL);
        } else {
            YEW_ASSERT(!f.ed.msg.active);
        }
        yew_lsp_client_stop(&f.ed, server, true);
        YEW_ASSERT(wait_drained(&f, 2000));
        life_fix_free(&f);
    }
}

void test_lsp_lifecycle_fakelsp_initialize_timeout_kills_child(void)
{
    LifeFix f;
    LspServer *server;
    YewJob *job;
    pid_t pgid;

    life_fix_init(&f, "session-timeout", 30);
    server = life_server(&f);
    job = yew_job_find(&f.ed, server->job);
    YEW_ASSERT_NOT_NULL(job);
    pgid = job->pgid;
    YEW_ASSERT(wait_state(&f, YEW_LSP_DEAD, 1000));
    YEW_ASSERT(wait_drained(&f, 2000));
    errno = 0;
    YEW_ASSERT(kill(-pgid, 0) == -1 && errno == ESRCH);
    life_fix_free(&f);
}

static u32 open_fd_count(void)
{
    u32 count = 0U;
    int fd;

    for (fd = 0; fd < 256; fd++)
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
            count++;
    return count;
}

void test_lsp_lifecycle_fakelsp_repeated_sessions_do_not_leak_fds(void)
{
    u32 before = open_fd_count();
    u32 i;

    for (i = 0U; i < 200U; i++) {
        LifeFix f;
        LspServer *server;

        life_fix_init(&f, "session-utf8", 1000);
        YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
        server = life_server(&f);
        yew_lsp_client_stop(&f.ed, server, true);
        YEW_ASSERT(wait_drained(&f, 2000));
        life_fix_free(&f);
    }
    YEW_ASSERT_EQ_U64(open_fd_count(), before);
}

void test_lsp_lifecycle_fakelsp_open_edit_save_close_sequence(void)
{
    static const char transcript[] =
        "seq:initialize\nseq:initialized\nseq:didOpen\n"
        "seq:didChange\nseq:didSave\nseq:didClose\n"
        "seq:shutdown\nseq:exit\n";
    LifeFix f;
    LspServer *server;
    YewJob *job;
    EditCtx ec;

    life_fix_init(&f, "session-sync", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&f.ed, &ec);
    life_pump_once(&f.ed);
    YEW_ASSERT_EQ_U64(yew_ed_file_save(&f.ed, false), YEW_CMD_OK);
    yew_lsp_client_close_buffer(&f.ed, &f.ed.buffer);
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    job = yew_job_find(&f.ed, server->job);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT_EQ_I64(job->exit_code, 0);
    YEW_ASSERT_EQ_U64(job->framed_err.len, sizeof(transcript) - 1U);
    YEW_ASSERT_EQ_MEM(job->framed_err.data, transcript,
                      sizeof(transcript) - 1U);
    life_fix_free(&f);
}

void test_lsp_lifecycle_fakelsp_delayed_response_is_stale(void)
{
    LifeFix f;
    LspServer *server;
    RpcPending pending;
    StaleSeen seen;
    EditCtx ec;
    i64 start;

    life_fix_init(&f, "session-stale", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    (void)memset(&pending, 0, sizeof(pending));
    (void)memset(&seen, 0, sizeof(seen));
    pending.buf_id = f.ed.buffer.id;
    pending.gen = f.ed.buffer.tb->gen;
    pending.sent_ms = f.ed.now_ms;
    pending.cb = stale_callback;
    pending.ctx = &seen;
    YEW_ASSERT(yew_rpc_call(&server->rpc, "textDocument/hover",
                            (const u8 *)"{}", 2U, &pending) != 0U);
    life_pump_once(&f.ed);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&f.ed, &ec);
    start = yew_now_ms();
    while (server->dropped_stale == 0U && yew_now_ms() - start <= 2000)
        life_pump_once(&f.ed);
    YEW_ASSERT_EQ_U64(server->dropped_stale, 1U);
    YEW_ASSERT_EQ_U64(seen.calls, 0U);
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_editor_free_is_graceful(void)
{
    static const char transcript[] =
        "seq:initialize\nseq:initialized\nseq:didOpen\n"
        "seq:shutdown\nseq:exit\n";
    LifeFix f;
    LspServer *server;
    YewJob *job;
    u32 job_id;

    life_fix_init(&f, "session-utf8", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    job_id = server->job;
    yew_lsp_client_free(&f.ed);
    YEW_ASSERT_NULL(f.ed.lsp);
    job = yew_job_find(&f.ed, job_id);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT(job->drained);
    YEW_ASSERT_EQ_U64(job->state, YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(job->exit_code, 0);
    YEW_ASSERT_EQ_U64(job->framed_err.len, sizeof(transcript) - 1U);
    YEW_ASSERT_EQ_MEM(job->framed_err.data, transcript,
                      sizeof(transcript) - 1U);
    life_fix_free(&f);
}
