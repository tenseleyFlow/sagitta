#ifndef YEW_MOD_AI_POLICY_H
#define YEW_MOD_AI_POLICY_H

#include <stdbool.h>

#include "mod/ai/redact.h"

typedef struct Ed Ed;

typedef struct AiPolicyBundle {
    AiRedactPolicy *redact;
    AiPathPolicy *paths;
} AiPolicyBundle;

/* Test/discovery seam.  SHIPPED is mandatory; USER may be NULL or missing. */
bool yew_ai_policy_load_paths(const char *shipped, const char *user,
                              bool deny_replace, bool exclude_replace,
                              AiPolicyBundle *out);
void yew_ai_policy_bundle_drop(AiPolicyBundle *bundle);

/* Reloads and atomically swaps the cached policies owned by Ed->ai. */
bool yew_ai_policy_reload(Ed *ed);
void yew_ai_policy_ensure(Ed *ed);

#endif
