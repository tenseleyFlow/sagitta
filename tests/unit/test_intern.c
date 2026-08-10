#include "harness.h"

#include "util/intern.h"

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
