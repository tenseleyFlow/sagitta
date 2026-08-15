#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "edit/notify.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/json.h"
#include "mod/lsp/jsonrpc.h"
#include "mod/lsp/sync.h"
#include "util/sort.h"

enum {
    LSP_NOTE_SAMPLES = 1001,
    LSP_DIAG_SAMPLES = 101,
    LSP_STREAM_SAMPLES = 801,
    LSP_LINE_BYTES = 4096,
    LSP_DIAGNOSTICS = 10000,
    LSP_VIEWPORT_LINES = 50,
    LSP_STREAM_CHUNK = 64 * 1024,
    LSP_NOTE_P99_BUDGET_NS = 100000,
    LSP_VIEW_P99_BUDGET_NS = 5000000,
    LSP_KEY_P99_BUDGET_NS = 5000000
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

static volatile u64 lsp_perf_sink;

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
    const Diagnostic *found[4];
    u64 samples[LSP_DIAG_SAMPLES];
    Bytebuf text;
    Bytebuf json;
    Arena parsed;
    JsonErr err;
    JsonValue *arr;
    Ed ed;
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
    yew_diag_replace(&ed, &ed.buffer, 1U, arr, 1);
    if (ed.buffer.diag == NULL ||
        ed.buffer.diag->d.len != LSP_DIAGNOSTICS)
        goto done_ed;
    for (i = 0U; i < LSP_DIAG_SAMPLES; i++) {
        u32 line;
        u64 started = now_ns();

        for (line = 0U; line < LSP_VIEWPORT_LINES; line++) {
            u32 n = yew_diag_at_line(
                &ed.buffer, LINENO(4975U + line), found,
                YEW_ARRAY_LEN(found));

            if (n != 1U)
                goto done_ed;
            lsp_perf_sink ^= found[0]->identity + found[0]->cache.lo;
        }
        samples[i] = now_ns() - started;
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
} StreamCount;

static void count_message(void *ctx, const JsonValue *message)
{
    StreamCount *count = ctx;

    if (message != NULL && message->kind == YEW_JS_OBJ)
        count->messages++;
}

static bool make_stream_chunk(Bytebuf *chunk)
{
    static const u8 body[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"$/progress\",\"params\":null}";
    RpcTx tx;

    yew_rpctx_init(&tx);
    yew_rpctx_send(&tx, body, sizeof(body) - 1U);
    while (chunk->len + tx.pending.len <= LSP_STREAM_CHUNK)
        bytebuf_append(chunk, tx.pending.data, tx.pending.len);
    yew_rpctx_free(&tx);
    return chunk->len != 0U;
}

static bool measure_stream_keypress(Timing *out)
{
    u64 samples[LSP_STREAM_SAMPLES];
    Bytebuf chunk;
    RpcConn rpc;
    StreamCount count = {0U};
    Ed ed;
    u64 bytes = 0U;
    u32 i;
    bool ok = false;

    bytebuf_init(&chunk);
    if (!make_stream_chunk(&chunk))
        goto done_chunk;
    yew_rpc_conn_init(&rpc);
    yew_rpc_set_handler(&rpc, count_message, &count);
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, NULL, 0U, "perf_stream.c"))
        goto done_ed;
    for (i = 0U; i < LSP_STREAM_SAMPLES; i++) {
        Key key;
        u64 started;

        if (!yew_rpc_feed_stdout(&rpc, chunk.data, chunk.len)) {
            (void)fprintf(stderr,
                          "perf_lsp: stream died at chunk %u: %s\n",
                          i, rpc.rx.err);
            goto done_ed;
        }
        bytes += chunk.len;
        (void)memset(&key, 0, sizeof(key));
        key.code = YEW_KEY_RIGHT;
        started = now_ns();
        yew_dispatch_key(&ed, key, (i64)i);
        samples[i] = now_ns() - started;
        lsp_perf_sink ^= ed.dispatch_count + count.messages;
    }
    if (bytes < UINT64_C(50) * 1024U * 1024U || count.messages == 0U) {
        (void)fprintf(stderr,
                      "perf_lsp: stream short bytes=%llu messages=%llu\n",
                      (unsigned long long)bytes,
                      (unsigned long long)count.messages);
        goto done_ed;
    }
    summarize(samples, LSP_STREAM_SAMPLES, out);
    ok = true;
done_ed:
    yew_ed_free(&ed);
    yew_rpc_conn_free(&rpc);
done_chunk:
    bytebuf_free(&chunk);
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
        {"framed_50m_keypress", 0U, 0U, 0U, 0U, 0U,
         LSP_KEY_P99_BUDGET_NS}
    };
    bool measure = argc == 2 && strcmp(argv[1], "--measure") == 0;
    size_t i;
    int status = 0;

    if (argc > 2 || (argc == 2 && !measure)) {
        (void)fprintf(stderr, "usage: %s [--measure]\n", argv[0]);
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
    if (!measure_stream_keypress(&rows[3])) {
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
