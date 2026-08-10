#include "harness.h"

#include "util/sort.h"

typedef struct {
    int key;
    int original_pos;
} SortItem;

static int compare_sort_items(const void *lhs, const void *rhs, void *ctx)
{
    const SortItem *a = lhs;
    const SortItem *b = rhs;

    (void)ctx;
    return (a->key > b->key) - (a->key < b->key);
}

void test_sort_stable_ties(void)
{
    SortItem items[] = {
        {2, 0}, {1, 1}, {2, 2}, {1, 3}, {3, 4}, {2, 5}
    };

    yew_sort_stable(items, YEW_ARRAY_LEN(items), sizeof(items[0]),
                    compare_sort_items, NULL);
    YEW_ASSERT_EQ_I64(items[0].key, 1);
    YEW_ASSERT_EQ_I64(items[0].original_pos, 1);
    YEW_ASSERT_EQ_I64(items[1].original_pos, 3);
    YEW_ASSERT_EQ_I64(items[2].original_pos, 0);
    YEW_ASSERT_EQ_I64(items[3].original_pos, 2);
    YEW_ASSERT_EQ_I64(items[4].original_pos, 5);
    YEW_ASSERT_EQ_I64(items[5].key, 3);
    YEW_ASSERT_EQ_I64(items[5].original_pos, 4);
}

void test_sort_empty(void)
{
    SortItem item = {7, 9};

    yew_sort_stable(&item, 0U, sizeof(item), compare_sort_items, NULL);
    YEW_ASSERT_EQ_I64(item.key, 7);
    yew_sort_stable(&item, 1U, sizeof(item), compare_sort_items, NULL);
    YEW_ASSERT_EQ_I64(item.original_pos, 9);
}
