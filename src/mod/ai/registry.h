#ifndef YEW_MOD_AI_REGISTRY_H
#define YEW_MOD_AI_REGISTRY_H

#include <stddef.h>

#include "fl/value.h"
#include "mod/ai/backend.h"

typedef struct AiBackendEntry {
    AiBackend backend;
    AiCooldown cooldown;
    const char *url_text;
    void *endpoint;
} AiBackendEntry;

/* Called exactly once for a successfully parsed entry.  A plain-HTTP
 * implementation may resolve url->host here, cache its sockaddr in
 * *endpoint, and update url->loopback from that exact address set. */
typedef bool (*AiBackendPrepareFn)(void *ctx, const char *url_text,
                                   HttpUrl *url, u8 transport,
                                   void **endpoint, char *err,
                                   size_t errsz);
typedef void (*AiBackendReleaseFn)(void *ctx, void *endpoint);

typedef struct AiOwnedBackend AiOwnedBackend;

typedef struct AiBackendRegistry {
    AiOwnedBackend *entries;
    u32 len;
    u32 cap;
    AiBackendPrepareFn prepare;
    AiBackendReleaseFn release;
    void *prepare_ctx;
} AiBackendRegistry;

void yew_ai_registry_init(AiBackendRegistry *registry,
                          AiBackendPrepareFn prepare,
                          AiBackendReleaseFn release, void *prepare_ctx);
void yew_ai_registry_drop(AiBackendRegistry *registry);

/* Register or replace one backend.  Replacement preserves its insertion
 * position and is transactional: failure leaves the old entry untouched. */
bool yew_ai_registry_put(AiBackendRegistry *registry, const FlStr *name,
                         const FlMap *config, char *err, size_t errsz);

/* Replace the complete registry from a name -> config map.  Iteration order
 * is the Fletch map's insertion order; failure leaves the registry intact. */
bool yew_ai_registry_reload(AiBackendRegistry *registry,
                            const FlMap *backends, char *err, size_t errsz);

const AiBackendEntry *yew_ai_registry_find(const AiBackendRegistry *registry,
                                           const char *name);
AiBackendEntry *yew_ai_registry_find_mut(AiBackendRegistry *registry,
                                         const char *name);
u32 yew_ai_registry_count(const AiBackendRegistry *registry);
const AiBackendEntry *yew_ai_registry_at(const AiBackendRegistry *registry,
                                         u32 index);

#endif
