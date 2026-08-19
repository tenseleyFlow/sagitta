#include "mod/ai/stream.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mod/lsp/json.h"
#include "util/arena.h"

static void stream_error(AiStream *s, const char *msg)
{
    if (s->err[0] == '\0')
        (void)snprintf(s->err, sizeof(s->err), "%s", msg);
}

static bool stream_append(AiStream *s, Bytebuf *dst, const u8 *bytes,
                          size_t len, size_t cap, const char *err)
{
    if (len > cap - dst->len) {
        stream_error(s, err);
        return false;
    }
    bytebuf_append(dst, bytes, len);
    return true;
}

static void stream_dispatch(AiStream *s, const u8 *type, size_t tlen,
                            const u8 *data, size_t dlen, bool terminator,
                            AiEventFn on_event, void *ctx)
{
    AiEvent event;

    event.type = type;
    event.tlen = (u32)tlen;
    event.data = data;
    event.dlen = (u32)dlen;
    event.terminator = terminator;
    s->events++;
    if (terminator)
        s->terminated = true;
    if (on_event != NULL)
        on_event(ctx, &event);
}

static void stream_strip_bom(AiStream *s, const u8 **line, size_t *len)
{
    static const u8 bom[] = {0xefU, 0xbbU, 0xbfU};

    if (s->saw_bom)
        return;
    s->saw_bom = true;
    if (*len >= sizeof(bom) && memcmp(*line, bom, sizeof(bom)) == 0) {
        *line += sizeof(bom);
        *len -= sizeof(bom);
    }
}

static bool bytes_equal(const u8 *bytes, size_t len, const char *text)
{
    size_t n = strlen(text);

    return len == n && memcmp(bytes, text, n) == 0;
}

static void stream_sse_dispatch(AiStream *s, bool framed,
                                AiEventFn on_event, void *ctx)
{
    bool done;

    if (s->data.len == 0U) {
        s->data.len = 0U;
        s->evt.len = 0U;
        s->have_data_line = false;
        return;
    }
    done = bytes_equal(s->data.data, s->data.len, "[DONE]");
    stream_dispatch(s, s->evt.data, s->evt.len,
                    done ? NULL : s->data.data,
                    done ? 0U : s->data.len, framed && done, on_event, ctx);
    s->data.len = 0U;
    s->evt.len = 0U;
    s->have_data_line = false;
    if (framed && done)
        s->ended = true;
}

static void stream_sse_line(AiStream *s, const u8 *line, size_t len,
                            AiEventFn on_event, void *ctx)
{
    const u8 *colon;
    const u8 *value;
    size_t field_len;
    size_t value_len;

    stream_strip_bom(s, &line, &len);
    if (len == 0U) {
        stream_sse_dispatch(s, true, on_event, ctx);
        return;
    }
    if (line[0] == (u8)':')
        return;
    colon = memchr(line, ':', len);
    field_len = colon == NULL ? len : (size_t)(colon - line);
    value = colon == NULL ? line + len : colon + 1;
    value_len = colon == NULL ? 0U : len - field_len - 1U;
    if (value_len != 0U && value[0] == (u8)' ') {
        value++;
        value_len--;
    }
    if (field_len == 4U && memcmp(line, "data", 4U) == 0) {
        static const u8 lf = (u8)'\n';

        if (s->have_data_line &&
            !stream_append(s, &s->data, &lf, 1U,
                           YEW_HTTP_MAX_BODY,
                           "stream event exceeded 64 MiB"))
            return;
        s->have_data_line = true;
        (void)stream_append(s, &s->data, value, value_len,
                            YEW_HTTP_MAX_BODY,
                            "stream event exceeded 64 MiB");
    } else if (field_len == 5U && memcmp(line, "event", 5U) == 0) {
        s->evt.len = 0U;
        (void)stream_append(s, &s->evt, value, value_len,
                            YEW_AI_LINE_MAX,
                            "stream event name exceeded 1 MiB");
    }
}

static bool stream_ndjson_done(AiStream *s, const u8 *line, size_t len)
{
    Arena arena;
    JsonErr json_error;
    JsonValue *root;
    const JsonValue *done;
    bool is_done;

    arena_init(&arena);
    root = yew_json_parse(&arena, line, (u64)len, &json_error);
    if (root == NULL) {
        stream_error(s, json_error.msg);
        arena_free_all(&arena);
        return false;
    }
    done = yew_json_get(root, "done");
    is_done = done != NULL && done->kind == YEW_JS_BOOL &&
              yew_json_bool(done, false);
    arena_free_all(&arena);
    return is_done;
}

static void stream_ndjson_line(AiStream *s, const u8 *line, size_t len,
                               AiEventFn on_event, void *ctx)
{
    bool done;

    stream_strip_bom(s, &line, &len);
    if (len == 0U)
        return;
    done = stream_ndjson_done(s, line, len);
    if (s->err[0] != '\0')
        return;
    stream_dispatch(s, NULL, 0U, line, len, done, on_event, ctx);
    if (done)
        s->ended = true;
}

static void stream_line(AiStream *s, AiEventFn on_event, void *ctx)
{
    if (s->mode == YEW_AISTREAM_SSE)
        stream_sse_line(s, s->line.data, s->line.len, on_event, ctx);
    else
        stream_ndjson_line(s, s->line.data, s->line.len, on_event, ctx);
    s->line.len = 0U;
}

static void stream_feed_lines(AiStream *s, const u8 *bytes, u64 len,
                              AiEventFn on_event, void *ctx)
{
    u64 i;

    for (i = 0U; i < len && s->err[0] == '\0' && !s->ended; i++) {
        u8 byte = bytes[i];

        if (s->pending_cr) {
            s->pending_cr = false;
            if (byte == (u8)'\n')
                continue;
        }
        if (byte == (u8)'\r' || byte == (u8)'\n') {
            stream_line(s, on_event, ctx);
            s->pending_cr = byte == (u8)'\r';
        } else if (s->line.len == YEW_AI_LINE_MAX) {
            stream_error(s, "stream line exceeded 1 MiB");
        } else {
            bytebuf_push_u8(&s->line, byte);
        }
    }
}

static void stream_finish_lines(AiStream *s, AiEventFn on_event, void *ctx)
{
    if (s->mode == YEW_AISTREAM_NDJSON) {
        if (s->line.len != 0U)
            stream_error(s, "stream ended with a partial ndjson line");
    } else {
        if (s->line.len != 0U)
            stream_line(s, on_event, ctx);
        if (s->err[0] == '\0' && s->data.len != 0U) {
            stream_sse_dispatch(s, false, on_event, ctx);
            stream_error(s, "stream ended without a blank line");
        }
    }
    s->ended = true;
}

static void stream_feed_whole(AiStream *s, const u8 *bytes, u64 len,
                              bool at_eof, AiEventFn on_event, void *ctx)
{
    const u8 *data;
    size_t data_len;

    if (len > YEW_HTTP_MAX_BODY - s->data.len) {
        stream_error(s, "response exceeded 64 MiB");
        return;
    }
    bytebuf_append(&s->data, bytes, (size_t)len);
    if (!at_eof)
        return;
    data = s->data.data;
    data_len = s->data.len;
    stream_strip_bom(s, &data, &data_len);
    stream_dispatch(s, NULL, 0U, data, data_len, true, on_event, ctx);
    s->ended = true;
}

void yew_ai_stream_init(AiStream *s, AiStreamMode mode)
{
    memset(s, 0, sizeof(*s));
    s->mode = (u8)mode;
    bytebuf_init(&s->line);
    bytebuf_init(&s->data);
    bytebuf_init(&s->evt);
    if (mode != YEW_AISTREAM_SSE && mode != YEW_AISTREAM_NDJSON &&
        mode != YEW_AISTREAM_WHOLE)
        stream_error(s, "invalid AI stream mode");
}

void yew_ai_stream_free(AiStream *s)
{
    bytebuf_free(&s->evt);
    bytebuf_free(&s->data);
    bytebuf_free(&s->line);
    memset(s, 0, sizeof(*s));
}

void yew_ai_stream_feed(AiStream *s, const u8 *bytes, u64 len, bool at_eof,
                        AiEventFn on_event, void *ctx)
{
    if (s == NULL || s->err[0] != '\0' || s->ended)
        return;
    if (bytes == NULL && len != 0U) {
        stream_error(s, "stream input is null");
        return;
    }
    if (len > UINT64_MAX - s->bytes) {
        stream_error(s, "stream byte count overflow");
        return;
    }
    s->bytes += len;
    if (s->mode == YEW_AISTREAM_WHOLE) {
        stream_feed_whole(s, bytes, len, at_eof, on_event, ctx);
        return;
    }
    stream_feed_lines(s, bytes, len, on_event, ctx);
    if (at_eof && s->err[0] == '\0' && !s->ended)
        stream_finish_lines(s, on_event, ctx);
}
