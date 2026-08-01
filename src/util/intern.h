#ifndef SAG_UTIL_INTERN_H
#define SAG_UTIL_INTERN_H

#include <stddef.h>

#include "util/arena.h"
#include "util/base.h"
#include "util/strmap.h"

typedef struct {
    Strmap map;
    const char **strings;
    size_t len;
    size_t cap;
    Arena *arena;
} Interner;

void interner_init(Interner *interner, Arena *arena);
u32 sag_intern(Interner *interner, const char *str, size_t len);
u32 sag_intern_cstr(Interner *interner, const char *str);
const char *sag_intern_str(const Interner *interner, u32 id);
size_t sag_intern_count(const Interner *interner);
void interner_free(Interner *interner);

#endif
