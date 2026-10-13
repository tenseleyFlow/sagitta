/*
 * YEW-F-082 — direct whole-literal matching can split a UTF-8 codepoint.
 *
 * Correct behavior: a quoted invalid byte is one surrogate-escape
 * codepoint.  It must not match the lead byte of a valid UTF-8 sequence,
 * because a replacement using that span would leave an orphan continuation.
 *
 * Baseline failure: the oversized-literal route compares bytes and checks
 * only the candidate's start.  The final raw C2 below therefore consumes the
 * first byte of the valid C2 A2 encoding and reports [0, 4095).
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "util/arena.h"
#include "util/buf.h"

bool test_yew_f_082(char *why, size_t why_cap)
{
    enum { PREFIX = 4094, PATTERN_BYTES = PREFIX + 1,
           SUBJECT_BYTES = PREFIX + 2 };
    u8 literal[PATTERN_BYTES];
    u8 subject[SUBJECT_BYTES];
    Arena arena;
    Bytebuf pattern;
    YewReErr err = {0};
    YewReMatch match;
    YewReInput input;
    YewRe *re;
    bool passed = false;

    (void)memset(literal, 'a', PREFIX);
    literal[PREFIX] = 0xC2U;
    (void)memset(subject, 'a', PREFIX);
    subject[PREFIX] = 0xC2U;
    subject[PREFIX + 1U] = 0xA2U;
    bytebuf_init(&pattern);
    yew_re_quote(&pattern, literal, sizeof(literal));
    arena_init(&arena);
    re = yew_re_compile(&arena, (const char *)pattern.data, pattern.len,
                        0U, &err);
    if (re == NULL) {
        (void)snprintf(why, why_cap, "compile failed: %s",
                       err.msg != NULL ? err.msg : "(no message)");
        goto done;
    }
    input = yew_re_input_bytes(subject, sizeof(subject));
    if (yew_re_search(re, &input, BYTEOFF(0U), &match) ||
        yew_re_match_at(re, &input, BYTEOFF(0U), &match) ||
        yew_re_test(re, &input, BYTEOFF(0U))) {
        (void)snprintf(why, why_cap,
                       "invalid C2 matched inside valid C2 A2");
        goto done;
    }
    input = yew_re_input_bytes(literal, sizeof(literal));
    if (!yew_re_search(re, &input, BYTEOFF(0U), &match) ||
        match.g[0].lo != 0U || match.g[0].hi != sizeof(literal)) {
        (void)snprintf(why, why_cap,
                       "invalid C2 no longer matched itself");
        goto done;
    }
    passed = true;
done:
    arena_free_all(&arena);
    bytebuf_free(&pattern);
    return passed;
}
