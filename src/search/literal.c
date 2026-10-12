/* Sprint 20 §7: the literal prefilter. */
#include "search/regex_internal.h"

#include <string.h>

void yew_lit_prepare(ReLit *l, Arena *a, const u8 *s, u32 n)
{
    u32 i;
    u32 prefix = 0U;

    if (l == NULL || a == NULL || s == NULL || n == 0U)
        return;
    l->n = n;
    if (n <= sizeof(l->small)) {
        (void)memcpy(l->small, s, n);
        l->s = l->small;
    } else {
        u8 *copy = arena_alloc(a, n, 1U);

        (void)memcpy(copy, s, n);
        l->s = copy;
        l->kmp = arena_alloc(a, (size_t)n * sizeof(*l->kmp),
                             _Alignof(u32));
        l->kmp[0] = 0U;
        for (i = 1U; i < n; i++) {
            while (prefix != 0U && l->s[prefix] != l->s[i])
                prefix = l->kmp[prefix - 1U];
            if (l->s[prefix] == l->s[i])
                prefix++;
            l->kmp[i] = prefix;
        }
    }
    for (i = 0U; i < 256U; i++)
        l->skip[i] = n;
    /* Last byte excluded: the classic Horspool table. */
    for (i = 0U; i + 1U < n; i++)
        l->skip[l->s[i]] = n - 1U - i;
}

u64 yew_lit_find(const ReLit *l, const u8 *hay, u64 n)
{
    const u8 *probe;
    const u8 *end;

    if (l->kind == RE_LIT_NONE || l->n == 0U || n < l->n)
        return UINT64_MAX;
    if (l->n == 1U) {
        /* libc's memchr is SIMD-optimized and is the fastest thing we
         * are allowed to call — the stdlib is not a dependency. */
        const u8 *hit = memchr(hay, l->s[0], (size_t)n);

        return hit == NULL ? UINT64_MAX : (u64)(hit - hay);
    }
    /* libc's memchr is normally vectorized.  Searching for the required
     * last byte first keeps the exact binary semantics of Horspool while
     * avoiding its scalar byte-at-a-time candidate loop on large editor
     * buffers.  The full memcmp remains the authority at each candidate. */
    probe = hay + l->n - 1U;
    end = hay + n;
    while (probe < end) {
        const u8 *hit = memchr(probe, l->s[l->n - 1U],
                               (size_t)(end - probe));
        const u8 *candidate;

        if (hit == NULL)
            return UINT64_MAX;
        candidate = hit - (l->n - 1U);
        if (memcmp(candidate, l->s, (size_t)l->n) == 0)
            return (u64)(candidate - hay);
        probe = hit + 1U;
    }
    return UINT64_MAX;
}
