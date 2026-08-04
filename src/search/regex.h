#ifndef SAG_SEARCH_REGEX_H
#define SAG_SEARCH_REGEX_H

/*
 * Sprint 20: the bespoke regex engine.
 *
 * Thompson construction into a small program, executed by a Pike VM
 * (NFA simulation with submatch capture, leftmost-first).  THERE IS NO
 * BACKTRACKING ANYWHERE.  That is the product feature, not an
 * implementation detail: Sprint 21's incremental search recompiles and
 * re-scans on every keystroke, so a pattern like (a+)+ typed into the
 * prompt must not be able to freeze the editor.  Cost is O(n*m) always,
 * with no pattern-dependent cliff.
 *
 * Lookaround, backreferences and atomic groups are permanent non-goals,
 * not deferrals — see the rejection message in regex.c.
 *
 * KNOWN GAP, tracked not hidden.  The sprint defines \w as Alphabetic ∪
 * Nd ∪ Pc and specifies twelve POSIX bracket classes over Unicode
 * general categories.  Sprint 2's generated tables carry grapheme,
 * width and word-break properties but NOT general category, so:
 *
 *   - \w \d \s and \b are defined off the UAX #29 word-break properties.
 *     That buys something worth having — search boundaries agree exactly
 *     with Sprint 16's W-mode word motion — but it also means Han
 *     ideographs are word-break Other, so \w+ does not match CJK text.
 *     For a Unicode-correct editor that is a real limitation, not a
 *     nuance.
 *   - [[:digit:]], [[:space:]] and [[:word:]] work; the other nine POSIX
 *     classes report an error naming the missing tables rather than
 *     silently answering with an ASCII approximation.
 *
 * Closing both needs the Unicode generator to emit a category field.
 */

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct TextBuf TextBuf;
typedef struct SagRe SagRe; /* arena-owned, immutable once compiled */

enum {
    SAG_RE_ICASE = 1U << 0,     /* simple case folding, s02 tables      */
    SAG_RE_DOTALL = 1U << 1,    /* '.' also matches '\n'                */
    SAG_RE_LITERAL = 1U << 2,   /* the whole pattern is literal text    */
    SAG_RE_NOCAPTURE = 1U << 3  /* caller promises not to read groups   */
};

enum {
    SAG_RE_MAX_GROUPS = 32,
    SAG_RE_MAX_PROG = 4096,
    SAG_RE_MAX_REPEAT = 1000,
    SAG_RE_MAX_CLASSES = 256,
    SAG_RE_MAX_PATTERN = 64U * 1024U
};

/* `off` is a byte offset into the pattern so Sprint 18's prompt can point
 * a caret at the offending construct while the user is still typing. */
typedef struct SagReErr {
    u32 off;
    const char *msg;
} SagReErr;

typedef struct SagReMatch {
    Span g[SAG_RE_MAX_GROUPS]; /* g[0] is the whole match               */
    u32 ngroups;
} SagReMatch;

/*
 * One input struct, two backings, no vtable and no allocation: a TextBuf
 * (searched through its iterator) or a flat byte range.  `window` bounds
 * the region under consideration and is what \A and \z anchor to.
 */
typedef struct SagReInput {
    const TextBuf *tb; /* NULL => raw bytes                             */
    const u8 *bytes;
    u64 len;
    Span window;
} SagReInput;

SagRe *sag_re_compile(Arena *a, const char *pat, size_t len, u32 flags,
                      SagReErr *err);

/* Leftmost-first match at or after `from`. */
bool sag_re_search(const SagRe *re, const SagReInput *in, ByteOff from,
                   SagReMatch *out);
/* The last match starting strictly before `before`. */
bool sag_re_search_back(const SagRe *re, const SagReInput *in,
                        ByteOff before, SagReMatch *out);
/* Anchored at `at`: the match must start exactly there. */
bool sag_re_match_at(const SagRe *re, const SagReInput *in, ByteOff at,
                     SagReMatch *out);
bool sag_re_test(const SagRe *re, const SagReInput *in, ByteOff from);
u32 sag_re_group_count(const SagRe *re);
/* Minimum codepoints any match consumes; 0 when a match may be empty. */
u32 sag_re_min_len(const SagRe *re);

/* Convenience for callers holding a plain buffer. */
SagReInput sag_re_input_bytes(const u8 *bytes, u64 len);
SagReInput sag_re_input_textbuf(const TextBuf *tb);

#endif
