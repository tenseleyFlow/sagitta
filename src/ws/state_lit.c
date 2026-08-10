/* Frozen-schema accessors.  The hand-written codec itself is test-only. */
#include "ws/fllit.h"

#include <string.h>

const FlLit *sag_fl_get(const FlLit *map, const char *key)
{
    u32 i;
    u64 n;

    if (map == NULL || map->kind != FL_LIT_MAP || key == NULL)
        return NULL;
    n = (u64)strlen(key);
    for (i = 0U; i < map->len; i++) {
        if (map->keylens[i] == n &&
            memcmp(map->keys[i], key, (size_t)n) == 0)
            return map->items[i];
    }
    return NULL;
}

u32 sag_fl_len(const FlLit *list)
{
    if (list == NULL ||
        (list->kind != FL_LIT_LIST && list->kind != FL_LIT_MAP))
        return 0U;
    return list->len;
}

const FlLit *sag_fl_at(const FlLit *list, u32 i)
{
    if (list == NULL ||
        (list->kind != FL_LIT_LIST && list->kind != FL_LIT_MAP) ||
        i >= list->len)
        return NULL;
    return list->items[i];
}

i64 sag_fl_int_or(const FlLit *v, i64 dflt)
{
    return v != NULL && v->kind == FL_LIT_INT ? v->i : dflt;
}

bool sag_fl_bool_or(const FlLit *v, bool dflt)
{
    return v != NULL && v->kind == FL_LIT_BOOL ? v->i != 0 : dflt;
}

const char *sag_fl_str_or(const FlLit *v, const char *dflt, u64 *n)
{
    if (v != NULL && v->kind == FL_LIT_STR) {
        if (n != NULL)
            *n = v->slen;
        return v->s;
    }
    if (n != NULL)
        *n = dflt == NULL ? 0U : (u64)strlen(dflt);
    return dflt;
}
