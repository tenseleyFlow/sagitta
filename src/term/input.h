#ifndef SAG_TERM_INPUT_H
#define SAG_TERM_INPUT_H

#include <stdbool.h>
#include <stddef.h>

#include "term/tty.h"
#include "util/base.h"
#include "util/buf.h"

#define SAG_ESC_TIMEOUT_MS 25
#define SAG_IN_MAX_BUFFER (1024U * 1024U)
#define SAG_IN_PASTE_CHUNK 4096U
#define SAG_IN_STRING_MAX 8192U
#define SAG_KEY_BASE 0x110000u

typedef enum {
    SAG_MOD_SHIFT = 1,
    SAG_MOD_ALT = 2,
    SAG_MOD_CTRL = 4,
    SAG_MOD_SUPER = 8,
    SAG_MOD_HYPER = 16,
    SAG_MOD_META = 32,
    SAG_MOD_CAPS = 64,
    SAG_MOD_NUM = 128
} SagMods;

typedef enum {
    SAG_EV_NONE = 0,
    SAG_EV_KEY,
    SAG_EV_PASTE_BEGIN,
    SAG_EV_PASTE_DATA,
    SAG_EV_PASTE_END,
    SAG_EV_MOUSE,
    SAG_EV_FOCUS
} EvKind;

/* Append-only: these ordinals become serialized data in Sprint 25. */
typedef enum {
    SAG_KEY_ESCAPE = SAG_KEY_BASE,
    SAG_KEY_ENTER,
    SAG_KEY_TAB,
    SAG_KEY_BACKSPACE,
    SAG_KEY_INSERT,
    SAG_KEY_DELETE,
    SAG_KEY_UP,
    SAG_KEY_DOWN,
    SAG_KEY_RIGHT,
    SAG_KEY_LEFT,
    SAG_KEY_HOME,
    SAG_KEY_END,
    SAG_KEY_PAGE_UP,
    SAG_KEY_PAGE_DOWN,
    SAG_KEY_BEGIN,
    SAG_KEY_F1,
    SAG_KEY_F2,
    SAG_KEY_F3,
    SAG_KEY_F4,
    SAG_KEY_F5,
    SAG_KEY_F6,
    SAG_KEY_F7,
    SAG_KEY_F8,
    SAG_KEY_F9,
    SAG_KEY_F10,
    SAG_KEY_F11,
    SAG_KEY_F12,
    SAG_KEY_F13,
    SAG_KEY_F14,
    SAG_KEY_F15,
    SAG_KEY_F16,
    SAG_KEY_F17,
    SAG_KEY_F18,
    SAG_KEY_F19,
    SAG_KEY_F20,
    SAG_KEY_F21,
    SAG_KEY_F22,
    SAG_KEY_F23,
    SAG_KEY_F24,
    SAG_KEY_F25,
    SAG_KEY_F26,
    SAG_KEY_F27,
    SAG_KEY_F28,
    SAG_KEY_F29,
    SAG_KEY_F30,
    SAG_KEY_F31,
    SAG_KEY_F32,
    SAG_KEY_F33,
    SAG_KEY_F34,
    SAG_KEY_F35,
    SAG_KEY_KP_0,
    SAG_KEY_KP_1,
    SAG_KEY_KP_2,
    SAG_KEY_KP_3,
    SAG_KEY_KP_4,
    SAG_KEY_KP_5,
    SAG_KEY_KP_6,
    SAG_KEY_KP_7,
    SAG_KEY_KP_8,
    SAG_KEY_KP_9,
    SAG_KEY_KP_DECIMAL,
    SAG_KEY_KP_DIVIDE,
    SAG_KEY_KP_MULTIPLY,
    SAG_KEY_KP_SUBTRACT,
    SAG_KEY_KP_ADD,
    SAG_KEY_KP_ENTER,
    SAG_KEY_KP_EQUAL,
    SAG_KEY_KP_SEPARATOR,
    SAG_KEY_KP_LEFT,
    SAG_KEY_KP_RIGHT,
    SAG_KEY_KP_UP,
    SAG_KEY_KP_DOWN,
    SAG_KEY_KP_PAGE_UP,
    SAG_KEY_KP_PAGE_DOWN,
    SAG_KEY_KP_HOME,
    SAG_KEY_KP_END,
    SAG_KEY_KP_INSERT,
    SAG_KEY_KP_DELETE,
    SAG_KEY_KP_BEGIN,
    SAG_KEY_CAPS_LOCK,
    SAG_KEY_SCROLL_LOCK,
    SAG_KEY_NUM_LOCK,
    SAG_KEY_PRINT_SCREEN,
    SAG_KEY_PAUSE,
    SAG_KEY_MENU,
    SAG_KEY_FOCUS_IN,
    SAG_KEY_FOCUS_OUT,
    SAG_KEY_LEFT_SHIFT,
    SAG_KEY_LEFT_CTRL,
    SAG_KEY_LEFT_ALT,
    SAG_KEY_LEFT_SUPER,
    SAG_KEY_LEFT_HYPER,
    SAG_KEY_LEFT_META,
    SAG_KEY_RIGHT_SHIFT,
    SAG_KEY_RIGHT_CTRL,
    SAG_KEY_RIGHT_ALT,
    SAG_KEY_RIGHT_SUPER,
    SAG_KEY_RIGHT_HYPER,
    SAG_KEY_RIGHT_META
} SagKeyCode;

typedef enum {
    SAG_MB_NONE = 0,
    SAG_MB_LEFT,
    SAG_MB_MIDDLE,
    SAG_MB_RIGHT,
    SAG_MB_WHEEL_UP,
    SAG_MB_WHEEL_DOWN,
    SAG_MB_WHEEL_LEFT,
    SAG_MB_WHEEL_RIGHT,
    SAG_MB_BACK,
    SAG_MB_FORWARD
} SagMouseButton;

enum {
    SAG_KEY_PRESS = 1,
    SAG_KEY_REPEAT = 2,
    SAG_KEY_RELEASE = 3
};

typedef struct Key {
    u32 code;
    u16 kind;
    u16 mods;
    u16 col;
    u16 row;
    u8 ev;
    u8 button;
    u8 ntext;
    u8 text[16];
} Key;
_Static_assert(sizeof(Key) == 32, "Key is half a cache line");

/*
 * Unicode key codes and named codes occupy disjoint ranges.  Bindings use
 * code+mods while insertion uses text: for example Shift+2 may have code '2'
 * and text "@".  ntext is capped at 15 bytes (the final byte is reserved
 * headroom for consumers that make a temporary C string); truncation is at
 * a codepoint boundary. Legacy Ctrl+Shift+letter cannot be distinguished from
 * Ctrl+letter and is normalized to the lowercase codepoint.
 *
 * Kitty flags pushed by Sagitta are exactly 1|4|16 == 21: disambiguated
 * escapes, alternate keys, and associated text.  Event types and "all keys"
 * (2 and 8) are deliberately not requested.
 *
 * Paste payload is always data, never keys.  A payload containing its own
 * terminal-provided end marker necessarily ends framing early; consumers must
 * keep paste insertion visible and reversible, never execute pasted text.
 */
typedef struct In {
    Bytebuf buf;
    size_t rd;
    u8 state;
    u8 utf8_state;
    u32 utf8_cp;
    u16 par[16];
    u8 sub[16];
    u8 npar;
    u8 priv;
    u8 inter;
    i64 deadline;
    bool in_paste;
    Bytebuf paste;
    TtyCaps caps;
    u32 dropped;

    /* Parser-private continuation state; public only because In is stackable. */
    size_t seq_start;
    size_t scan;
    size_t string_len;
    u32 xpar[16][16];
    bool xpresent[16][16];
    u8 xnsub[16];
    u8 pending_mods;
    u8 log_mask;
    bool eof;
} In;

/*
 * Feeders must drain between bounded reads. At most 1 MiB of unread input is
 * retained; an overflowing suffix is rejected as one dropped hostile shape,
 * while every already accepted byte remains unchanged. String bodies that hit
 * their 8 KiB cap stay quarantined until ST/BEL/EOF.
 * sag_input_disable() always pops kitty state before disabling 2004, 1002,
 * 1006, and 1004 in that restore order; an unmatched pop is harmless.
 */

void sag_input_init(In *in, const TtyCaps *caps);
void sag_input_free(In *in);
void sag_input_seed(In *in, const Bytebuf *pending);
void sag_input_feed(In *in, const u8 *b, size_t n);
bool sag_input_next(In *in, i64 now_ms, Key *out);
i64 sag_input_deadline(const In *in, i64 now_ms);
void sag_input_eof(In *in);
const u8 *sag_input_paste_chunk(const In *in, size_t *n);
void sag_input_enable(int wfd, const TtyCaps *caps);
void sag_input_disable(int wfd);

#endif
