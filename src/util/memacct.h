#ifndef YEW_UTIL_MEMACCT_H
#define YEW_UTIL_MEMACCT_H

/*
 * Resident-memory accounting for structures that promise a byte budget.
 *
 * A budget stated in payload bytes undercounts what an allocator actually
 * holds: every heap block carries a header and is rounded up, and every
 * growable array holds its unused capacity.  Owners that enforce a cap
 * charge heap blocks through yew_heap_cost() and arrays by CAPACITY, so the
 * number they compare against the cap tracks resident memory.
 *
 * The model is deliberately allocator-neutral and slightly pessimistic:
 * glibc's ptmalloc uses an 8-byte header with 16-byte granularity and a
 * 32-byte minimum chunk, and musl's mallocng a 4-byte in-band header with
 * 16-byte slots.  Charging a 16-byte header rounded to 16 covers both.
 */

#include "util/base.h"

enum {
    YEW_HEAP_CHUNK_OVERHEAD = 16,
    YEW_HEAP_CHUNK_ALIGN = 16
};

/* Resident bytes of one heap block holding `n` payload bytes. */
static inline u64 yew_heap_cost(u64 n)
{
    u64 total;

    if (n == 0U)
        return 0U;
    if (n > UINT64_MAX - (YEW_HEAP_CHUNK_OVERHEAD + YEW_HEAP_CHUNK_ALIGN))
        return UINT64_MAX;
    total = n + YEW_HEAP_CHUNK_OVERHEAD + (YEW_HEAP_CHUNK_ALIGN - 1U);
    return total & ~(u64)(YEW_HEAP_CHUNK_ALIGN - 1U);
}

/* Resident bytes of a heap array with `cap` elements of `size` bytes. */
static inline u64 yew_heap_array_cost(u64 cap, u64 size)
{
    if (size != 0U && cap > UINT64_MAX / size)
        return UINT64_MAX;
    return yew_heap_cost(cap * size);
}

/* The next capacity of a Vec (util/vec.h) that must hold one more element:
 * the growth policy is duplicated here so a cost can be predicted before
 * the push that would pay it. */
static inline u64 yew_vec_next_cap(u64 len, u64 cap)
{
    u64 next;

    if (len < cap)
        return cap;
    next = cap != 0U ? cap : 8U;
    while (next < len + 1U) {
        if (next > UINT64_MAX / 2U)
            return len + 1U;
        next *= 2U;
    }
    return next;
}

/* Resident growth of a Vec of `size`-byte elements pushing one more. */
static inline u64 yew_vec_push_cost(u64 len, u64 cap, u64 size)
{
    u64 next = yew_vec_next_cap(len, cap);

    if (next == cap)
        return 0U;
    return yew_heap_array_cost(next, size) - yew_heap_array_cost(cap, size);
}

static inline u64 yew_sat_add(u64 a, u64 b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

#endif
