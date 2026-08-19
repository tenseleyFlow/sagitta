#ifndef YEW_AI_BACKEND_H
#define YEW_AI_BACKEND_H

#include "mod/ai/http.h"
#include "mod/ai/stream.h"
#include "mod/lsp/json.h"
#include "util/base.h"

typedef enum {
    YEW_AI_OLLAMA = 0,
    YEW_AI_OPENAI,
    YEW_AI_ANTHROPIC,
    YEW_AI_NKIND
} AiKind;

typedef enum {
    YEW_AI_TR_HTTP = 0,
    YEW_AI_TR_CURL
} AiTransport;

typedef enum {
    YEW_AI_OK = 0,
    YEW_AI_ERR_UNREACHABLE,
    YEW_AI_ERR_AUTH,
    YEW_AI_ERR_RATELIMIT,
    YEW_AI_ERR_MODEL,
    YEW_AI_ERR_PROTOCOL,
    YEW_AI_ERR_TIMEOUT,
    YEW_AI_ERR_TLS,
    YEW_AI_ERR_TOO_LARGE,
    YEW_AI_ERR_NO_CURL,
    YEW_AI_ERR_CANCELLED
} AiErrKind;

typedef struct AiErr {
    u8 kind;
    i64 retry_ms;
    char msg[192];
} AiErr;

typedef struct AiCooldown {
    i64 until_ms;
    u32 consecutive;
} AiCooldown;

typedef struct AiText {
    const u8 *bytes;
    u32 len;
} AiText;

/* The prompt is already assembled and privacy-checked by its caller. */
typedef struct AiPrompt {
    AiText system;
    AiText prefix;
    AiText suffix;
    const AiText *stops;
    u32 nstops;
} AiPrompt;

typedef struct AiBackend {
    const char *name;
    u8 kind;
    u8 transport;
    HttpUrl url;
    const char *model;
    const char *key_env;
    char *const *key_cmd;
    i64 max_tokens;
    double temperature;
    bool stream;
    bool fim;
} AiBackend;

typedef struct AiBlockState AiBlockState;

/* Per-request state. Text spans returned in AiAdapterEvent borrow the parsed
 * JSON value and are valid only until its arena is released. */
typedef struct AiAdapterState {
    AiBlockState *blocks;
    u32 nblocks;
    u32 block_cap;
    u32 event_type_mismatches;
    i64 input_tokens;
    i64 output_tokens;
    char message_id[96];
    char model[128];
    char stop_reason[32];
    char stop_sequence[64];
    bool terminal;
} AiAdapterState;

typedef struct AiAdapterEvent {
    const u8 *text;
    u32 len;
    bool has_text;
    bool terminal;
    bool event_type_mismatch;
} AiAdapterEvent;

typedef struct AiAdapter {
    const char *kind_name;
    void (*build)(JsonW *w, const AiBackend *b, const AiPrompt *p);
    const char *(*path_gen)(const AiBackend *b);
    const char *(*path_models)(const AiBackend *b);
    AiStreamMode stream_mode;
    bool (*delta)(const JsonValue *v, const u8 **txt, u32 *len);
    void (*consume)(AiAdapterState *state, const AiEvent *event,
                    const JsonValue *value, AiAdapterEvent *out);
    AiErrKind (*classify)(u16 status, const JsonValue *body);
} AiAdapter;

extern const AiAdapter yew_ai_adapters[YEW_AI_NKIND];

void yew_ai_adapter_state_init(AiAdapterState *state);
void yew_ai_adapter_state_free(AiAdapterState *state);
void yew_ai_err_format(AiErr *out, AiErrKind kind,
                       const AiBackend *backend, u16 status,
                       i64 retry_ms, const char *detail);
void yew_ai_cooldown_init(AiCooldown *cooldown);
void yew_ai_cooldown_note(AiCooldown *cooldown, AiErrKind kind,
                          i64 retry_after_ms, i64 now_ms,
                          i64 backoff_max_ms);
i64 yew_ai_cooldown_remaining(const AiCooldown *cooldown, i64 now_ms);

#endif
