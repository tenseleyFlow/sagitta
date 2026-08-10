#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 26 §4 fuzz: the .gitignore compiler and matcher.
 *
 * WHAT IS BEING DEFENDED.  An ignore file is user-authored text that
 * reaches a pattern matcher, and the matcher runs against every path in
 * a 100 000-file walk.  Two failure modes matter and neither announces
 * itself:
 *
 *   THE BACKTRACKER LOOPS.  glob_match backtracks iteratively over `*`
 *   and `**`.  A pattern like `a**b**c` against a long path is exactly
 *   the shape that makes a naive backtracker take exponential time or
 *   never terminate — and a finder that hangs on one repository's
 *   .gitignore is indistinguishable from a crash, except nobody gets a
 *   core file.
 *
 *   IT READS PAST THE END.  Bracket sets scan forward for `]`, escapes
 *   consume the next byte, and `**` peeks at the one after.  Every one
 *   of those is an off-by-one waiting to happen on a truncated pattern,
 *   and ASan is how we find out.
 *
 * So: random pattern bytes, random path bytes, and a deliberate
 * adversarial set of shapes chosen to be hard rather than random.  The
 * check is not "does it match correctly" — that is test_gitignore.c's
 * job against known answers — it is "does it ANSWER at all, bounded,
 * without reading memory it does not own".
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/arena.h"
#include "ws/gitignore.h"

typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(r->s >> 33);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

static i64 now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

/*
 * The bytes a real ignore file is made of, weighted toward the
 * metacharacters.  Uniform random bytes would spend almost all their
 * time on patterns that are one literal run and never reach the
 * backtracker at all.
 */
static const char gi_alphabet[] = "*?[]!^-/\\.abcXYZ09 \t#";

static void random_pattern(Rng *r, char *out, size_t cap)
{
    u32 len = rng_below(r, (u32)cap - 1U);
    u32 i;

    for (i = 0U; i < len; i++)
        out[i] = gi_alphabet[rng_below(r, (u32)sizeof(gi_alphabet) - 1U)];
    out[len] = '\0';
}

static void random_path(Rng *r, char *out, size_t cap)
{
    static const char path_alphabet[] = "abcXYZ09/._-";
    u32 len = 1U + rng_below(r, (u32)cap - 2U);
    u32 i;

    for (i = 0U; i < len; i++)
        out[i] =
            path_alphabet[rng_below(r, (u32)sizeof(path_alphabet) - 1U)];
    out[len] = '\0';
}

/*
 * Shapes chosen to be HARD, not random.
 *
 * Every one of these is a known way to make a glob matcher misbehave:
 * nested stars that each need backtracking, alternating star and
 * literal so no prefix ever settles, unterminated brackets, escapes at
 * the very end of the pattern where the next byte does not exist.
 */
static const char *const gi_adversarial[] = {
    "a**b**c**d**e",
    "*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*",
    "**/**/**/**/**",
    "[",
    "[]",
    "[!",
    "[a-",
    "[[[[[[[[",
    "]]]]]]]]",
    "\\",
    "a\\",
    "*\\",
    "[\\",
    "**",
    "/",
    "//",
    "///",
    "!",
    "!!",
    "!/",
    "*/",
    "/*",
    "a/**/b/**/c",
    "?????????????????????????",
    "*?*?*?*?*?*?*?*?*?*?",
    "[a-z][a-z][a-z][a-z][a-z][a-z][a-z][a-z]",
    "\\**\\**\\**",
    "#notacomment",
    "\\#escaped",
    "trailing   ",
    "trailing\\ ",
    "a[b-",
    "[^]",
    "[]]",
    "**a**"
};

/* One pattern against one path, bounded in time. */
static bool check_one(Arena *a, const char *pattern, const char *path,
                      char *why, size_t why_cap)
{
    GiSet *g;
    i64 start;
    i64 elapsed;
    char text[1200];

    (void)snprintf(text, sizeof(text), "%s\n", pattern);
    g = yew_gi_compile(a, NULL, text, (u64)strlen(text), NULL);
    /* A pattern that compiles to nothing is a legitimate answer — a
     * blank line, a comment, a lone `!`. */
    if (g == NULL)
        return true;
    start = now_ms();
    (void)yew_gi_match(g, path, false);
    (void)yew_gi_match(g, path, true);
    (void)yew_gi_prunable(g, path);
    (void)yew_gi_negated(g, path);
    elapsed = now_ms() - start;
    /*
     * 250 ms for four calls against one path is enormous — the real
     * numbers are nanoseconds.  Anything near this is a backtracker
     * that has gone exponential, which is the failure this bound
     * exists to catch before it becomes a hang in the field.
     */
    if (elapsed > 250) {
        (void)snprintf(why, why_cap,
                       "pattern '%s' took %lld ms against a %u-byte path",
                       pattern, (long long)elapsed,
                       (unsigned)strlen(path));
        return false;
    }
    return true;
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Arena a;
    Rng rng;
    char pattern[128];
    char path[512];
    size_t k;
    u32 i;
    bool ok = true;

    rng.s = 0x9E3779B97F4A7C15ULL;
    for (k = 0U; k < len; k++)
        rng.s = rng.s * 31U + data[k];
    arena_init(&a);

    /* The case bytes themselves, as both pattern and path: random input
     * is its own test and reaches the tokenizer's error paths. */
    {
        size_t n = len < sizeof(pattern) - 1U ? len : sizeof(pattern) - 1U;

        (void)memcpy(pattern, data, n);
        pattern[n] = '\0';
        /* NULs and newlines would end the pattern early; they are worth
         * testing but not worth spending the whole budget on. */
        for (k = 0U; k < n; k++) {
            if (pattern[k] == '\0' || pattern[k] == '\n')
                pattern[k] = 'a';
        }
        random_path(&rng, path, sizeof(path));
        ok = check_one(&a, pattern, path, why, why_cap);
    }

    /* The adversarial shapes, each against a long path — a short path
     * cannot make a backtracker work hard. */
    for (i = 0U; ok && i < YEW_ARRAY_LEN(gi_adversarial); i++) {
        static const char long_path[] =
            "aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddddddddd/eeeeeeeeee/"
            "ffffffffff/gggggggggg/hhhhhhhhhh/iiiiiiiiii/jjjjjjjjjj/"
            "kkkkkkkkkk/llllllllll/mmmmmmmmmm/nnnnnnnnnn/oooooooooo.c";

        ok = check_one(&a, gi_adversarial[i], long_path, why, why_cap);
        if (ok) {
            random_path(&rng, path, sizeof(path));
            ok = check_one(&a, gi_adversarial[i], path, why, why_cap);
        }
    }

    /* And a round of purely random patterns from the weighted
     * alphabet. */
    for (i = 0U; ok && i < 64U; i++) {
        random_pattern(&rng, pattern, sizeof(pattern));
        random_path(&rng, path, sizeof(path));
        ok = check_one(&a, pattern, path, why, why_cap);
    }

    /*
     * A multi-rule file, since precedence and the negation scan walk
     * every rule: a set whose rules individually terminate could still
     * be pathological as a chain.
     */
    if (ok) {
        char text[4096];
        size_t at = 0U;
        GiSet *g;

        for (i = 0U; i < 32U && at + 130U < sizeof(text); i++) {
            random_pattern(&rng, pattern, sizeof(pattern));
            at += (size_t)snprintf(text + at, sizeof(text) - at, "%s\n",
                                   pattern);
        }
        g = yew_gi_compile(&a, NULL, text, (u64)at, NULL);
        if (g != NULL) {
            i64 start = now_ms();

            for (i = 0U; i < 32U; i++) {
                random_path(&rng, path, sizeof(path));
                (void)yew_gi_match(g, path, (i & 1U) != 0U);
                (void)yew_gi_prunable(g, path);
            }
            if (now_ms() - start > 1000) {
                (void)snprintf(why, why_cap,
                               "a 32-rule set took %lld ms over 32 paths",
                               (long long)(now_ms() - start));
                ok = false;
            }
        }
    }

    arena_free_all(&a);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_gitignore", NULL, run_session);
}
