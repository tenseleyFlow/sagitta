#include "util/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/base.h"
#include "util/log.h"

#define ARENA_FIRST_BLOCK (64u * 1024u)

struct ArenaBlock {
    ArenaBlock *next;
    size_t cap;
    size_t used;
    unsigned char payload[];
};

void arena_init(Arena *arena)
{
    arena->head = NULL;
    arena->next_block_size = ARENA_FIRST_BLOCK;
}

static ArenaBlock *arena_new_block(Arena *arena, size_t min_payload)
{
    size_t cap = arena->next_block_size;
    ArenaBlock *block;

    if (cap == 0)
        cap = ARENA_FIRST_BLOCK;
    while (cap < min_payload) {
        if (cap > SIZE_MAX / 2) {
            cap = min_payload;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX - sizeof(*block))
        SAG_BUG("arena block size overflow: %zu bytes", cap);
    block = sag_xmalloc(sizeof(*block) + cap);
    block->next = arena->head;
    block->cap = cap;
    block->used = 0;
    arena->head = block;

    if (cap > SIZE_MAX / 2)
        arena->next_block_size = SIZE_MAX;
    else
        arena->next_block_size = cap * 2;
    return block;
}

void *arena_alloc(Arena *arena, size_t size, size_t align)
{
    ArenaBlock *block = arena->head;
    uintptr_t base;
    uintptr_t aligned;
    uintptr_t end;

    if (align == 0 || (align & (align - 1)) != 0)
        SAG_BUG("arena_alloc: alignment %zu is not a power of two", align);

    if (block) {
        base = (uintptr_t)block->payload + block->used;
        if (base <= UINTPTR_MAX - (align - 1)) {
            aligned = (base + align - 1) & ~(uintptr_t)(align - 1);
            end = (uintptr_t)block->payload + block->cap;
            if (aligned <= end && size <= (size_t)(end - aligned)) {
                block->used = (size_t)(aligned - (uintptr_t)block->payload) +
                              size;
                return (void *)aligned;
            }
        }
    }

    if (size > SIZE_MAX - (align - 1))
        SAG_BUG("arena allocation size overflow: %zu + %zu", size, align - 1);
    block = arena_new_block(arena, size + align - 1);
    base = (uintptr_t)block->payload;
    if (base > UINTPTR_MAX - (align - 1))
        SAG_BUG("arena address overflow");
    aligned = (base + align - 1) & ~(uintptr_t)(align - 1);
    block->used = (size_t)(aligned - base) + size;
    return (void *)aligned;
}

char *arena_strndup(Arena *arena, const char *str, size_t len)
{
    char *copy;

    if (len == SIZE_MAX)
        SAG_BUG("arena string size overflow");
    copy = arena_alloc(arena, len + 1, 1);
    if (len)
        memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

char *arena_strdup(Arena *arena, const char *str)
{
    return arena_strndup(arena, str, strlen(str));
}

void arena_free_all(Arena *arena)
{
    ArenaBlock *block = arena->head;

    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    arena_init(arena);
}
