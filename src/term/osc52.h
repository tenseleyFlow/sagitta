#ifndef SAG_TERM_OSC52_H
#define SAG_TERM_OSC52_H

#include <stdbool.h>

#include "util/base.h"
#include "util/buf.h"

#define SAG_OSC52_DEFAULT_MAX 100000u
#define SAG_OSC52_SCREEN_CHUNK 768u

typedef enum SagOsc52Mode {
    SAG_OSC52_OFF = 0,
    SAG_OSC52_PLAIN,
    SAG_OSC52_TMUX,
    SAG_OSC52_SCREEN
} SagOsc52Mode;

typedef const char *(*SagOsc52EnvFn)(const char *name);

SagOsc52Mode sag_osc52_mode(SagOsc52EnvFn getv);
u64 sag_osc52_max(SagOsc52EnvFn getv);
const char *sag_osc52_target(SagOsc52EnvFn getv);

/* Appends a complete sequence, or leaves out unchanged on refusal. */
bool sag_osc52_build(Bytebuf *out, const u8 *payload, u64 payload_len,
                     const char *target, SagOsc52Mode mode, u64 max_base64);
bool sag_osc52_build_env(Bytebuf *out, const u8 *payload, u64 payload_len,
                         SagOsc52EnvFn getv);

#endif
