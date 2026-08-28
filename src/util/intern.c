#include "util/intern.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

void interner_init(Interner *interner, Arena *arena)
{
    strmap_init(&interner->map);
    interner->cap = 64;
    interner->strings = yew_xmalloc(interner->cap * sizeof(*interner->strings));
    interner->lens = yew_xmalloc(interner->cap * sizeof(*interner->lens));
    interner->strings[0] = NULL;
    interner->lens[0] = 0U;
    interner->len = 1;
    interner->arena = arena;
}

u32 yew_intern(Interner *interner, const char *str, size_t len)
{
    void *found = strmap_get(&interner->map, str, len);
    const char *copy;
    u32 id;

    if (found)
        return (u32)(uintptr_t)found;
    if (interner->len > UINT32_MAX)
        YEW_BUG("interner overflow: more than 2^32-1 strings");
    if (interner->len == interner->cap) {
        size_t new_cap;

        if (interner->cap > SIZE_MAX / 2)
            YEW_BUG("interner string table overflow");
        new_cap = interner->cap * 2;
        interner->strings = yew_xreallocarray(
            interner->strings, new_cap, sizeof(*interner->strings));
        interner->lens = yew_xreallocarray(
            interner->lens, new_cap, sizeof(*interner->lens));
        interner->cap = new_cap;
    }
    copy = arena_strndup(interner->arena, str, len);
    id = (u32)interner->len;
    interner->lens[interner->len] = len;
    interner->strings[interner->len++] = copy;
    (void)strmap_put(&interner->map, copy, len, (void *)(uintptr_t)id);
    return id;
}

u32 yew_intern_cstr(Interner *interner, const char *str)
{
    return yew_intern(interner, str, strlen(str));
}

const char *yew_intern_str(const Interner *interner, u32 id)
{
    if (id == 0 || (size_t)id >= interner->len)
        return NULL;
    return interner->strings[id];
}

size_t yew_intern_len(const Interner *interner, u32 id)
{
    if (id == 0 || (size_t)id >= interner->len)
        return 0U;
    return interner->lens[id];
}

size_t yew_intern_count(const Interner *interner)
{
    return interner->len - 1;
}

void interner_free(Interner *interner)
{
    strmap_free(&interner->map);
    yew_xfree(interner->strings);
    yew_xfree(interner->lens);
    interner->strings = NULL;
    interner->lens = NULL;
    interner->len = 0;
    interner->cap = 0;
    interner->arena = NULL;
}
