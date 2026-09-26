#include "harness.h"

#include "util/intern.h"

#include <stdio.h>

void test_intern_roundtrip(void)
{
    Arena arena;
    Interner interner;
    u32 alpha;
    u32 beta;

    arena_init(&arena);
    interner_init(&interner, &arena);
    alpha = yew_intern(&interner, "alpha", 5U);
    beta = yew_intern_cstr(&interner, "beta");
    YEW_ASSERT_EQ_STR(yew_intern_str(&interner, alpha), "alpha");
    YEW_ASSERT_EQ_STR(yew_intern_str(&interner, beta), "beta");
    YEW_ASSERT_NULL(yew_intern_str(&interner, 0U));
    YEW_ASSERT_NULL(yew_intern_str(&interner, 99U));
    interner_free(&interner);
    arena_free_all(&arena);
}

void test_intern_id_stability(void)
{
    Arena arena;
    Interner interner;
    u32 alpha;
    u32 beta;

    arena_init(&arena);
    interner_init(&interner, &arena);
    alpha = yew_intern_cstr(&interner, "alpha");
    beta = yew_intern_cstr(&interner, "beta");
    YEW_ASSERT_EQ_U64(alpha, 1U);
    YEW_ASSERT_EQ_U64(beta, 2U);
    YEW_ASSERT_EQ_U64(yew_intern_cstr(&interner, "alpha"), alpha);
    YEW_ASSERT_EQ_U64(yew_intern_count(&interner), 2U);
    interner_free(&interner);
    arena_free_all(&arena);
}

/* interner_insert_cost covers the tables, the lookup map, and the arena
 * copy: the predicted cost equals the measured growth of all three. */
void test_intern_insert_cost_predicts_resident_growth(void)
{
    Arena arena;
    Interner in;
    size_t i;

    arena_init(&arena);
    interner_init(&in, &arena);
    for (i = 0U; i < 5000U; i++) {
        char name[32];
        int n = snprintf(name, sizeof(name), "interned_%zu", i);
        u64 before = interner_resident_bytes(&in) +
                     arena_resident_bytes(&arena);
        u64 cost = interner_insert_cost(&in, (size_t)n);

        YEW_ASSERT(n > 0);
        (void)yew_intern(&in, name, (size_t)n);
        YEW_ASSERT_EQ_U64(interner_resident_bytes(&in) +
                              arena_resident_bytes(&arena) - before,
                          cost);
        /* A second intern of the same string allocates nothing. */
        before = interner_resident_bytes(&in) + arena_resident_bytes(&arena);
        (void)yew_intern(&in, name, (size_t)n);
        YEW_ASSERT_EQ_U64(interner_resident_bytes(&in) +
                              arena_resident_bytes(&arena),
                          before);
    }
    interner_free(&in);
    arena_free_all(&arena);
}
