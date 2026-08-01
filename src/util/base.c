#include "util/base.h"

#include <stdint.h>
#include <stdlib.h>

#include "util/log.h"

void *sag_xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);

    if (!ptr)
        SAG_BUG("out of memory allocating %zu bytes", size);
    return ptr;
}

void *sag_xrealloc(void *ptr, size_t size)
{
    void *resized = realloc(ptr, size ? size : 1);

    if (!resized)
        SAG_BUG("out of memory reallocating to %zu bytes", size);
    return resized;
}

void *sag_xcalloc(size_t count, size_t size)
{
    void *ptr;

    if (count == 0 || size == 0) {
        ptr = calloc(1, 1);
        if (!ptr)
            SAG_BUG("out of memory allocating zero bytes");
        return ptr;
    }
    if (size != 0 && count > SIZE_MAX / size)
        SAG_BUG("allocation size overflow: %zu * %zu", count, size);
    ptr = calloc(count, size);
    if (!ptr)
        SAG_BUG("out of memory allocating %zu elements of %zu bytes", count,
                size);
    return ptr;
}

void *sag_xreallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
        SAG_BUG("allocation size overflow: %zu * %zu", count, size);
    return sag_xrealloc(ptr, count * size);
}
