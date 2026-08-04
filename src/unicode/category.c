#include "unicode/category.h"

/* Binary search over the sparse high-plane ranges; the two-level trie
 * covers everything below SAG_CAT_TRIE_HI. */
u16 sag_cat_hi_lookup(u32 cp)
{
    size_t lo = 0U;
    size_t hi = sag_cat_hi_len;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (cp < sag_cat_hi[mid].lo)
            hi = mid;
        else if (cp > sag_cat_hi[mid].hi)
            lo = mid + 1U;
        else
            return sag_cat_hi[mid].rec;
    }
    return 0U;
}
