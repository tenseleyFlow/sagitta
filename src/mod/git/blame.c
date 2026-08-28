#include "mod/git/blame.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/git/git.h"
#include "util/arena.h"
#include "util/log.h"

typedef struct BlameMeta {
    char sha[YEW_BLAME_SHA_MAX + 1U];
    char *author;
    char *summary;
    i64 author_time;
    bool uncommitted;
} BlameMeta;

typedef struct BlameBlock {
    BlameLine *lines;
    u64 text_gen;
    u32 buf_id;
    u32 lo;
    u32 hi;
} BlameBlock;

typedef struct BlameFlight {
    BlameRequest request;
    bool active;
} BlameFlight;

struct BlameCache {
    BlameBlock *blocks;
    size_t nblocks;
    size_t blocks_cap;
    BlameMeta **meta;
    size_t nmeta;
    size_t meta_cap;
    BlameFlight flights[YEW_BLAME_MAX_INFLIGHT];
    BlameRequest observed;
    BlameLine stale_view;
    i64 observed_ms;
    u64 next_token;
    u32 inflight;
    bool observed_valid;
    bool observed_pending;
};

static char *blame_dup(const char *text)
{
    size_t len;
    char *copy;

    if (text == NULL)
        text = "";
    len = strlen(text);
    copy = yew_xmalloc(len + 1U);
    (void)memcpy(copy, text, len + 1U);
    return copy;
}

static void block_drop(BlameBlock *block)
{
    yew_xfree(block->lines);
    (void)memset(block, 0, sizeof(*block));
}

BlameCache *yew_blame_cache_new(void)
{
    BlameCache *cache = yew_xcalloc(1U, sizeof(*cache));

    cache->next_token = 1U;
    return cache;
}

void yew_blame_cache_free(BlameCache *cache)
{
    size_t i;

    if (cache == NULL)
        return;
    for (i = 0U; i < cache->nblocks; i++)
        block_drop(&cache->blocks[i]);
    for (i = 0U; i < cache->nmeta; i++) {
        yew_xfree(cache->meta[i]->author);
        yew_xfree(cache->meta[i]->summary);
        yew_xfree(cache->meta[i]);
    }
    yew_xfree(cache->blocks);
    yew_xfree(cache->meta);
    yew_xfree(cache);
}

void yew_blame_quantize(LineNo top, LineNo bottom, u64 line_count,
                        u32 *range_lo, u32 *range_hi)
{
    u64 lo;
    u64 hi;

    if (line_count == 0U) {
        if (range_lo != NULL)
            *range_lo = 0U;
        if (range_hi != NULL)
            *range_hi = 0U;
        return;
    }
    if (bottom.v < top.v)
        bottom = top;
    if (line_count > UINT32_MAX)
        line_count = UINT32_MAX;
    if (top.v > line_count)
        top.v = line_count;
    if (bottom.v >= line_count && line_count != 0U)
        bottom.v = line_count - 1U;
    lo = (top.v / YEW_BLAME_RANGE_LINES) * YEW_BLAME_RANGE_LINES;
    hi = ((bottom.v / YEW_BLAME_RANGE_LINES) + 1U) *
         YEW_BLAME_RANGE_LINES;
    if (hi > line_count)
        hi = line_count;
    if (range_lo != NULL)
        *range_lo = (u32)lo;
    if (range_hi != NULL)
        *range_hi = (u32)hi;
}

static bool request_same(const BlameRequest *a, const BlameRequest *b)
{
    return a->buf_id == b->buf_id && a->text_gen == b->text_gen &&
           a->range_lo == b->range_lo && a->range_hi == b->range_hi;
}

static BlameBlock *block_find(BlameCache *cache, u32 buf_id, u32 lo)
{
    size_t i;

    for (i = 0U; i < cache->nblocks; i++) {
        if (cache->blocks[i].buf_id == buf_id && cache->blocks[i].lo == lo)
            return &cache->blocks[i];
    }
    return NULL;
}

static bool request_inflight(const BlameCache *cache,
                             const BlameRequest *request)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cache->flights); i++) {
        if (cache->flights[i].active &&
            request_same(&cache->flights[i].request, request))
            return true;
    }
    return false;
}

void yew_blame_cache_observe(BlameCache *cache, u32 buf_id, u64 text_gen,
                             LineNo top, LineNo bottom, u64 line_count,
                             i64 now_ms)
{
    BlameRequest next = {0};
    BlameBlock *block;

    if (cache == NULL || buf_id == 0U || line_count == 0U)
        return;
    next.buf_id = buf_id;
    next.text_gen = text_gen;
    yew_blame_quantize(top, bottom, line_count, &next.range_lo,
                       &next.range_hi);
    if (next.range_lo == next.range_hi)
        return;
    if (cache->observed_valid && request_same(&cache->observed, &next))
        return;
    cache->observed = next;
    cache->observed_valid = true;
    cache->observed_ms = now_ms;
    block = block_find(cache, buf_id, next.range_lo);
    cache->observed_pending =
        (block == NULL || block->text_gen != text_gen) &&
        !request_inflight(cache, &next);
}

bool yew_blame_cache_take_request(BlameCache *cache, i64 now_ms,
                                  BlameRequest *request)
{
    size_t i;
    BlameRequest next;

    if (cache == NULL || request == NULL || !cache->observed_pending ||
        cache->inflight >= YEW_BLAME_MAX_INFLIGHT ||
        now_ms < cache->observed_ms ||
        now_ms - cache->observed_ms < YEW_BLAME_DEBOUNCE_MS)
        return false;
    next = cache->observed;
    if (request_inflight(cache, &next)) {
        cache->observed_pending = false;
        return false;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(cache->flights); i++) {
        if (!cache->flights[i].active)
            break;
    }
    if (i == YEW_ARRAY_LEN(cache->flights))
        YEW_BUG("blame cache inflight count disagrees with slots");
    next.token = cache->next_token++;
    if (next.token == 0U)
        next.token = cache->next_token++;
    cache->flights[i].request = next;
    cache->flights[i].active = true;
    cache->inflight++;
    cache->observed_pending = false;
    *request = next;
    return true;
}

static BlameFlight *flight_find(BlameCache *cache,
                                const BlameRequest *request)
{
    size_t i;

    if (cache == NULL || request == NULL || request->token == 0U)
        return NULL;
    for (i = 0U; i < YEW_ARRAY_LEN(cache->flights); i++) {
        if (cache->flights[i].active &&
            cache->flights[i].request.token == request->token &&
            request_same(&cache->flights[i].request, request))
            return &cache->flights[i];
    }
    return NULL;
}

static void flight_drop(BlameCache *cache, BlameFlight *flight)
{
    if (cache == NULL || flight == NULL || !flight->active)
        return;
    (void)memset(flight, 0, sizeof(*flight));
    if (cache->inflight == 0U)
        YEW_BUG("blame cache inflight underflow");
    cache->inflight--;
}

void yew_blame_cache_fail(BlameCache *cache, const BlameRequest *request)
{
    BlameFlight *flight = flight_find(cache, request);

    if (flight != NULL && cache->observed_valid &&
        request_same(&cache->observed, &flight->request))
        cache->observed_pending = true;
    flight_drop(cache, flight);
}

static bool sha_zero(const char *sha)
{
    size_t i;

    if (sha == NULL || sha[0] == '\0')
        return false;
    for (i = 0U; sha[i] != '\0'; i++) {
        if (sha[i] != '0')
            return false;
    }
    return true;
}

static BlameMeta *meta_find(BlameCache *cache, const char *sha)
{
    size_t i;

    for (i = 0U; i < cache->nmeta; i++) {
        if (strcmp(cache->meta[i]->sha, sha) == 0)
            return cache->meta[i];
    }
    return NULL;
}

static BlameMeta *meta_merge(BlameCache *cache, const GitCommitMeta *source)
{
    BlameMeta *meta = meta_find(cache, source->sha);
    size_t sha_len;

    if (meta != NULL) {
        return meta;
    }
    meta = yew_xcalloc(1U, sizeof(*meta));
    sha_len = strlen(source->sha);
    if (sha_len > YEW_BLAME_SHA_MAX)
        sha_len = YEW_BLAME_SHA_MAX;
    (void)memcpy(meta->sha, source->sha, sha_len);
    meta->sha[sha_len] = '\0';
    meta->author = blame_dup(source->author);
    meta->summary = blame_dup(source->summary);
    meta->author_time = source->author_time;
    meta->uncommitted = sha_zero(meta->sha);
    if (cache->nmeta == cache->meta_cap) {
        size_t cap = cache->meta_cap == 0U ? 8U : cache->meta_cap * 2U;

        cache->meta = yew_xreallocarray(cache->meta, cap,
                                        sizeof(*cache->meta));
        cache->meta_cap = cap;
    }
    cache->meta[cache->nmeta++] = meta;
    return meta;
}

static BlameBlock *block_slot(BlameCache *cache, const BlameRequest *request)
{
    BlameBlock *block = block_find(cache, request->buf_id, request->range_lo);

    if (block != NULL)
        return block;
    if (cache->nblocks == cache->blocks_cap) {
        size_t cap = cache->blocks_cap == 0U ? 8U : cache->blocks_cap * 2U;

        cache->blocks = yew_xreallocarray(cache->blocks, cap,
                                          sizeof(*cache->blocks));
        (void)memset(cache->blocks + cache->blocks_cap, 0,
                     (cap - cache->blocks_cap) * sizeof(*cache->blocks));
        cache->blocks_cap = cap;
    }
    block = &cache->blocks[cache->nblocks++];
    block->buf_id = request->buf_id;
    block->lo = request->range_lo;
    return block;
}

bool yew_blame_cache_finish(BlameCache *cache, const BlameRequest *request,
                            const u8 *output, u64 output_len)
{
    BlameFlight *flight = flight_find(cache, request);
    Arena parsed_arena;
    GitBlameLineList parsed_lines;
    GitCommitMetaList parsed_meta;
    GitParseErr error;
    BlameLine *lines = NULL;
    BlameMeta **meta = NULL;
    BlameBlock *block;
    u64 count;
    size_t i;
    bool ok = false;

    if (flight == NULL || (output == NULL && output_len != 0U))
        return false;
    arena_init(&parsed_arena);
    (void)memset(&parsed_lines, 0, sizeof(parsed_lines));
    (void)memset(&parsed_meta, 0, sizeof(parsed_meta));
    if (yew_git_parse_blame(&parsed_arena, output, output_len, &parsed_lines,
                            &parsed_meta, &error) == 0U)
        goto done;
    count = (u64)request->range_hi - request->range_lo;
    if (count > SIZE_MAX / sizeof(*lines))
        goto done;
    lines = yew_xcalloc((size_t)count, sizeof(*lines));
    meta = parsed_meta.len == 0U ? NULL :
           yew_xcalloc(parsed_meta.len, sizeof(*meta));
    for (i = 0U; i < parsed_meta.len; i++)
        meta[i] = meta_merge(cache, &parsed_meta.data[i]);
    for (i = 0U; i < parsed_lines.len; i++) {
        GitBlameLine source = parsed_lines.data[i];
        BlameMeta *m;
        u64 line;

        if (source.commit >= parsed_meta.len || source.lineno == 0U)
            goto done;
        line = (u64)source.lineno - 1U;
        if (line < request->range_lo || line >= request->range_hi)
            continue;
        m = meta[source.commit];
        lines[line - request->range_lo] =
            (BlameLine){m->sha, m->author, m->summary, m->author_time,
                        m->uncommitted, false};
    }
    block = block_slot(cache, request);
    if (block->lines == NULL || block->text_gen <= request->text_gen) {
        yew_xfree(block->lines);
        block->lines = lines;
        lines = NULL;
        block->text_gen = request->text_gen;
        block->hi = request->range_hi;
    }
    ok = true;
done:
    yew_xfree(lines);
    yew_xfree(meta);
    arena_free_all(&parsed_arena);
    if (!ok && cache->observed_valid &&
        request_same(&cache->observed, &flight->request))
        cache->observed_pending = true;
    flight_drop(cache, flight);
    return ok;
}

const BlameLine *yew_blame_cache_at(BlameCache *cache, u32 buf_id,
                                    u64 text_gen, LineNo line)
{
    u32 lo;
    BlameBlock *block;
    const BlameLine *found;

    if (cache == NULL || line.v > UINT32_MAX)
        return NULL;
    lo = ((u32)line.v / YEW_BLAME_RANGE_LINES) * YEW_BLAME_RANGE_LINES;
    block = block_find(cache, buf_id, lo);
    if (block == NULL || block->lines == NULL || line.v >= block->hi)
        return NULL;
    found = &block->lines[line.v - block->lo];
    if (found->sha == NULL)
        return NULL;
    if (block->text_gen == text_gen)
        return found;
    cache->stale_view = *found;
    cache->stale_view.stale = true;
    return &cache->stale_view;
}

u32 yew_blame_cache_inflight(const BlameCache *cache)
{
    return cache == NULL ? 0U : cache->inflight;
}

size_t yew_blame_cache_metadata_count(const BlameCache *cache)
{
    return cache == NULL ? 0U : cache->nmeta;
}

static size_t relative_write(char *dst, size_t cap, long long value,
                             const char *unit)
{
    int n;

    if (cap == 0U)
        return 0U;
    n = snprintf(dst, cap, "%lld %s%s ago", value, unit,
                 value == 1 ? "" : "s");
    if (n < 0) {
        dst[0] = '\0';
        return 0U;
    }
    return (size_t)n;
}

size_t yew_blame_relative_time(char *dst, size_t cap, i64 author_epoch,
                               i64 now_epoch)
{
    static const struct {
        i64 seconds;
        const char *unit;
    } units[] = {
        {365 * 24 * 60 * 60, "year"}, {30 * 24 * 60 * 60, "month"},
        {7 * 24 * 60 * 60, "week"}, {24 * 60 * 60, "day"},
        {60 * 60, "hour"}, {60, "minute"}, {1, "second"}
    };
    i64 age = now_epoch >= author_epoch ? now_epoch - author_epoch : 0;
    size_t i;

    if (dst == NULL || cap == 0U)
        return 0U;
    if (age == 0) {
        (void)snprintf(dst, cap, "now");
        return 3U;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(units); i++) {
        if (age >= units[i].seconds) {
            long long value = (long long)(age / units[i].seconds);

            return relative_write(dst, cap, value, units[i].unit);
        }
    }
    return 0U;
}

size_t yew_blame_format(char *dst, size_t cap, const BlameLine *line,
                        i64 now_epoch)
{
    char relative[64];
    int n;

    if (dst == NULL || cap == 0U || line == NULL)
        return 0U;
    if (line->uncommitted) {
        n = snprintf(dst, cap, "  ▏ (uncommitted)");
    } else {
        (void)yew_blame_relative_time(relative, sizeof(relative),
                                      line->author_time, now_epoch);
        n = snprintf(dst, cap, "  ▏ %s, %s · %s",
                     line->author == NULL || line->author[0] == '\0' ?
                         "(unknown)" : line->author,
                     relative,
                     line->summary == NULL ? "" : line->summary);
    }
    if (n < 0) {
        dst[0] = '\0';
        return 0U;
    }
    return (size_t)n;
}

bool yew_blame_layout(u16 row_cells, u16 text_end, bool wrapped,
                      bool final_display_row, u16 *annotation_col,
                      u16 *available_cells)
{
    u16 available = text_end < row_cells ? (u16)(row_cells - text_end) : 0U;

    if (available_cells != NULL)
        *available_cells = available;
    if (annotation_col != NULL)
        *annotation_col = text_end < row_cells ? text_end : row_cells;
    return (!wrapped || final_display_row) &&
           available >= YEW_BLAME_MIN_REMAINING_CELLS;
}
