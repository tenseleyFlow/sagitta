/*
 * YEW-F-081 — long quoted literals exceed the smaller VM-program limit.
 *
 * Correct behavior: yew_re_quote() output within the documented pattern
 * limit compiles and matches the source bytes exactly, even when the literal
 * itself would require more than YEW_RE_MAX_PROG Thompson instructions.
 *
 * Baseline failure: 4,240 ordinary bytes quote to 4,240 literal atoms.  The
 * compiler emits one RE_CHAR per atom plus capture bookkeeping, crosses the
 * 4,096-instruction cap, and rejects the safe literal as "pattern too
 * complex".  Scheduled run 38281035643 found the same length and failure;
 * its minimized artifact has SHA-256
 * ce12bafa65771eef818550609e72f585e52b004b92c6f4c41e2c9567baddf0e1.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "util/arena.h"
#include "util/buf.h"

bool test_yew_f_081(char *why, size_t why_cap)
{
    enum { LITERAL_LEN = 4240 };
    u8 literal[LITERAL_LEN];
    Arena arena;
    Bytebuf pattern;
    YewReErr err = {0};
    YewReMatch match;
    YewReInput input;
    YewRe *re;
    bool passed = false;

    (void)memset(literal, 'q', sizeof(literal));
    bytebuf_init(&pattern);
    yew_re_quote(&pattern, literal, sizeof(literal));
    arena_init(&arena);
    re = yew_re_compile(&arena, (const char *)pattern.data, pattern.len,
                        0U, &err);
    if (re == NULL) {
        (void)snprintf(why, why_cap, "compile failed at %u: %s",
                       (unsigned)err.off,
                       err.msg != NULL ? err.msg : "(no message)");
        goto done;
    }
    input = yew_re_input_bytes(literal, sizeof(literal));
    (void)memset(&match, 0, sizeof(match));
    if (!yew_re_search(re, &input, BYTEOFF(0U), &match) ||
        match.ngroups != 1U || match.g[0].lo != 0U ||
        match.g[0].hi != sizeof(literal)) {
        (void)snprintf(why, why_cap,
                       "quoted literal did not match exactly");
        goto done;
    }
    literal[LITERAL_LEN / 2U] = 'r';
    input = yew_re_input_bytes(literal, sizeof(literal));
    if (yew_re_search(re, &input, BYTEOFF(0U), &match)) {
        (void)snprintf(why, why_cap,
                       "quoted literal matched perturbed bytes");
        goto done;
    }
    passed = true;
done:
    arena_free_all(&arena);
    bytebuf_free(&pattern);
    return passed;
}
