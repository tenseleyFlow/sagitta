/*
 * Sprint 20 DoD 7: sag_re_compile over arbitrary bytes.
 *
 * The contract is narrow and total: for ANY input the compiler either
 * returns a program or returns NULL with an error whose offset points
 * inside the pattern.  It must never crash, never loop, and never report
 * a caret position past the end of what the user typed — Sprint 21 draws
 * that caret under a live prompt, so an out-of-range offset is a visible
 * corruption rather than a quiet one.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "util/arena.h"

static bool compile_one(const u8 *data, size_t len, u32 flags,
                        char *why, size_t why_cap)
{
    Arena arena;
    SagReErr err;
    SagRe *re;

    arena_init(&arena);
    (void)memset(&err, 0, sizeof(err));
    /* A sentinel the compiler must overwrite on failure. */
    err.off = 0xFFFFFFFFU;
    re = sag_re_compile(&arena, (const char *)data, len, flags, &err);
    if (re == NULL) {
        if (err.msg == NULL) {
            (void)snprintf(why, why_cap,
                           "compile failed with no message (flags %u)",
                           (unsigned)flags);
            arena_free_all(&arena);
            return false;
        }
        if ((size_t)err.off > len) {
            (void)snprintf(why, why_cap,
                           "error offset %u exceeds pattern length %zu "
                           "(flags %u): %s",
                           (unsigned)err.off, len, (unsigned)flags,
                           err.msg);
            arena_free_all(&arena);
            return false;
        }
        arena_free_all(&arena);
        return true;
    }
    /* A successful compile must produce a usable program: group 0 always
     * exists, and searching must terminate on a small input. */
    if (sag_re_group_count(re) == 0U ||
        sag_re_group_count(re) > SAG_RE_MAX_GROUPS) {
        (void)snprintf(why, why_cap, "compiled with %u groups",
                       (unsigned)sag_re_group_count(re));
        arena_free_all(&arena);
        return false;
    }
    {
        static const u8 probe[] = "abc 123 \xE6\xBC\xA2\xF0\x9F\x98\x80";
        SagReInput in = sag_re_input_bytes(probe, sizeof(probe) - 1U);

        /* Running it is part of the contract: a program that compiles
         * but hangs the matcher is not a success. */
        (void)sag_re_search(re, &in, BYTEOFF(0U), NULL);
        (void)sag_re_test(re, &in, BYTEOFF(0U));
    }
    arena_free_all(&arena);
    return true;
}

static bool check_re_compile(const u8 *data, size_t len,
                             char *why, size_t why_cap)
{
    static const u32 flag_sets[] = {
        0U,
        SAG_RE_ICASE,
        SAG_RE_DOTALL,
        SAG_RE_LITERAL,
        SAG_RE_ICASE | SAG_RE_DOTALL
    };
    size_t i;

    /* Every flag combination is a different parse path — ICASE folds
     * literals into classes, LITERAL bypasses the grammar entirely. */
    for (i = 0U; i < SAG_ARRAY_LEN(flag_sets); i++) {
        if (!compile_one(data, len, flag_sets[i], why, why_cap))
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_re_compile", NULL,
                         check_re_compile);
}
