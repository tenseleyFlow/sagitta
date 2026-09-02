#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/notify.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/json.h"
#include "mod/lsp/jsonrpc.h"
#include "mod/lsp/rename.h"
#include "mod/lsp/sync.h"
#include "term/grid.h"
#include "term/render.h"
#include "ui/draw.h"
#include "util/sort.h"

enum {
    LSP_NOTE_SAMPLES = 1001,
    LSP_DIAG_SAMPLES = 101,
    LSP_LINE_BYTES = 4096,
    LSP_DIAGNOSTICS = 10000,
    LSP_RENAME_EDITS = 20000,
    LSP_RENAME_SAMPLES = 5,
    LSP_VIEWPORT_LINES = 50,
    LSP_VIEWPORT_COLS = 200,
    LSP_STREAM_SAMPLE_CAP = 16384,
    LSP_NOTE_P99_BUDGET_NS = 100000,
    LSP_VIEW_P99_BUDGET_NS = 5000000,
    LSP_KEY_P99_BUDGET_NS = 5000000,
    LSP_RENAME_P99_BUDGET_NS = 300000000
};

#define LSP_STREAM_BYTES (UINT64_C(50) * 1024U * 1024U)

typedef struct Timing {
    const char *name;
    u64 median_ns;
    u64 p99_ns;
    u64 maximum_ns;
    u64 baseline_median_ns;
    u64 baseline_p99_ns;
    u64 budget_p99_ns;
} Timing;

static volatile u64 lsp_perf_sink;
static const u8 lsp_stream_frame[] =
    "Content-Length: 53\r\n\r\n"
    "{\"jsonrpc\":\"2.0\",\"method\":\"$/progress\",\"params\":null}";

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_lsp: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    const u64 a = *(const u64 *)left;
    const u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void summarize(u64 *samples, size_t len, Timing *out)
{
    yew_sort_stable(samples, len, sizeof(*samples), cmp_u64, NULL);
    out->median_ns = samples[len / 2U];
    out->p99_ns = samples[(len * 99U) / 100U];
    out->maximum_ns = samples[len - 1U];
}

static bool measure_note_edit(u8 pos_enc, Timing *out)
{
    u8 line[LSP_LINE_BYTES + 1U];
    u64 samples[LSP_NOTE_SAMPLES];
    LspDoc doc;
    Ed ed;
    u32 i;
    bool ok = false;

    /* Four-byte scalars make UTF-16 exercise its scan and pair count. */
    for (i = 0U; i < LSP_LINE_BYTES; i += 4U) {
        line[i] = 0xf0U;
        line[i + 1U] = 0x9fU;
        line[i + 2U] = 0x98U;
        line[i + 3U] = 0x80U;
    }
    line[LSP_LINE_BYTES] = '\n';
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, line, sizeof(line), "perf_lsp.c"))
        goto done_ed;
    yew_lsp_doc_init(&doc, ed.buffer.id, "file:///perf_lsp.c");
    doc.open = true;
    doc.version = 1;
    for (i = 0U; i < LSP_NOTE_SAMPLES; i++) {
        ByteOff at = BYTEOFF((u64)(i % (LSP_LINE_BYTES / 4U)) * 4U);
        u64 started = now_ns();

        yew_lsp_doc_note_edit(&doc, pos_enc, 2U, ed.buffer.tb,
                              YEW_JOURNAL_DEL, at, 4U);
        samples[i] = now_ns() - started;
        if (doc.pending.len != 1U)
            goto done_doc;
        lsp_perf_sink ^= (u64)doc.pending.data[0].sc +
                         (u64)doc.pending.data[0].ec;
        doc.pending.len = 0U;
    }
    summarize(samples, LSP_NOTE_SAMPLES, out);
    ok = true;
done_doc:
    yew_lsp_doc_free(&doc);
done_ed:
    yew_ed_free(&ed);
    return ok;
}

static bool make_diagnostics(Bytebuf *json)
{
    JsonW w;
    u32 i;

    yew_jsonw_init(&w, json);
    yew_jsonw_arr(&w);
    for (i = 0U; i < LSP_DIAGNOSTICS; i++) {
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "range");
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "start");
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "line");
        yew_jsonw_int(&w, (i64)i);
        yew_jsonw_key(&w, "character");
        yew_jsonw_int(&w, 0);
        yew_jsonw_obj_end(&w);
        yew_jsonw_key(&w, "end");
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "line");
        yew_jsonw_int(&w, (i64)i);
        yew_jsonw_key(&w, "character");
        yew_jsonw_int(&w, 1);
        yew_jsonw_obj_end(&w);
        yew_jsonw_obj_end(&w);
        yew_jsonw_key(&w, "severity");
        yew_jsonw_int(&w, 1 + (i64)(i % 4U));
        yew_jsonw_key(&w, "source");
        yew_jsonw_cstr(&w, "perf");
        yew_jsonw_key(&w, "message");
        yew_jsonw_cstr(&w, "diagnostic message");
        yew_jsonw_obj_end(&w);
    }
    yew_jsonw_arr_end(&w);
    return json->len <= YEW_JSON_MAX_BYTES;
}

static bool measure_diag_viewport(Timing *out)
{
    u64 samples[LSP_DIAG_SAMPLES];
    Bytebuf text;
    Bytebuf json;
    Arena parsed;
    JsonErr err;
    JsonValue *arr;
    Ed ed;
    TtyCaps caps;
    u32 i;
    bool parsed_ready = false;
    bool ok = false;

    bytebuf_init(&text);
    bytebuf_init(&json);
    for (i = 0U; i < LSP_DIAGNOSTICS; i++)
        bytebuf_append(&text, "x\n", 2U);
    if (!make_diagnostics(&json))
        goto done_buffers;
    arena_init(&parsed);
    parsed_ready = true;
    arr = yew_json_parse(&parsed, json.data, json.len, &err);
    if (arr == NULL || arr->kind != YEW_JS_ARR)
        goto done_arena;
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, text.data, text.len, "perf_diag.c"))
        goto done_ed;
    (void)memset(&caps, 0, sizeof(caps));
    /* Reserve the visible tab strip and status row outside the measured
     * document viewport. */
    if (!yew_grid_init(&ed.grid, &ed.interner,
                       LSP_VIEWPORT_LINES + 2U, LSP_VIEWPORT_COLS))
        goto done_ed;
    ed.grid_ready = true;
    yew_render_init(&ed.render, &caps, NULL);
    ed.render_ready = true;
    yew_ed_layout(&ed);
    if (ed.win == NULL || ed.win->rect.h != LSP_VIEWPORT_LINES ||
        ed.win->cs.curs.len == 0U) {
        (void)fprintf(stderr,
                      "perf_lsp: viewport setup rows=%u cursors=%zu\n",
                      ed.win == NULL ? 0U : ed.win->rect.h,
                      ed.win == NULL ? 0U : ed.win->cs.curs.len);
        goto done_ed;
    }
    ed.win->cs.curs.data[ed.win->cs.primary].pos =
        yew_textbuf_line_start(ed.buffer.tb, LINENO(5000U));
    ed.win->cs.curs.data[ed.win->cs.primary].anchor =
        ed.win->cs.curs.data[ed.win->cs.primary].pos;
    yew_vp_center(ed.win);
    yew_diag_replace(&ed, &ed.buffer, 1U, arr, 1);
    if (ed.buffer.diag == NULL ||
        ed.buffer.diag->d.len != LSP_DIAGNOSTICS ||
        ed.win->gutter_signs.len != LSP_DIAGNOSTICS) {
        (void)fprintf(stderr,
                      "perf_lsp: diagnostic setup diagnostics=%zu signs=%u\n",
                      ed.buffer.diag == NULL ? 0U : ed.buffer.diag->d.len,
                      ed.win->gutter_signs.len);
        goto done_ed;
    }
    for (i = 0U; i < LSP_DIAG_SAMPLES; i++) {
        size_t emitted;
        size_t cell_count;
        u32 styled = 0U;
        u64 started = now_ns();

        yew_draw_panes(&ed);
        yew_grid_mark_all(&ed.grid);
        ed.frame.len = 0U;
        emitted = yew_render_frame(&ed.render, &ed.grid, &ed.frame);
        yew_grid_flip(&ed.grid);
        samples[i] = now_ns() - started;
        cell_count = (size_t)ed.grid.rows * ed.grid.cols;
        for (size_t cell = 0U; cell < cell_count; cell++) {
            if ((ed.grid.back[cell].attrs &
                 (YEW_CELL_UL_ERROR | YEW_CELL_UL_WARN |
                  YEW_CELL_UL_INFO)) != 0U)
                styled++;
        }
        if (emitted == 0U || emitted != ed.frame.len || styled == 0U) {
            (void)fprintf(stderr,
                          "perf_lsp: diagnostic frame emitted=%zu "
                          "frame=%zu styled=%u\n",
                          emitted, ed.frame.len, styled);
            goto done_ed;
        }
        lsp_perf_sink ^= (u64)emitted + styled;
    }
    summarize(samples, LSP_DIAG_SAMPLES, out);
    ok = true;
done_ed:
    yew_ed_free(&ed);
done_arena:
    if (parsed_ready)
        arena_free_all(&parsed);
done_buffers:
    bytebuf_free(&json);
    bytebuf_free(&text);
    return ok;
}

typedef struct StreamCount {
    u64 messages;
    bool destroyed;
} StreamCount;

typedef struct StreamOwner {
    RpcConn rpc;
    StreamCount *count;
} StreamOwner;

static void count_message(void *ctx, const JsonValue *message)
{
    StreamCount *count = ctx;

    if (message != NULL && message->kind == YEW_JS_OBJ)
        count->messages++;
}

static bool stream_feed(void *opaque, const u8 *bytes, u64 len)
{
    StreamOwner *owner = opaque;

    return yew_rpc_feed_stdout(&owner->rpc, bytes, len);
}

static bool stream_finish(void *opaque)
{
    StreamOwner *owner = opaque;

    return yew_rpc_finish_stdout(&owner->rpc);
}

static u64 stream_tx_view(void *opaque, const u8 **bytes)
{
    StreamOwner *owner = opaque;

    return yew_rpc_tx_view(&owner->rpc, bytes);
}

static void stream_tx_consume(void *opaque, u64 len)
{
    StreamOwner *owner = opaque;

    yew_rpc_tx_consume(&owner->rpc, len);
}

static i64 stream_deadline(const void *opaque)
{
    const StreamOwner *owner = opaque;

    return yew_rpc_job_deadline(&owner->rpc);
}

static void stream_tick(void *opaque, Ed *ed, i64 now_ms)
{
    StreamOwner *owner = opaque;

    yew_rpc_job_tick(&owner->rpc, ed, now_ms);
}

static void stream_destroy(void *opaque)
{
    StreamOwner *owner = opaque;

    yew_rpc_conn_free(&owner->rpc);
    owner->count->destroyed = true;
    free(owner);
}

static const YewJobFramedOps stream_ops = {
    stream_feed, stream_finish, stream_tx_view, stream_tx_consume,
    stream_deadline, stream_tick, stream_destroy
};

static bool write_all(int fd, const u8 *bytes, size_t len)
{
    while (len != 0U) {
        ssize_t n = write(fd, bytes, len);

        if (n > 0) {
            bytes += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool measure_rename_plan(Timing *out)
{
    char root[] = "/tmp/yew-perf-rename-XXXXXX";
    char path[160] = {0};
    char uri[192];
    u64 samples[LSP_RENAME_SAMPLES];
    Bytebuf text;
    Bytebuf json;
    Arena arena;
    JsonErr json_err;
    JsonValue *workspace_edit;
    JsonW writer;
    Ed ed;
    char rename_err[YEW_RENAME_ERROR_MAX] = {0};
    int fd = -1;
    u32 i;
    bool made_root = false;
    bool arena_ready = false;
    bool ed_ready = false;
    bool ok = false;

    bytebuf_init(&text);
    bytebuf_init(&json);
    if (mkdtemp(root) == NULL)
        goto done;
    made_root = true;
    if (snprintf(path, sizeof(path), "%s/rename.c", root) <= 0 ||
        snprintf(uri, sizeof(uri), "file://%s", path) <= 0)
        goto done;
    for (i = 0U; i < LSP_RENAME_EDITS; i++)
        bytebuf_append(&text, "alpha\n", sizeof("alpha\n") - 1U);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || !write_all(fd, text.data, text.len))
        goto done;
    if (close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;

    yew_jsonw_init(&writer, &json);
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "changes");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, uri);
    yew_jsonw_arr(&writer);
    for (i = 0U; i < LSP_RENAME_EDITS; i++) {
        yew_jsonw_obj(&writer);
        yew_jsonw_key(&writer, "range");
        yew_jsonw_obj(&writer);
        yew_jsonw_key(&writer, "start");
        yew_jsonw_obj(&writer);
        yew_jsonw_key(&writer, "line");
        yew_jsonw_int(&writer, i);
        yew_jsonw_key(&writer, "character");
        yew_jsonw_int(&writer, 0);
        yew_jsonw_obj_end(&writer);
        yew_jsonw_key(&writer, "end");
        yew_jsonw_obj(&writer);
        yew_jsonw_key(&writer, "line");
        yew_jsonw_int(&writer, i);
        yew_jsonw_key(&writer, "character");
        yew_jsonw_int(&writer, 5);
        yew_jsonw_obj_end(&writer);
        yew_jsonw_obj_end(&writer);
        yew_jsonw_key(&writer, "newText");
        yew_jsonw_cstr(&writer, "omega");
        yew_jsonw_obj_end(&writer);
    }
    yew_jsonw_arr_end(&writer);
    yew_jsonw_obj_end(&writer);
    yew_jsonw_obj_end(&writer);
    arena_init(&arena);
    arena_ready = true;
    workspace_edit = yew_json_parse(&arena, json.data, json.len, &json_err);
    if (workspace_edit == NULL)
        goto done;

    yew_ed_init(&ed);
    ed_ready = true;
    ed.ws.dir = arena_strdup(&ed.arena, root);
    if (yew_ed_open(&ed, path) != YEW_LOAD_OK)
        goto done;
    for (i = 0U; i < LSP_RENAME_SAMPLES; i++) {
        RenamePlan plan;
        u64 started;

        yew_lsp_rename_plan_init(&plan);
        started = now_ns();
        if (!yew_lsp_rename_preflight(&ed, workspace_edit,
                                       YEW_POSENC_UTF8, "alpha", "omega",
                                       &plan, rename_err)) {
            yew_lsp_rename_plan_free(&plan);
            goto done;
        }
        samples[i] = now_ns() - started;
        if (plan.nedits != LSP_RENAME_EDITS || plan.files.len != 1U) {
            yew_lsp_rename_plan_free(&plan);
            goto done;
        }
        lsp_perf_sink ^= plan.nedits + plan.files.len;
        yew_lsp_rename_plan_free(&plan);
    }
    summarize(samples, LSP_RENAME_SAMPLES, out);
    ok = true;
done:
    if (!ok)
        (void)fprintf(stderr, "perf_lsp: rename plan invariant failed: %s\n",
                      rename_err);
    if (ed_ready)
        yew_ed_free(&ed);
    if (arena_ready)
        arena_free_all(&arena);
    if (fd >= 0)
        (void)close(fd);
    if (made_root) {
        if (path[0] != '\0')
            (void)unlink(path);
        (void)rmdir(root);
    }
    bytebuf_free(&json);
    bytebuf_free(&text);
    return ok;
}

static int run_stream_server(void)
{
    u64 sent = 0U;

    while (sent < LSP_STREAM_BYTES) {
        if (!write_all(STDOUT_FILENO, lsp_stream_frame,
                       sizeof(lsp_stream_frame) - 1U))
            return 1;
        sent += sizeof(lsp_stream_frame) - 1U;
    }
    return 0;
}

static bool measure_stream_keypress(const char *self, Timing *out)
{
    u64 samples[LSP_STREAM_SAMPLE_CAP];
    StreamCount count = {0U, false};
    StreamOwner *owner;
    Ed ed;
    YewJobSpec spec = {0};
    char *child_argv[3];
    char err[160] = {0};
    u32 id;
    size_t nsamples = 0U;
    i64 deadline;
    bool ok = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, NULL, 0U, "perf_stream.c"))
        goto done_ed;
    owner = calloc(1U, sizeof(*owner));
    if (owner == NULL)
        goto done_ed;
    yew_rpc_conn_init(&owner->rpc);
    owner->count = &count;
    yew_rpc_set_handler(&owner->rpc, count_message, &count);
    child_argv[0] = (char *)self;
    child_argv[1] = (char *)"--server";
    child_argv[2] = NULL;
    spec.argv = child_argv;
    spec.cwd = ".";
    spec.sink = YEW_SINK_FRAMED;
    spec.display = "perf-lsp-stream";
    spec.framed_owner = owner;
    spec.framed_ops = &stream_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    if (id == 0U) {
        stream_destroy(owner);
        (void)fprintf(stderr, "perf_lsp: stream spawn failed: %s\n", err);
        goto done_ed;
    }
    deadline = (i64)(now_ns() / UINT64_C(1000000)) + 30000;
    for (;;) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        YewJob *job = yew_job_find(&ed, id);
        u64 before;
        u64 started;
        u32 n = 0U;
        Key key;

        if (job == NULL)
            goto done_ed;
        if (job->drained)
            break;
        if ((i64)(now_ns() / UINT64_C(1000000)) > deadline) {
            (void)fprintf(stderr, "perf_lsp: stream timed out\n");
            goto done_ed;
        }
        yew_job_collect_fds(&ed, pfd, &n);
        if (poll(pfd, (nfds_t)n, 100) < 0 && errno != EINTR)
            goto done_ed;
        job = yew_job_find(&ed, id);
        if (job == NULL)
            goto done_ed;
        before = job->bytes_out;
        (void)memset(&key, 0, sizeof(key));
        key.code = YEW_KEY_RIGHT;
        started = now_ns();
        yew_job_pump(&ed, pfd, n);
        yew_dispatch_key(&ed, key, (i64)nsamples);
        job = yew_job_find(&ed, id);
        if (job == NULL)
            goto done_ed;
        if (job->bytes_out != before) {
            if (nsamples == LSP_STREAM_SAMPLE_CAP) {
                (void)fprintf(stderr,
                              "perf_lsp: stream exceeded sample cap\n");
                goto done_ed;
            }
            samples[nsamples++] = now_ns() - started;
        }
        yew_job_reap(&ed);
        yew_job_settle(&ed);
        lsp_perf_sink ^= ed.dispatch_count + count.messages;
    }
    {
        YewJob *job = yew_job_find(&ed, id);

        if (job == NULL || job->bytes_out < LSP_STREAM_BYTES ||
            job->state != YEW_JOB_EXITED || job->exit_code != 0 ||
            job->framed_failed || !count.destroyed ||
            count.messages != job->bytes_out /
                              (sizeof(lsp_stream_frame) - 1U) ||
            nsamples < 100U) {
            (void)fprintf(stderr,
                          "perf_lsp: stream invariant bytes=%llu "
                          "messages=%llu samples=%zu destroyed=%u\n",
                          (unsigned long long)(job == NULL ? 0U :
                                                       job->bytes_out),
                          (unsigned long long)count.messages, nsamples,
                          count.destroyed ? 1U : 0U);
            goto done_ed;
        }
    }
    summarize(samples, nsamples, out);
    ok = true;
done_ed:
    yew_ed_free(&ed);
    return ok;
}

static bool load_baselines(Timing *rows, size_t count)
{
    FILE *fp = fopen("tests/perf/baselines/lsp.txt", "r");
    char line[160];

    if (fp == NULL)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char name[64];
        unsigned long long median;
        unsigned long long p99;
        size_t i;

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
    for (size_t i = 0U; i < count; i++) {
        if (rows[i].baseline_median_ns == 0U ||
            rows[i].baseline_p99_ns == 0U)
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    Timing rows[] = {
        {"note_edit_utf8_4k", 0U, 0U, 0U, 0U, 0U,
         LSP_NOTE_P99_BUDGET_NS},
        {"note_edit_utf16_4k", 0U, 0U, 0U, 0U, 0U,
         LSP_NOTE_P99_BUDGET_NS},
        {"diag_viewport_10k", 0U, 0U, 0U, 0U, 0U,
         LSP_VIEW_P99_BUDGET_NS},
        {"rename_plan_20k", 0U, 0U, 0U, 0U, 0U,
         LSP_RENAME_P99_BUDGET_NS},
        {"framed_50m_keypress", 0U, 0U, 0U, 0U, 0U,
         LSP_KEY_P99_BUDGET_NS}
    };
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;
    bool server = argc == 2 && strcmp(argv[1], "--server") == 0;
    size_t i;
    int status = 0;

    if (server)
        return run_stream_server();
    if (argc > 2 || (argc == 2 && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure|--server]\n", argv[0]);
        return 2;
    }
    if (!measure_note_edit(YEW_POSENC_UTF8, &rows[0])) {
        (void)fprintf(stderr, "perf_lsp: utf-8 note-edit invariant failed\n");
        return 2;
    }
    if (!measure_note_edit(YEW_POSENC_UTF16, &rows[1])) {
        (void)fprintf(stderr, "perf_lsp: utf-16 note-edit invariant failed\n");
        return 2;
    }
    if (!measure_diag_viewport(&rows[2])) {
        (void)fprintf(stderr, "perf_lsp: diagnostic viewport invariant failed\n");
        return 2;
    }
    if (!measure_rename_plan(&rows[3])) {
        (void)fprintf(stderr, "perf_lsp: rename plan invariant failed\n");
        return 2;
    }
    if (!measure_stream_keypress(argv[0], &rows[4])) {
        (void)fprintf(stderr, "perf_lsp: framed stream invariant failed\n");
        return 2;
    }
    if (!measure && !load_baselines(rows, YEW_ARRAY_LEN(rows))) {
        (void)fprintf(stderr, "perf_lsp: missing or invalid baseline\n");
        return 2;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        bool regression = rows[i].p99_ns > rows[i].budget_p99_ns;

        (void)printf("lsp.%s median_ns=%llu p99_ns=%llu max_ns=%llu "
                     "budget_ns=%llu%s\n",
                     rows[i].name,
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
            status = 1;
    }
    return status;
}
