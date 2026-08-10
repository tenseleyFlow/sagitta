#ifndef YEW_UTIL_VEC_H
#define YEW_UTIL_VEC_H

#include <stddef.h>
#include <stdlib.h>

#include "util/base.h"
#include "util/log.h"

/* Declare a zero-initializable, type-safe dynamic array. */
#define VEC_DECL(Name, T)                                                      \
    typedef struct {                                                           \
        T *data;                                                               \
        size_t len;                                                            \
        size_t cap;                                                            \
    } Name;                                                                    \
    static inline void Name##_reserve(Name *vec, size_t need)                  \
    {                                                                          \
        size_t cap;                                                            \
        if (vec->cap >= need)                                                  \
            return;                                                            \
        cap = vec->cap ? vec->cap : 8;                                         \
        while (cap < need) {                                                   \
            if (cap > SIZE_MAX / 2) {                                          \
                cap = need;                                                    \
                break;                                                         \
            }                                                                  \
            cap *= 2;                                                          \
        }                                                                      \
        vec->data = yew_xreallocarray(vec->data, cap, sizeof(T));              \
        vec->cap = cap;                                                        \
    }                                                                          \
    static inline void Name##_push(Name *vec, T value)                         \
    {                                                                          \
        if (vec->len == SIZE_MAX)                                              \
            YEW_BUG("vector length overflow");                                \
        Name##_reserve(vec, vec->len + 1);                                     \
        vec->data[vec->len++] = value;                                         \
    }                                                                          \
    static inline void Name##_free(Name *vec)                                  \
    {                                                                          \
        free(vec->data);                                                       \
        vec->data = NULL;                                                      \
        vec->len = 0;                                                          \
        vec->cap = 0;                                                          \
    }                                                                          \
    typedef int Name##_require_semicolon

#endif
