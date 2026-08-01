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
    alpha = sag_intern(&interner, "alpha", 5U);
    beta = sag_intern_cstr(&interner, "beta");
    SAG_ASSERT_EQ_STR(sag_intern_str(&interner, alpha), "alpha");
    SAG_ASSERT_EQ_STR(sag_intern_str(&interner, beta), "beta");
    SAG_ASSERT_NULL(sag_intern_str(&interner, 0U));
    SAG_ASSERT_NULL(sag_intern_str(&interner, 99U));
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
    alpha = sag_intern_cstr(&interner, "alpha");
    beta = sag_intern_cstr(&interner, "beta");
    SAG_ASSERT_EQ_U64(alpha, 1U);
    SAG_ASSERT_EQ_U64(beta, 2U);
    SAG_ASSERT_EQ_U64(sag_intern_cstr(&interner, "alpha"), alpha);
    SAG_ASSERT_EQ_U64(sag_intern_count(&interner), 2U);
    interner_free(&interner);
    arena_free_all(&arena);
}
