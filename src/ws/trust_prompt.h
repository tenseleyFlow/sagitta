#ifndef SAG_WS_TRUST_PROMPT_H
#define SAG_WS_TRUST_PROMPT_H

/* Event-loop UI for workspace trust; this module never reads input itself. */

#include <stdbool.h>

#include "util/base.h"
#include "ws/trust.h"

typedef struct Ed Ed;

typedef void (*SagTrustPromptDone)(Ed *ed, SagTrustAnswer answer, void *ctx);

typedef struct SagTrustPrompt {
    SagTrustDb *db;
    const SagTrustProbe *probe;
    SagTrustPromptDone done;
    void *ctx;
    SagTrustDecision reason;
    u32 view_buffer_id;
    bool active;
    bool viewing;
} SagTrustPrompt;

/* Caller retains db/probe/context until done is called or the Ed is freed. */
bool sag_trust_prompt_begin(Ed *ed, SagTrustDb *db,
                            const SagTrustProbe *probe,
                            SagTrustDecision reason,
                            SagTrustPromptDone done, void *ctx);
bool sag_trust_prompt_key(Ed *ed, u8 answer);
void sag_trust_prompt_buffer_closed(Ed *ed, u32 buffer_id);
void sag_trust_prompt_cancel(Ed *ed);

#endif /* SAG_WS_TRUST_PROMPT_H */
