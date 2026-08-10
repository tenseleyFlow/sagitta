#ifndef YEW_UI_TYPEJUMP_H
#define YEW_UI_TYPEJUMP_H

/*
 * Sprint 26 §8: type-to-jump, for lists that cannot carry a filter
 * line — s24's group picker listing, and (Sprint 52) the F-mode tree,
 * which is where fuss's behaviour comes from.
 *
 * FOUR RULES, and the third is the one people get wrong:
 *
 * 1. A printable key INSIDE the window appends to the pattern; the same
 *    key after the window expires REPLACES it.  Typing `s`, pausing,
 *    then `s` again means "find the next thing starting with s", not
 *    "find `ss`".
 *
 * 2. The deadline is a TIMER HEAP entry, not a lazily-checked
 *    timestamp.  The status hint showing the accumulated pattern has to
 *    clear on its own with no further keys, or it sits there
 *    indefinitely on an idle editor promising something that is no
 *    longer true — the same discipline as s24's 500 ms digit window.
 *
 * 3. IF THE SELECTED ITEM ALREADY SCORES EXACT, STAY PUT.  fuss's rule
 *    verbatim.  Typing `README` while sitting on `README` must not jump
 *    to `README.md` two rows down because it also matched; an exact
 *    match under the cursor is the answer.
 *
 * 4. Non-printable keys clear the state FIRST, then dispatch normally.
 *    A letter typed much later must not read as a continuation of
 *    whatever was typed before an arrow key.
 */

#include "term/input.h"
#include "ui/picker.h"
#include "util/base.h"

#define YEW_TYPEJUMP_RESET_MS 500

enum {
    YEW_TYPEJUMP_PAT_MAX = 64
};

typedef struct TypeJump {
    char pat[YEW_TYPEJUMP_PAT_MAX];
    u32 len;
    i64 deadline_ms;
} TypeJump;

void yew_typejump_clear(TypeJump *tj);

/*
 * Consumes a key, moving `*sel` to the best match.
 *
 * False when the key was NOT consumed, so a host list keeps its own
 * bindings — a type-to-jump that swallowed every key would make the
 * list it decorates unusable.
 */
bool yew_typejump_key(TypeJump *tj, const Key *k, i64 now_ms,
                      const PickItem *items, u32 n, u32 *sel);

/* True while a pattern is accumulating, for the status hint. */
bool yew_typejump_active(const TypeJump *tj, i64 now_ms);

#endif
