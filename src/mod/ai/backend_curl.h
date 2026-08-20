#ifndef YEW_MOD_AI_BACKEND_CURL_H
#define YEW_MOD_AI_BACKEND_CURL_H

#include <stddef.h>

#include "mod/ai/backend.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;

typedef struct AiCurlRequest {
    const char *url;
    const char *method;
    const HttpHdr *hdrs;
    u32 nhdr;
    const u8 *body;
    u64 blen;
    /* Zero selects the pinned Sprint 48 defaults.  Nonzero values come
     * from the validated global AI timeout options. */
    i64 connect_timeout_ms;
    i64 total_timeout_ms;
} AiCurlRequest;

typedef enum AiCurlAuthKind {
    YEW_CURL_AUTH_NONE,
    YEW_CURL_AUTH_BEARER,
    YEW_CURL_AUTH_X_API_KEY
} AiCurlAuthKind;

/* Kept separate from ordinary headers so logging and request inspection
 * cannot accidentally treat credentials as public request metadata. */
typedef struct AiCurlSecret {
    AiCurlAuthKind kind;
    const u8 *bytes;
    size_t len;
} AiCurlSecret;

typedef enum AiCurlState {
    YEW_CURL_UNKNOWN,
    YEW_CURL_OK,
    YEW_CURL_ABSENT,
    YEW_CURL_TOO_OLD
} AiCurlState;

/* Explicit caller-owned process-lifetime cache.  It must outlive a pending
 * probe job; no mutable process-global state is used. */
typedef struct AiCurlProbe {
    AiCurlState state;
    Bytebuf out;
    Bytebuf err;
    Ed *ed;
    u32 job_id;
    u32 probes;
    u32 major;
    u32 minor;
    u32 patch;
    char version[48];
    bool running;
} AiCurlProbe;

/* Fixed for every request: secrets and request data belong only on stdin. */
char *const *yew_ai_curl_argv(void);

/* Builds the complete curl --config - input transactionally.  The output
 * allocation is secured against secret-bearing realloc remnants. */
bool yew_ai_curl_config(Bytebuf *out, const AiCurlRequest *req,
                        const AiCurlSecret *secret,
                        char *err, size_t errsz);

AiErrKind yew_ai_curl_exit_class(int exit_code, int termsig);

void yew_ai_curl_probe_init(AiCurlProbe *probe);
void yew_ai_curl_probe_free(AiCurlProbe *probe);
/* First call starts `curl --version` and returns false with an empty error.
 * Later calls consult the cached result and never spawn a second probe. */
bool yew_ai_curl_probe(Ed *ed, AiCurlProbe *probe, char *err, size_t errsz);
const char *yew_ai_curl_probe_version(const AiCurlProbe *probe);

#endif
