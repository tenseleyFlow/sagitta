#define _XOPEN_SOURCE 700

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mod/lsp/client.h"
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
