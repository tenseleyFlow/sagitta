/*
 * Sprint 20 DoD 8 + 11: the error table and the emitted program shape.
 *
 * The offsets matter as much as the messages.  Sprint 21's incremental
 * search renders SagReErr.off as a caret under the pattern while the
 * user is still typing, so an offset that points at the wrong construct
 * is a visible bug — and one that no "did it compile?" test would catch.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/regex_internal.h"
#include "util/arena.h"

typedef struct ErrRow {
    const char *pat;
    u32 flags;
    const char *msg;
    i64 off; /* -1: do not check the offset, only the message */
} ErrRow;

#define ANY_OFF -1

/* The permanent non-goals share one sentence; spelling it once keeps the
 * rows readable and makes a drift in the wording a single-line diff. */
#define WHY "(sagitta's regex engine is linear-time by design; " \
            "see man sagitta-regex)"

static const ErrRow errors[] = {
    /* --- §8 table, row for row ------------------------------- */
    {"[abc", 0U, "unterminated character class", 0},
    {"x[a-", 0U, "unterminated character class", 1},
    {"(abc", 0U, "unterminated group", 0},
    {"a(b(c)", 0U, "unterminated group", 1},
    {"abc)", 0U, "unmatched )", 3},
    {")", 0U, "unmatched )", 0},
    {"*a", 0U, "nothing to repeat", 0},
    {"+", 0U, "nothing to repeat", 0},
    {"?x", 0U, "nothing to repeat", 0},
    {"a{2,1}", 0U, "invalid repeat: min 2 > max 1", 1},
    {"a{0,2000}", 0U, "repeat count 2000 exceeds 1000", 1},
    {"a{2000}", 0U, "repeat count 2000 exceeds 1000", 1},
    {"abc\\", 0U, "trailing backslash", 3},
    {"\\", 0U, "trailing backslash", 0},
    {"a\\q", 0U, "unknown escape '\\q'", 1},
    {"\\x{}", 0U, "invalid codepoint escape", 0},
    {"\\x{110000}", 0U, "invalid codepoint escape", 0},
    {"\\x{ZZ}", 0U, "invalid codepoint escape", 0},
    {"\\xZZ", 0U, "invalid codepoint escape", 0},
    /* The classic POSIX mistake: one layer of brackets, not two. */
    {"[:alpha:]", 0U, "[:alpha:] is only valid inside [...]", 0},
    {"x[:digit:]", 0U, "[:digit:] is only valid inside [...]", 1},
    /* Offset 1, not 0: the caret points at the inner `[:` — the bad
     * construct — rather than at the enclosing class that is fine. */
    {"[[:foo:]]", 0U, "unknown POSIX class '[:foo:]'", 1},

    /* --- permanent non-goals, NOT sprint deferrals ------------ */
    {"(?=a)", 0U, "lookaround is not supported " WHY, 0},
    {"(?!a)", 0U, "lookaround is not supported " WHY, 0},
    {"(?<=a)", 0U, "lookaround is not supported " WHY, 0},
    {"(?<!a)", 0U, "lookaround is not supported " WHY, 0},
    {"(?>a)", 0U, "atomic groups are not supported " WHY, 0},
    {"a*+", 0U, "possessive quantifiers are not supported " WHY, 2},
    {"(a)\\1", 0U, "backreferences are not supported " WHY, 3},
    {"\\9", 0U, "backreferences are not supported " WHY, 0},
    {"(?P<n>a)", 0U, "named groups are not supported in 1.0", 0},

    /* --- misc parse errors ------------------------------------ */
    {"[z-a]", 0U, "invalid range in character class", 0},
    {"(?q)", 0U, "unknown group flags", ANY_OFF}
};

static void check_error(const ErrRow *row)
{
    Arena arena;
    SagReErr err;
    SagRe *re;

    arena_init(&arena);
    (void)memset(&err, 0, sizeof(err));
    err.off = 0xFFFFFFFFU;
    re = sag_re_compile(&arena, row->pat, strlen(row->pat), row->flags,
                        &err);
    if (re != NULL)
        (void)fprintf(stderr, "/%s/ compiled but should have failed\n",
                      row->pat);
    SAG_ASSERT_NULL(re);
    SAG_ASSERT_NOT_NULL(err.msg);
    if (err.msg != NULL && strcmp(err.msg, row->msg) != 0)
        (void)fprintf(stderr, "/%s/ said \"%s\", want \"%s\"\n", row->pat,
                      err.msg, row->msg);
    SAG_ASSERT_EQ_STR(err.msg, row->msg);
    /* The offset must land inside the pattern no matter what. */
    SAG_ASSERT(err.off <= (u32)strlen(row->pat));
    if (row->off != ANY_OFF) {
        if (err.off != (u32)row->off)
            (void)fprintf(stderr, "/%s/ off=%u, want %lld\n", row->pat,
                          (unsigned)err.off, (long long)row->off);
        SAG_ASSERT_EQ_U64(err.off, (u64)row->off);
    }
    arena_free_all(&arena);
}

void test_re_error_table(void)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(errors); i++)
        check_error(&errors[i]);
}

void test_re_non_goals_are_not_sprint_deferrals(void)
{
    static const char *const patterns[] = {
        "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?>a)", "a*+", "\\1"
    };
    size_t i;

    /*
     * DoD 8's other half.  Every other unimplemented thing in this tree
     * says "Sprint N"; these must not, because they are never coming —
     * they are incompatible with the linear-time guarantee, not waiting
     * on it.  A message naming a sprint would promise a feature that
     * will never arrive.
     */
    for (i = 0U; i < SAG_ARRAY_LEN(patterns); i++) {
        Arena arena;
        SagReErr err = {0, NULL};

        arena_init(&arena);
        SAG_ASSERT_NULL(sag_re_compile(&arena, patterns[i],
                                       strlen(patterns[i]), 0U, &err));
        SAG_ASSERT_NOT_NULL(err.msg);
        SAG_ASSERT_NULL(strstr(err.msg, "Sprint"));
        SAG_ASSERT_NOT_NULL(strstr(err.msg, "linear-time by design"));
        arena_free_all(&arena);
    }
}

/* ---------------------------------------------------------------- */
/* Program shape (§5)                                               */
/* ---------------------------------------------------------------- */

static const char *op_name(u8 op)
{
    switch ((ReOp)op) {
    case RE_CHAR:   return "CHAR";
    case RE_CLASS:  return "CLASS";
    case RE_ANY:    return "ANY";
    case RE_SPLIT:  return "SPLIT";
    case RE_JMP:    return "JMP";
    case RE_SAVE:   return "SAVE";
    case RE_BOL:    return "BOL";
    case RE_EOL:    return "EOL";
    case RE_BOT:    return "BOT";
    case RE_EOT:    return "EOT";
    case RE_WORDB:  return "WORDB";
    case RE_NWORDB: return "NWORDB";
    case RE_MATCH:  return "MATCH";
    default:        break;
    }
    return "?";
}

/* Renders the forward program as a space-separated opcode sequence so a
 * shape can be asserted as a readable string rather than index math. */
static void shape_of(const SagRe *re, char *out, size_t cap)
{
    size_t at = 0U;
    u32 i;

    out[0] = '\0';
    for (i = 0U; i < re->nprog && at + 8U < cap; i++) {
        int n = snprintf(out + at, cap - at, "%s%s", at == 0U ? "" : " ",
                         op_name(re->prog[i].op));

        if (n <= 0)
            return;
        at += (size_t)n;
    }
}

static void check_shape(const char *pat, u32 flags, const char *want)
{
    Arena arena;
    SagRe *re;
    char got[512];

    arena_init(&arena);
    re = sag_re_compile(&arena, pat, strlen(pat), flags, NULL);
    SAG_ASSERT_NOT_NULL(re);
    if (re != NULL) {
        shape_of(re, got, sizeof(got));
        if (strcmp(got, want) != 0)
            (void)fprintf(stderr, "/%s/ shape\n  got  %s\n  want %s\n",
                          pat, got, want);
        SAG_ASSERT_EQ_STR(got, want);
    }
    arena_free_all(&arena);
}

void test_re_program_shapes(void)
{
    /* Every program is wrapped in SAVE 0 ... SAVE 1, MATCH. */
    check_shape("a", 0U, "SAVE CHAR SAVE MATCH");
    check_shape("abc", 0U, "SAVE CHAR CHAR CHAR SAVE MATCH");
    check_shape(".", 0U, "SAVE ANY SAVE MATCH");
    check_shape("[a-z]", 0U, "SAVE CLASS SAVE MATCH");
    check_shape("\\d", 0U, "SAVE CLASS SAVE MATCH");
    /* Alternation: SPLIT, branch, JMP over the second branch. */
    check_shape("a|b", 0U, "SAVE SPLIT CHAR JMP CHAR SAVE MATCH");
    /* Star: SPLIT at the top, JMP back to it. */
    check_shape("a*", 0U, "SAVE SPLIT CHAR JMP SAVE MATCH");
    /* Lazy star emits the same shape — only the SPLIT targets swap, which
     * is why greediness cannot be read off the opcode sequence alone. */
    check_shape("a*?", 0U, "SAVE SPLIT CHAR JMP SAVE MATCH");
    /* Plus: body first, then the SPLIT that loops back. */
    check_shape("a+", 0U, "SAVE CHAR SPLIT SAVE MATCH");
    check_shape("a?", 0U, "SAVE SPLIT CHAR SAVE MATCH");
    /* Groups bracket their body with SAVE pairs. */
    check_shape("(a)", 0U, "SAVE SAVE CHAR SAVE SAVE MATCH");
    /* Non-capturing groups consume no slots and emit no SAVEs. */
    check_shape("(?:a)", 0U, "SAVE CHAR SAVE MATCH");
    /* {m,n} expands by copying: 2 mandatory, 1 optional. */
    check_shape("a{2,3}", 0U, "SAVE CHAR CHAR SPLIT CHAR SAVE MATCH");
    check_shape("a{3}", 0U, "SAVE CHAR CHAR CHAR SAVE MATCH");
    /* {m,} is m copies then a star. */
    check_shape("a{2,}", 0U, "SAVE CHAR CHAR SPLIT CHAR JMP SAVE MATCH");
    /* Anchors and boundaries are their own opcodes. */
    check_shape("^a$", 0U, "SAVE BOL CHAR EOL SAVE MATCH");
    check_shape("\\Aa\\z", 0U, "SAVE BOT CHAR EOT SAVE MATCH");
    check_shape("\\ba\\B", 0U, "SAVE WORDB CHAR NWORDB SAVE MATCH");
    /* ICASE turns a literal into a class at COMPILE time, so the VM does
     * no case work per input codepoint. */
    check_shape("k", SAG_RE_ICASE, "SAVE CLASS SAVE MATCH");
    /* A literal-flag pattern is all CHARs, metacharacters included. */
    check_shape("a*b", SAG_RE_LITERAL, "SAVE CHAR CHAR CHAR SAVE MATCH");
    /* The empty pattern still saves and matches. */
    check_shape("", 0U, "SAVE SAVE MATCH");
}

void test_re_reverse_program_swaps_anchors(void)
{
    Arena arena;
    SagRe *re;

    /*
     * §6c: the reverse program is the same AST with concatenation
     * flipped and the directional anchors swapped, so one compiler
     * serves both scan directions.  \b is symmetric and stays put.
     */
    arena_init(&arena);
    re = sag_re_compile(&arena, "^ab$", 4U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT_EQ_U64(re->nprog, re->nrprog);
    /* Forward: BOL then CHAR a, CHAR b, then EOL. */
    SAG_ASSERT_EQ_U64(re->prog[1].op, (u64)RE_BOL);
    SAG_ASSERT_EQ_U64(re->prog[2].arg, (u64)'a');
    SAG_ASSERT_EQ_U64(re->prog[3].arg, (u64)'b');
    SAG_ASSERT_EQ_U64(re->prog[4].op, (u64)RE_EOL);
    /*
     * Reverse: the forward pattern's LAST element is emitted first, with
     * its anchor swapped.  So `$` (EOL) leads as a BOL, the characters
     * come in the other order, and `^` trails as an EOL.  Reading this
     * as "EOL first" gets it exactly backwards.
     */
    SAG_ASSERT_EQ_U64(re->rprog[1].op, (u64)RE_BOL);
    SAG_ASSERT_EQ_U64(re->rprog[2].arg, (u64)'b');
    SAG_ASSERT_EQ_U64(re->rprog[3].arg, (u64)'a');
    SAG_ASSERT_EQ_U64(re->rprog[4].op, (u64)RE_EOL);

    re = sag_re_compile(&arena, "\\Aa\\z", 5U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT_EQ_U64(re->rprog[1].op, (u64)RE_BOT);
    SAG_ASSERT_EQ_U64(re->rprog[3].op, (u64)RE_EOT);

    re = sag_re_compile(&arena, "\\ba", 3U, 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    SAG_ASSERT_EQ_U64(re->rprog[2].op, (u64)RE_WORDB);
    arena_free_all(&arena);
}

void test_re_limits_fire_during_emission(void)
{
    Arena arena;
    SagReErr err;
    char big[8192];
    size_t at = 0U;
    int i;

    arena_init(&arena);
    /* DoD 11: 33 groups is one past the limit. */
    for (i = 0; i < 33; i++)
        at += (size_t)snprintf(big + at, sizeof(big) - at, "(a)");
    (void)memset(&err, 0, sizeof(err));
    SAG_ASSERT_NULL(sag_re_compile(&arena, big, at, 0U, &err));
    SAG_ASSERT_EQ_STR(err.msg, "too many capture groups (max 32)");

    /*
     * Program limit, checked DURING emission.  a{1000} nested inside
     * another {1000} would expand to a million instructions; the point
     * is that it is rejected without ever allocating them.
     */
    at = (size_t)snprintf(big, sizeof(big), "(a{1000}){1000}");
    (void)memset(&err, 0, sizeof(err));
    SAG_ASSERT_NULL(sag_re_compile(&arena, big, at, 0U, &err));
    SAG_ASSERT_EQ_STR(err.msg,
                      "pattern too complex (program limit 4096)");

    /* Pattern length. */
    {
        char *huge = malloc(SAG_RE_MAX_PATTERN + 16U);

        SAG_ASSERT_NOT_NULL(huge);
        (void)memset(huge, 'a', SAG_RE_MAX_PATTERN + 8U);
        (void)memset(&err, 0, sizeof(err));
        SAG_ASSERT_NULL(sag_re_compile(&arena, huge,
                                       SAG_RE_MAX_PATTERN + 8U, 0U,
                                       &err));
        SAG_ASSERT_EQ_STR(err.msg, "pattern too long (max 64 KiB)");
        free(huge);
    }
    arena_free_all(&arena);
}

void test_re_inst_size_budget(void)
{
    /* DoD 11.  The program is walked linearly per input codepoint, so
     * instruction size is working-set size. */
    SAG_ASSERT(sizeof(ReInst) <= 16U);
}

void test_re_error_offsets_are_always_in_range(void)
{
    /* A caret rendered past the end of the prompt is worse than no
     * caret; fuzzing covers this broadly, this pins the contract. */
    static const char *const bad[] = {
        "[", "(", ")", "*", "a{", "a{2,1}", "\\", "\\x", "\\x{",
        "[[:", "(?", "(?i", "a**", "[a-", "\\q", "(((", "[]"
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(bad); i++) {
        Arena arena;
        SagReErr err;

        arena_init(&arena);
        (void)memset(&err, 0, sizeof(err));
        err.off = 0xFFFFFFFFU;
        if (sag_re_compile(&arena, bad[i], strlen(bad[i]), 0U, &err) ==
            NULL) {
            SAG_ASSERT_NOT_NULL(err.msg);
            SAG_ASSERT(err.off <= (u32)strlen(bad[i]));
        }
        arena_free_all(&arena);
    }
}
