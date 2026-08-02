#ifndef SAG_EDIT_DISPATCH_H
#define SAG_EDIT_DISPATCH_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/keymap.h"
#include "edit/mode.h"
#include "term/input.h"
#include "util/base.h"

#define SAG_COUNT_MAX 99999U
#define SAG_CHORD_TIMEOUT_DEFAULT_MS 500U

typedef struct Ed Ed;

typedef struct Chord {
    KeyId seq[SAG_CHORD_MAX];
    u8 n;
    i32 layer;
    u32 count;
    bool count_given;
    i64 deadline;
} Chord;

void sag_dispatch_init(Ed *ed);
void sag_dispatch_free(Ed *ed);
void sag_dispatch_key(Ed *ed, Key key, i64 now_ms);
void sag_dispatch_tick(Ed *ed, i64 now_ms);
i64 sag_dispatch_deadline(const Ed *ed);
const Chord *sag_dispatch_pending(const Ed *ed);
const char *sag_dispatch_owner(const Ed *ed);
const char *sag_dispatch_message(const Ed *ed);
void sag_dispatch_clear_message(Ed *ed);
void sag_dispatch_set_mode(Ed *ed, Mode mode);

#endif
