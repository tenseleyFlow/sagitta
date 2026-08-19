#ifndef YEW_MOD_AI_KEY_H
#define YEW_MOD_AI_KEY_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

typedef struct Ed Ed;
typedef struct AiBackend AiBackend;
typedef struct AiErr AiErr;

enum { YEW_AI_KEY_MAX = 8U * 1024U };

typedef struct AiKeyCacheEntry AiKeyCacheEntry;

/* Caller-owned session cache.  Backend addresses must remain stable until
 * reload/drop, which matches ownership by the backend registry. */
typedef struct AiKeyCache {
    AiKeyCacheEntry *entries;
    size_t len;
    size_t cap;
    i64 command_timeout_ms;
    bool enabled;
} AiKeyCache;

/*
 * The only API that resolves API-key bytes.  `out` is wiped before every
 * attempt and remains wiped on failure.  On success the caller owns wiping
 * the returned NUL-terminated key as soon as its request has been built.
 *
 * This primitive deliberately performs one resolution.  The explicit cache
 * API below is intended for ownership by the Ed/backend registry.
 */
bool yew_ai_key_get(Ed *ed, const AiBackend *backend, char *out,
                    size_t outsz, AiErr *err);

/* Cache-on is the default.  Disabling an active cache wipes it immediately,
 * then every get resolves the source again. */
void yew_ai_key_cache_init(AiKeyCache *cache);
void yew_ai_key_cache_enable(AiKeyCache *cache, bool enabled);
void yew_ai_key_cache_set_timeout(AiKeyCache *cache, i64 timeout_ms);
bool yew_ai_key_cache_get(Ed *ed, AiKeyCache *cache,
                          const AiBackend *backend, char *out, size_t outsz,
                          AiErr *err);

/* Reload securely empties the cache but preserves its policy.  Drop securely
 * empties it and resets the whole caller-owned object. */
void yew_ai_key_cache_reload(AiKeyCache *cache);
void yew_ai_key_cache_drop(AiKeyCache *cache);

#endif
