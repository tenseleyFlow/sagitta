#define _XOPEN_SOURCE 700

#include "harness.h"

#include <dirent.h>
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
#include "mod/lsp/diag.h"
#include "mod/lsp/features.h"
#include "mod/lsp/lsp.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/jumplist.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "term/grid.h"
#include "ui/message.h"
#include "ui/complmenu.h"
#include "ui/picker.h"
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
    struct sigaction saved_sigpipe;
    const char *args[3];
    char helper[4096];
    char path[256];
    char marker[256];
} LifeFix;

static bool wait_server_drained(LifeFix *f, LspServer *server,
                                i64 timeout_ms);
static bool bytes_contain(const u8 *bytes, size_t len, const char *needle);

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
    char marker[] = "/tmp/yew-lsp-restart-XXXXXX";
    struct sigaction ignored;
    int fd;

    (void)memset(f, 0, sizeof(*f));
    (void)memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    YEW_ASSERT_EQ_I64(sigemptyset(&ignored.sa_mask), 0);
    YEW_ASSERT_EQ_I64(sigaction(SIGPIPE, &ignored, &f->saved_sigpipe), 0);
    helper_path(f->helper, sizeof(f->helper));
    fd = mkstemp(tmp);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, "int x;\n", 7U), 7);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    (void)snprintf(f->path, sizeof(f->path), "%s", tmp);
    fd = mkstemp(marker);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_I64(unlink(marker), 0);
    (void)snprintf(f->marker, sizeof(f->marker), "%s", marker);
    f->args[0] = mode;
    f->args[1] = f->marker;
    f->args[2] = NULL;
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
    YEW_ASSERT_NOT_NULL(yew_lsp_doc_for_buffer(&f->ed, &f->ed.buffer));
}

static void life_fix_free(LifeFix *f)
{
    int marker_rc;

    yew_ed_free(&f->ed);
    YEW_ASSERT_EQ_I64(sigaction(SIGPIPE, &f->saved_sigpipe, NULL), 0);
    YEW_ASSERT_EQ_I64(unlink(f->path), 0);
    errno = 0;
    marker_rc = unlink(f->marker);
    YEW_ASSERT(marker_rc == 0 || errno == ENOENT);
}

static void life_marker_write(LifeFix *f, const char *text)
{
    size_t len = strlen(text);
    int fd = open(f->marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  0600);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, len), (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
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

    if (doc == NULL)
        YEW_BUG("LSP lifecycle fixture lost its document");
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

static bool wait_completion_rows(LifeFix *f, size_t rows, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (f->ed.win->compl.items.len == rows)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_panel_title(LifeFix *f, const char *title, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        Panel *panel = &f->ed.win->panel;

        if (panel->open && panel->title != NULL &&
            strcmp(panel->title, title) == 0)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_message(LifeFix *f, const char *message, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (f->ed.msg.active && strcmp(f->ed.msg.text, message) == 0)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_capability_clear(LifeFix *f, u32 capability,
                                  i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (!yew_lsp_has(life_server(f), capability))
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_rpc_tx_empty(LifeFix *f, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (life_server(f)->rpc.tx.pending.len == 0U)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static LspServer *highlight_life_ready(LifeFix *f)
{
    LspServer *server;

    life_fix_init(f, "session-utf8", 1000);
    YEW_ASSERT(wait_state(f, YEW_LSP_READY, 2000));
    YEW_ASSERT(wait_rpc_tx_empty(f, 2000));
    server = life_server(f);
    server->caps.bits |= YEW_LSPC_DOCUMENT_HIGHLIGHT;
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DOCUMENT_HIGHLIGHT));
    YEW_ASSERT_EQ_U64(server->rpc.npending, 0U);
    return server;
}

static u64 highlight_fire(LifeFix *f, i64 elapsed_ms)
{
    i64 armed_at = f->ed.now_ms;

    yew_lsp_highlight_cursor(&f->ed, f->ed.win);
    YEW_ASSERT(f->ed.win->lsp_highlight.timer != YEW_TIMER_NONE);
    f->ed.now_ms = armed_at + elapsed_ms;
    yew_timers_fire(&f->ed.timers, &f->ed, f->ed.now_ms);
    return f->ed.win->lsp_highlight.request;
}

static bool highlight_dispatch(LifeFix *f, u64 id, const char *member)
{
    char json[1024];
    Arena arena;
    int n;
    bool dispatched;

    n = snprintf(json, sizeof(json),
                 "{\"jsonrpc\":\"2.0\",\"id\":%llu,%s}",
                 (unsigned long long)id, member);
    YEW_ASSERT(n >= 0 && (size_t)n < sizeof(json));
    arena_init(&arena);
    dispatched = yew_lsp_dispatch_response(life_server(f),
                                            parse(&arena, json));
    arena_free_all(&arena);
    return dispatched;
}

static void highlight_life_finish(LifeFix *f, LspServer *server)
{
    /* The response is injected directly so these tests can isolate editor
     * lifecycle state while the existing fakelsp waits for shutdown. */
    server->rpc.tx.pending.len = 0U;
    server->rpc.tx.sent = 0U;
    yew_lsp_client_stop(&f->ed, server, true);
    YEW_ASSERT(wait_server_drained(f, server, 2000));
    life_fix_free(f);
}

static bool wait_current_path(LifeFix *f, const char *path, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        Buffer *buffer = yew_ed_doc(&f->ed);

        if (buffer != NULL && buffer->path != NULL &&
            strcmp(buffer->path, path) == 0)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_picker_rows(LifeFix *f, u32 rows, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (yew_picker_active(&f->ed) &&
            yew_picker_total(&f->ed) == rows)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_selected_doc(LifeFix *f, const char *doc, i64 timeout_ms)
{
    i64 start = yew_now_ms();
    size_t len = strlen(doc);

    while (yew_now_ms() - start <= timeout_ms) {
        ComplMenu *menu = &f->ed.win->compl;

        if (menu->sel >= 0 && (size_t)menu->sel < menu->items.len &&
            menu->items.data[menu->sel].doc_len == len &&
            memcmp(menu->items.data[menu->sel].doc, doc, len) == 0)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static bool wait_lsp_shadow(LifeFix *f, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        if (f->ed.win->shadow.live &&
            f->ed.win->shadow.sug.prov == (u8)YEW_SHADOW_LSP)
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

void test_lsp_lifecycle_completion_resolve_cancel_and_shadow(void)
{
    const ShadowProvider *provider = yew_lsp_shadow_provider();
    LifeFix f;
    LspServer *server;
    ShadowReq shadow = {0};
    Key panel = {0};
    Key down = {0};

    life_fix_init(&f, "session-features", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_COMPLETION));
    YEW_ASSERT(server->caps.resolve_completion);
    YEW_ASSERT(yew_lsp_complete(&f.ed, f.ed.win));
    YEW_ASSERT(wait_completion_rows(&f, 2U, 2000));
    YEW_ASSERT_EQ_MEM(f.ed.win->compl.items.data[0].doc,
                      "preliminary", 11U);

    panel.kind = YEW_EV_KEY;
    panel.ev = YEW_KEY_PRESS;
    panel.code = ' ';
    panel.mods = YEW_MOD_CTRL;
    down.kind = YEW_EV_KEY;
    down.ev = YEW_KEY_PRESS;
    down.code = YEW_KEY_DOWN;
    YEW_ASSERT(yew_compl_key(&f.ed, f.ed.win, &panel));
    YEW_ASSERT(f.ed.win->compl.source_resolve != 0U);
    YEW_ASSERT(yew_compl_key(&f.ed, f.ed.win, &down));
    YEW_ASSERT_EQ_I64(f.ed.win->compl.sel, 1);
    YEW_ASSERT_EQ_U64(server->rpc.npending, 1U);
    YEW_ASSERT(wait_selected_doc(&f, "resolved puts", 2000));
    YEW_ASSERT_EQ_MEM(f.ed.win->compl.items.data[1].detail,
                      "void puts", 9U);
    yew_compl_close(&f.ed, f.ed.win);

    YEW_ASSERT_NOT_NULL(provider);
    YEW_ASSERT_EQ_U64(provider->prov, YEW_SHADOW_LSP);
    f.ed.win->shadow.seq_next[YEW_SHADOW_LSP] = 2U;
    shadow.buf_id = f.ed.buffer.id;
    shadow.buf_gen = f.ed.buffer.tb->gen;
    shadow.pos = BYTEOFF(0U);
    shadow.line = yew_textbuf_line_span(f.ed.buffer.tb, LINENO(0U));
    shadow.seq = 1U;
    shadow.prov = (u8)YEW_SHADOW_LSP;
    YEW_ASSERT(provider->request(&f.ed, &shadow));
    YEW_ASSERT(wait_lsp_shadow(&f, 2000));
    YEW_ASSERT_EQ_MEM(f.ed.win->shadow.sug.text, "printf", 6U);
    YEW_ASSERT_EQ_U64(f.ed.win->shadow.sug.len, 6U);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_method_not_found_clears_completion(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-completion-missing", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_COMPLETION));
    YEW_ASSERT(yew_lsp_complete(&f.ed, f.ed.win));
    YEW_ASSERT(wait_capability_clear(&f, YEW_LSPC_COMPLETION, 2000));
    YEW_ASSERT(!f.ed.win->compl.open);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_hover_signature_panels_and_auto_trigger(void)
{
    static const char hover[] = "**int** docs";
    static const char signature[] = "sum(int a, int b)";
    LifeFix f;
    LspServer *server;
    Panel *panel;

    life_fix_init(&f, "session-panels", 1000);
    f.ed.win->rect = (Rect){0U, 0U, 80U, 24U};
    f.ed.win->vp.cols = 80U;
    f.ed.win->vp.rows = 24U;
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_HOVER));
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_SIGNATURE));
    YEW_ASSERT_EQ_STR(server->caps.sig_trigger, "(");

    YEW_ASSERT(yew_lsp_hover(&f.ed, f.ed.win));
    YEW_ASSERT(wait_panel_title(&f, "hover", 2000));
    panel = &f.ed.win->panel;
    YEW_ASSERT_EQ_U64(panel->place, YEW_PANEL_BELOW);
    YEW_ASSERT_EQ_U64(panel->len, sizeof(hover) - 1U);
    YEW_ASSERT_EQ_MEM(panel->body, hover, sizeof(hover) - 1U);
    YEW_ASSERT(panel->mark_live);
    YEW_ASSERT_EQ_U64(panel->mark.lo, 0U);
    YEW_ASSERT_EQ_U64(panel->mark.hi, 3U);
    YEW_ASSERT_EQ_U64(panel->mark_buf_id, f.ed.buffer.id);
    YEW_ASSERT_EQ_U64(panel->mark_buf_gen, f.ed.buffer.tb->gen);
    YEW_ASSERT_EQ_STR(panel->mark_role, "lsp.hover_range");

    yew_lsp_signature_maybe_auto_trigger(&f.ed, f.ed.win,
                                          (const u8 *)"(", 1U);
    YEW_ASSERT(f.ed.win->panel_source_request != 0U);
    YEW_ASSERT(wait_panel_title(&f, "signature", 2000));
    panel = &f.ed.win->panel;
    YEW_ASSERT_EQ_U64(panel->place, YEW_PANEL_ABOVE);
    YEW_ASSERT_EQ_U64(panel->len, sizeof(signature) - 1U);
    YEW_ASSERT_EQ_MEM(panel->body, signature, sizeof(signature) - 1U);
    YEW_ASSERT_EQ_U64(panel->emph.len, 1U);
    YEW_ASSERT_EQ_U64(panel->emph.data[0].lo, 4U);
    YEW_ASSERT_EQ_U64(panel->emph.data[0].hi, 9U);
    YEW_ASSERT(!panel->mark_live);

    YEW_ASSERT(yew_lsp_hover(&f.ed, f.ed.win));
    YEW_ASSERT(wait_message(&f, "no hover information here", 2000));
    YEW_ASSERT(!f.ed.win->panel.open);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_method_not_found_clears_panel_capabilities(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-panel-missing", 1000);
    f.ed.win->rect = (Rect){0U, 0U, 80U, 24U};
    f.ed.win->vp.cols = 80U;
    f.ed.win->vp.rows = 24U;
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_HOVER));
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_SIGNATURE));

    YEW_ASSERT(yew_lsp_hover(&f.ed, f.ed.win));
    YEW_ASSERT(wait_capability_clear(&f, YEW_LSPC_HOVER, 2000));
    YEW_ASSERT(yew_lsp_signature(&f.ed, f.ed.win));
    YEW_ASSERT(wait_capability_clear(&f, YEW_LSPC_SIGNATURE, 2000));
    YEW_ASSERT(!f.ed.win->panel.open);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_definition_response_jumps_to_single_location(void)
{
    LifeFix f;
    LspServer *server;
    Cursor *cursor;

    life_fix_init(&f, "session-definition", 1000);
    life_marker_write(&f, "abcdef\n");
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DEFINITION));
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_REFERENCES));
    cursor = yew_ed_cursor(&f.ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(1U);
    cursor->anchor = cursor->pos;

    YEW_ASSERT(yew_lsp_goto_definition(&f.ed, f.ed.win));
    YEW_ASSERT(wait_current_path(&f, f.marker, 2000));
    cursor = yew_ed_cursor(&f.ed);
    YEW_ASSERT_NOT_NULL(cursor);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 2U);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&f.ed.win->jumps), 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->nav_source_request, 0U);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_references_response_opens_multi_location_picker(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-references", 1000);
    life_marker_write(&f, "abcdef\n");
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 24U, 80U));
    f.ed.grid_ready = true;
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_REFERENCES));

    YEW_ASSERT(yew_lsp_references(&f.ed, f.ed.win));
    YEW_ASSERT(wait_picker_rows(&f, 2U, 2000));
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&f.ed.win->jumps), 0U);
    yew_picker_close(&f.ed, false);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&f.ed.win->jumps), 0U);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_empty_definition_reports_exact_info_message(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-definition-empty", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);

    YEW_ASSERT(yew_lsp_goto_definition(&f.ed, f.ed.win));
    YEW_ASSERT(wait_message(&f, "fakelsp: no definition found", 2000));
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_INFO);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&f.ed.win->jumps), 0U);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_method_not_found_clears_definition_capability(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-definition-missing", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DEFINITION));

    YEW_ASSERT(yew_lsp_goto_definition(&f.ed, f.ed.win));
    YEW_ASSERT(wait_capability_clear(&f, YEW_LSPC_DEFINITION, 2000));
    YEW_ASSERT(!yew_lsp_goto_definition(&f.ed, f.ed.win));
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "fakelsp does not support definition");

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_document_symbols_open_nested_picker(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-symbols", 1000);
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 24U, 80U));
    f.ed.grid_ready = true;
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DOCUMENT_SYMBOL));

    YEW_ASSERT(yew_lsp_symbols(&f.ed, f.ed.win));
    YEW_ASSERT(wait_picker_rows(&f, 2U, 2000));
    YEW_ASSERT_EQ_U64(f.ed.win->symbol_source_request, 0U);
    yew_picker_close(&f.ed, false);

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_empty_document_symbols_report_exact_info_message(void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-symbols-empty", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DOCUMENT_SYMBOL));

    YEW_ASSERT(yew_lsp_symbols(&f.ed, f.ed.win));
    YEW_ASSERT(wait_message(&f, "fakelsp: no document symbols found",
                            2000));
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_INFO);
    YEW_ASSERT(!yew_picker_active(&f.ed));

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_method_not_found_clears_document_symbol_capability(
    void)
{
    LifeFix f;
    LspServer *server;

    life_fix_init(&f, "session-symbols-missing", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    YEW_ASSERT(yew_lsp_has(server, YEW_LSPC_DOCUMENT_SYMBOL));

    YEW_ASSERT(yew_lsp_symbols(&f.ed, f.ed.win));
    YEW_ASSERT(wait_capability_clear(&f, YEW_LSPC_DOCUMENT_SYMBOL, 2000));
    YEW_ASSERT(!yew_lsp_symbols(&f.ed, f.ed.win));
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "fakelsp does not support document symbols");

    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_document_highlight_waits_for_idle(void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    i64 armed_at = f.ed.now_ms;
    const u8 *tx = NULL;
    u64 tx_len;
    u64 request;

    yew_lsp_highlight_cursor(&f.ed, f.ed.win);
    YEW_ASSERT(f.ed.win->lsp_highlight.timer != YEW_TIMER_NONE);
    yew_timers_fire(&f.ed.timers, &f.ed, armed_at + 299);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.request, 0U);
    YEW_ASSERT_EQ_U64(server->rpc.npending, 0U);

    f.ed.now_ms = armed_at + 300;
    yew_timers_fire(&f.ed.timers, &f.ed, f.ed.now_ms);
    request = f.ed.win->lsp_highlight.request;
    YEW_ASSERT(request != 0U);
    YEW_ASSERT_EQ_U64(server->rpc.npending, 1U);
    tx_len = yew_rpc_tx_view(&server->rpc, &tx);
    YEW_ASSERT(tx_len != 0U);
    YEW_ASSERT_NOT_NULL(tx);
    YEW_ASSERT(bytes_contain(
        tx, tx_len, "\"method\":\"textDocument/documentHighlight\""));
    YEW_ASSERT(highlight_dispatch(&f, request, "\"result\":[]"));
    highlight_life_finish(&f, server);
}

void test_lsp_lifecycle_document_highlight_separates_read_and_write_overlays(
    void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    u64 request = highlight_fire(&f, 300);

    YEW_ASSERT(request != 0U);
    YEW_ASSERT(highlight_dispatch(&f, request,
        "\"result\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},"
        "\"kind\":2},{\"range\":{\"start\":{\"line\":0,"
        "\"character\":4},\"end\":{\"line\":0,\"character\":5}},"
        "\"kind\":3}]"));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.data[0].lo, 0U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.data[0].hi, 3U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.data[0].lo, 4U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.data[0].hi, 5U);
    YEW_ASSERT_EQ_U64(f.ed.win->overlay.spans.len, 0U);
    highlight_life_finish(&f, server);
}

void test_lsp_lifecycle_document_highlight_edit_clears_and_cancels_stale_response(
    void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    Cursor *cursor;
    EditCtx edit;
    u64 request = highlight_fire(&f, 300);
    u64 stale;

    YEW_ASSERT(highlight_dispatch(&f, request,
        "\"result\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},"
        "\"kind\":2}]"));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.len, 1U);
    cursor = yew_ed_cursor(&f.ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(1U);
    cursor->anchor = cursor->pos;
    stale = highlight_fire(&f, 300);
    YEW_ASSERT(stale != 0U);
    YEW_ASSERT_NOT_NULL(yew_rpc_pending(&server->rpc, stale));

    edit = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)" ", 1U));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.len, 0U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.len, 0U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.request, 0U);
    YEW_ASSERT_NULL(yew_rpc_pending(&server->rpc, stale));
    YEW_ASSERT(!highlight_dispatch(&f, stale,
        "\"result\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
        "\"kind\":3}]"));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.len, 0U);
    yew_ed_finish_edit(&f.ed, &edit);
    highlight_life_finish(&f, server);
}

void test_lsp_lifecycle_document_highlight_cursor_rearm_rejects_stale_response(
    void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    Cursor *cursor;
    u64 stale = highlight_fire(&f, 300);
    u64 current;

    YEW_ASSERT(stale != 0U);
    cursor = yew_ed_cursor(&f.ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(1U);
    cursor->anchor = cursor->pos;
    yew_lsp_highlight_cursor(&f.ed, f.ed.win);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.request, 0U);
    YEW_ASSERT(f.ed.win->lsp_highlight.timer != YEW_TIMER_NONE);
    YEW_ASSERT(!highlight_dispatch(&f, stale,
        "\"result\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},"
        "\"kind\":2}]"));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.len, 0U);

    f.ed.now_ms += 300;
    yew_timers_fire(&f.ed.timers, &f.ed, f.ed.now_ms);
    current = f.ed.win->lsp_highlight.request;
    YEW_ASSERT(current != 0U);
    YEW_ASSERT(current != stale);
    YEW_ASSERT(highlight_dispatch(&f, current,
        "\"result\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":4},\"end\":{\"line\":0,\"character\":5}},"
        "\"kind\":3}]"));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.len, 1U);
    highlight_life_finish(&f, server);
}

void test_lsp_lifecycle_document_highlight_focus_return_rearms(void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    Win *win = f.ed.win;
    u64 stale = highlight_fire(&f, 300);
    u64 current;

    YEW_ASSERT(stale != 0U);
    f.ed.win = NULL;
    YEW_ASSERT(highlight_dispatch(&f, stale,
        "\"result\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},"
        "\"kind\":2}]"));
    YEW_ASSERT_EQ_U64(win->lsp_highlight.request, 0U);
    YEW_ASSERT(!win->lsp_highlight.cursor_valid);
    YEW_ASSERT_EQ_U64(win->lsp_highlight.read.spans.len, 0U);

    f.ed.win = win;
    yew_lsp_highlight_cursor(&f.ed, win);
    YEW_ASSERT(win->lsp_highlight.timer != YEW_TIMER_NONE);
    f.ed.now_ms += 300;
    yew_timers_fire(&f.ed.timers, &f.ed, f.ed.now_ms);
    current = win->lsp_highlight.request;
    YEW_ASSERT(current != 0U);
    YEW_ASSERT(current != stale);
    YEW_ASSERT(highlight_dispatch(&f, current, "\"result\":[]"));
    highlight_life_finish(&f, server);
}

void test_lsp_lifecycle_method_not_found_clears_highlight_capability(void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    u64 request = highlight_fire(&f, 300);

    YEW_ASSERT(request != 0U);
    YEW_ASSERT(highlight_dispatch(&f, request,
        "\"error\":{\"code\":-32601,\"message\":\"missing\"}"));
    YEW_ASSERT(!yew_lsp_has(server, YEW_LSPC_DOCUMENT_HIGHLIGHT));
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.request, 0U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.read.spans.len, 0U);
    YEW_ASSERT_EQ_U64(f.ed.win->lsp_highlight.write.spans.len, 0U);
    highlight_life_finish(&f, server);
}

void test_lsp_lifecycle_unfocused_method_not_found_clears_highlight_capability(
    void)
{
    LifeFix f;
    LspServer *server = highlight_life_ready(&f);
    Win *win = f.ed.win;
    u64 request = highlight_fire(&f, 300);

    YEW_ASSERT(request != 0U);
    f.ed.win = NULL;
    YEW_ASSERT(highlight_dispatch(&f, request,
        "\"error\":{\"code\":-32601,\"message\":\"missing\"}"));
    YEW_ASSERT(!yew_lsp_has(server, YEW_LSPC_DOCUMENT_HIGHLIGHT));
    YEW_ASSERT_EQ_U64(win->lsp_highlight.request, 0U);
    YEW_ASSERT(!win->lsp_highlight.cursor_valid);
    YEW_ASSERT_EQ_U64(win->lsp_highlight.read.spans.len, 0U);
    YEW_ASSERT_EQ_U64(win->lsp_highlight.write.spans.len, 0U);
    f.ed.win = win;
    highlight_life_finish(&f, server);
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

void test_lsp_lifecycle_fakelsp_rejects_missing_sync(void)
{
    LifeFix f;
    LspServer *server;

    yew_test_capture_log();
    life_fix_init(&f, "session-nosync", 1000);
    server = life_server(&f);
    YEW_ASSERT(wait_state(&f, YEW_LSP_DEAD, 2000));
    YEW_ASSERT(f.ed.msg.active);
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT(strstr(f.ed.msg.text,
                      "does not support document synchronization") != NULL);
    YEW_ASSERT(strstr(f.ed.msg.text, "LSP is off for this workspace") != NULL);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR,
                                     "does not support document synchronization"));
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
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
    DIR *dir = opendir("/proc/self/fd");
    u32 count = 0U;

    if (dir != NULL) {
        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL)
            if (entry->d_name[0] != '.')
                count++;
        (void)closedir(dir);
        return count;
    }
    {
        int fd;

        for (fd = 0; fd < 256; fd++)
            if (fcntl(fd, F_GETFD) != -1)
                count++;
    }
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

void test_lsp_lifecycle_language_change_closes_document(void)
{
    static const char transcript[] =
        "seq:initialize\nseq:initialized\nseq:didOpen\n"
        "seq:didClose\nseq:shutdown\nseq:exit\n";
    LifeFix f;
    LspServer *server;
    YewJob *job;
    CmdCtx cx;
    Win win;

    life_fix_init(&f, "session-close", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    (void)memset(&cx, 0, sizeof(cx));
    (void)memset(&win, 0, sizeof(win));
    win.buf = &f.ed.buffer;
    cx.ed = &f.ed;
    cx.win = &win;
    cx.sarg = "none";
    cx.sarg_len = 4U;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64(yew_cmd_invoke(yew_cmd_lookup("ed.syn.set", 10U),
                                     &cx), YEW_CMD_OK);
    YEW_ASSERT_NULL(f.ed.buffer.lang);
    YEW_ASSERT_NULL(yew_lsp_doc_for_buffer(&f.ed, &f.ed.buffer));
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    job = yew_job_find(&f.ed, server->job);
    YEW_ASSERT_NOT_NULL(job);
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

static bool bytes_contain(const u8 *bytes, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    if (n > len)
        return false;
    for (i = 0U; i + n <= len; i++)
        if (memcmp(bytes + i, needle, n) == 0)
            return true;
    return false;
}

static void dispatch_diag(LifeFix *f, LspServer *server, i64 version)
{
    LspDoc *doc = yew_lsp_doc_for_buffer(&f->ed, &f->ed.buffer);
    char json[1024];
    char version_field[48];
    Arena arena;
    JsonValue *value;
    int n;

    YEW_ASSERT_NOT_NULL(doc);
    if (version < 0)
        version_field[0] = '\0';
    else
        (void)snprintf(version_field, sizeof(version_field),
                       "\"version\":%lld,", (long long)version);
    n = snprintf(json, sizeof(json),
        "{\"jsonrpc\":\"2.0\","
        "\"method\":\"textDocument/publishDiagnostics\","
        "\"params\":{\"uri\":\"%s\",%s"
        "\"diagnostics\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},"
        "\"severity\":1,\"message\":\"boom\"}]}}",
        doc->uri, version_field);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(json));
    arena_init(&arena);
    value = parse(&arena, json);
    yew_lsp_server_dispatch_value(server, value);
    arena_free_all(&arena);
}

void test_lsp_lifecycle_unversioned_diagnostics_track_flush(void)
{
    LifeFix f;
    LspServer *server;
    LspDoc *doc;
    EditCtx ec;

    life_fix_init(&f, "session-unversioned", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    doc = yew_lsp_doc_for_buffer(&f.ed, &f.ed.buffer);
    YEW_ASSERT_NOT_NULL(doc);
    YEW_ASSERT_EQ_U64(doc->sent_gen, f.ed.buffer.tb->gen);
    dispatch_diag(&f, server, -1);
    YEW_ASSERT_NOT_NULL(f.ed.buffer.diag);
    YEW_ASSERT(!f.ed.buffer.diag->stale);

    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&f.ed, &ec);
    YEW_ASSERT(doc->sent_gen != f.ed.buffer.tb->gen);
    dispatch_diag(&f, server, -1);
    YEW_ASSERT(f.ed.buffer.diag->stale);
    life_pump_once(&f.ed);
    YEW_ASSERT_EQ_U64(doc->sent_gen, f.ed.buffer.tb->gen);
    dispatch_diag(&f, server, -1);
    YEW_ASSERT(!f.ed.buffer.diag->stale);
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

static bool wait_new_ready(LifeFix *f, u32 old_job, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        LspServer *server = life_server(f);
        LspDoc *doc = yew_lsp_doc_for_buffer(&f->ed, &f->ed.buffer);

        if (server->job != old_job && server->state == YEW_LSP_READY &&
            doc != NULL && doc->open)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

void test_lsp_lifecycle_diagnostics_follow_live_document_version(void)
{
    LifeFix f;
    LspServer *server;
    LspDoc *doc;
    u32 old_job;

    life_fix_init(&f, "session-utf8", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    doc = yew_lsp_doc_for_buffer(&f.ed, &f.ed.buffer);
    YEW_ASSERT_NOT_NULL(doc);
    YEW_ASSERT_EQ_I64(doc->version, 1);
    dispatch_diag(&f, server, 0);
    YEW_ASSERT_NULL(f.ed.buffer.diag);
    dispatch_diag(&f, server, 1);
    YEW_ASSERT_NOT_NULL(f.ed.buffer.diag);
    YEW_ASSERT_EQ_U64(f.ed.buffer.diag->d.len, 1U);
    doc->full_sync = true;
    doc->insert_waiting = true;
    Vec_LspChange_push(&doc->pending, ((LspChange){0}));
    old_job = server->job;
    YEW_ASSERT(yew_lsp_client_restart(&f.ed, &f.ed.buffer));
    YEW_ASSERT_NULL(f.ed.buffer.diag);
    YEW_ASSERT_EQ_I64(doc->version, 0);
    YEW_ASSERT(!doc->open);
    YEW_ASSERT(!doc->full_sync);
    YEW_ASSERT(!doc->insert_waiting);
    YEW_ASSERT_EQ_U64(doc->pending.len, 0U);
    YEW_ASSERT(wait_new_ready(&f, old_job, 3000));
    YEW_ASSERT_EQ_I64(doc->version, 1);
    dispatch_diag(&f, server, 1);
    YEW_ASSERT_NOT_NULL(f.ed.buffer.diag);
    YEW_ASSERT_EQ_U64(f.ed.buffer.diag->d.len, 1U);
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_drained(&f, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_automatic_restart_reopens_documents(void)
{
    LifeFix f;
    LspServer *server;
    LspDoc *doc;
    u32 old_job;

    life_fix_init(&f, "session-crash-restart", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    doc = yew_lsp_doc_for_buffer(&f.ed, &f.ed.buffer);
    YEW_ASSERT_NOT_NULL(doc);
    dispatch_diag(&f, server, 1);
    YEW_ASSERT_NOT_NULL(f.ed.buffer.diag);
    {
        YewJob *job = yew_job_find(&f.ed, server->job);

        YEW_ASSERT_NOT_NULL(job);
        YEW_ASSERT_EQ_I64(kill(-job->pgid, SIGKILL), 0);
    }
    doc->full_sync = true;
    Vec_LspChange_push(&doc->pending, ((LspChange){0}));
    old_job = server->job;
    YEW_ASSERT(wait_new_ready(&f, old_job, 3000));
    YEW_ASSERT_NULL(f.ed.buffer.diag);
    YEW_ASSERT(doc->open);
    YEW_ASSERT_EQ_I64(doc->version, 1);
    YEW_ASSERT(!doc->full_sync);
    YEW_ASSERT(!doc->insert_waiting);
    YEW_ASSERT_EQ_U64(doc->pending.len, 0U);
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_drained(&f, 2000));
    life_fix_free(&f);
}

static bool wait_restart_notice(LifeFix *f, u32 restarts, i64 timeout_ms)
{
    i64 start = yew_now_ms();

    while (yew_now_ms() - start <= timeout_ms) {
        LspServer *server = life_server(f);

        if (server->restarts == restarts && f->ed.msg.active)
            return true;
        life_pump_once(&f->ed);
    }
    return false;
}

static void kill_life_server(LifeFix *f, LspServer *server)
{
    YewJob *job = yew_job_find(&f->ed, server->job);

    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT_EQ_I64(kill(-job->pgid, SIGKILL), 0);
}

void test_lsp_lifecycle_restart_message_severities(void)
{
    LifeFix f;
    LspServer *server;
    u32 old_job;

    life_fix_init(&f, "session-utf8", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    server->restarts = 1U;
    server->first_restart_ms = f.ed.now_ms;
    old_job = server->job;
    kill_life_server(&f, server);
    YEW_ASSERT(wait_restart_notice(&f, 2U, 2000));
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_INFO);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "fakelsp restarted");
    server->next_try_ms = f.ed.now_ms;
    YEW_ASSERT(wait_new_ready(&f, old_job, 2000));

    yew_msg_clear(&f.ed);
    server->restarts = 2U;
    server->first_restart_ms = f.ed.now_ms;
    old_job = server->job;
    kill_life_server(&f, server);
    YEW_ASSERT(wait_restart_notice(&f, 3U, 2000));
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_WARN);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "fakelsp restarted (3)");
    server->next_try_ms = f.ed.now_ms;
    YEW_ASSERT(wait_new_ready(&f, old_job, 2000));
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_server_drained(&f, server, 2000));
    life_fix_free(&f);
}

void test_lsp_lifecycle_stop_escalates_to_sigkill(void)
{
    LifeFix f;
    LspServer *server;
    YewJob *job;
    i64 start;

    life_fix_init(&f, "session-resistant", 1000);
    YEW_ASSERT(wait_state(&f, YEW_LSP_READY, 2000));
    server = life_server(&f);
    start = yew_now_ms();
    yew_lsp_client_stop(&f.ed, server, true);
    YEW_ASSERT(wait_drained(&f, 2000));
    YEW_ASSERT(yew_now_ms() - start >= 350);
    job = yew_job_find(&f.ed, server->job);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT_EQ_U64(job->state, YEW_JOB_SIGNALED);
    YEW_ASSERT_EQ_I64(job->termsig, SIGKILL);
    life_fix_free(&f);
}

void test_lsp_server_requests_receive_protocol_results(void)
{
    LspServer server;
    Arena arena;
    JsonValue *request;

    (void)memset(&server, 0, sizeof(server));
    yew_rpc_conn_init(&server.rpc);
    server.rpc_live = true;
    arena_init(&arena);
    request = parse(&arena,
        "{\"jsonrpc\":\"2.0\",\"id\":7,"
        "\"method\":\"workspace/configuration\","
        "\"params\":{\"items\":[{},{},{}]}}"
    );
    yew_lsp_server_dispatch_value(&server, request);
    YEW_ASSERT(bytes_contain(server.rpc.tx.pending.data,
                             server.rpc.tx.pending.len,
                             "\"id\":7,\"result\":[null,null,null]"));
    yew_rpctx_consume(&server.rpc.tx, server.rpc.tx.pending.len);
    arena_free_all(&arena);
    arena_init(&arena);
    request = parse(&arena,
        "{\"jsonrpc\":\"2.0\",\"id\":8,"
        "\"method\":\"window/workDoneProgress/create\","
        "\"params\":{\"token\":1}}"
    );
    yew_lsp_server_dispatch_value(&server, request);
    YEW_ASSERT(bytes_contain(server.rpc.tx.pending.data,
                             server.rpc.tx.pending.len,
                             "\"id\":8,\"result\":null"));
    arena_free_all(&arena);
    yew_rpc_conn_free(&server.rpc);
    server.rpc_live = false;
}
