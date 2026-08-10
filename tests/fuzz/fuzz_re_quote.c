/*
 * Sprint 21 DoD 13: the yew_re_quote round-trip law.
 *
 * For any literal s, compiling yew_re_quote(s) must produce a pattern
 * that matches s -- and matches exactly s, starting at 0 and ending at
 * the end.  "Nothing shorter" is the half that catches a metacharacter
 * escaping into syntax: if a `+` in the middle of s survived unquoted,
 * the pattern would still compile and would still match, just not the
 * whole string, and `*` on a word containing punctuation would silently
 * search for the wrong thing.
 *
 * The fuzz input is used as the literal, not as a pattern, so every
 * byte sequence the editor could ever pull out of a buffer -- invalid
 * UTF-8, embedded NULs, control characters -- is a test case.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search/regex.h"
#include "util/arena.h"
#include "util/buf.h"

static bool quote_round_trip(const u8 *data, size_t len,
                             char *why, size_t why_cap)
{
    Arena arena;
    Bytebuf pat;
    YewReErr err;
    YewRe *re;
    bool ok = false;

    bytebuf_init(&pat);
    yew_re_quote(&pat, data, len);
    arena_init(&arena);
    (void)memset(&err, 0, sizeof(err));
    re = yew_re_compile(&arena, (const char *)pat.data, pat.len, 0U, &err);
    if (re == NULL) {
        (void)snprintf(why, why_cap,
                       "quoted literal did not compile at %u: %s",
                       (unsigned)err.off,
                       err.msg != NULL ? err.msg : "(no message)");
        goto done;
    }
    {
        YewReInput in = yew_re_input_bytes(data, (u64)len);
        YewReMatch m;

        (void)memset(&m, 0, sizeof(m));
        if (!yew_re_search(re, &in, BYTEOFF(0U), &m)) {
            (void)snprintf(why, why_cap,
                           "quoted literal does not match its own text");
            goto done;
        }
        if (m.g[0].lo != 0U || m.g[0].hi != (u64)len) {
            (void)snprintf(why, why_cap,
                           "quoted literal matched %llu..%llu of %zu -- a "
                           "metacharacter survived quoting",
                           (unsigned long long)m.g[0].lo,
                           (unsigned long long)m.g[0].hi, len);
            goto done;
        }
    }
    /*
     * Anchoring both ends is not enough on its own: a pattern that
     * matched anything of the right length would pass.  Perturbing one
     * byte must break the match, which is what makes this a round trip
     * rather than a length check.
     */
    if (len > 0U) {
        u8 *bent = malloc(len);
        YewReInput in;
        YewReMatch m;

        if (bent == NULL) {
            (void)snprintf(why, why_cap, "out of memory");
            goto done;
        }
        (void)memcpy(bent, data, len);
        /* Flipping the low bit changes a byte without ever creating or
         * destroying a UTF-8 lead, so the perturbed string stays the
         * same length in bytes. */
        bent[len / 2U] = (u8)(bent[len / 2U] ^ 0x01U);
        in = yew_re_input_bytes(bent, (u64)len);
        (void)memset(&m, 0, sizeof(m));
        if (yew_re_search(re, &in, BYTEOFF(0U), &m) &&
            m.g[0].lo == 0U && m.g[0].hi == (u64)len) {
            (void)snprintf(why, why_cap,
                           "quoted literal also matches a perturbed "
                           "string -- it compiled to a wildcard");
            free(bent);
            goto done;
        }
        free(bent);
    }
    ok = true;
done:
    arena_free_all(&arena);
    bytebuf_free(&pat);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_re_quote", NULL,
                         quote_round_trip);
}
