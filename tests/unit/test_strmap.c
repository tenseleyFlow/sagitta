#include "harness.h"

#include "util/strmap.h"

#include <stdio.h>
#include <string.h>

enum { ENTRY_COUNT = 10000 };

static void fill_map(Strmap *map, char keys[ENTRY_COUNT][24],
                     size_t values[ENTRY_COUNT])
{
    size_t i;

    strmap_init(map);
    for (i = 0U; i < ENTRY_COUNT; i++) {
        int n = snprintf(keys[i], 24U, "key-%05zu", i);

        SAG_ASSERT(n > 0);
        values[i] = i;
        SAG_ASSERT_NULL(strmap_put(map, keys[i], (size_t)n, &values[i]));
    }
}

void test_strmap_order(void)
{
    Strmap map;
    char keys[ENTRY_COUNT][24];
    size_t values[ENTRY_COUNT];
    StrmapIter iter;
    size_t i;

    fill_map(&map, keys, values);
    SAG_ASSERT_EQ_U64(strmap_len(&map), ENTRY_COUNT);
    iter = strmap_iter(&map);
    for (i = 0U; i < ENTRY_COUNT; i++) {
        const char *key;
        size_t key_len;
        void *value;

        SAG_ASSERT(strmap_iter_next(&iter, &key, &key_len, &value));
        SAG_ASSERT_EQ_U64(key_len, strlen(keys[i]));
        SAG_ASSERT_EQ_MEM(key, keys[i], key_len);
        SAG_ASSERT(value == &values[i]);
    }
    SAG_ASSERT(!strmap_iter_next(&iter, NULL, NULL, NULL));
    strmap_free(&map);
}

void test_strmap_replace_keeps_order(void)
{
    Strmap map;
    char keys[ENTRY_COUNT][24];
    size_t values[ENTRY_COUNT];
    size_t replacement = 999999U;
    StrmapIter iter;
    const char *key;
    size_t i;

    fill_map(&map, keys, values);
    SAG_ASSERT(strmap_get(&map, keys[42], strlen(keys[42])) == &values[42]);
    SAG_ASSERT(strmap_put(&map, keys[42], strlen(keys[42]), &replacement) ==
               &values[42]);
    SAG_ASSERT_EQ_U64(strmap_len(&map), ENTRY_COUNT);
    iter = strmap_iter(&map);
    for (i = 0U; i <= 42U; i++)
        SAG_ASSERT(strmap_iter_next(&iter, &key, NULL, NULL));
    SAG_ASSERT_EQ_STR(key, keys[42]);
    SAG_ASSERT(strmap_get(&map, keys[42], strlen(keys[42])) == &replacement);
    strmap_free(&map);
}
