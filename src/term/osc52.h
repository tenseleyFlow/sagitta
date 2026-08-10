#ifndef YEW_TERM_OSC52_H
#define YEW_TERM_OSC52_H

#include <stdbool.h>

#include "util/base.h"
#include "util/buf.h"

#define YEW_OSC52_DEFAULT_MAX 100000u
#define YEW_OSC52_SCREEN_CHUNK 768u

typedef enum YewOsc52Mode {
    YEW_OSC52_OFF = 0,
    YEW_OSC52_PLAIN,
    YEW_OSC52_TMUX,
    YEW_OSC52_SCREEN
} YewOsc52Mode;

typedef const char *(*YewOsc52EnvFn)(const char *name);

YewOsc52Mode yew_osc52_mode(YewOsc52EnvFn getv);
u64 yew_osc52_max(YewOsc52EnvFn getv);
const char *yew_osc52_target(YewOsc52EnvFn getv);

/* Appends a complete sequence, or leaves out unchanged on refusal. */
bool yew_osc52_build(Bytebuf *out, const u8 *payload, u64 payload_len,
                     const char *target, YewOsc52Mode mode, u64 max_base64);
bool yew_osc52_build_env(Bytebuf *out, const u8 *payload, u64 payload_len,
                         YewOsc52EnvFn getv);

#endif
