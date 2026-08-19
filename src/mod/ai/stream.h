#ifndef YEW_AI_STREAM_H
#define YEW_AI_STREAM_H

#include <stdbool.h>

#include "mod/ai/http.h"
#include "util/base.h"
#include "util/buf.h"

enum { YEW_AI_LINE_MAX = 1024U * 1024U };

typedef enum {
    YEW_AISTREAM_SSE,
    YEW_AISTREAM_NDJSON,
    YEW_AISTREAM_WHOLE
} AiStreamMode;

typedef struct AiEvent {
    const u8 *type;
    u32 tlen;
    const u8 *data;
    u32 dlen;
    bool terminator;
} AiEvent;

typedef void (*AiEventFn)(void *ctx, const AiEvent *ev);

typedef struct AiStream {
    u8 mode;
    bool pending_cr;
    bool saw_bom;
    bool have_data_line;
    bool terminated;
    bool ended;
    Bytebuf line;
    Bytebuf data;
    Bytebuf evt;
    u64 events;
    u64 bytes;
    char err[96];
} AiStream;

void yew_ai_stream_init(AiStream *s, AiStreamMode m);
void yew_ai_stream_free(AiStream *s);
/* Event byte spans remain valid only for the duration of the callback. */
void yew_ai_stream_feed(AiStream *s, const u8 *b, u64 n, bool at_eof,
                        AiEventFn on_event, void *ctx);

#endif
