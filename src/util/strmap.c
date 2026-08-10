#include "util/strmap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define STRMAP_FIRST_SLOTS 64u

static u64 strmap_hash(const char *key, size_t key_len)
{
    u64 hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0; i < key_len; i++) {
        hash ^= (u8)key[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void strmap_init(Strmap *map)
{
    memset(map, 0, sizeof(*map));
}

static size_t strmap_probe(const Strmap *map, const char *key, size_t key_len)
{
    size_t mask = map->slot_count - 1;
    size_t slot = (size_t)strmap_hash(key, key_len) & mask;

    for (;;) {
        u32 stored = map->slots[slot];

        if (stored == 0)
            return slot;
        {
            const StrmapEntry *entry = &map->entries[stored - 1];
            if (entry->key_len == key_len &&
                (key_len == 0 || memcmp(entry->key, key, key_len) == 0))
                return slot;
        }
        slot = (slot + 1) & mask;
    }
}

static void strmap_grow_slots(Strmap *map)
{
    size_t new_count;
    size_t i;

    if (map->slot_count == 0) {
        new_count = STRMAP_FIRST_SLOTS;
    } else {
        if (map->slot_count > SIZE_MAX / 2)
            YEW_BUG("string map hash table overflow");
        new_count = map->slot_count * 2;
    }
    free(map->slots);
    map->slots = yew_xcalloc(new_count, sizeof(*map->slots));
    map->slot_count = new_count;
    for (i = 0; i < map->len; i++) {
        const StrmapEntry *entry = &map->entries[i];
        map->slots[strmap_probe(map, entry->key, entry->key_len)] =
            (u32)(i + 1);
    }
}

void *strmap_put(Strmap *map, const char *key, size_t key_len, void *value)
{
    size_t slot;
    StrmapEntry *entry;

    if (map->len >= UINT32_MAX)
        YEW_BUG("string map overflow: more than 2^32-1 entries");
    if (map->slot_count == 0 ||
        map->len + 1 >= map->slot_count - map->slot_count / 4)
        strmap_grow_slots(map);

    slot = strmap_probe(map, key, key_len);
    if (map->slots[slot] != 0) {
        void *previous;

        entry = &map->entries[map->slots[slot] - 1];
        previous = entry->value;
        entry->value = value;
        return previous;
    }

    if (map->len == map->cap) {
        size_t new_cap;

        if (map->cap == 0)
            new_cap = 16;
        else {
            if (map->cap > SIZE_MAX / 2)
                YEW_BUG("string map entry array overflow");
            new_cap = map->cap * 2;
        }
        map->entries = yew_xreallocarray(map->entries, new_cap,
                                         sizeof(*map->entries));
        map->cap = new_cap;
    }
    entry = &map->entries[map->len];
    if (key_len == SIZE_MAX)
        YEW_BUG("string map key size overflow");
    entry->key = yew_xmalloc(key_len + 1);
    if (key_len)
        memcpy(entry->key, key, key_len);
    entry->key[key_len] = '\0';
    entry->key_len = key_len;
    entry->value = value;
    map->len++;
    map->slots[slot] = (u32)map->len;
    return NULL;
}

void *strmap_get(const Strmap *map, const char *key, size_t key_len)
{
    u32 stored;

    if (map->slot_count == 0)
        return NULL;
    stored = map->slots[strmap_probe(map, key, key_len)];
    return stored ? map->entries[stored - 1].value : NULL;
}

bool strmap_has(const Strmap *map, const char *key, size_t key_len)
{
    if (map->slot_count == 0)
        return false;
    return map->slots[strmap_probe(map, key, key_len)] != 0;
}

size_t strmap_len(const Strmap *map)
{
    return map->len;
}

void strmap_free(Strmap *map)
{
    size_t i;

    for (i = 0; i < map->len; i++)
        free(map->entries[i].key);
    free(map->entries);
    free(map->slots);
    strmap_init(map);
}

StrmapIter strmap_iter(const Strmap *map)
{
    StrmapIter iter = {map, 0};
    return iter;
}

bool strmap_iter_next(StrmapIter *iter, const char **key, size_t *key_len,
                      void **value)
{
    const StrmapEntry *entry;

    if (iter->index >= iter->map->len)
        return false;
    entry = &iter->map->entries[iter->index++];
    if (key)
        *key = entry->key;
    if (key_len)
        *key_len = entry->key_len;
    if (value)
        *value = entry->value;
    return true;
}
