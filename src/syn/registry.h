#ifndef YEW_SYN_REGISTRY_H
#define YEW_SYN_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "syn/defs.h"

typedef struct BuiltinDef {
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    bool tried;
} BuiltinDef;

typedef struct BuiltinRegistry {
    Arena first_line_arena;
    SynLangDesc *desc;
    BuiltinDef *loaded;
    YewRe **first_line_re;
    size_t len;
    bool ready;
} BuiltinRegistry;

typedef struct SynDetectEntry {
    const char *key;
    u32 lang;
} SynDetectEntry;

typedef struct SynDetectIndex {
    const SynDetectEntry *exact;
    size_t nexact;
    const SynDetectEntry *extensions;
    size_t nextensions;
    const SynDetectEntry *globs;
    size_t nglobs;
    const SynDetectEntry *shebangs;
    size_t nshebangs;
    const SynDetectEntry *first_lines;
    size_t nfirst_lines;
} SynDetectIndex;

typedef SynDef *(*BuiltinRegistryLoadFn)(Arena *arena, DiagCtx *dc,
                                        const SynLangSeed *seed, void *ctx);

void yew_syn_builtin_registry_build(BuiltinRegistry *out,
                                    const SynLangSeed *seed, size_t len);
void yew_syn_builtin_registry_free(BuiltinRegistry *registry);
const SynLangDesc *yew_syn_builtin_registry_desc_at(
    const BuiltinRegistry *registry, size_t ordinal);
SynDef *yew_syn_builtin_registry_load(BuiltinRegistry *registry,
                                      const SynLangSeed *seed, size_t len,
                                      u32 lang, BuiltinRegistryLoadFn load,
                                      void *ctx);
YewRe *yew_syn_builtin_registry_first_line(BuiltinRegistry *registry,
                                           size_t ordinal);

#endif
