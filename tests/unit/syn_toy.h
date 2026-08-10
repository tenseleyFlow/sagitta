#ifndef YEW_TEST_SYN_TOY_H
#define YEW_TEST_SYN_TOY_H

#include "search/regex.h"
#include "syn/engine.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    SYN_TOY_MAIN = 0,
    SYN_TOY_STRING,
    SYN_TOY_COMMENT_BLOCK,
    SYN_TOY_COMMENT_LINE,
    SYN_TOY_NCTXS
};

typedef struct SynToy {
    Arena arena;
    Arena aux_arena;
    Interner aux;
    SynCtx ctxs[SYN_TOY_NCTXS];
    SynRule rules[10];
    SynDef def;
    SynEngine *engine;
} SynToy;

void syn_toy_init(SynToy *toy);
void syn_toy_free(SynToy *toy);
u32 syn_toy_state(SynToy *toy, u16 ctx);
u32 syn_toy_line(SynToy *toy, u32 entry, const char *line,
                 SynSpan *spans, u32 cap, SynLineOut *out);

#endif
