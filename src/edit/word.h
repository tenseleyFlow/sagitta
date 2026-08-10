#ifndef YEW_EDIT_WORD_H
#define YEW_EDIT_WORD_H

#include <stdbool.h>

#include "edit/motion.h"

/* The UAX #29 oracle accepts codepoint boundaries, including invalid-byte
 * escape boundaries.  Editor motions below only expose grapheme boundaries. */
bool yew_word_boundary(const TextBuf *tb, ByteOff pos);

ByteOff yew_word_sub_next(UnitCtx *u, ByteOff p);
ByteOff yew_word_sub_prev(UnitCtx *u, ByteOff p);

#endif
