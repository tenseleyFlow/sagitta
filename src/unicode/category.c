#include "unicode/category.h"

/* Binary search over the sparse high-plane ranges; the two-level trie
 * covers everything below YEW_CAT_TRIE_HI. */
u16 yew_cat_hi_lookup(u32 cp)
{
    size_t lo = 0U;
    size_t hi = yew_cat_hi_len;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (cp < yew_cat_hi[mid].lo)
            hi = mid;
        else if (cp > yew_cat_hi[mid].hi)
            lo = mid + 1U;
        else
            return yew_cat_hi[mid].rec;
    }
    return 0U;
}
