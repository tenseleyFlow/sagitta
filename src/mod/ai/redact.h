#ifndef YEW_MOD_AI_REDACT_H
#define YEW_MOD_AI_REDACT_H

/*
 * Sprint 50's privacy boundary is deliberately conservative.  A cloud
 * caller blocks on a hit rather than pretending that replacing one known
 * credential makes the surrounding context safe.  A loopback caller may
 * instead elide the returned span: the bytes do not leave the machine, so
 * the remaining concern is confusing the model rather than disclosure.
 *
 * Policies are explicit objects.  Construction compiles each expression
 * once into an owned arena; scanning performs no allocation and examines
 * only the two byte ranges present in AiCtx.
 */

#include <stdbool.h>
#include <stddef.h>

#include "mod/ai/context.h"
#include "text/coords.h"
#include "util/base.h"

typedef struct AiRedactPolicy AiRedactPolicy;
typedef struct AiPathPolicy AiPathPolicy;

typedef struct AiRedactSpec {
    const char *name;
    const char *re;
    u32 flags;
    const char *note;
    const char *source;
    u32 line_1based;
} AiRedactSpec;

typedef struct AiRedactError {
    const char *source;
    const char *rule;
    const char *message;
    u32 line_1based;
    u32 pattern_off;
} AiRedactError;

/* User rows append after the shipped set unless replace is true.  An
 * invalid user row is reported through err and skipped; it never disables
 * shipped protection.  The first invalid row is reported while later valid
 * rows are still installed. */
AiRedactPolicy *yew_ai_redact_policy_new(const AiRedactSpec *user,
                                         size_t user_len, bool replace,
                                         AiRedactError *err);
void yew_ai_redact_policy_free(AiRedactPolicy *policy);
size_t yew_ai_redact_policy_len(const AiRedactPolicy *policy);
const char *yew_ai_redact_policy_rule_name(const AiRedactPolicy *policy,
                                           size_t index);
bool yew_ai_redact_scan(const AiRedactPolicy *policy, const AiCtx *ctx,
                        RedactHit *hit);

typedef struct AiPathError {
    const char *pattern;
    const char *message;
    size_t index;
} AiPathError;

typedef struct AiPathHit {
    const char *pattern;
} AiPathHit;

/* User globs have the same append/replace lifecycle as redaction rows.
 * '**' is rejected rather than silently acquiring recursive semantics. */
AiPathPolicy *yew_ai_path_policy_new(const char *const *user,
                                     size_t user_len, bool replace,
                                     AiPathError *err);
void yew_ai_path_policy_free(AiPathPolicy *policy);
size_t yew_ai_path_policy_len(const AiPathPolicy *policy);
bool yew_ai_path_excluded(const AiPathPolicy *policy, const char *path,
                          AiPathHit *hit);
bool yew_ai_path_glob_valid(const char *glob);

#endif
