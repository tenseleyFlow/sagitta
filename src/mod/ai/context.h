#ifndef YEW_MOD_AI_CONTEXT_H
#define YEW_MOD_AI_CONTEXT_H

/* Sprint 49: the single-buffer, byte-budgeted context boundary.  Multi-file
 * context is deliberately not a 1.0 feature: it would need a second
 * staleness and privacy model rather than being a larger version of this. */

#include <stdbool.h>

#include "edit/shadow.h"
#include "mod/ai/backend.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef struct AiCtx {
    const u8 *prefix;
    u32 plen;
    const u8 *suffix;
    u32 slen;
    const char *path;
    const char *lang;
    u32 line_1based;
    bool truncated_head;
    bool truncated_tail;
} AiCtx;

typedef struct RedactHit {
    const char *rule;
    u32 line_1based;
    ByteOff off;
    u32 len;
    bool in_prefix;
} RedactHit;

typedef bool (*AiRedactCheck)(Ed *ed, const AiCtx *ctx, RedactHit *hit);
typedef void (*AiContextBuildTestFn)(void *opaque);

/* A standard-C hook keeps Sprint 49's safe default and lets Sprint 50 own
 * policy without weak symbols or compiler-specific attributes. */
void yew_ai_redact_hook_set(AiRedactCheck check);
bool yew_ai_redact_check(Ed *ed, const AiCtx *ctx, RedactHit *hit);

/* Test observation seam: production leaves this unset.  The callback runs
 * at function entry, making an excluded-path early return distinguishable
 * from a context that was built and later discarded. */
void yew_ai_context_build_test_set(AiContextBuildTestFn observe,
                                   void *opaque);

bool yew_ai_context_build(Ed *ed, Win *win, const ShadowReq *request,
                          Arena *arena, AiCtx *out, AiErr *err);

#endif
