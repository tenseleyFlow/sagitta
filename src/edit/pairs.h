#ifndef YEW_EDIT_PAIRS_H
#define YEW_EDIT_PAIRS_H

/*
 * Sprint 57.16 §4: auto-closing brackets and quotes.
 *
 * WHERE THE PAIR TABLE LIVES, AND WHY.  The table is built in to
 * src/edit/pairs.c, keyed by language NAME (Buffer::lang), not carried
 * on SynLangDesc.  Adding a `pairs` sibling to the syntax descriptor would
 * touch parse_language's key whitelist, the cache blob, the
 * YEW_SYN_TABLE_VERSION bump, scripts/gen-langtab, the regenerated
 * src/syn/langs_gen.c that scripts/check-syn-assets.sh verifies, and
 * potentially all 48 of the runtime/syntax Fletch files — to express a
 * difference that, across those 48 languages, amounts to "does the
 * backtick pair" and "does the apostrophe pair".  That is a schema change
 * and an asset regeneration spent on two booleans, and it puts an
 * editing-behaviour concern inside a language DETECTION asset.  One
 * reviewable array here says the same thing, and leaves the descriptor
 * schema free for a sprint that genuinely needs per-language data authored
 * in Fletch.  FOLLOW-UP:
 * if user-authored languages ever need their own pairs, the table moves to
 * SynLangDesc as one deliberate schema sprint, not as a side effect.
 *
 * WHAT IS REMEMBERED.  Each auto-inserted closer is remembered as a Mark,
 * not an offset: marks are adjusted by the edit chokepoint, so the memory
 * survives arbitrary typing, deletion and multi-cursor edits between the
 * delimiters.  That is what makes "type between the pair, then type the
 * closer" skip rather than duplicate.
 *
 * UNDO DOES NOT RESTORE THE STACK.  Undoing past an auto-close leaves no
 * entry behind, so a restored pair simply behaves literally from then on:
 * the closer is ordinary text and typing one inserts one.  That is safe —
 * it can only ever produce a character the user actually typed.
 */

#include "text/coords.h"
#include "text/mark.h"
#include "util/base.h"

typedef struct Buffer Buffer;

enum { YEW_PAIR_STACK_MAX = 32 };

typedef struct PairMark {
    MarkId close;
    u8 closer;
} PairMark;

typedef struct PairState {
    PairMark v[YEW_PAIR_STACK_MAX];
    u8 n;
    /*
     * One-line cache of yew_syn_in_string_or_comment, which heap-allocates
     * a span array and re-lexes the line per call.  Invalidated by any edit
     * to the cached line and by any change to the line count.  A pairing
     * insert edits the caret's line, so consecutive openers do re-ask; the
     * cache exists so that repeat questions about an unchanged line are
     * free, not to skip questions an edit has made stale.
     */
    u64 syn_line;
    u64 syn_lines;
    bool syn_in;
    bool syn_valid;
} PairState;

typedef enum PairAction {
    YEW_PAIR_LITERAL = 0,
    YEW_PAIR_CLOSE,
    YEW_PAIR_SKIP
} PairAction;

/* Cheap per-keystroke gate: true only for a byte that can appear in some
 * pair table.  Nothing else in this module runs for ordinary characters. */
bool yew_pairs_interesting(u8 byte);

/*
 * What typing `byte` at `at` should do.  YEW_PAIR_CLOSE fills *closer with
 * the byte to insert after the caret.  YEW_PAIR_SKIP means the byte at `at`
 * is a remembered closer and the caret should advance over it instead.
 */
PairAction yew_pairs_decide(Buffer *b, ByteOff at, u8 byte, u8 *closer);

/* Records the closer just inserted at `closer_at`. */
void yew_pairs_remember(Buffer *b, ByteOff closer_at, u8 closer);

/* True when `at` sits inside a remembered, still-empty pair; *both is the
 * two-byte span one Backspace should remove. */
bool yew_pairs_backspace(Buffer *b, ByteOff at, Span *both);

/* Forgets every remembered pair; the caller still owns the MarkSet. */
void yew_pairs_clear(Buffer *b);

/* Edit-chokepoint notification: invalidates the cached syntax answer. */
void yew_pairs_note_edit(Buffer *b, LineNo line);

#endif /* YEW_EDIT_PAIRS_H */
