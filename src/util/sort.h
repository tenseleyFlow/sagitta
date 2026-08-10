#ifndef YEW_UTIL_SORT_H
#define YEW_UTIL_SORT_H

#include <stddef.h>

void yew_sort_stable(void *base, size_t n, size_t elem_size,
                     int (*cmp)(const void *a, const void *b, void *ctx),
                     void *ctx);

#endif
