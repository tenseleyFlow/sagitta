#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "mod/ai/stream.h"

typedef struct EventsSeen {
    u64 count;
    u64 data_bytes;
    u64 hash;
    bool valid;
} EventsSeen;

typedef struct StreamResult {
    bool pending_cr;
    bool saw_bom;
    bool terminated;
    bool ended;
    u64 events;
    u64 data_bytes;
    u64 event_hash;
    size_t line_len;
    size_t data_len;
    size_t evt_len;
    u64 line_hash;
    u64 data_hash;
    u64 evt_hash;
    char err[96];
} StreamResult;

static u64 hash_bytes(u64 hash, const u8 *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static void event_seen(void *ctx, const AiEvent *event)
{
    EventsSeen *seen = ctx;
    const u8 marker = event->terminator ? 1U : 0U;

    if (!seen->valid)
        return;
    if (event->tlen > YEW_AI_LINE_MAX ||
        event->dlen > YEW_HTTP_MAX_BODY ||
        event->dlen > YEW_HTTP_MAX_BODY - seen->data_bytes) {
        seen->valid = false;
        return;
    }
    seen->hash = hash_bytes(seen->hash, (const u8 *)&event->tlen,
                            sizeof(event->tlen));
    seen->hash = hash_bytes(seen->hash, event->type, event->tlen);
    seen->hash = hash_bytes(seen->hash, (const u8 *)&event->dlen,
                            sizeof(event->dlen));
    seen->hash = hash_bytes(seen->hash, event->data, event->dlen);
    seen->hash = hash_bytes(seen->hash, &marker, 1U);
    seen->data_bytes += event->dlen;
    seen->count++;
}

static size_t next_chunk(const u8 *data, size_t len, size_t at,
                         unsigned schedule)
{
    size_t chunk;

    if (schedule == 0U)
        return len - at;
    if (schedule == 1U)
        return 1U;
    chunk = 1U + (size_t)(data[at] & 31U);
    if (chunk > len - at)
        chunk = len - at;
    return chunk;
}

static bool run_stream(const u8 *data, size_t len, AiStreamMode mode,
                       unsigned schedule, StreamResult *result,
                       char *why, size_t why_cap)
{
    AiStream stream;
    EventsSeen seen;
    size_t at = 0U;

    (void)memset(&seen, 0, sizeof(seen));
    seen.hash = UINT64_C(1469598103934665603);
    seen.valid = true;
    yew_ai_stream_init(&stream, mode);
    while (at < len && stream.err[0] == '\0' && !stream.ended) {
        size_t chunk = next_chunk(data, len, at, schedule);

        yew_ai_stream_feed(&stream, data + at, (u64)chunk,
                           at + chunk == len, event_seen, &seen);
        at += chunk;
    }
    if (len == 0U)
        yew_ai_stream_feed(&stream, NULL, 0U, true, event_seen, &seen);
    if (!seen.valid) {
        yew_ai_stream_free(&stream);
        return fail(why, why_cap,
                    "AI stream emitted an event outside resource caps");
    }
    if (stream.line.len > YEW_AI_LINE_MAX ||
        stream.data.len > YEW_HTTP_MAX_BODY ||
        stream.evt.len > YEW_AI_LINE_MAX ||
        stream.events != seen.count || seen.data_bytes > stream.bytes ||
        seen.count > stream.bytes + 1U) {
        yew_ai_stream_free(&stream);
        return fail(why, why_cap, "AI stream exceeded a resource cap");
    }
    if (stream.err[0] == '\0' && !stream.ended) {
        yew_ai_stream_free(&stream);
        return fail(why, why_cap, "AI stream was not terminal at EOF");
    }

    (void)memset(result, 0, sizeof(*result));
    result->pending_cr = stream.pending_cr;
    result->saw_bom = stream.saw_bom;
    result->terminated = stream.terminated;
    result->ended = stream.ended;
    result->events = stream.events;
    result->data_bytes = seen.data_bytes;
    result->event_hash = seen.hash;
    result->line_len = stream.line.len;
    result->data_len = stream.data.len;
    result->evt_len = stream.evt.len;
    result->line_hash = hash_bytes(UINT64_C(1469598103934665603),
                                   stream.line.data, stream.line.len);
    result->data_hash = hash_bytes(UINT64_C(1469598103934665603),
                                   stream.data.data, stream.data.len);
    result->evt_hash = hash_bytes(UINT64_C(1469598103934665603),
                                  stream.evt.data, stream.evt.len);
    (void)memcpy(result->err, stream.err, sizeof(result->err));
    yew_ai_stream_free(&stream);
    return true;
}

static bool same_result(const StreamResult *a, const StreamResult *b)
{
    return a->pending_cr == b->pending_cr &&
           a->saw_bom == b->saw_bom &&
           a->terminated == b->terminated &&
           a->ended == b->ended &&
           a->events == b->events &&
           a->data_bytes == b->data_bytes &&
           a->event_hash == b->event_hash &&
           a->line_len == b->line_len &&
           a->data_len == b->data_len &&
           a->evt_len == b->evt_len &&
           a->line_hash == b->line_hash &&
           a->data_hash == b->data_hash &&
           a->evt_hash == b->evt_hash &&
           memcmp(a->err, b->err, sizeof(a->err)) == 0;
}

static bool check_mode(const u8 *data, size_t len, AiStreamMode mode,
                       char *why, size_t why_cap)
{
    StreamResult whole;
    StreamResult bytewise;
    StreamResult varied;

    if (!run_stream(data, len, mode, 0U, &whole, why, why_cap) ||
        !run_stream(data, len, mode, 1U, &bytewise, why, why_cap) ||
        !run_stream(data, len, mode, 2U, &varied, why, why_cap))
        return false;
    if (!same_result(&whole, &bytewise) ||
        !same_result(&whole, &varied))
        return fail(why, why_cap,
                    "AI stream result depends on input chunk boundaries");
    return true;
}

static bool check_input(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    return check_mode(data, len, YEW_AISTREAM_SSE, why, why_cap) &&
           check_mode(data, len, YEW_AISTREAM_NDJSON, why, why_cap) &&
           check_mode(data, len, YEW_AISTREAM_WHOLE, why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_ai_stream", NULL, check_input);
}
