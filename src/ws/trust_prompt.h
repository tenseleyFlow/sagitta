#ifndef YEW_WS_TRUST_PROMPT_H
#define YEW_WS_TRUST_PROMPT_H

/* Event-loop UI for workspace trust; this module never reads input itself. */

#include <stdbool.h>

#include "util/base.h"
#include "ws/trust.h"

typedef struct Ed Ed;

typedef void (*YewTrustPromptDone)(Ed *ed, YewTrustAnswer answer, void *ctx);

typedef struct YewTrustPrompt {
    YewTrustDb *db;
    const YewTrustProbe *probe;
    YewTrustPromptDone done;
    void *ctx;
    YewTrustDecision reason;
    u32 view_buffer_id;
    bool active;
    bool viewing;
} YewTrustPrompt;

/* Caller retains db/probe/context until done is called or the Ed is freed. */
bool yew_trust_prompt_begin(Ed *ed, YewTrustDb *db,
                            const YewTrustProbe *probe,
                            YewTrustDecision reason,
                            YewTrustPromptDone done, void *ctx);
bool yew_trust_prompt_key(Ed *ed, u8 answer);
void yew_trust_prompt_buffer_closed(Ed *ed, u32 buffer_id);
void yew_trust_prompt_cancel(Ed *ed);

#endif /* YEW_WS_TRUST_PROMPT_H */
