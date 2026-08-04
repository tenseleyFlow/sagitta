#ifndef SAG_SEARCH_REGEX_INTERNAL_H
#define SAG_SEARCH_REGEX_INTERNAL_H

/* Shared between the parser, the compiler and the execution engines. */

#include "search/regex.h"
#include "text/piece.h"
#include "util/base.h"
#include "util/buf.h"

typedef enum {
    RE_CHAR,   /* arg = codepoint; match and advance                     */
    RE_CLASS,  /* arg = class index; match if cp is in it; advance       */
    RE_ANY,    /* any codepoint (arg = 1 to also match '\n')             */
    RE_SPLIT,  /* try x first, then y — priority defines greediness      */
    RE_JMP,    /* pc = x                                                 */
    RE_SAVE,   /* arg = slot (2g start, 2g+1 end); consumes no input     */
    RE_BOL,
    RE_EOL,    /* line anchors, CRLF-aware                               */
    RE_BOT,
    RE_EOT,    /* window anchors \A \z                                   */
    RE_WORDB,
    RE_NWORDB, /* \b \B                                                  */
    RE_MATCH
} ReOp;

typedef struct ReInst {
    u32 x;
    u32 y;
    u32 arg;
    u8 op;
} ReInst;
_Static_assert(sizeof(ReInst) <= 16, "inst bloat");

/* A class is a sorted, non-overlapping interval list over codepoints.
 * Negation is folded in at build time so matching is one binary search. */
typedef struct ReRange {
    u32 lo;
    u32 hi;
} ReRange;

typedef struct ReClass {
    ReRange *r;
    u32 n;
} ReClass;

typedef enum {
    RE_LIT_NONE = 0,
    RE_LIT_BYTE,
    RE_LIT_BMH,
    RE_LIT_WHOLE
} ReLitKind;

typedef struct ReLit {
    u8 kind;
    u8 s[32];
    u32 n;
    u32 skip[256];
    bool anchored;
} ReLit;

struct SagRe {
    ReInst *prog;
    u32 nprog;
    ReInst *rprog; /* reverse program, Sprint 20 §6c                     */
    u32 nrprog;
    ReClass *classes;
    u32 nclasses;
    u32 ngroups;
    ReLit lit;
    u32 flags;
    u32 min_len;
};

/* ---------------------------------------------------------------- */
/* AST                                                              */
/* ---------------------------------------------------------------- */

typedef enum {
    RE_A_EMPTY,
    RE_A_CHAR,
    RE_A_CLASS,
    RE_A_ANY,
    RE_A_CAT,
    RE_A_ALT,
    RE_A_STAR,
    RE_A_PLUS,
    RE_A_QUEST,
    RE_A_REPEAT,
    RE_A_GROUP,
    RE_A_BOL,
    RE_A_EOL,
    RE_A_BOT,
    RE_A_EOT,
    RE_A_WORDB,
    RE_A_NWORDB
} ReAstKind;

typedef struct ReAst ReAst;
struct ReAst {
    u8 kind;
    bool greedy;
    u32 cp;      /* RE_A_CHAR                                           */
    u32 cls;     /* RE_A_CLASS index                                    */
    u32 group;   /* RE_A_GROUP slot, 0 = non-capturing                  */
    u32 min;
    u32 max;     /* RE_A_REPEAT; max = UINT32_MAX means unbounded       */
    bool dotall; /* RE_A_ANY captured at parse time, per inline flags   */
    ReAst *a;
    ReAst *b;
};

/* Parser output.  Classes are interned into the program as they are
 * parsed so the compiler never re-walks them. */
typedef struct ReParse {
    Arena *arena;
    const u8 *pat;
    size_t len;
    size_t at;
    u32 flags;      /* current (inline flags mutate this)               */
    u32 base_flags; /* as given to sag_re_compile                       */
    u32 ngroups;    /* including group 0                                */
    ReClass *classes;
    u32 nclasses;
    u32 ncap_classes;
    SagReErr *err;
    bool failed;
} ReParse;

ReAst *sag_re_parse(ReParse *p);
void sag_re_fail(ReParse *p, size_t off, const char *msg);
/* Interns a class, returning its index (or UINT32_MAX on overflow). */
u32 sag_re_class_intern(ReParse *p, ReRange *ranges, u32 n, bool negate);
/* Builds the class for a Perl shorthand: 'w','d','s' and their negations. */
u32 sag_re_class_perl(ReParse *p, char which);
/* Simple case folding: appends the folded partners of `cp`. */
u32 sag_re_fold_partners(u32 cp, u32 out[4]);

bool sag_re_class_has(const ReClass *c, u32 cp);

/* Literal prefilter (§7). */
void sag_bmh_build(ReLit *l);
u64 sag_lit_find(const ReLit *l, const u8 *hay, u64 n);
/* The Pike VM, anchored at `start`: the match must begin exactly there.
 * This is the correctness reference the lazy DFA is checked against. */
bool sag_re_pike_run(const SagRe *re, const SagReInput *in, u64 start,
                     SagReMatch *out);

/* Character-property predicates, defined off the word-break tables so a
 * search \b and Sprint 16's W-mode word motion agree about what a word
 * is.  A user who selects a word with W and searches \bword\b must not
 * get two different answers. */
bool sag_re_is_word(u32 cp);
bool sag_re_is_digit(u32 cp);
bool sag_re_is_space(u32 cp);

#endif
