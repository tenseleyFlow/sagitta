#ifndef YEW_EDIT_BLOCK_H
#define YEW_EDIT_BLOCK_H

#include "edit/motion.h"

enum { YEW_BLOCK_SCAN_LINES = 2000 };

typedef struct BlockProvider {
    const char *name;
    int priority;
    bool (*enclosing)(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                      Span *out);
    void *ctx;
} BlockProvider;

void yew_block_register(BlockProvider provider);
bool yew_block_level(UnitCtx *u, ByteOff p, u32 level, Span *out);
void yew_block_provider_syntax_install(BlockProvider provider);
bool yew_block_match(UnitCtx *u, ByteOff p, bool next, ByteOff *out);

#endif
