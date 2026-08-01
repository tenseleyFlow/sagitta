#include "harness.h"

#include "util/vec.h"

VEC_DECL(VecU32, u32);

void test_vec_growth(void)
{
    VecU32 vec = {0};
    size_t i;

    for (i = 0U; i < 1024U; i++)
        VecU32_push(&vec, (u32)(i * 3U));
    SAG_ASSERT_EQ_U64(vec.len, 1024U);
    SAG_ASSERT(vec.cap >= vec.len);
    SAG_ASSERT_EQ_U64(vec.data[0], 0U);
    SAG_ASSERT_EQ_U64(vec.data[511], 1533U);
    SAG_ASSERT_EQ_U64(vec.data[1023], 3069U);
    VecU32_free(&vec);
}

void test_vec_free_resets(void)
{
    VecU32 vec = {0};

    VecU32_push(&vec, 7U);
    VecU32_free(&vec);
    SAG_ASSERT_NULL(vec.data);
    SAG_ASSERT_EQ_U64(vec.len, 0U);
    SAG_ASSERT_EQ_U64(vec.cap, 0U);
}
