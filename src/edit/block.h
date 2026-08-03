#ifndef SAG_EDIT_BLOCK_H
#define SAG_EDIT_BLOCK_H

#include "edit/motion.h"

enum { SAG_BLOCK_SCAN_LINES = 2000 };

typedef struct BlockProvider {
    const char *name;
    int priority;
    bool (*enclosing)(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                      Span *out);
    void *ctx;
} BlockProvider;

void sag_block_register(BlockProvider provider);
bool sag_block_level(UnitCtx *u, ByteOff p, u32 level, Span *out);
void sag_block_provider_syntax_install(BlockProvider provider);
bool sag_block_match(UnitCtx *u, ByteOff p, bool next, ByteOff *out);

#endif
