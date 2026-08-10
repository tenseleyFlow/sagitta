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
