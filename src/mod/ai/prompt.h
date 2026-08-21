#ifndef YEW_MOD_AI_PROMPT_H
#define YEW_MOD_AI_PROMPT_H

#include "mod/ai/backend.h"
#include "mod/ai/context.h"
#include "mod/lsp/json.h"

typedef enum AiTemplate {
    YEW_AI_TPL_FIM = 0,
    YEW_AI_TPL_CHAT
} AiTemplate;

AiTemplate yew_ai_template_of(const AiBackend *backend);
void yew_ai_prompt_build(JsonW *writer, const AiBackend *backend,
                         const AiCtx *context);

/* In-place, length-carrying response normalization. */
u32 yew_ai_response_trim(u8 *bytes, u32 len, const AiCtx *context,
                         u32 max_lines);

const char *yew_ai_chat_system_prompt(void);

#endif
