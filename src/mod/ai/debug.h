#ifndef YEW_MOD_AI_DEBUG_H
#define YEW_MOD_AI_DEBUG_H

#include "util/base.h"

typedef struct Ed Ed;
typedef struct AiSecretHeader AiSecretHeader;

bool yew_ai_debug_bodies_enabled(Ed *ed);
void yew_ai_debug_secret(Ed *ed, const AiSecretHeader *secret);
void yew_ai_debug_body(Ed *ed, const char *kind, const u8 *bytes, u32 len);

#endif /* YEW_MOD_AI_DEBUG_H */
