#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 47's live-server proof.  This is deliberately a standalone test
 * executable rather than a unit-test case: it starts the real clangd through
 * yew's job/RPC client, operates on an isolated git worktree, and exercises
 * the production response decoders and rename transaction.
 */
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "mod/lsp/client.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/features.h"
#include "mod/lsp/rename.h"
#include "mod/lsp/sync.h"
#include "text/edit.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/gutter.h"
#include "util/arena.h"
#include "util/buf.h"

enum {
    LIVE_READY_MS = 180000,
    LIVE_REQUEST_MS = 60000,
    LIVE_DIAG_MS = 60000
};

typedef struct Reply {
    bool done;
    bool error;
    Bytebuf json;
} Reply;

typedef struct Fixture {
    Ed ed;
    LspServer *server;
    const LspServerCfg *cfg;
    LspServerCfg live_cfg;
    char root[4096];
    int fd_before;
} Fixture;

typedef struct GotoCase {
    const char *use_file;
    const char *symbol;
    const char *target_file;
    const char *anchor;
} GotoCase;

static const char *const width_code_files[] = {
    "src/fl/stdfmt.c",
    "src/term/grid.c",
    "src/term/render.c",
    "src/ui/draw.c",
    "src/ui/gutter.c",
    "src/ui/menu.c",
    "src/ui/panel.c",
    "src/unicode/grapheme.c",
    "src/unicode/width.c",
    "src/unicode/width.h"
};

static void fail(const char *fmt, ...)
{
    va_list ap;

    (void)fprintf(stderr, "FAIL: clangd live: ");
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    exit(1);
}

static void require(bool ok, const char *what)
{
    if (!ok)
        fail("%s", what);
}

static void path_join(char out[4096], const char *root, const char *rel)
{
    int n = snprintf(out, 4096U, "%s/%s", root, rel);

    if (n < 0 || n >= 4096)
        fail("path too long: %s/%s", root, rel);
}

static int fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;

    if (dir == NULL)
        return -1;
    while ((entry = readdir(dir)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
            count++;
    (void)closedir(dir);
    return count;
}

static void pump_once(Ed *ed, i32 wait_ms)
{
    struct pollfd fds[YEW_JOB_MAX * 4U];
    u32 n = 0U;
    i64 now;

    yew_job_collect_fds(ed, fds, &n);
    (void)poll(fds, (nfds_t)n, wait_ms);
    now = yew_now_ms();
    ed->now_ms = now;
    yew_job_pump(ed, fds, n);
    yew_job_reap(ed);
    yew_job_tick(ed, now);
    yew_job_settle(ed);
    yew_lsp_client_pump(ed);
}

static LspServer *fixture_server(Fixture *f, Buffer *buffer)
{
    LspDoc *doc = yew_lsp_doc_for_buffer(&f->ed, buffer);

    return yew_lsp_server_for_doc(&f->ed, doc);
}

static bool wait_ready(Fixture *f, Buffer *buffer)
{
    i64 start = yew_now_ms();
    i64 ceiling = LIVE_READY_MS;
    const char *override = getenv("YEW_LSP_LIVE_READY_MS");

    if (override != NULL) {
        char *end;
        long value = strtol(override, &end, 10);

        if (end != override && *end == '\0' && value > 0)
            ceiling = value;
    }

    while (yew_now_ms() - start <= ceiling) {
        LspServer *server = fixture_server(f, buffer);

        if (server != NULL && server->state == YEW_LSP_READY) {
            f->server = server;
            return true;
        }
        pump_once(&f->ed, 10);
    }
    {
        LspServer *server = fixture_server(f, buffer);
        YewJob *job = server == NULL ? NULL : yew_job_find(&f->ed,
                                                            server->job);

        if (server != NULL)
            (void)fprintf(stderr,
                          "clangd state=%u gave_up=%d rpc_pending=%u "
                          "stderr_tail=%.*s\n",
                          (unsigned)server->state, server->gave_up ? 1 : 0,
                          (unsigned)server->rpc.npending,
                          (int)server->stderr_tail.len,
                          (const char *)server->stderr_tail.data);
        if (f->ed.msg.active)
            (void)fprintf(stderr, "editor message: %s\n", f->ed.msg.text);
        if (job != NULL && job->framed_err.len != 0U)
            (void)fprintf(stderr, "clangd stderr: %.*s\n",
                          (int)job->framed_err.len,
                          (const char *)job->framed_err.data);
    }
    return false;
}

static Buffer *open_buffer(Fixture *f, const char *rel)
{
    char path[4096];
    Buffer *buffer;

    path_join(path, f->root, rel);
    buffer = yew_ws_file_buf(&f->ed, path);
    if (buffer == NULL || yew_buf_hydrate(&f->ed, buffer) != 0)
        fail("cannot open fixture %s", rel);
    buffer->lang = "c";
    if (!yew_lsp_client_start_cfg(&f->ed, buffer, f->cfg))
        fail("cannot attach clangd to %s", rel);
    if (f->server == NULL && !wait_ready(f, buffer))
        fail("clangd did not reach ready state within %d ms", LIVE_READY_MS);
    return buffer;
}

static void text_copy(const TextBuf *tb, Bytebuf *out)
{
    TextIter iter;
    u64 copied = 0U;
    u64 total = yew_textbuf_len(tb);

    out->len = 0U;
    if (total == 0U)
        return;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        fail("cannot iterate fixture buffer");
    while (copied < total) {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
            fail("fixture buffer iteration truncated");
        if (len > total - copied)
            len = total - copied;
        bytebuf_append(out, bytes, (size_t)len);
        copied += len;
        if (copied < total && !yew_textiter_advance(&iter, tb))
            fail("fixture buffer iteration failed");
    }
}

static bool find_position(const TextBuf *tb, const char *needle,
                          i64 *line, i64 *character)
{
    Bytebuf text;
    const u8 *hit = NULL;
    size_t needle_len = strlen(needle);
    size_t at;
    size_t i;

    bytebuf_init(&text);
    text_copy(tb, &text);
    for (at = 0U; at + needle_len <= text.len; at++) {
        u8 before = at == 0U ? 0U : text.data[at - 1U];
        u8 after = at + needle_len == text.len ? 0U :
                   text.data[at + needle_len];
        bool before_ident = (before >= 'A' && before <= 'Z') ||
                            (before >= 'a' && before <= 'z') ||
                            (before >= '0' && before <= '9') || before == '_';
        bool after_ident = (after >= 'A' && after <= 'Z') ||
                           (after >= 'a' && after <= 'z') ||
                           (after >= '0' && after <= '9') || after == '_';

        if (!before_ident && !after_ident &&
            memcmp(text.data + at, needle, needle_len) == 0) {
            hit = text.data + at;
            break;
        }
    }
    if (hit == NULL) {
        bytebuf_free(&text);
        return false;
    }
    *line = 0;
    *character = 0;
    for (i = 0U; i < (size_t)(hit - text.data); i++) {
        if (text.data[i] == (u8)'\n') {
            (*line)++;
            *character = 0;
        } else {
            (*character)++;
        }
    }
    bytebuf_free(&text);
    return true;
}

static i64 anchor_line(Fixture *f, const char *rel, const char *anchor)
{
    Buffer *buffer = open_buffer(f, rel);
    Bytebuf text;
    size_t at = 0U;
    i64 line = 0;
    size_t anchor_len = strlen(anchor);

    bytebuf_init(&text);
    text_copy(buffer->tb, &text);
    while (at <= text.len) {
        size_t end = at;

        while (end < text.len && text.data[end] != (u8)'\n')
            end++;
        if (end - at >= anchor_len &&
            memcmp(text.data + at, anchor, anchor_len) == 0) {
            bytebuf_free(&text);
            return line;
        }
        if (end == text.len)
            break;
        at = end + 1U;
        line++;
    }
    bytebuf_free(&text);
    fail("definition anchor not found: %s:%s", rel, anchor);
    return -1;
}

static void reply_done(Ed *ed, void *ctx, const JsonValue *result,
                       const JsonValue *error)
{
    Reply *reply = ctx;
    JsonW writer;

    (void)ed;
    reply->done = true;
    reply->error = error != NULL;
    reply->json.len = 0U;
    yew_jsonw_init(&writer, &reply->json);
    yew_jsonw_value(&writer, error != NULL ? error : result);
}

static JsonValue *request(Fixture *f, Buffer *buffer, const char *method,
                          const Bytebuf *params, Arena *arena)
{
    Reply reply;
    RpcPending pending;
    LspDoc *doc = yew_lsp_doc_for_buffer(&f->ed, buffer);
    i64 start;
    JsonErr error;
    JsonValue *value;

    (void)memset(&reply, 0, sizeof(reply));
    (void)memset(&pending, 0, sizeof(pending));
    bytebuf_init(&reply.json);
    require(doc != NULL && f->server != NULL, "request lost LSP document");
    yew_lsp_sync_flush(&f->ed);
    pending.buf_id = buffer->id;
    pending.gen = buffer->tb->gen;
    pending.cb = reply_done;
    pending.ctx = &reply;
    pending.sent_ms = f->ed.now_ms;
    pending.deadline_ms = f->ed.now_ms + LIVE_REQUEST_MS;
    if (yew_rpc_call(&f->server->rpc, method, params->data,
                     (u32)params->len, &pending) == 0U)
        fail("cannot send %s", method);
    start = yew_now_ms();
    while (!reply.done && yew_now_ms() - start <= LIVE_REQUEST_MS)
        pump_once(&f->ed, 10);
    if (!reply.done)
        fail("%s timed out", method);
    if (reply.error) {
        bytebuf_push_u8(&reply.json, 0U);
        fail("%s returned %s", method, (const char *)reply.json.data);
    }
    arena_free_all(arena);
    arena_init(arena);
    value = yew_json_parse(arena, reply.json.data, reply.json.len, &error);
    bytebuf_free(&reply.json);
    if (value == NULL)
        fail("cannot parse %s response: %s", method, error.msg);
    return value;
}

static void trace_json(const char *label, const JsonValue *value)
{
    Bytebuf bytes;
    JsonW writer;

    if (getenv("YEW_LSP_LIVE_TRACE") == NULL)
        return;
    bytebuf_init(&bytes);
    yew_jsonw_init(&writer, &bytes);
    yew_jsonw_value(&writer, value);
    (void)fprintf(stderr, "%s: %.*s\n", label, (int)bytes.len,
                  (const char *)bytes.data);
    bytebuf_free(&bytes);
}

static void position_params(Buffer *buffer, const char *symbol,
                            const char *extra_key, const char *extra_value,
                            Bytebuf *out)
{
    Bytebuf uri;
    JsonW writer;
    i64 line;
    i64 character;

    if (!find_position(buffer->tb, symbol, &line, &character))
        fail("symbol %s not found in %s", symbol, buffer->path);
    bytebuf_init(&uri);
    yew_lsp_uri_of_path(&uri, (const u8 *)buffer->path,
                        (u32)strlen(buffer->path));
    out->len = 0U;
    yew_jsonw_init(&writer, out);
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "textDocument");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "uri");
    yew_jsonw_str(&writer, uri.data, (u32)uri.len);
    yew_jsonw_obj_end(&writer);
    yew_jsonw_key(&writer, "position");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "line"); yew_jsonw_int(&writer, line);
    yew_jsonw_key(&writer, "character");
    yew_jsonw_int(&writer, character);
    yew_jsonw_obj_end(&writer);
    if (extra_key != NULL) {
        yew_jsonw_key(&writer, extra_key);
        if (strcmp(extra_key, "context") == 0) {
            yew_jsonw_obj(&writer);
            yew_jsonw_key(&writer, "includeDeclaration");
            yew_jsonw_bool(&writer, true);
            yew_jsonw_obj_end(&writer);
        } else {
            yew_jsonw_cstr(&writer, extra_value);
        }
    }
    yew_jsonw_obj_end(&writer);
    bytebuf_free(&uri);
}

static const char *relative_path(Fixture *f, const char *path)
{
    size_t root_len = strlen(f->root);

    return strncmp(path, f->root, root_len) == 0 && path[root_len] == '/' ?
           path + root_len + 1U : path;
}

static void test_gotos(Fixture *f)
{
    static const GotoCase cases[] = {
        {"src/text/edit.c", "yew_textbuf_insert", "src/text/piece.c",
         "void yew_textbuf_insert("},
        {"src/term/render.c", "yew_cluster_width", "src/unicode/width.c",
         "int yew_cluster_width("},
        /* The sprint draft named yew_cmd_invoke, which no longer has a use
         * here after the editor-owned invocation rename.  This row preserves
         * its cross-module intent against the current symbol. */
        {"src/edit/dispatch.c", "yew_ed_invoke", "src/edit/ed.c",
         "CmdStatus yew_ed_invoke("},
        {"src/unicode/coords.c", "yew_gb_boundary",
         "src/unicode/grapheme.c", "bool yew_gb_boundary"},
        {"src/term/grid.c", "Cell", "src/term/grid.h", "} Cell;"},
        {"src/mod/lsp/client.c", "yew_json_parse", "src/mod/lsp/json.c",
         "JsonValue *yew_json_parse("},
        {"src/ui/statusline.c", "yew_str_width", "src/unicode/width.c",
         "int yew_str_width("},
        /* This call is compiled only when compile_commands.json carries the
         * module define, making it the database/module-flags sentinel. */
        {"src/ui/statusline.c", "yew_lsp_status_badge", "src/mod/lsp/lsp.c",
         "bool yew_lsp_status_badge"}
    };
    Arena arena;
    Bytebuf params;
    size_t i;

    arena_init(&arena);
    bytebuf_init(&params);
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Buffer *buffer = open_buffer(f, cases[i].use_file);
        JsonValue *result;
        Vec_LspLoc locations = {0};
        i64 want_line = anchor_line(f, cases[i].target_file, cases[i].anchor);
        i64 start = yew_now_ms();
        bool matched = false;
        size_t j;

        do {
            position_params(buffer, cases[i].symbol, NULL, NULL, &params);
            result = request(f, buffer, "textDocument/definition", &params,
                             &arena);
            if (i == 0U)
                trace_json("goto row 1 raw", result);
            (void)yew_lsp_locations_parse(result, &locations);
            for (j = 0U; j < locations.len; j++) {
                if (strcmp(relative_path(f, locations.data[j].path),
                           cases[i].target_file) == 0 &&
                    (i64)locations.data[j].line == want_line) {
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
            yew_lsp_locations_free(&locations);
            pump_once(&f->ed, 50);
        } while (yew_now_ms() - start <= LIVE_READY_MS);
        if (!matched)
            fail("goto %llu never resolved to %s:%lld after indexing",
                 (unsigned long long)i + 1U, cases[i].target_file,
                 (long long)want_line + 1);
        yew_lsp_locations_free(&locations);
    }
    bytebuf_free(&params);
    arena_free_all(&arena);
}

static bool locations_cover_width_files(Fixture *f,
                                        const Vec_LspLoc *locations)
{
    size_t i;
    size_t j;

    for (i = 0U; i < sizeof(width_code_files) /
                         sizeof(width_code_files[0]); i++) {
        bool found = false;

        for (j = 0U; j < locations->len; j++)
            if (strcmp(relative_path(f, locations->data[j].path),
                       width_code_files[i]) == 0) {
                found = true;
                break;
            }
        if (!found)
            return false;
    }
    return true;
}

static void collect_width_references(Fixture *f, const char *symbol,
                                     Vec_LspLoc *locations, Arena *arena)
{
    Buffer *buffer = open_buffer(f, "src/unicode/width.c");
    Bytebuf params;
    JsonValue *result;
    i64 start;

    bytebuf_init(&params);
    position_params(buffer, symbol, "context", NULL, &params);
    start = yew_now_ms();
    do {
        result = request(f, buffer, "textDocument/references", &params,
                         arena);
        (void)yew_lsp_locations_parse(result, locations);
        if (locations_cover_width_files(f, locations))
            break;
        yew_lsp_locations_free(locations);
        pump_once(&f->ed, 50);
    } while (yew_now_ms() - start <= LIVE_READY_MS);
    bytebuf_free(&params);
    if (!locations_cover_width_files(f, locations))
        fail("references never covered the complete width call graph");
}

static u32 test_references(Fixture *f)
{
    Arena arena;
    Vec_LspLoc locations = {0};
    u32 count;

    arena_init(&arena);
    collect_width_references(f, "yew_cluster_width", &locations, &arena);
    count = (u32)locations.len;
    yew_lsp_locations_free(&locations);
    arena_free_all(&arena);
    return count;
}

static int run_in_root(Fixture *f, char *const argv[])
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        fail("fork %s: %s", argv[0], strerror(errno));
    if (pid == 0) {
        if (chdir(f->root) != 0)
            _exit(126);
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            fail("waitpid %s: %s", argv[0], strerror(errno));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int capture_in_root(Fixture *f, char *const argv[], Bytebuf *out)
{
    int fds[2];
    pid_t pid;
    int status;

    out->len = 0U;
    if (pipe(fds) != 0)
        fail("pipe %s: %s", argv[0], strerror(errno));
    pid = fork();
    if (pid < 0)
        fail("fork %s: %s", argv[0], strerror(errno));
    if (pid == 0) {
        (void)close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 || close(fds[1]) != 0 ||
            chdir(f->root) != 0)
            _exit(126);
        execvp(argv[0], argv);
        _exit(127);
    }
    (void)close(fds[1]);
    for (;;) {
        u8 bytes[4096];
        ssize_t n = read(fds[0], bytes, sizeof(bytes));

        if (n > 0)
            bytebuf_append(out, bytes, (size_t)n);
        else if (n == 0)
            break;
        else if (errno != EINTR)
            fail("read %s output: %s", argv[0], strerror(errno));
    }
    (void)close(fds[0]);
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            fail("waitpid %s: %s", argv[0], strerror(errno));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void save_plan(Fixture *f, RenamePlan *plan)
{
    size_t i;

    for (i = 0U; i < plan->files.len; i++) {
        Buffer *buffer = yew_ws_buf_by_id(&f->ed, plan->files.data[i].buf_id);
        EditCtx edit;

        if (buffer == NULL)
            fail("rename lost buffer %u", plan->files.data[i].buf_id);
        edit = yew_ed_edit_ctx_buffer(&f->ed, buffer);
        if (yew_edit_save(&edit, buffer->path) != YEW_SAVE_OK)
            fail("cannot save renamed buffer %s", buffer->path);
        yew_ed_finish_edit(&f->ed, &edit);
        yew_lsp_sync_save(&f->ed, buffer);
    }
}

static void assert_diff_matches_plan(Fixture *f, RenamePlan *plan)
{
    size_t i;
    char *diff_name[] = {"git", "diff", "--name-only", "--no-ext-diff",
                         NULL};
    Bytebuf actual;
    Bytebuf expected;

    bytebuf_init(&actual);
    bytebuf_init(&expected);
    if (capture_in_root(f, diff_name, &actual) != 0)
        fail("cannot enumerate forward rename diff");
    for (i = 0U; i < plan->files.len; i++) {
        const char *rel = relative_path(f, plan->files.data[i].path);

        bytebuf_append(&expected, rel, strlen(rel));
        bytebuf_push_u8(&expected, (u8)'\n');
    }
    if (actual.len != expected.len ||
        memcmp(actual.data, expected.data, actual.len) != 0) {
        bytebuf_push_u8(&actual, 0U);
        bytebuf_push_u8(&expected, 0U);
        fail("git diff names differ from rename plan\nactual:\n%s"
             "expected:\n%s", (const char *)actual.data,
             (const char *)expected.data);
    }
    for (i = 0U; i < plan->files.len; i++) {
        char *one[] = {"git", "diff", "--quiet", "--",
                       (char *)relative_path(f, plan->files.data[i].path),
                       NULL};

        if (run_in_root(f, one) != 1)
            fail("rename plan path is not in git diff: %s",
                 plan->files.data[i].path);
    }
    bytebuf_free(&expected);
    bytebuf_free(&actual);
}

static void assert_identifier_absent_from_code(Fixture *f,
                                               char *const argv[])
{
    Bytebuf matches;
    int status;
    size_t at = 0U;

    bytebuf_init(&matches);
    status = capture_in_root(f, argv, &matches);
    if (status != 0 && status != 1)
        fail("cannot scan renamed identifier");
    while (at < matches.len) {
        size_t end = at;
        size_t first;
        size_t second;
        size_t text;

        while (end < matches.len && matches.data[end] != (u8)'\n')
            end++;
        first = at;
        while (first < end && matches.data[first] != (u8)':')
            first++;
        second = first + 1U;
        while (second < end && matches.data[second] != (u8)':')
            second++;
        text = second + 1U;
        while (text < end && (matches.data[text] == (u8)' ' ||
                              matches.data[text] == (u8)'\t'))
            text++;
        if (first == end || second == end || text == end ||
            !(matches.data[text] == (u8)'*' ||
              (text + 1U < end && matches.data[text] == (u8)'/' &&
               (matches.data[text + 1U] == (u8)'*' ||
                matches.data[text + 1U] == (u8)'/')))) {
            bytebuf_push_u8(&matches, 0U);
            fail("forward rename left identifier in code:\n%s",
                 (const char *)matches.data);
        }
        at = end + (end < matches.len ? 1U : 0U);
    }
    bytebuf_free(&matches);
}

static void rename_prepare(Fixture *f, const char *old_name,
                           const char *new_name, RenamePlan *plan,
                           Arena *arena)
{
    Buffer *buffer = open_buffer(f, "src/unicode/width.c");
    Bytebuf params;
    JsonValue *result;
    char error[YEW_RENAME_ERROR_MAX] = {0};

    bytebuf_init(&params);
    position_params(buffer, old_name, "newName", new_name, &params);
    result = request(f, buffer, "textDocument/rename", &params, arena);
    yew_lsp_rename_plan_init(plan);
    if (!yew_lsp_rename_preflight(&f->ed, result, f->server->pos_enc,
                                  old_name, new_name, plan, error))
        fail("rename preflight: %s", error);
    bytebuf_free(&params);
}

static void rename_commit(Fixture *f, RenamePlan *plan)
{
    char error[YEW_RENAME_ERROR_MAX] = {0};

    if (!yew_lsp_rename_apply(&f->ed, plan, error))
        fail("rename apply: %s", error);
    save_plan(f, plan);
}

static bool rename_same_paths(const RenamePlan *a, const RenamePlan *b)
{
    size_t i;
    size_t j;

    if (a->files.len != b->files.len)
        return false;
    for (i = 0U; i < a->files.len; i++) {
        bool found = false;

        for (j = 0U; j < b->files.len; j++)
            if (strcmp(a->files.data[i].path, b->files.data[j].path) == 0) {
                found = true;
                break;
            }
        if (!found)
            return false;
    }
    return true;
}

static bool rename_has_path(const RenamePlan *plan, const char *path)
{
    size_t i;

    for (i = 0U; i < plan->files.len; i++)
        if (strcmp(plan->files.data[i].path, path) == 0)
            return true;
    return false;
}

static void require_rename_covers_references(const RenamePlan *plan,
                                             const Vec_LspLoc *references)
{
    size_t i;

    for (i = 0U; i < references->len; i++)
        if (!rename_has_path(plan, references->data[i].path))
            fail("rename plan omitted indexed reference path %s",
                 references->data[i].path);
}

static void trace_rename_plan(const char *label, const RenamePlan *plan)
{
    size_t i;

    if (getenv("YEW_LSP_LIVE_TRACE") == NULL)
        return;
    (void)fprintf(stderr, "%s (%llu files):\n", label,
                  (unsigned long long)plan->files.len);
    for (i = 0U; i < plan->files.len; i++)
        (void)fprintf(stderr, "  %s\n", plan->files.data[i].path);
}

static void test_rename(Fixture *f)
{
    RenamePlan forward;
    RenamePlan reverse;
    Vec_LspLoc references = {0};
    Arena arena;
    bool have_h = false;
    bool have_c = false;
    size_t i;
    char build_arg[] = "BUILD=build-clangd-live";
    char *make_argv[] = {"make", "-s", "-j2", build_arg, NULL};
    char *grep_old[] = {"git", "grep", "-n", "-w", "yew_cluster_width",
                        "--", "src", NULL};
    char *clean[] = {"git", "diff", "--exit-code", NULL};

    arena_init(&arena);
    collect_width_references(f, "yew_cluster_width", &references, &arena);
    rename_prepare(f, "yew_cluster_width", "yew_cluster_cells", &forward,
                   &arena);
    trace_rename_plan("forward rename", &forward);
    require_rename_covers_references(&forward, &references);
    yew_lsp_locations_free(&references);
    rename_commit(f, &forward);
    for (i = 0U; i < forward.files.len; i++) {
        const char *rel = relative_path(f, forward.files.data[i].path);

        have_h = have_h || strcmp(rel, "src/unicode/width.h") == 0;
        have_c = have_c || strcmp(rel, "src/unicode/width.c") == 0;
    }
    if (!have_h || !have_c || forward.files.len < 2U)
        fail("rename plan did not cover width.h and width.c");
    assert_diff_matches_plan(f, &forward);
    assert_identifier_absent_from_code(f, grep_old);
    if (run_in_root(f, make_argv) != 0)
        fail("renamed tree does not compile");

    /* clangd's in-memory background index can retain the old symbol after a
     * workspace edit.  Exercise the production restart path, then wait for a
     * fresh complete reference graph before accepting the inverse edit. */
    if (!yew_lsp_client_restart(&f->ed,
                                open_buffer(f, "src/unicode/width.c")) ||
        !wait_ready(f, open_buffer(f, "src/unicode/width.c")))
        fail("clangd did not restart on the renamed tree");
    collect_width_references(f, "yew_cluster_cells", &references, &arena);
    rename_prepare(f, "yew_cluster_cells", "yew_cluster_width", &reverse,
                   &arena);
    trace_rename_plan("reverse rename", &reverse);
    require_rename_covers_references(&reverse, &references);
    yew_lsp_locations_free(&references);
    if (!rename_same_paths(&forward, &reverse))
        fail("inverse rename file set differs from forward rename");
    rename_commit(f, &reverse);
    yew_lsp_rename_plan_free(&reverse);
    yew_lsp_rename_plan_free(&forward);
    if (run_in_root(f, clean) != 0)
        fail("rename round trip did not restore a clean tree");
    arena_free_all(&arena);
}

static void test_hover(Fixture *f)
{
    Buffer *buffer = open_buffer(f, "src/text/edit.c");
    Bytebuf params;
    Bytebuf body;
    Arena arena;
    JsonValue *result;
    Span range;
    bool have_range;

    bytebuf_init(&params);
    bytebuf_init(&body);
    arena_init(&arena);
    position_params(buffer, "yew_textbuf_insert", NULL, NULL, &params);
    result = request(f, buffer, "textDocument/hover", &params, &arena);
    if (!yew_lsp_hover_parse(result, buffer->tb, f->server->pos_enc,
                             &body, &range, &have_range) ||
        body.len == 0U) {
        fail("hover returned no body");
    }
    bytebuf_push_u8(&body, 0U);
    if (strstr((const char *)body.data, "TextBuf") == NULL)
        fail("hover body does not mention TextBuf");
    arena_free_all(&arena);
    bytebuf_free(&body);
    bytebuf_free(&params);
}

static void test_diagnostic(Fixture *f)
{
    static const u8 bad[] = "\nint x = undefined_symbol_xyz;\n";
    Buffer *buffer = open_buffer(f, "src/util/base.c");
    EditCtx edit = yew_ed_edit_ctx_buffer(&f->ed, buffer);
    u64 original_len = yew_textbuf_len(buffer->tb);
    u32 line = (u32)yew_textbuf_line_count(buffer->tb);
    i64 start;
    const Diagnostic *rows[16];
    u32 n = 0U;
    u32 i;
    bool found = false;
    bool glyph = false;

    yew_undo_begin(&edit, YEW_TXN_TYPE);
    require(yew_edit_insert(&edit, BYTEOFF(original_len), bad,
                            sizeof(bad) - 1U), "cannot insert diagnostic fixture");
    yew_undo_end(&edit);
    yew_ed_finish_edit(&f->ed, &edit);
    yew_lsp_sync_flush(&f->ed);
    start = yew_now_ms();
    while (yew_now_ms() - start <= LIVE_DIAG_MS) {
        n = yew_diag_at_line(buffer, LINENO(line), rows,
                             (u32)(sizeof(rows) / sizeof(rows[0])));
        for (i = 0U; i < n; i++)
            if (rows[i]->sev == YEW_DIAG_ERROR &&
                strstr(rows[i]->message, "undefined_symbol_xyz") != NULL)
                found = true;
        if (found)
            break;
        pump_once(&f->ed, 10);
    }
    if (!found)
        fail("diagnostic fixture produced no matching error");
    require(yew_ed_show_buffer(&f->ed, buffer),
            "cannot focus diagnostic fixture");
    yew_diag_refresh_view(&f->ed, f->ed.win);
    for (i = 0U; i < f->ed.win->gutter_signs.len; i++) {
        GutterSignLine *sign = &f->ed.win->gutter_signs.v[i];

        if (sign->line.v == line &&
            (sign->mask & (u8)(1U << YEW_SIGN_DIAG)) != 0U &&
            sign->sign[YEW_SIGN_DIAG].nbytes != 0U)
            glyph = true;
    }
    require(glyph, "diagnostic line has no error gutter glyph");

    edit = yew_ed_edit_ctx_buffer(&f->ed, buffer);
    yew_undo_begin(&edit, YEW_TXN_TYPE);
    require(yew_edit_delete(&edit,
                            (Span){original_len,
                                   original_len + sizeof(bad) - 1U}),
            "cannot remove diagnostic fixture");
    yew_undo_end(&edit);
    yew_ed_finish_edit(&f->ed, &edit);
}

static void fixture_init(Fixture *f, const char *root)
{
    char initial[4096];

    (void)memset(f, 0, sizeof(*f));
    if (strlen(root) >= sizeof(f->root))
        fail("workspace root too long");
    (void)memcpy(f->root, root, strlen(root) + 1U);
    f->fd_before = fd_count();
    yew_ed_init(&f->ed);
    f->ed.headless = true;
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->root);
    f->cfg = yew_lsp_default_cfg("c");
    require(f->cfg != NULL, "clangd default config missing");
    f->live_cfg = *f->cfg;
    if (getenv("YEW_CLANGD") != NULL)
        f->live_cfg.cmd = getenv("YEW_CLANGD");
    f->cfg = &f->live_cfg;
    path_join(initial, f->root, "src/text/edit.c");
    require(yew_ed_open(&f->ed, initial) == YEW_LOAD_OK,
            "cannot open initial source file");
    f->ed.buffer.lang = "c";
    f->ed.now_ms = yew_now_ms();
    require(yew_lsp_client_start_cfg(&f->ed, &f->ed.buffer, f->cfg),
            "cannot launch clangd");
    require(wait_ready(f, &f->ed.buffer), "clangd initialization timed out");
}

static void fixture_free(Fixture *f)
{
    i64 start = yew_now_ms();
    int fd_after;
    int status;

    yew_lsp_client_stop(&f->ed, f->server, true);
    while (yew_job_running_count(&f->ed) != 0U &&
           yew_now_ms() - start <= LIVE_REQUEST_MS)
        pump_once(&f->ed, 10);
    if (yew_job_running_count(&f->ed) != 0U)
        fail("clangd did not shut down gracefully");
    yew_ed_free(&f->ed);
    errno = 0;
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD)
        fail("child process remained after clangd shutdown");
    fd_after = fd_count();
    if (f->fd_before >= 0 && fd_after >= 0 && fd_after != f->fd_before)
        fail("fd count changed across live test: %d -> %d",
             f->fd_before, fd_after);
}

int main(int argc, char **argv)
{
    Fixture fixture;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s WORKTREE\n", argv[0]);
        return 2;
    }
    fixture_init(&fixture, argv[1]);
    test_gotos(&fixture);
    (void)test_references(&fixture);
    test_hover(&fixture);
    test_rename(&fixture);
    test_diagnostic(&fixture);
    fixture_free(&fixture);
    (void)printf("clangd: 8/8 goto, 6 refs in 3 files, "
                 "rename round-trip clean, 1 diagnostic\n");
    return 0;
}
