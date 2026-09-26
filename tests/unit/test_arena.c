#include "harness.h"

#include "util/arena.h"

#include <stdint.h>
#include <stdalign.h>

typedef struct {
    alignas(16) unsigned char bytes[16];
} Aligned16;

void test_arena_align(void)
{
    Arena arena;
    void *first;
    Aligned16 *aligned;

    arena_init(&arena);
    first = arena_alloc(&arena, 1U, 1U);
    aligned = arena_alloc(&arena, sizeof(*aligned), alignof(Aligned16));
    YEW_ASSERT_NOT_NULL(first);
    YEW_ASSERT_NOT_NULL(aligned);
    YEW_ASSERT_EQ_U64((uintptr_t)aligned % 16U, 0U);
    arena_free_all(&arena);
    YEW_ASSERT_NULL(arena.head);
}

void test_arena_strdup(void)
{
    Arena arena;
    char *whole;
    char *prefix;

    arena_init(&arena);
    whole = arena_strdup(&arena, "arena-owned");
    prefix = arena_strndup(&arena, "prefix-tail", 6U);
    YEW_ASSERT_EQ_STR(whole, "arena-owned");
    YEW_ASSERT_EQ_STR(prefix, "prefix");
    YEW_ASSERT(whole != prefix);
    arena_free_all(&arena);
}

/* arena_alloc_cost is zero while the head block has room and otherwise the
 * exact resident cost of the block the allocation opens. */
void test_arena_alloc_cost_predicts_resident_growth(void)
{
    Arena arena;
    size_t i;

    arena_init(&arena);
    YEW_ASSERT_EQ_U64(arena_resident_bytes(&arena), 0U);
    for (i = 0U; i < 4000U; i++) {
        size_t size = 1U + (i * 37U) % 900U;
        size_t align = (size_t)1U << (i % 4U);
        u64 before = arena_resident_bytes(&arena);
        u64 cost = arena_alloc_cost(&arena, size, align);

        YEW_ASSERT_NOT_NULL(arena_alloc(&arena, size, align));
        YEW_ASSERT_EQ_U64(arena_resident_bytes(&arena) - before, cost);
    }
    /* One allocation larger than the next block still gets its own. */
    {
        u64 before = arena_resident_bytes(&arena);
        u64 cost = arena_alloc_cost(&arena, 3U * 1024U * 1024U, 8U);

        YEW_ASSERT(cost >= 3U * 1024U * 1024U);
        YEW_ASSERT_NOT_NULL(arena_alloc(&arena, 3U * 1024U * 1024U, 8U));
        YEW_ASSERT_EQ_U64(arena_resident_bytes(&arena) - before, cost);
    }
    arena_free_all(&arena);
    YEW_ASSERT_EQ_U64(arena_resident_bytes(&arena), 0U);
}
