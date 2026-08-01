#include "util/sort.h"

#include <stdlib.h>
#include <string.h>

#include "util/base.h"
#include "util/log.h"

static void merge_runs(unsigned char *dst, const unsigned char *src,
                       size_t lo, size_t mid, size_t hi, size_t elem_size,
                       int (*cmp)(const void *, const void *, void *),
                       void *ctx)
{
    size_t left = lo;
    size_t right = mid;
    size_t out = lo;

    while (left < mid && right < hi) {
        if (cmp(src + right * elem_size, src + left * elem_size, ctx) < 0) {
            memcpy(dst + out * elem_size, src + right * elem_size, elem_size);
            right++;
        } else {
            memcpy(dst + out * elem_size, src + left * elem_size, elem_size);
            left++;
        }
        out++;
    }
    if (left < mid) {
        memcpy(dst + out * elem_size, src + left * elem_size,
               (mid - left) * elem_size);
    } else if (right < hi) {
        memcpy(dst + out * elem_size, src + right * elem_size,
               (hi - right) * elem_size);
    }
}

void sag_sort_stable(void *base, size_t n, size_t elem_size,
                     int (*cmp)(const void *, const void *, void *), void *ctx)
{
    unsigned char *items = base;
    unsigned char *scratch;
    size_t width;

    if (n < 2 || elem_size == 0)
        return;
    if (!cmp)
        SAG_BUG("sag_sort_stable: NULL comparator");
    scratch = sag_xreallocarray(NULL, n, elem_size);

    width = 1;
    for (;;) {
        size_t lo = 0;

        while (lo < n) {
            size_t mid = n - lo < width ? n : lo + width;
            size_t remaining = n - mid;
            size_t hi = remaining < width ? n : mid + width;

            merge_runs(scratch, items, lo, mid, hi, elem_size, cmp, ctx);
            lo = hi;
        }
        memcpy(items, scratch, n * elem_size);
        if (width >= n - width)
            break;
        width *= 2;
    }
    free(scratch);
}
