#define _POSIX_C_SOURCE 200809L

#include "mod/ai/stats.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/data.h"
#include "fl/gc.h"
#include "fl/vm.h"
#include "mod/ai/ai_int.h"
#include "text/file.h"
#include "ui/message.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/xdg.h"

enum {
    AI_STATS_LATENCY_CAP = 256U,
    AI_STATS_SAVE_MS = 5U * 60U * 1000U
};

typedef struct AiStats {
    u64 requests;
    u64 declined;
    u64 delivered;
    u64 cancelled;
    u64 errors;
    u64 accepted_word;
    u64 accepted_line;
    u64 accepted_all;
    u64 dismissed;
    u64 bytes_suggested;
    u64 bytes_accepted;
    u64 first_token_ms_sum;
    u64 first_token_ms_max;
    u64 n_first_token;
    u64 tokens_in;
    u64 tokens_out;
    u64 first_token_ms[AI_STATS_LATENCY_CAP];
    u32 latency_at;
    u32 latency_len;
} AiStats;

typedef struct AiStatsRow {
    char *backend;
    AiStats stats;
} AiStatsRow;

struct AiStatsState {
    AiStatsRow *rows;
    u32 len;
    u32 cap;
    i64 last_save_ms;
    bool loaded;
    bool dirty;
};

typedef struct StatsDoc {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} StatsDoc;

static char *stats_path(bool ensure)
{
    char *dir = yew_xdg_state_dir();
    char *path;
    size_t n;

    if (dir == NULL)
        return NULL;
    if (ensure && !yew_mkdirs(dir, 0700U)) {
        free(dir);
        return NULL;
    }
    n = strlen(dir) + sizeof("/ai_stats.fl");
    path = yew_xmalloc(n);
    (void)snprintf(path, n, "%s/ai_stats.fl", dir);
    free(dir);
    return path;
}

static void stats_doc_init(StatsDoc *doc)
{
    (void)memset(doc, 0, sizeof(*doc));
    arena_init(&doc->arena);
    interner_init(&doc->in, &doc->arena);
    fl_diag_init(&doc->dc, &doc->arena);
    (void)fl_vm_init(&doc->vm, &doc->arena, &doc->in, &doc->dc);
}

static void stats_doc_free(StatsDoc *doc)
{
    fl_vm_free(&doc->vm);
    interner_free(&doc->in);
    arena_free_all(&doc->arena);
}

static bool stats_read(const char *path, Bytebuf *out, bool *missing)
{
    u8 chunk[8192];
    int fd;

    *missing = false;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        *missing = errno == ENOENT;
        return false;
    }
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof(chunk));

        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return false;
        }
        if ((size_t)got > FL_DATA_MAX_BYTES - out->len) {
            (void)close(fd);
            return false;
        }
        bytebuf_append(out, chunk, (size_t)got);
    }
    return close(fd) == 0;
}

static bool value_key_eq(FlValue key, const char *want)
{
    const FlStr *s;
    size_t n = strlen(want);

    if (key.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)key.as.o;
    return s->len == n && memcmp(s->b, want, n) == 0;
}

static bool map_get(const FlMap *map, const char *name, FlValue *out)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    while (fl_map_iter(map, &cursor, &key, &value)) {
        if (!value_key_eq(key, name))
            continue;
        if (out != NULL)
            *out = value;
        return true;
    }
    return false;
}

static u64 map_u64(const FlMap *map, const char *name)
{
    FlValue value;

    return map_get(map, name, &value) && value.t == (u8)FL_INT &&
                   value.as.i >= 0 ?
               (u64)value.as.i : 0U;
}

static AiStatsRow *stats_row(AiStatsState *state, const char *backend,
                             bool create)
{
    u32 i;
    AiStatsRow *row;

    if (state == NULL || backend == NULL || backend[0] == '\0')
        return NULL;
    for (i = 0U; i < state->len; i++)
        if (strcmp(state->rows[i].backend, backend) == 0)
            return &state->rows[i];
    if (!create)
        return NULL;
    if (state->len == state->cap) {
        u32 cap = state->cap == 0U ? 4U : state->cap * 2U;

        state->rows = yew_xrealloc(state->rows,
                                   (size_t)cap * sizeof(*state->rows));
        (void)memset(state->rows + state->cap, 0,
                     (size_t)(cap - state->cap) * sizeof(*state->rows));
        state->cap = cap;
    }
    row = &state->rows[state->len++];
    row->backend = yew_xmalloc(strlen(backend) + 1U);
    (void)memcpy(row->backend, backend, strlen(backend) + 1U);
    return row;
}

static void stats_load_row(AiStats *stats, const FlMap *map)
{
    FlValue latency;
    const FlList *list;
    u32 i;

#define LOAD(field_) stats->field_ = map_u64(map, #field_)
    LOAD(requests);
    LOAD(declined);
    LOAD(delivered);
    LOAD(cancelled);
    LOAD(errors);
    LOAD(accepted_word);
    LOAD(accepted_line);
    LOAD(accepted_all);
    LOAD(dismissed);
    LOAD(bytes_suggested);
    LOAD(bytes_accepted);
    LOAD(first_token_ms_sum);
    LOAD(first_token_ms_max);
    LOAD(n_first_token);
    LOAD(tokens_in);
    LOAD(tokens_out);
#undef LOAD
    if (!map_get(map, "first_token_ms", &latency) ||
        latency.t != (u8)FL_LIST)
        return;
    list = (const FlList *)latency.as.o;
    for (i = 0U; i < list->n && i < AI_STATS_LATENCY_CAP; i++) {
        if (list->v[i].t != (u8)FL_INT || list->v[i].as.i < 0)
            continue;
        stats->first_token_ms[stats->latency_len++] =
            (u64)list->v[i].as.i;
    }
    stats->latency_at = stats->latency_len % AI_STATS_LATENCY_CAP;
}

static void stats_load(AiStatsState *state)
{
    char *path;
    Bytebuf bytes;
    bool missing;
    StatsDoc doc;
    FlValue root;
    FlValue schema;
    FlValue backends;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    if (state == NULL || state->loaded)
        return;
    state->loaded = true;
    path = stats_path(false);
    if (path == NULL)
        return;
    bytebuf_init(&bytes);
    if (!stats_read(path, &bytes, &missing)) {
        bytebuf_free(&bytes);
        free(path);
        return;
    }
    free(path);
    stats_doc_init(&doc);
    root = fl_data_read(&doc.vm, (const char *)bytes.data, bytes.len,
                        &doc.dc);
    bytebuf_free(&bytes);
    if (fl_diag_errors(&doc.dc) != 0U || root.t != (u8)FL_MAP ||
        !map_get((const FlMap *)root.as.o, "schema", &schema) ||
        schema.t != (u8)FL_INT || schema.as.i != 1 ||
        !map_get((const FlMap *)root.as.o, "backends", &backends) ||
        backends.t != (u8)FL_MAP) {
        stats_doc_free(&doc);
        return;
    }
    while (fl_map_iter((const FlMap *)backends.as.o, &cursor, &key,
                       &value)) {
        const FlStr *name;
        char *copy;
        AiStatsRow *row;

        if (key.t != (u8)FL_STR || value.t != (u8)FL_MAP)
            continue;
        name = (const FlStr *)key.as.o;
        copy = yew_xmalloc((size_t)name->len + 1U);
        (void)memcpy(copy, name->b, name->len);
        copy[name->len] = '\0';
        row = stats_row(state, copy, true);
        free(copy);
        if (row != NULL)
            stats_load_row(&row->stats, (const FlMap *)value.as.o);
    }
    stats_doc_free(&doc);
}

static FlValue stats_key(StatsDoc *doc, const char *name)
{
    return FL_OBJ_V(FL_STR,
                    fl_str_new(&doc->vm, name, (u32)strlen(name)));
}

static i64 stats_i64(u64 value)
{
    return value > (u64)INT64_MAX ? INT64_MAX : (i64)value;
}

static void map_put(StatsDoc *doc, FlMap *map, const char *name,
                    FlValue value)
{
    (void)fl_map_set(&doc->vm, map, stats_key(doc, name), value);
}

static FlMap *stats_emit_row(StatsDoc *doc, const AiStats *stats)
{
    FlMap *map = fl_map_new(&doc->vm);
    FlList *latency;
    u32 i;

#define EMIT(field_) map_put(doc, map, #field_, FL_INT_V(stats_i64(stats->field_)))
    EMIT(requests);
    EMIT(declined);
    EMIT(delivered);
    EMIT(cancelled);
    EMIT(errors);
    EMIT(accepted_word);
    EMIT(accepted_line);
    EMIT(accepted_all);
    EMIT(dismissed);
    EMIT(bytes_suggested);
    EMIT(bytes_accepted);
    EMIT(first_token_ms_sum);
    EMIT(first_token_ms_max);
    EMIT(n_first_token);
    EMIT(tokens_in);
    EMIT(tokens_out);
#undef EMIT
    latency = fl_list_new(&doc->vm);
    for (i = 0U; i < stats->latency_len; i++) {
        u32 at = stats->latency_len == AI_STATS_LATENCY_CAP ?
                 (stats->latency_at + i) % AI_STATS_LATENCY_CAP : i;

        (void)fl_list_push(&doc->vm, latency,
                           FL_INT_V(stats_i64(stats->first_token_ms[at])));
    }
    map_put(doc, map, "first_token_ms", FL_OBJ_V(FL_LIST, latency));
    return map;
}

static bool stats_save(AiStatsState *state)
{
    char *path;
    StatsDoc doc;
    FlMap *root;
    FlMap *backends;
    Bytebuf out;
    u32 i;
    bool ok;

    if (state == NULL || !state->dirty)
        return true;
    path = stats_path(true);
    if (path == NULL)
        return false;
    stats_doc_init(&doc);
    root = fl_map_new(&doc.vm);
    backends = fl_map_new(&doc.vm);
    map_put(&doc, root, "schema", FL_INT_V(1));
    map_put(&doc, root, "backends", FL_OBJ_V(FL_MAP, backends));
    for (i = 0U; i < state->len; i++)
        (void)fl_map_set(&doc.vm, backends,
                         stats_key(&doc, state->rows[i].backend),
                         FL_OBJ_V(FL_MAP,
                                  stats_emit_row(&doc,
                                                 &state->rows[i].stats)));
    bytebuf_init(&out);
    bytebuf_append(&out,
                   (const u8 *)"# yew AI statistics — local-only and hand-editable.\n",
                   sizeof("# yew AI statistics — local-only and hand-editable.\n") - 1U);
    fl_data_write(&out, FL_OBJ_V(FL_MAP, root), 0U);
    ok = yew_file_write_atomic(path, out.data, out.len, 0600) == YEW_SAVE_OK;
    bytebuf_free(&out);
    stats_doc_free(&doc);
    free(path);
    if (ok)
        state->dirty = false;
    return ok;
}

AiStatsState *yew_ai_stats_new(void)
{
    AiStatsState *state = yew_xcalloc(1U, sizeof(*state));

    state->last_save_ms = -1;
    return state;
}

void yew_ai_stats_free(Ed *ed, AiStatsState *state)
{
    u32 i;

    (void)ed;
    if (state == NULL)
        return;
    stats_load(state);
    (void)stats_save(state);
    for (i = 0U; i < state->len; i++)
        free(state->rows[i].backend);
    free(state->rows);
    free(state);
}

static AiStats *stats_for(Ed *ed, const char *backend)
{
    AiStatsState *state;
    AiStatsRow *row;

    if (ed == NULL || ed->ai == NULL || ed->ai->stats == NULL)
        return NULL;
    state = ed->ai->stats;
    stats_load(state);
    row = stats_row(state, backend, true);
    if (row == NULL)
        return NULL;
    state->dirty = true;
    return &row->stats;
}

void yew_ai_stats_pump(Ed *ed, i64 now_ms)
{
    AiStatsState *state;

    if (ed == NULL || ed->ai == NULL || ed->ai->stats == NULL)
        return;
    state = ed->ai->stats;
    if (!state->dirty)
        return;
    if (state->last_save_ms < 0 ||
        now_ms - state->last_save_ms >= (i64)AI_STATS_SAVE_MS) {
        if (stats_save(state))
            state->last_save_ms = now_ms;
    }
}

#define STATS_NOTE(name_, field_)                                      \
    void name_(Ed *ed, const char *backend)                            \
    {                                                                  \
        AiStats *stats = stats_for(ed, backend);                        \
        if (stats != NULL)                                             \
            stats->field_++;                                           \
    }

STATS_NOTE(yew_ai_stats_request, requests)
STATS_NOTE(yew_ai_stats_decline, declined)
STATS_NOTE(yew_ai_stats_cancel, cancelled)
STATS_NOTE(yew_ai_stats_error, errors)
STATS_NOTE(yew_ai_stats_dismiss, dismissed)

#undef STATS_NOTE

void yew_ai_stats_delivery(Ed *ed, const char *backend, u64 bytes)
{
    AiStats *stats = stats_for(ed, backend);

    if (stats == NULL)
        return;
    stats->delivered++;
    stats->bytes_suggested += bytes;
}

void yew_ai_stats_accept(Ed *ed, const char *backend, u8 kind, u64 bytes)
{
    AiStats *stats = stats_for(ed, backend);

    if (stats == NULL)
        return;
    if (kind == 0U)
        stats->accepted_word++;
    else if (kind == 1U)
        stats->accepted_line++;
    else
        stats->accepted_all++;
    stats->bytes_accepted += bytes;
}

void yew_ai_stats_finish(Ed *ed, const char *backend, i64 first_token_ms,
                         i64 tokens_in, i64 tokens_out)
{
    AiStats *stats = stats_for(ed, backend);

    if (stats == NULL)
        return;
    if (first_token_ms >= 0) {
        u64 latency = (u64)first_token_ms;

        stats->first_token_ms_sum += latency;
        if (latency > stats->first_token_ms_max)
            stats->first_token_ms_max = latency;
        stats->n_first_token++;
        stats->first_token_ms[stats->latency_at] = latency;
        stats->latency_at = (stats->latency_at + 1U) % AI_STATS_LATENCY_CAP;
        if (stats->latency_len < AI_STATS_LATENCY_CAP)
            stats->latency_len++;
    }
    if (tokens_in > 0)
        stats->tokens_in += (u64)tokens_in;
    if (tokens_out > 0)
        stats->tokens_out += (u64)tokens_out;
}

static u64 stats_p50(const AiStats *stats)
{
    u64 values[AI_STATS_LATENCY_CAP];
    u32 i;

    if (stats->latency_len == 0U)
        return 0U;
    for (i = 0U; i < stats->latency_len; i++) {
        u32 j = i;
        u64 value = stats->first_token_ms[i];

        while (j != 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
    return values[(stats->latency_len - 1U) / 2U];
}

CmdStatus yew_ai_cmd_stats(CmdCtx *cx)
{
    static const char heading[] =
        "backend  requests  delivered  accepted  accept%  "
        "p50 first token  tokens out\n";
    static const char empty[] = "(no AI requests recorded)\n";
    AiStatsState *state;
    Bytebuf out;
    u32 i;

    if (cx == NULL || cx->ed == NULL || cx->ed->ai == NULL ||
        cx->ed->ai->stats == NULL)
        return YEW_CMD_ERR_STATE;
    state = cx->ed->ai->stats;
    stats_load(state);
    bytebuf_init(&out);
    bytebuf_append(&out, heading, sizeof(heading) - 1U);
    for (i = 0U; i < state->len; i++) {
        const AiStatsRow *row = &state->rows[i];
        u64 accepted = row->stats.accepted_word +
                       row->stats.accepted_line +
                       row->stats.accepted_all;
        double percent = row->stats.delivered == 0U ? 0.0 :
                         (double)accepted * 100.0 /
                             (double)row->stats.delivered;
        char line[256];
        int n = snprintf(line, sizeof(line),
                         "%-8s %9llu %10llu %9llu %7.1f%% %13llu ms %11llu\n",
                         row->backend,
                         (unsigned long long)row->stats.requests,
                         (unsigned long long)row->stats.delivered,
                         (unsigned long long)accepted, percent,
                         (unsigned long long)stats_p50(&row->stats),
                         (unsigned long long)row->stats.tokens_out);

        if (n > 0)
            bytebuf_append(&out, line,
                           (size_t)n < sizeof(line) ? (size_t)n :
                                                     sizeof(line) - 1U);
    }
    if (state->len == 0U)
        bytebuf_append(&out, empty, sizeof(empty) - 1U);
    yew_msg(cx->ed, YEW_MSG_INFO, "%.*s", (int)out.len,
            (const char *)out.data);
    bytebuf_free(&out);
    return YEW_CMD_OK;
}
