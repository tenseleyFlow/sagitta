#ifndef YEW_SEARCH_REGEX_H
#define YEW_SEARCH_REGEX_H

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
typedef struct YewRe YewRe; /* arena-owned, immutable once compiled */

/*
 * Reusable execution storage for callers that perform many anchored
 * probes.  The object itself is caller-owned; its implementation remains
 * private so Pike's thread representation is not part of the public ABI.
 * Initialize once, reuse serially, then free.  It owns no regex or input.
 */
typedef struct YewReWorkspace {
    void *impl;
} YewReWorkspace;

enum {
    YEW_RE_ICASE = 1U << 0,     /* simple case folding, s02 tables      */
    YEW_RE_DOTALL = 1U << 1,    /* '.' also matches '\n'                */
    YEW_RE_LITERAL = 1U << 2,   /* the whole pattern is literal text    */
    YEW_RE_NOCAPTURE = 1U << 3  /* caller promises not to read groups   */
};

enum {
    YEW_RE_MAX_GROUPS = 32,
    YEW_RE_MAX_PROG = 4096,
    YEW_RE_MAX_REPEAT = 1000,
    YEW_RE_MAX_CLASSES = 256,
    YEW_RE_MAX_PATTERN = 64U * 1024U
};

/* `off` is a byte offset into the pattern so Sprint 18's prompt can point
 * a caret at the offending construct while the user is still typing. */
typedef struct YewReErr {
    u32 off;
    const char *msg;
} YewReErr;

typedef struct YewReMatch {
    /* g[0] is the whole match; an unmatched optional group has both
     * endpoints set to UINT64_MAX. */
    Span g[YEW_RE_MAX_GROUPS];
    u32 ngroups;
} YewReMatch;

/*
 * One input struct, two backings, no vtable and no allocation: a TextBuf
 * (searched through its iterator) or a flat byte range.  `window` bounds
 * the region under consideration and is what \A and \z anchor to.
 */
typedef struct YewReInput {
    const TextBuf *tb; /* NULL => raw bytes                             */
    const u8 *bytes;
    u64 len;
    Span window;
} YewReInput;

YewRe *yew_re_compile(Arena *a, const char *pat, size_t len, u32 flags,
                      YewReErr *err);

/* Leftmost-first match at or after `from`. */
bool yew_re_search(const YewRe *re, const YewReInput *in, ByteOff from,
                   YewReMatch *out);
/* The last match starting strictly before `before`. */
bool yew_re_search_back(const YewRe *re, const YewReInput *in,
                        ByteOff before, YewReMatch *out);
/* Anchored at `at`: the match must start exactly there. */
bool yew_re_match_at(const YewRe *re, const YewReInput *in, ByteOff at,
                     YewReMatch *out);
void yew_re_workspace_init(YewReWorkspace *workspace);
void yew_re_workspace_free(YewReWorkspace *workspace);
bool yew_re_match_at_ws(YewReWorkspace *workspace, const YewRe *re,
                        const YewReInput *in, ByteOff at, YewReMatch *out);
bool yew_re_test(const YewRe *re, const YewReInput *in, ByteOff from);
u32 yew_re_group_count(const YewRe *re);
/* Minimum codepoints any match consumes; 0 when a match may be empty. */
u32 yew_re_min_len(const YewRe *re);
/* Exact byte length when the whole pattern is one literal, otherwise 0.
 * The live-search UI uses this to chunk the overwhelmingly common case
 * without changing regex-anchor semantics at artificial chunk edges. */
u32 yew_re_whole_literal_bytes(const YewRe *re);
/* True only for one class-or-any atom, optionally repeated without an
 * upper bound.  Syntax-definition validation uses this narrow shape to
 * diagnose first-match rules that make later rules unreachable. */
bool yew_re_is_simple_catch_all(const YewRe *re);

/* Convenience for callers holding a plain buffer. */
YewReInput yew_re_input_bytes(const u8 *bytes, u64 len);
YewReInput yew_re_input_textbuf(const TextBuf *tb);
/* One byte, either backing.  Seeks per call: fine for a match-sized
 * span, wrong for a scan. */
bool yew_re_input_byte(const YewReInput *in, u64 off, u8 *out);


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
void yew_re_quote(Bytebuf *out, const u8 *s, size_t n);

/*
 * Sprint 21 §2 inputs to the smartcase table.  "Uppercase literal" is a
 * property of what was TYPED: `\Wfoo`, `\x41` and `[[:upper:]]` all
 * report false, because none of them is the user asking for case
 * sensitivity.  `\c`/`\C` outrank every option.
 */
bool yew_re_has_upper_literal(const YewRe *re);
bool yew_re_forces_icase(const YewRe *re);
bool yew_re_forces_case(const YewRe *re);

/* Conservative byte set for an anchored match at offset zero.  A set bit
 * means "may begin with this byte"; false positives only cost a VM probe,
 * while a false negative would make syntax highlighting incorrect. */
void yew_re_first_bytes(const YewRe *re, u8 first[32]);

/* Stable, pointer-free compiled form used by the syntax-definition cache.
 * Unpacking performs bounds/shape validation and never parses a pattern. */
bool yew_re_pack(const YewRe *re, Bytebuf *out);
YewRe *yew_re_unpack(Arena *a, const u8 *data, size_t len, size_t *used);

#endif
