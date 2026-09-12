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
void yew_block_provider_syntax_install(bool enabled);
bool yew_syn_in_string_or_comment(const Buffer *buf, ByteOff off);
/*
 * Observable only for the Sprint 57.16 pairing cache and perf tests: the
 * query heap-allocates a span array and re-lexes the line, so "how many
 * times did typing ask it" is a real budget, not a curiosity.
 */
u64 yew_syn_in_string_or_comment_calls(void);
void yew_syn_in_string_or_comment_calls_reset(void);
bool yew_block_match(UnitCtx *u, ByteOff p, bool next, ByteOff *out);

#endif
