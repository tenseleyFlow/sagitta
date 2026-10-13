/*
 * YEW-F-083 — backward regex search skips overlapping starts.
 *
 * Correct behavior: yew_re_search_back returns the last match whose start is
 * strictly before the supplied offset.  For /aa/ in "aaa" before 3, that is
 * [1,3), even though it overlaps the earlier [0,2) match.
 *
 * Baseline failure: the forward scan inside each backward window advances to
 * the previous match end and therefore never considers start offset 1.
 */
#include "audit.h"

#include <stdio.h>

#include "search/regex.h"
#include "util/arena.h"

bool test_yew_f_083(char *why, size_t why_cap)
{
    static const u8 subject[] = {'a', 'a', 'a'};
    Arena arena;
    YewReErr err = {0};
    YewReMatch match;
    YewReInput input = yew_re_input_bytes(subject, sizeof(subject));
    YewRe *re;
    bool passed;

    arena_init(&arena);
    re = yew_re_compile(&arena, "aa", 2U, 0U, &err);
    passed = re != NULL &&
             yew_re_search_back(re, &input, BYTEOFF(sizeof(subject)),
                                &match) &&
             match.g[0].lo == 1U && match.g[0].hi == 3U;
    if (!passed)
        (void)snprintf(why, why_cap,
                       "backward /aa/ over aaa returned %llu..%llu",
                       (unsigned long long)match.g[0].lo,
                       (unsigned long long)match.g[0].hi);
    arena_free_all(&arena);
    return passed;
}
