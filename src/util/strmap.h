#ifndef SAG_UTIL_STRMAP_H
#define SAG_UTIL_STRMAP_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

typedef struct {
    char *key;
    size_t key_len;
    void *value;
} StrmapEntry;

typedef struct {
    StrmapEntry *entries;
    size_t len;
    size_t cap;
    u32 *slots; /* entry index + 1; zero is empty */
    size_t slot_count;
} Strmap;

void strmap_init(Strmap *map);
/* Returns the previous value, or NULL for a new key. */
void *strmap_put(Strmap *map, const char *key, size_t key_len, void *value);
void *strmap_get(const Strmap *map, const char *key, size_t key_len);
bool strmap_has(const Strmap *map, const char *key, size_t key_len);
size_t strmap_len(const Strmap *map);
void strmap_free(Strmap *map);

typedef struct {
    const Strmap *map;
    size_t index;
} StrmapIter;

StrmapIter strmap_iter(const Strmap *map);
bool strmap_iter_next(StrmapIter *iter, const char **key, size_t *key_len,
                      void **value);

#endif
