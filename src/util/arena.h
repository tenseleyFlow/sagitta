#ifndef YEW_UTIL_ARENA_H
#define YEW_UTIL_ARENA_H

#include <stddef.h>

#include "util/base.h"

typedef struct ArenaBlock ArenaBlock;

typedef struct Arena {
    ArenaBlock *head;
    size_t next_block_size;
} Arena;

void arena_init(Arena *arena);
/* align must be a nonzero power of two. */
void *arena_alloc(Arena *arena, size_t size, size_t align);
char *arena_strdup(Arena *arena, const char *str);
char *arena_strndup(Arena *arena, const char *str, size_t len);
void arena_free_all(Arena *arena);
/* Resident bytes of every block the arena holds (util/memacct.h model). */
u64 arena_resident_bytes(const Arena *arena);
/* Resident growth an arena_alloc(size, align) would cause: zero when the
 * current block has room, else the new block's cost. */
u64 arena_alloc_cost(const Arena *arena, size_t size, size_t align);

#endif
