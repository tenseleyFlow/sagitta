#ifndef SAG_UTIL_ARENA_H
#define SAG_UTIL_ARENA_H

#include <stddef.h>

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

#endif
