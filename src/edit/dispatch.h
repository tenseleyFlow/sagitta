#ifndef YEW_EDIT_DISPATCH_H
#define YEW_EDIT_DISPATCH_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/keymap.h"
#include "edit/mode.h"
#include "term/input.h"
#include "util/base.h"

#define YEW_COUNT_MAX 99999U
#define YEW_CHORD_TIMEOUT_DEFAULT_MS 500U

typedef struct Ed Ed;

typedef struct Chord {
    KeyId seq[YEW_CHORD_MAX];
    u8 n;
    i32 layer;
    u32 count;
    bool count_given;
    i64 deadline;
} Chord;

void yew_dispatch_init(Ed *ed);
void yew_dispatch_free(Ed *ed);
void yew_dispatch_key(Ed *ed, Key key, i64 now_ms);
void yew_dispatch_tick(Ed *ed, i64 now_ms);
i64 yew_dispatch_deadline(const Ed *ed);
const Chord *yew_dispatch_pending(const Ed *ed);
const char *yew_dispatch_owner(const Ed *ed);
const char *yew_dispatch_message(const Ed *ed);
void yew_dispatch_clear_message(Ed *ed);
void yew_dispatch_set_mode(Ed *ed, Mode mode);

#endif
