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
 * Character classes come from the Unicode general-category table
 * (src/unicode/category.h): \w is Alphabetic ∪ Nd ∪ Pc, \d is Nd, and
 * all twelve POSIX bracket classes are defined over categories rather
 * than ASCII approximations.
 *
 * Worth knowing: \w and Sprint 16's W-mode word motion deliberately
 * disagree about CJK.  Motion uses the UAX #29 word-break properties,
 * where Han is Other so that segmentation does not glue ideographs
 * together; \w uses Alphabetic, so \w+ matches Chinese text.  The two
 * answer different questions — "where does a word end" versus "is this
 * a letter" — and each uses the table that answers its own.
 */

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

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


/*
 * Sprint 21 §6.  Appends `s` to `out`, escaped so that compiling the
 * result yields a pattern matching exactly `s` — no metacharacter in
 * the literal survives as syntax.  Bytes >= 0x80 pass through, so a
 * quoted CJK word is still readable in the message line and register.
 *
 * The single escaper: `*`/`#`, `:s//`, and any later surface that
 * searches for text the user did not type as a pattern must call this
 * rather than roll their own.
 */
void sag_re_quote(Bytebuf *out, const u8 *s, size_t n);

#endif
