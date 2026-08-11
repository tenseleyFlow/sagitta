#include "syn/registry.h"

#include <stdlib.h>
#include <string.h>

#include "util/base.h"

void yew_syn_builtin_registry_build(BuiltinRegistry *out,
                                    const SynLangSeed *seed, size_t len)
{
    size_t i;

    if (out == NULL || (len != 0U && seed == NULL))
        return;
    (void)memset(out, 0, sizeof(*out));
    arena_init(&out->first_line_arena);
    if (len != 0U) {
        out->desc = yew_xcalloc(len, sizeof(*out->desc));
        out->loaded = yew_xcalloc(len, sizeof(*out->loaded));
        out->first_line_re = yew_xcalloc(len, sizeof(*out->first_line_re));
    }
    out->len = len;
    for (i = 0U; i < len; i++) {
        out->desc[i] = (SynLangDesc){
            seed[i].id, seed[i].name, seed[i].source,
            (const char **)seed[i].extensions, seed[i].nextensions,
            (const char **)seed[i].filenames, seed[i].nfilenames,
            (const char **)seed[i].shebangs, seed[i].nshebangs,
            seed[i].first_line, seed[i].priority, seed[i].comment
        };
    }
    out->ready = true;
}

void yew_syn_builtin_registry_free(BuiltinRegistry *registry)
{
    size_t i;

    if (registry == NULL || !registry->ready)
        return;
    for (i = 0U; i < registry->len; i++) {
        if (registry->loaded[i].def != NULL)
            yew_syn_def_dispose(registry->loaded[i].def);
        if (registry->loaded[i].tried)
            arena_free_all(&registry->loaded[i].arena);
    }
    arena_free_all(&registry->first_line_arena);
    free(registry->first_line_re);
    free(registry->loaded);
    free(registry->desc);
    (void)memset(registry, 0, sizeof(*registry));
}

const SynLangDesc *yew_syn_builtin_registry_desc_at(
    const BuiltinRegistry *registry, size_t ordinal)
{
    if (registry == NULL || !registry->ready || ordinal >= registry->len)
        return NULL;
    return &registry->desc[ordinal];
}

SynDef *yew_syn_builtin_registry_load(BuiltinRegistry *registry,
                                      const SynLangSeed *seed, size_t len,
                                      u32 lang, BuiltinRegistryLoadFn load,
                                      void *ctx)
{
    size_t i;

    if (registry == NULL || !registry->ready || seed == NULL ||
        len != registry->len || load == NULL)
        return NULL;
    for (i = 0U; i < len; i++) {
        BuiltinDef *slot;

        if (seed[i].id != lang)
            continue;
        slot = &registry->loaded[i];
        if (slot->tried)
            return slot->def;
        slot->tried = true;
        arena_init(&slot->arena);
        fl_diag_init(&slot->dc, &slot->arena);
        slot->def = load(&slot->arena, &slot->dc, &seed[i], ctx);
        return slot->def;
    }
    return NULL;
}

YewRe *yew_syn_builtin_registry_first_line(BuiltinRegistry *registry,
                                           size_t ordinal)
{
    const SynLangDesc *lang;
    YewReErr err = {0U, NULL};

    lang = yew_syn_builtin_registry_desc_at(registry, ordinal);
    if (lang == NULL || lang->first_line == NULL)
        return NULL;
    if (registry->first_line_re[ordinal] == NULL) {
        registry->first_line_re[ordinal] = yew_re_compile(
            &registry->first_line_arena, lang->first_line,
            strlen(lang->first_line), 0U, &err);
    }
    return registry->first_line_re[ordinal];
}
