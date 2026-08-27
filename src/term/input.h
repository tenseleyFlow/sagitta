#ifndef YEW_TERM_INPUT_H
#define YEW_TERM_INPUT_H

#include <stdbool.h>
#include <stddef.h>

#include "term/tty.h"
#include "util/base.h"
#include "util/buf.h"

#define YEW_ESC_TIMEOUT_MS 25
#define YEW_IN_MAX_BUFFER (1024U * 1024U)
#define YEW_IN_PASTE_CHUNK 4096U
#define YEW_IN_STRING_MAX 8192U
#define YEW_KEY_BASE 0x110000u

typedef enum {
    YEW_MOD_SHIFT = 1,
    YEW_MOD_ALT = 2,
    YEW_MOD_CTRL = 4,
    YEW_MOD_SUPER = 8,
    YEW_MOD_HYPER = 16,
    YEW_MOD_META = 32,
    YEW_MOD_CAPS = 64,
    YEW_MOD_NUM = 128
} YewMods;

typedef enum {
    YEW_EV_NONE = 0,
    YEW_EV_KEY,
    YEW_EV_PASTE_BEGIN,
    YEW_EV_PASTE_DATA,
    YEW_EV_PASTE_END,
    YEW_EV_MOUSE,
    YEW_EV_FOCUS
} EvKind;

/* Append-only: these ordinals become serialized data in Sprint 25. */
typedef enum {
    YEW_KEY_ESCAPE = YEW_KEY_BASE,
    YEW_KEY_ENTER,
    YEW_KEY_TAB,
    YEW_KEY_BACKSPACE,
    YEW_KEY_INSERT,
    YEW_KEY_DELETE,
    YEW_KEY_UP,
    YEW_KEY_DOWN,
    YEW_KEY_RIGHT,
    YEW_KEY_LEFT,
    YEW_KEY_HOME,
    YEW_KEY_END,
    YEW_KEY_PAGE_UP,
    YEW_KEY_PAGE_DOWN,
    YEW_KEY_BEGIN,
    YEW_KEY_F1,
    YEW_KEY_F2,
    YEW_KEY_F3,
    YEW_KEY_F4,
    YEW_KEY_F5,
    YEW_KEY_F6,
    YEW_KEY_F7,
    YEW_KEY_F8,
    YEW_KEY_F9,
    YEW_KEY_F10,
    YEW_KEY_F11,
    YEW_KEY_F12,
    YEW_KEY_F13,
    YEW_KEY_F14,
    YEW_KEY_F15,
    YEW_KEY_F16,
    YEW_KEY_F17,
    YEW_KEY_F18,
    YEW_KEY_F19,
    YEW_KEY_F20,
    YEW_KEY_F21,
    YEW_KEY_F22,
    YEW_KEY_F23,
    YEW_KEY_F24,
    YEW_KEY_F25,
    YEW_KEY_F26,
    YEW_KEY_F27,
    YEW_KEY_F28,
    YEW_KEY_F29,
    YEW_KEY_F30,
    YEW_KEY_F31,
    YEW_KEY_F32,
    YEW_KEY_F33,
    YEW_KEY_F34,
    YEW_KEY_F35,
    YEW_KEY_KP_0,
    YEW_KEY_KP_1,
    YEW_KEY_KP_2,
    YEW_KEY_KP_3,
    YEW_KEY_KP_4,
    YEW_KEY_KP_5,
    YEW_KEY_KP_6,
    YEW_KEY_KP_7,
    YEW_KEY_KP_8,
    YEW_KEY_KP_9,
    YEW_KEY_KP_DECIMAL,
    YEW_KEY_KP_DIVIDE,
    YEW_KEY_KP_MULTIPLY,
    YEW_KEY_KP_SUBTRACT,
    YEW_KEY_KP_ADD,
    YEW_KEY_KP_ENTER,
    YEW_KEY_KP_EQUAL,
    YEW_KEY_KP_SEPARATOR,
    YEW_KEY_KP_LEFT,
    YEW_KEY_KP_RIGHT,
    YEW_KEY_KP_UP,
    YEW_KEY_KP_DOWN,
    YEW_KEY_KP_PAGE_UP,
    YEW_KEY_KP_PAGE_DOWN,
    YEW_KEY_KP_HOME,
    YEW_KEY_KP_END,
    YEW_KEY_KP_INSERT,
    YEW_KEY_KP_DELETE,
    YEW_KEY_KP_BEGIN,
    YEW_KEY_CAPS_LOCK,
    YEW_KEY_SCROLL_LOCK,
    YEW_KEY_NUM_LOCK,
    YEW_KEY_PRINT_SCREEN,
    YEW_KEY_PAUSE,
    YEW_KEY_MENU,
    YEW_KEY_FOCUS_IN,
    YEW_KEY_FOCUS_OUT,
    YEW_KEY_LEFT_SHIFT,
    YEW_KEY_LEFT_CTRL,
    YEW_KEY_LEFT_ALT,
    YEW_KEY_LEFT_SUPER,
    YEW_KEY_LEFT_HYPER,
    YEW_KEY_LEFT_META,
    YEW_KEY_RIGHT_SHIFT,
    YEW_KEY_RIGHT_CTRL,
    YEW_KEY_RIGHT_ALT,
    YEW_KEY_RIGHT_SUPER,
    YEW_KEY_RIGHT_HYPER,
    YEW_KEY_RIGHT_META
} YewKeyCode;

typedef enum {
    YEW_MB_NONE = 0,
    YEW_MB_LEFT,
    YEW_MB_MIDDLE,
    YEW_MB_RIGHT,
    YEW_MB_WHEEL_UP,
    YEW_MB_WHEEL_DOWN,
    YEW_MB_WHEEL_LEFT,
    YEW_MB_WHEEL_RIGHT,
    YEW_MB_BACK,
    YEW_MB_FORWARD
} YewMouseButton;

enum {
    YEW_KEY_PRESS = 1,
    YEW_KEY_REPEAT = 2,
    YEW_KEY_RELEASE = 3
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
 * Kitty flags pushed by yew are exactly 1|4|16 == 21: disambiguated
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
    /* Feed/EOF work that has not yet reached a parser wait point. */
    bool dispatch_ready;
} In;

/*
 * Feeders must drain between bounded reads. At most 1 MiB of unread input is
 * retained; an overflowing suffix is rejected as one dropped hostile shape,
 * while every already accepted byte remains unchanged. String bodies that hit
 * their 8 KiB cap stay quarantined until ST/BEL/EOF.
 * yew_input_disable() always pops kitty state before disabling 2004, 1002,
 * 1006, and 1004 in that restore order; an unmatched pop is harmless.
 */

void yew_input_init(In *in, const TtyCaps *caps);
void yew_input_free(In *in);
void yew_input_seed(In *in, const Bytebuf *pending);
void yew_input_feed(In *in, const u8 *b, size_t n);
bool yew_input_next(In *in, i64 now_ms, Key *out);
bool yew_input_dispatch_ready(const In *in);
i64 yew_input_deadline(const In *in, i64 now_ms);
void yew_input_eof(In *in);
const u8 *yew_input_paste_chunk(const In *in, size_t *n);
void yew_input_enable(int wfd, const TtyCaps *caps);
void yew_input_disable(int wfd);

#endif
