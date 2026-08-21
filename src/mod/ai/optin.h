#ifndef YEW_MOD_AI_OPTIN_H
#define YEW_MOD_AI_OPTIN_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;

typedef enum YewAiOptinBackend {
    YEW_AI_OPTIN_LOCAL = 1,
    YEW_AI_OPTIN_CLOUD
} YewAiOptinBackend;

typedef bool (*YewAiOptinCommit)(Ed *ed, YewAiOptinBackend backend,
                                 char scope, void *ctx);

typedef struct YewAiOptin {
    Ed *ed;
    YewAiOptinCommit commit;
    void *ctx;
    YewAiOptinBackend backend;
    char scope;
    u8 phase;
    bool active;
} YewAiOptin;

bool yew_ai_optin_begin(YewAiOptin *optin, Ed *ed,
                        YewAiOptinCommit commit, void *ctx);
/* Unit/embedding seam: production callers use yew_ai_optin_begin(). */
bool yew_ai_optin_begin_checked(YewAiOptin *optin, Ed *ed,
                                YewAiOptinCommit commit, void *ctx,
                                bool has_tty);
/* Production commit path with an explicit tty decision for unit/embedding
 * tests.  Interactive callers continue to use ed.ai.enable. */
bool yew_ai_optin_begin_default_checked(YewAiOptin *optin, Ed *ed,
                                        bool has_tty);
void yew_ai_optin_cancel(YewAiOptin *optin);
const char *yew_ai_optin_no_tty_message(void);
/* Pure targeted-edit seam used by the atomic writer and regression tests. */
bool yew_ai_optin_config_merge(Bytebuf *out, const char *old,
                               size_t old_len,
                               YewAiOptinBackend backend, bool allow_all);

#endif
