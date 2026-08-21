#include "mod/ai/debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/backend.h"
#include "ui/message.h"

#define AI_DEBUG_LOG_MAX (1024U * 1024U)

static bool debug_append(AiState *state, const void *bytes, size_t len);

bool yew_ai_debug_bodies_enabled(Ed *ed)
{
    const char *environment = getenv("YEW_AI_DEBUG");
    OptVal option;

    return environment != NULL && strcmp(environment, "1") == 0 &&
           yew_opt_get(ed, NULL, NULL, "ai.debug_bodies", 15U, &option) &&
           option.type == (u8)YEW_OPT_BOOL && option.as.b;
}

void yew_ai_debug_secret(Ed *ed, const AiSecretHeader *secret)
{
    char line[96];
    const char *name;
    int n;
    size_t len;

    if (ed == NULL || ed->ai == NULL || secret == NULL ||
        !yew_ai_debug_bodies_enabled(ed))
        return;
    if (secret->kind == (u8)YEW_AI_SECRET_BEARER)
        name = "authorization";
    else if (secret->kind == (u8)YEW_AI_SECRET_X_API_KEY)
        name = "x-api-key";
    else
        return;
    n = snprintf(line, sizeof(line), "debug %s: <redacted %u bytes>\n",
                 name, (unsigned)secret->len);
    if (n < 0)
        return;
    len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1U;
    (void)debug_append(ed->ai, line, len);
}

static bool debug_append(AiState *state, const void *bytes, size_t len)
{
    if (state->log.len > AI_DEBUG_LOG_MAX ||
        len > AI_DEBUG_LOG_MAX - state->log.len)
        return false;
    bytebuf_append(&state->log, bytes, len);
    return true;
}

void yew_ai_debug_body(Ed *ed, const char *kind, const u8 *bytes, u32 len)
{
    static const char warning[] =
        "WARN: AI debug body logging is enabled; prompts and completions "
        "contain source text\n";
    char header[96];
    int n;
    size_t header_len;

    if (ed == NULL || ed->ai == NULL || kind == NULL ||
        (len != 0U && bytes == NULL) ||
        !yew_ai_debug_bodies_enabled(ed))
        return;
    if (!ed->ai->debug_body_warned) {
        if (!debug_append(ed->ai, warning, sizeof(warning) - 1U))
            return;
        yew_msg(ed, YEW_MSG_WARN,
                "AI debug body logging is enabled; prompts and completions "
                "contain source text");
        ed->ai->debug_body_warned = true;
    }
    n = snprintf(header, sizeof(header), "debug %s body (%u bytes):\n",
                 kind, (unsigned)len);
    if (n < 0)
        return;
    header_len = (size_t)n < sizeof(header) ? (size_t)n :
                                               sizeof(header) - 1U;
    if (ed->ai->log.len > AI_DEBUG_LOG_MAX ||
        header_len + (size_t)len + 1U >
            AI_DEBUG_LOG_MAX - ed->ai->log.len)
        return;
    bytebuf_append(&ed->ai->log, header, header_len);
    bytebuf_append(&ed->ai->log, bytes, len);
    bytebuf_push_u8(&ed->ai->log, (u8)'\n');
}
