/* Sprint 20 §7: the literal prefilter. */
#include "search/regex_internal.h"

#include <string.h>

void yew_bmh_build(ReLit *l)
{
    u32 i;

    if (l->kind == RE_LIT_NONE || l->n == 0U)
        return;
    for (i = 0U; i < 256U; i++)
        l->skip[i] = l->n;
    /* Last byte excluded: the classic Horspool table. */
    for (i = 0U; i + 1U < l->n; i++)
        l->skip[l->s[i]] = l->n - 1U - i;
}

u64 yew_lit_find(const ReLit *l, const u8 *hay, u64 n)
{
    u64 at = 0U;

    if (l->kind == RE_LIT_NONE || l->n == 0U || n < l->n)
        return UINT64_MAX;
    if (l->n == 1U) {
        /* libc's memchr is SIMD-optimized and is the fastest thing we
         * are allowed to call — the stdlib is not a dependency. */
        const u8 *hit = memchr(hay, l->s[0], (size_t)n);

        return hit == NULL ? UINT64_MAX : (u64)(hit - hay);
    }
    while (at + l->n <= n) {
        if (hay[at + l->n - 1U] == l->s[l->n - 1U] &&
            memcmp(hay + at, l->s, (size_t)l->n) == 0)
            return at;
        at += l->skip[hay[at + l->n - 1U]];
    }
    return UINT64_MAX;
}
