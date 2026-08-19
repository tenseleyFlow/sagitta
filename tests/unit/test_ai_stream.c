#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "mod/ai/stream.h"
#include "util/buf.h"

typedef struct {
    Bytebuf type;
    Bytebuf data;
    u32 type_len[16];
    u32 data_len[16];
    size_t type_off[16];
    size_t data_off[16];
    bool terminator[16];
    u32 n;
} Events;

static void events_init(Events *events)
{
    memset(events, 0, sizeof(*events));
    bytebuf_init(&events->type);
    bytebuf_init(&events->data);
}

static void events_free(Events *events)
{
    bytebuf_free(&events->data);
    bytebuf_free(&events->type);
}

static void collect_event(void *ctx, const AiEvent *event)
{
    Events *events = ctx;
    u32 i = events->n;

    YEW_ASSERT(i < YEW_ARRAY_LEN(events->data_len));
    events->type_off[i] = events->type.len;
    events->data_off[i] = events->data.len;
    events->type_len[i] = event->tlen;
    events->data_len[i] = event->dlen;
    events->terminator[i] = event->terminator;
    bytebuf_append(&events->type, event->type, event->tlen);
    bytebuf_append(&events->data, event->data, event->dlen);
    events->n++;
}

static void assert_event(const Events *events, u32 i, const char *type,
                         const char *data, bool terminator)
{
    size_t tlen = strlen(type);
    size_t dlen = strlen(data);

    YEW_ASSERT(i < events->n);
    YEW_ASSERT_EQ_U64(events->type_len[i], tlen);
    if (tlen != 0U)
        YEW_ASSERT_EQ_MEM(events->type.data + events->type_off[i], type,
                          tlen);
    YEW_ASSERT_EQ_U64(events->data_len[i], dlen);
    if (dlen != 0U)
        YEW_ASSERT_EQ_MEM(events->data.data + events->data_off[i], data,
                          dlen);
    YEW_ASSERT(events->terminator[i] == terminator);
}

static void feed_text(AiStream *stream, const char *text, bool eof,
                      Events *events)
{
    yew_ai_stream_feed(stream, (const u8 *)text, (u64)strlen(text), eof,
                       collect_event, events);
}

void test_ai_stream_sse_fields_and_framing(void)
{
    static const char input[] =
        ": keepalive\n"
        "id: 12\n"
        "retry: 1000\n"
        "unknown: ignored\n"
        "event: token\n"
        "data: first\n"
        "data:second\n\n"
        "event:  spaced\n"
        "data:  two spaces\n\n";
    AiStream stream;
    Events events;

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
    feed_text(&stream, input, false, &events);
    YEW_ASSERT_EQ_U64(events.n, 2U);
    assert_event(&events, 0U, "token", "first\nsecond", false);
    assert_event(&events, 1U, " spaced", " two spaces", false);
    YEW_ASSERT_EQ_U64(stream.events, 2U);
    YEW_ASSERT_EQ_U64(stream.bytes, strlen(input));
    YEW_ASSERT_EQ_STR(stream.err, "");
    YEW_ASSERT(!stream.terminated);
    yew_ai_stream_free(&stream);
    events_free(&events);
}

static void assert_line_ending(const char *sep)
{
    Bytebuf input;
    AiStream stream;
    Events events;

    bytebuf_init(&input);
    bytebuf_printf(&input, "event: word%sdata: a%sdata:b%s%s",
                   sep, sep, sep, sep);
    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
    yew_ai_stream_feed(&stream, input.data, input.len, false,
                       collect_event, &events);
    YEW_ASSERT_EQ_U64(events.n, 1U);
    assert_event(&events, 0U, "word", "a\nb", false);
    YEW_ASSERT_EQ_STR(stream.err, "");
    yew_ai_stream_free(&stream);
    events_free(&events);
    bytebuf_free(&input);
}

void test_ai_stream_line_endings_and_chunks(void)
{
    static const char input[] = "data: split\r\n\r\n";
    AiStream stream;
    Events events;
    size_t i;

    assert_line_ending("\n");
    assert_line_ending("\r\n");
    assert_line_ending("\r");
    for (i = 0U; i <= strlen(input); i++) {
        events_init(&events);
        yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
        yew_ai_stream_feed(&stream, (const u8 *)input, i, false,
                           collect_event, &events);
        yew_ai_stream_feed(&stream, (const u8 *)input + i,
                           strlen(input) - i, false,
                           collect_event, &events);
        YEW_ASSERT_EQ_U64(events.n, 1U);
        assert_event(&events, 0U, "", "split", false);
        YEW_ASSERT_EQ_STR(stream.err, "");
        yew_ai_stream_free(&stream);
        events_free(&events);
    }
}

void test_ai_stream_bom_done_and_eof(void)
{
    static const u8 first[] = {0xefU};
    static const u8 rest[] = {
        0xbbU, 0xbfU, 'd', 'a', 't', 'a', ':', ' ', 'o', 'k', '\n', '\n',
        'd', 'a', 't', 'a', ':', ' ', '[', 'D', 'O', 'N', 'E', ']', '\n',
        '\n'
    };
    AiStream stream;
    Events events;

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
    yew_ai_stream_feed(&stream, first, sizeof(first), false,
                       collect_event, &events);
    yew_ai_stream_feed(&stream, rest, sizeof(rest), false,
                       collect_event, &events);
    YEW_ASSERT_EQ_U64(events.n, 2U);
    assert_event(&events, 0U, "", "ok", false);
    assert_event(&events, 1U, "", "", true);
    YEW_ASSERT(stream.terminated);
    YEW_ASSERT(stream.ended);
    YEW_ASSERT_EQ_STR(stream.err, "");
    yew_ai_stream_free(&stream);
    events_free(&events);

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
    feed_text(&stream, "data:no blank", true, &events);
    YEW_ASSERT_EQ_U64(events.n, 1U);
    assert_event(&events, 0U, "", "no blank", false);
    YEW_ASSERT_EQ_STR(stream.err, "stream ended without a blank line");
    YEW_ASSERT(!stream.terminated);
    YEW_ASSERT(stream.ended);
    yew_ai_stream_free(&stream);
    events_free(&events);

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
    feed_text(&stream, "data: [DONE]", true, &events);
    YEW_ASSERT_EQ_U64(events.n, 1U);
    assert_event(&events, 0U, "", "", false);
    YEW_ASSERT_EQ_STR(stream.err, "stream ended without a blank line");
    YEW_ASSERT(!stream.terminated);
    yew_ai_stream_free(&stream);
    events_free(&events);
}

void test_ai_stream_ndjson(void)
{
    static const char input[] =
        "\n{\"response\":\"a\",\"done\":false}\r\n"
        "{\"response\":\"b\",\"done\":true}\r";
    AiStream stream;
    Events events;

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_NDJSON);
    feed_text(&stream, input, true, &events);
    YEW_ASSERT_EQ_U64(events.n, 2U);
    assert_event(&events, 0U, "", "{\"response\":\"a\",\"done\":false}",
                 false);
    assert_event(&events, 1U, "", "{\"response\":\"b\",\"done\":true}",
                 true);
    YEW_ASSERT(stream.terminated);
    YEW_ASSERT_EQ_STR(stream.err, "");
    yew_ai_stream_free(&stream);
    events_free(&events);

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_NDJSON);
    feed_text(&stream, "{\"done\":false}", true, &events);
    YEW_ASSERT_EQ_U64(events.n, 0U);
    YEW_ASSERT_EQ_STR(stream.err, "stream ended with a partial ndjson line");
    yew_ai_stream_free(&stream);
    events_free(&events);

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_NDJSON);
    feed_text(&stream, "not json\n", false, &events);
    YEW_ASSERT_EQ_U64(events.n, 0U);
    YEW_ASSERT(stream.err[0] != '\0');
    yew_ai_stream_free(&stream);
    events_free(&events);
}

void test_ai_stream_whole_and_caps(void)
{
    static const u8 body[] = {0xefU, 0xbbU, 0xbfU, 'a', 0U, 0xffU};
    AiStream stream;
    Events events;
    u8 *long_line;

    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_WHOLE);
    yew_ai_stream_feed(&stream, body, 2U, false, collect_event, &events);
    yew_ai_stream_feed(&stream, body + 2U, sizeof(body) - 2U, true,
                       collect_event, &events);
    YEW_ASSERT_EQ_U64(events.n, 1U);
    YEW_ASSERT_EQ_U64(events.data_len[0], 3U);
    YEW_ASSERT_EQ_MEM(events.data.data, "a\0\xff", 3U);
    YEW_ASSERT(events.terminator[0]);
    YEW_ASSERT(stream.terminated);
    YEW_ASSERT_EQ_STR(stream.err, "");
    yew_ai_stream_free(&stream);
    events_free(&events);

    long_line = malloc((size_t)YEW_AI_LINE_MAX + 1U);
    YEW_ASSERT_NOT_NULL(long_line);
    memset(long_line, 'x', (size_t)YEW_AI_LINE_MAX + 1U);
    events_init(&events);
    yew_ai_stream_init(&stream, YEW_AISTREAM_SSE);
    yew_ai_stream_feed(&stream, long_line,
                       (u64)YEW_AI_LINE_MAX + 1U, false,
                       collect_event, &events);
    YEW_ASSERT_EQ_U64(events.n, 0U);
    YEW_ASSERT_EQ_STR(stream.err, "stream line exceeded 1 MiB");
    YEW_ASSERT_EQ_U64(stream.line.len, YEW_AI_LINE_MAX);
    yew_ai_stream_free(&stream);
    events_free(&events);
    free(long_line);
}
