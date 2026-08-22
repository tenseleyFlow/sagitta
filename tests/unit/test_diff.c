#include "harness.h"
#include "mod/git/gutter.h"
#include <string.h>

void test_git_line_hash_and_split(void)
{
    Arena a; u64 *h; u32 n; bool missing; u32 i;
    static const u8 text[] = "a\r\nb\nlast";
    arena_init(&a);
    YEW_ASSERT(yew_git_hash_lines(text, sizeof(text)-1U, &a, &h, &n, &missing));
    YEW_ASSERT_EQ_U64(n, 3U); YEW_ASSERT(missing);
    for (i=0; i<n; i++) YEW_ASSERT(h[i] != 0U);
    YEW_ASSERT(yew_git_line_hash((const u8*)"a",1) != yew_git_line_hash((const u8*)"b",1));
    arena_free_all(&a);
}

void test_git_diff_basic_shapes(void)
{
    Arena a; GitHunkVec v = {0}; u64 x[3]={1,2,3}, y[3]={1,4,3};
    arena_init(&a); YEW_ASSERT(yew_diff_lines(&a,x,3,y,3,4096,&v));
    YEW_ASSERT_EQ_U64(v.len,1U); YEW_ASSERT_EQ_U64(v.data[0].buf_lo.v,1U);
    YEW_ASSERT_EQ_U64(v.data[0].buf_n.v,1U); YEW_ASSERT(v.data[0].kind==YEW_HUNK_MOD);
    GitHunkVec_free(&v); arena_free_all(&a);
}
