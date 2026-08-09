#ifndef SAG_UTIL_INTERN_H
#define SAG_UTIL_INTERN_H

#include <stddef.h>

#include "util/arena.h"
#include "util/base.h"
#include "util/strmap.h"

typedef struct {
    Strmap map;
    const char **strings;
    /*
     * Lengths, parallel to `strings`.
     *
     * Interned bytes are NOT a C string.  Identifiers never hold a NUL,
     * but Fletch string literals do -- §1.5 lists `\0` among the
     * escapes -- and a caller that recovered the length with strlen
     * turned "a\0b" into "a" silently, one byte of user data at a time.
     * The stored copy is still NUL-terminated so the C-string readers
     * keep working; this is what the byte-exact readers use.
     */
    size_t *lens;
    size_t len;
    size_t cap;
    Arena *arena;
} Interner;

void interner_init(Interner *interner, Arena *arena);
u32 sag_intern(Interner *interner, const char *str, size_t len);
u32 sag_intern_cstr(Interner *interner, const char *str);
const char *sag_intern_str(const Interner *interner, u32 id);
/* The interned byte count for `id`, which may exceed strlen.  Zero for
 * an unknown id, matching sag_intern_str's NULL. */
size_t sag_intern_len(const Interner *interner, u32 id);
size_t sag_intern_count(const Interner *interner);
void interner_free(Interner *interner);

#endif
