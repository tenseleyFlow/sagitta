#define _POSIX_C_SOURCE 200809L

#include "syncli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/diag.h"
#include "fl/parse.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "text/piece.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

static const char syn_usage[] =
    "Usage:\n"
    "  yew syn list\n"
    "  yew syn compile [--all | FILE]\n"
    "  yew syn check FILE [--strict]\n"
    "  yew syn check --coverage FILE INPUT... [--strict]\n"
    "  yew syn check --embed [--strict]\n"
    "  yew syn dump FILE --tables\n"
    "  yew syn dump FILE --spans INPUT\n"
    "  yew syn cache clear|path\n"
    "\n"
    "Exit: 0 valid, 1 invalid definition or arguments, 3 I/O error,\n"
    "4 internal error.  --strict promotes warnings to errors.\n";

static void stderr_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                        const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)fputs(rendered, stderr);
}

static int usage_error(const char *msg)
{
    if (msg != NULL)
        (void)fprintf(stderr, "yew syn: error: %s\n", msg);
    (void)fputs(syn_usage, stderr);
    return YEW_EXIT_ERR;
}

static bool output(const Bytebuf *out)
{
    return out->len == 0U || fwrite(out->data, 1U, out->len, stdout) == out->len;
}

static char *slurp(const char *path, size_t *len, Arena *arena)
{
    FILE *fp;
    Bytebuf bytes;
    char *copy;

    errno = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) {
        (void)fprintf(stderr, "yew syn: cannot read %s: %s\n", path,
                      strerror(errno));
        return NULL;
    }
    bytebuf_init(&bytes);
    for (;;) {
        u8 chunk[65536];
        size_t got = fread(chunk, 1U, sizeof(chunk), fp);

        if (got != 0U)
            bytebuf_append(&bytes, chunk, got);
        if (got != sizeof(chunk))
            break;
    }
    if (ferror(fp) != 0) {
        int saved = errno;

        (void)fclose(fp);
        bytebuf_free(&bytes);
        (void)fprintf(stderr, "yew syn: cannot read %s: %s\n", path,
                      strerror(saved == 0 ? EIO : saved));
        return NULL;
    }
    if (fclose(fp) != 0) {
        bytebuf_free(&bytes);
        (void)fprintf(stderr, "yew syn: cannot close %s: %s\n", path,
                      strerror(errno));
        return NULL;
    }
    copy = arena_alloc(arena, bytes.len + 1U, 1U);
    if (bytes.len != 0U)
        (void)memcpy(copy, bytes.data, bytes.len);
    copy[bytes.len] = '\0';
    *len = bytes.len;
    bytebuf_free(&bytes);
    return copy;
}

static const SynLangDesc *lang_at(u32 ordinal)
{
    u32 id;
    u32 seen = 0U;

    for (id = 1U; id <= UINT16_MAX; id++) {
        const SynLangDesc *desc = yew_syn_lang_desc(id);

        if (desc == NULL)
            continue;
        if (seen == ordinal)
            return desc;
        seen++;
    }
    return NULL;
}

static int list_defs(bool bypass)
{
    Bytebuf out;
    const char *no_cache = getenv("YEW_NO_SYN_CACHE");
    u32 count = yew_syn_lang_count();
    u32 i;

    bytebuf_init(&out);
    for (i = 0U; i < count; i++) {
        const SynLangDesc *desc = lang_at(i);
        char *cache;
        const char *state;
        u32 j;

        if (desc == NULL) {
            bytebuf_free(&out);
            (void)fputs("yew syn: internal language table gap\n", stderr);
            return YEW_EXIT_BUG;
        }
        cache = yew_syn_cache_path(desc->name);
        state = bypass || (no_cache != NULL && strcmp(no_cache, "1") == 0) ?
                "bypassed" :
                cache != NULL && access(cache, F_OK) == 0 ? "warm" : "cold";
        bytebuf_printf(&out, "%s\t", desc->name);
        for (j = 0U; j < desc->nextensions; j++) {
            if (j != 0U)
                bytebuf_push_u8(&out, (u8)',');
            bytebuf_append(&out, desc->extensions[j],
                           strlen(desc->extensions[j]));
        }
        bytebuf_printf(&out, "\t%s\t%s\n", desc->source, state);
        free(cache);
    }
    if (!output(&out)) {
        bytebuf_free(&out);
        return YEW_EXIT_IO;
    }
    bytebuf_free(&out);
    return YEW_EXIT_OK;
}

static SynDef *load_def(const char *path, Arena *arena, DiagCtx *dc,
                        int *status)
{
    struct stat st;
    SynDef *def;

    errno = 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(path, R_OK) != 0) {
        int saved = errno == 0 ? EIO : errno;

        (void)fprintf(stderr, "yew syn: cannot read %s: %s\n", path,
                      strerror(saved));
        *status = YEW_EXIT_IO;
        return NULL;
    }
    fl_diag_init(dc, arena);
    fl_diag_set_sink(dc, stderr_sink, NULL);
    def = yew_syn_def_load(arena, dc, path);
    *status = def == NULL ? YEW_EXIT_ERR : YEW_EXIT_OK;
    return def;
}

static int check_coverage(const SynDef *def, const char *const *inputs,
                          u32 ninputs);
static void check_embed_source(Arena *arena, DiagCtx *dc, const char *source,
                               size_t len, u32 file_id, const SynDef *def);

static char *runtime_source(const SynLangDesc *desc)
{
    const char *root = getenv("YEW_RUNTIME_DIR");
    const char *relative = desc->source;
    char *path;
    size_t n;

    if (strncmp(relative, "runtime/", 8U) == 0)
        relative += 8U;
    if (root != NULL && root[0] != '\0') {
        n = strlen(root) + 1U + strlen(relative);
        path = yew_xmalloc(n + 1U);
        (void)snprintf(path, n + 1U, "%s/%s", root, relative);
        if (access(path, R_OK) == 0)
            return path;
        free(path);
    }
    n = strlen(YEW_RUNTIME_DIR_DEFAULT) + 1U + strlen(relative);
    path = yew_xmalloc(n + 1U);
    (void)snprintf(path, n + 1U, "%s/%s", YEW_RUNTIME_DIR_DEFAULT,
                   relative);
    if (access(path, R_OK) == 0)
        return path;
    free(path);
    path = yew_xmalloc(strlen(desc->source) + 1U);
    (void)memcpy(path, desc->source, strlen(desc->source) + 1U);
    return path;
}

static int compile_one(const char *path)
{
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    int status;

    arena_init(&arena);
    def = load_def(path, &arena, &dc, &status);
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&arena);
    return status;
}

static int compile_all(void)
{
    u32 count = yew_syn_lang_count();
    u32 i;

    for (i = 0U; i < count; i++) {
        const SynLangDesc *desc = lang_at(i);
        char *path;
        int status;

        if (desc == NULL)
            return YEW_EXIT_BUG;
        path = runtime_source(desc);
        status = compile_one(path);
        free(path);
        if (status != YEW_EXIT_OK)
            return status;
    }
    return YEW_EXIT_OK;
}

static int check_one(const char *path, bool strict, bool coverage,
                     const char *const *inputs, u32 ninputs)
{
    Arena arena;
    DiagCtx dc;
    char *source;
    size_t len = 0U;
    u32 file_id;
    u32 errors = 0U;
    u32 warnings = 0U;
    SynDef *def;
    int status;

    arena_init(&arena);
    source = slurp(path, &len, &arena);
    if (source == NULL) {
        arena_free_all(&arena);
        return YEW_EXIT_IO;
    }
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, stderr_sink, NULL);
    file_id = fl_diag_add_file(&dc, path, source, len);
    def = yew_syn_def_compile(&arena, &dc, (const u8 *)source, len, file_id,
                              &errors, &warnings);
    if (def != NULL)
        check_embed_source(&arena, &dc, source, len, file_id, def);
    errors = fl_diag_errors(&dc);
    warnings = dc.nwarnings;
    status = errors != 0U || (strict && warnings != 0U) ? YEW_EXIT_ERR :
             YEW_EXIT_OK;
    if (def != NULL && status == YEW_EXIT_OK && coverage)
        status = check_coverage(def, inputs, ninputs);
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&arena);
    return status;
}

#define SYN_EMBED_CHECK_PATH_MAX 64U

typedef struct EmbedCheckPath {
    u32 def;
    u16 ctx;
    u8 level;
} EmbedCheckPath;

typedef struct EmbedDepth {
    u32 frames;
    u32 defs;
    bool dynamic;
    char chain[512];
} EmbedDepth;

static i32 embed_def_index(const SynDef *const *defs, u32 ndefs,
                           const char *name)
{
    u32 i;

    if (name == NULL)
        return -1;
    for (i = 0U; i < ndefs; i++) {
        if (defs[i] != NULL && defs[i]->name != NULL &&
            strcmp(defs[i]->name, name) == 0)
            return (i32)i;
    }
    return -1;
}

static bool embed_closed_fallback(const char *name)
{
    /* The shipped JS/TS tagged-template table deliberately reserves gql
     * for fallback styling; Sprint 41.5 does not ship a GraphQL definition. */
    return name != NULL && strcmp(name, "gql") == 0;
}

static i32 embed_ctx_index(const SynDef *def, const char *name)
{
    u16 i;

    if (def == NULL || name == NULL)
        return -1;
    for (i = 0U; i < def->nctxs; i++) {
        const char *candidate = yew_syn_ctx_name(def, i);

        if (candidate != NULL && strcmp(candidate, name) == 0)
            return (i32)i;
    }
    return -1;
}

static bool embed_path_has(const EmbedCheckPath *path, u32 npath, u32 def,
                           u16 ctx, u8 level)
{
    u32 i;

    for (i = 0U; i < npath; i++) {
        if (path[i].def == def && path[i].ctx == ctx &&
            path[i].level == level)
            return true;
    }
    return false;
}

static bool embed_depth_better(const EmbedDepth *candidate,
                               const EmbedDepth *held)
{
    return candidate->frames > held->frames ||
           (candidate->frames == held->frames &&
            candidate->defs > held->defs);
}

static void embed_chain_prefix(EmbedDepth *depth, const char *host)
{
    size_t prefix = strlen(host);
    size_t held;

    if (prefix > sizeof(depth->chain) - 2U)
        prefix = sizeof(depth->chain) - 2U;
    held = strlen(depth->chain);
    if (held > sizeof(depth->chain) - prefix - 2U)
        held = sizeof(depth->chain) - prefix - 2U;
    (void)memmove(depth->chain + prefix + 1U, depth->chain, held);
    (void)memcpy(depth->chain, host, prefix);
    depth->chain[prefix] = '\x1f';
    depth->chain[prefix + 1U + held] = '\0';
}

static EmbedDepth embed_combined_depth_limit(
    const SynDef *const *defs, u32 ndefs, u32 def_index, u16 ctx,
    EmbedCheckPath *path, u32 npath, u32 def_cap, u8 level,
    bool require_dynamic, bool allow_dynamic)
{
    EmbedDepth best;
    const SynDef *def;
    const SynCtx *active;
    u32 i;

    (void)memset(&best, 0, sizeof(best));
    if (defs == NULL || def_index >= ndefs || defs[def_index] == NULL ||
        def_cap == 0U)
        return best;
    def = defs[def_index];
    best.frames = require_dynamic ? 0U : 1U;
    best.defs = require_dynamic ? 0U : 1U;
    (void)snprintf(best.chain, sizeof(best.chain), "%s",
                   def->name == NULL ? "<unnamed>" : def->name);
    if (ctx >= def->nctxs || npath >= SYN_EMBED_CHECK_PATH_MAX ||
        embed_path_has(path, npath, def_index, ctx, level))
        return best;
    path[npath].def = def_index;
    path[npath].ctx = ctx;
    path[npath].level = level;
    npath++;
    active = &def->ctxs[ctx];
    for (i = 0U; i < active->nrules; i++) {
        const SynRule *rule = &def->rules[active->first_rule + i];
        EmbedDepth candidate;

        if (rule->op == SYN_OP_PUSH) {
            u32 j;

            if (rule->npush == 0U) {
                candidate = embed_combined_depth_limit(
                    defs, ndefs, def_index, rule->target, path, npath,
                    def_cap, level, require_dynamic, allow_dynamic);
                if (candidate.frames != 0U)
                    candidate.frames++;
                if (embed_depth_better(&candidate, &best))
                    best = candidate;
            } else {
                for (j = 0U; j < rule->npush; j++) {
                    candidate = embed_combined_depth_limit(
                        defs, ndefs, def_index, rule->push[j], path, npath,
                        def_cap, level, require_dynamic, allow_dynamic);
                    if (candidate.frames != 0U)
                        candidate.frames += j + 1U;
                    if (embed_depth_better(&candidate, &best))
                        best = candidate;
                }
            }
        } else if (rule->op == SYN_OP_EMBED) {
            const char *guest_name = NULL;
            i32 guest_index = -1;
            u16 guest_ctx = 0U;
            u8 guest_level = level;

            if (rule->embed.lang_kind == SYN_EMBED_LANG_SELF) {
                guest_index = (i32)def_index;
                guest_name = def->name;
                guest_level = (u8)(level + 1U);
            } else if (rule->embed.lang_kind == SYN_EMBED_LANG_LITERAL) {
                guest_name = yew_intern_str(def->aux, rule->embed.lang);
                guest_index = embed_def_index(defs, ndefs, guest_name);
            }
            if (rule->embed.lang_kind == SYN_EMBED_LANG_CAPTURE) {
                u32 guest;

                if (!allow_dynamic || def_cap <= 1U)
                    continue;
                for (guest = 0U; guest < ndefs; guest++) {
                    EmbedDepth dynamic;

                    if (guest == def_index || defs[guest] == NULL)
                        continue;
                    guest_ctx = defs[guest]->root;
                    dynamic = embed_combined_depth_limit(
                        defs, ndefs, guest, guest_ctx, path, npath,
                        def_cap - 1U, level, false, true);
                    if (dynamic.frames == 0U)
                        continue;
                    dynamic.frames += 2U;
                    dynamic.defs++;
                    dynamic.dynamic = true;
                    embed_chain_prefix(&dynamic,
                                       def->name == NULL ? "<unnamed>" :
                                           def->name);
                    if (dynamic.defs <= def_cap &&
                        embed_depth_better(&dynamic, &best))
                        best = dynamic;
                }
                continue;
            }
            if (guest_index < 0)
                continue;
            guest_ctx = defs[guest_index]->root;
            if (rule->embed.ctx != 0U) {
                const char *name = yew_intern_str(def->aux, rule->embed.ctx);
                i32 found = embed_ctx_index(defs[guest_index], name);

                if (found < 0)
                    continue;
                guest_ctx = (u16)found;
            }
            if (def_cap <= 1U)
                continue;
            candidate = embed_combined_depth_limit(
                defs, ndefs, (u32)guest_index, guest_ctx, path, npath,
                def_cap - 1U, guest_level, require_dynamic,
                allow_dynamic);
            if (candidate.frames == 0U)
                continue;
            candidate.frames += 2U;
            candidate.defs++;
            embed_chain_prefix(&candidate,
                               def->name == NULL ? "<unnamed>" : def->name);
            if (candidate.defs <= def_cap &&
                embed_depth_better(&candidate, &best))
                best = candidate;
        }
    }
    return best;
}

static void print_embed_chain(FILE *stream, const char *chain)
{
    const unsigned char *p = (const unsigned char *)chain;

    while (*p != 0U) {
        if (*p == 0x1fU)
            (void)fputs("\342\206\222", stream);
        else
            (void)fputc(*p, stream);
        p++;
    }
}

static int check_embed(bool strict)
{
    const SynDef **defs;
    u32 ndefs = yew_syn_lang_count();
    u32 loaded = 0U;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 i;

    defs = yew_xcalloc(ndefs == 0U ? 1U : ndefs, sizeof(*defs));
    for (i = 0U; i < ndefs; i++) {
        const SynLangDesc *desc = lang_at(i);

        if (desc == NULL)
            errors++;
        else {
            defs[i] = yew_syn_def_for(desc->id);
            if (defs[i] == NULL)
                errors++;
            else
                loaded++;
        }
    }
    (void)printf("definition\tchain\tframes\tdefs\tresult\n");
    for (i = 0U; i < ndefs; i++) {
        EmbedCheckPath path[SYN_EMBED_CHECK_PATH_MAX];
        EmbedDepth static_depth;
        EmbedDepth dynamic_depth;
        const EmbedDepth *display;
        bool static_bad;
        bool dynamic_bad;
        const char *result = "ok";
        u32 ctx;

        if (defs[i] == NULL)
            continue;
        for (ctx = 0U; ctx < defs[i]->nctxs; ctx++) {
            const SynCtx *host = &defs[i]->ctxs[ctx];
            u32 j;

            for (j = 0U; j < host->nrules; j++) {
                const SynRule *rule =
                    &defs[i]->rules[host->first_rule + j];

                if (rule->op == SYN_OP_EMBED &&
                    rule->embed.lang_kind == SYN_EMBED_LANG_LITERAL) {
                    const char *guest =
                        yew_intern_str(defs[i]->aux, rule->embed.lang);
                    i32 guest_index = embed_def_index(defs, ndefs, guest);

                    if (guest_index < 0 && !embed_closed_fallback(guest)) {
                        (void)fprintf(stderr,
                                      "yew syn: error: embed: no definition named '%s'\n",
                                      guest == NULL ? "" : guest);
                        errors++;
                    } else if (guest_index >= 0 && rule->embed.ctx != 0U) {
                        const char *guest_ctx = yew_intern_str(
                            defs[i]->aux, rule->embed.ctx);

                        if (embed_ctx_index(defs[guest_index], guest_ctx) < 0) {
                            (void)fprintf(
                                stderr,
                                "yew syn: error: embed: definition '%s' has no context named '%s'\n",
                                guest, guest_ctx == NULL ? "" : guest_ctx);
                            errors++;
                        }
                    }
                }
            }
        }
        (void)memset(path, 0, sizeof(path));
        static_depth = embed_combined_depth_limit(
            defs, ndefs, i, defs[i]->root, path, 0U, YEW_SYN_DEF_MAX, 0U,
            false, false);
        (void)memset(path, 0, sizeof(path));
        dynamic_depth = embed_combined_depth_limit(
            defs, ndefs, i, defs[i]->root, path, 0U, YEW_SYN_DEF_MAX, 0U,
            true, true);
        static_bad = static_depth.frames > YEW_SYN_DEPTH_MAX;
        dynamic_bad = dynamic_depth.frames > YEW_SYN_DEPTH_MAX;
        if (static_bad) {
            result = "error";
            errors++;
        } else if (dynamic_bad) {
            result = "warning";
            warnings++;
        }
        if (static_bad)
            display = &static_depth;
        else if (dynamic_bad)
            display = &dynamic_depth;
        else
            display = embed_depth_better(&dynamic_depth, &static_depth) ?
                &dynamic_depth : &static_depth;
        (void)printf("%s\t", defs[i]->name);
        print_embed_chain(stdout, display->chain);
        (void)printf("\t%u/%u\t%u/%u\t%s\n", (unsigned)display->frames,
                     (unsigned)YEW_SYN_DEPTH_MAX, (unsigned)display->defs,
                     (unsigned)YEW_SYN_DEF_MAX, result);
        if (dynamic_bad) {
            (void)fprintf(stderr,
                          "yew syn: warning: dynamic embed in '%s' has worst installed chain ",
                          defs[i]->name);
            print_embed_chain(stderr, dynamic_depth.chain);
            (void)fprintf(stderr, "; cap exceeded by %u frame(s)\n",
                          (unsigned)(dynamic_depth.frames -
                                     YEW_SYN_DEPTH_MAX));
        }
        if (static_bad) {
            (void)fputs("yew syn: error: ", stderr);
            print_embed_chain(stderr, static_depth.chain);
            (void)fprintf(stderr,
                          " can reach depth %u; the cap is %u (definition levels %u/%u)\n",
                          (unsigned)static_depth.frames,
                          (unsigned)YEW_SYN_DEPTH_MAX,
                          (unsigned)static_depth.defs,
                          (unsigned)YEW_SYN_DEF_MAX);
        }
    }
    free(defs);
    if (ferror(stdout) != 0)
        return YEW_EXIT_IO;
    if (errors != 0U || (strict && warnings != 0U))
        return YEW_EXIT_ERR;
    return loaded == ndefs ? YEW_EXIT_OK : YEW_EXIT_ERR;
}

typedef struct EmbedSourceSites {
    FlNode **lang;
    u32 n;
    u32 cap;
} EmbedSourceSites;

static bool ast_string_eq(const FlNode *node, const Interner *in,
                          const char *text)
{
    const char *value;
    size_t n;

    if (node == NULL || node->kind != FL_A_LIT ||
        node->as.lit.lit != FL_L_STR)
        return false;
    value = yew_intern_str(in, node->as.lit.v.str_id);
    n = yew_intern_len(in, node->as.lit.v.str_id);
    return value != NULL && strlen(text) == n && memcmp(value, text, n) == 0;
}

static FlNode *ast_map_value(FlNode *map, const Interner *in,
                             const char *key)
{
    u32 i;

    if (map == NULL || map->kind != FL_A_MAP)
        return NULL;
    for (i = 0U; i < map->as.map.n; i++) {
        if (ast_string_eq(map->as.map.keys[i], in, key))
            return map->as.map.vals[i];
    }
    return NULL;
}

static void ast_embed_sites(FlNode *node, const Interner *in,
                            EmbedSourceSites *sites)
{
    u32 i;

    if (node == NULL)
        return;
    if (node->kind == FL_A_MAP) {
        FlNode *embed = ast_map_value(node, in, "embed");
        FlNode *lang = ast_map_value(embed, in, "lang");

        if (lang != NULL) {
            if (sites->n == sites->cap) {
                u32 next = sites->cap == 0U ? 16U : sites->cap * 2U;

                sites->lang = yew_xreallocarray(sites->lang, next,
                                                sizeof(*sites->lang));
                sites->cap = next;
            }
            sites->lang[sites->n++] = lang;
        }
        for (i = 0U; i < node->as.map.n; i++)
            ast_embed_sites(node->as.map.vals[i], in, sites);
    } else if (node->kind == FL_A_LIST) {
        for (i = 0U; i < node->as.list.n; i++)
            ast_embed_sites(node->as.list.items[i], in, sites);
    }
}

static FlSpan embed_selector_span(const FlNode *node, const Interner *in,
                                  bool dynamic)
{
    FlSpan span = node->sp;
    size_t n = yew_intern_len(in, node->as.lit.v.str_id);

    if (dynamic) {
        if (span.len != 0U)
            span.col += span.len - 1U;
        span.len = 1U;
    } else {
        span.col++;
        span.len = n > UINT32_MAX ? UINT32_MAX : (u32)n;
    }
    return span;
}

static void check_embed_source(Arena *arena, DiagCtx *dc, const char *source,
                               size_t len, u32 file_id, const SynDef *def)
{
    Interner in;
    FlNode *root;
    EmbedSourceSites sites;
    const SynDef **defs;
    u32 installed = yew_syn_lang_count();
    u32 ndefs = 0U;
    u32 subject;
    u32 i;
    bool has_dynamic = false;
    FlNode *static_site = NULL;
    EmbedCheckPath path[SYN_EMBED_CHECK_PATH_MAX];
    EmbedDepth depth;

    (void)memset(&sites, 0, sizeof(sites));
    interner_init(&in, arena);
    root = fl_parse_literal(arena, dc, &in, source, len, file_id);
    if (root == NULL) {
        free(sites.lang);
        interner_free(&in);
        return;
    }
    ast_embed_sites(root, &in, &sites);
    if (sites.n == 0U) {
        free(sites.lang);
        interner_free(&in);
        return;
    }
    defs = yew_xcalloc((size_t)installed + 1U, sizeof(*defs));
    for (i = 0U; i < installed; i++) {
        const SynLangDesc *desc = lang_at(i);
        const SynDef *candidate;

        if (desc == NULL)
            continue;
        candidate = yew_syn_def_for(desc->id);
        if (candidate == NULL ||
            (candidate->name != NULL && def->name != NULL &&
             strcmp(candidate->name, def->name) == 0))
            continue;
        defs[ndefs++] = candidate;
    }
    subject = ndefs;
    defs[ndefs++] = def;
    for (i = 0U; i < sites.n; i++) {
        FlNode *node = sites.lang[i];
        const char *selector;

        if (node->kind != FL_A_LIT || node->as.lit.lit != FL_L_STR)
            continue;
        selector = yew_intern_str(&in, node->as.lit.v.str_id);
        if (selector == NULL)
            continue;
        if (selector[0] == '@' && strcmp(selector, "@self") != 0) {
            has_dynamic = true;
            continue;
        }
        if (strcmp(selector, "@self") == 0) {
            if (static_site == NULL)
                static_site = node;
            continue;
        }
        if (embed_def_index(defs, ndefs, selector) < 0 &&
            !embed_closed_fallback(selector)) {
            fl_diag_emit(dc, FL_DIAG_ERROR,
                         embed_selector_span(node, &in, false),
                         "embed: no definition named '%s'", selector);
        } else if (static_site == NULL) {
            static_site = node;
        }
    }
    (void)memset(path, 0, sizeof(path));
    depth = embed_combined_depth_limit(
        defs, ndefs, subject, def->root, path, 0U, YEW_SYN_DEF_MAX, 0U,
        false, false);
    if (static_site != NULL && depth.frames > YEW_SYN_DEPTH_MAX) {
        char rendered[sizeof(depth.chain)];
        size_t at = 0U;
        const unsigned char *p = (const unsigned char *)depth.chain;

        while (*p != 0U && at + 4U < sizeof(rendered)) {
            if (*p == 0x1fU) {
                rendered[at++] = (char)0xe2;
                rendered[at++] = (char)0x86;
                rendered[at++] = (char)0x92;
            } else {
                rendered[at++] = (char)*p;
            }
            p++;
        }
        rendered[at] = '\0';
        fl_diag_emit(dc, FL_DIAG_ERROR,
                     embed_selector_span(static_site, &in, false),
                     "%s can reach depth %u; the cap is %u", rendered,
                     (unsigned)depth.frames,
                     (unsigned)YEW_SYN_DEPTH_MAX);
    }
    if (has_dynamic) {
        EmbedDepth dynamic_depth;

        (void)memset(path, 0, sizeof(path));
        dynamic_depth = embed_combined_depth_limit(
            defs, ndefs, subject, def->root, path, 0U, YEW_SYN_DEF_MAX,
            0U, true, true);
        if (dynamic_depth.frames > YEW_SYN_DEPTH_MAX) {
            char rendered[sizeof(dynamic_depth.chain)];
            size_t at = 0U;
            const unsigned char *p =
                (const unsigned char *)dynamic_depth.chain;

            while (*p != 0U && at + 4U < sizeof(rendered)) {
                if (*p == 0x1fU) {
                    rendered[at++] = (char)0xe2;
                    rendered[at++] = (char)0x86;
                    rendered[at++] = (char)0x92;
                } else {
                    rendered[at++] = (char)*p;
                }
                p++;
            }
            rendered[at] = '\0';
            for (i = 0U; i < sites.n; i++) {
                FlNode *node = sites.lang[i];
                const char *selector;

                if (node->kind != FL_A_LIT || node->as.lit.lit != FL_L_STR)
                    continue;
                selector = yew_intern_str(&in, node->as.lit.v.str_id);

                if (selector != NULL && selector[0] == '@' &&
                    strcmp(selector, "@self") != 0) {
                    fl_diag_emit(
                        dc, FL_DIAG_WARNING,
                        embed_selector_span(node, &in, true),
                        "dynamic embed has worst installed chain %s; cap exceeded by %u frame(s)",
                        rendered,
                        (unsigned)(dynamic_depth.frames -
                                   YEW_SYN_DEPTH_MAX));
                    break;
                }
            }
        }
    }
    free(defs);
    free(sites.lang);
    interner_free(&in);
}

static const char *op_name(u8 op)
{
    switch ((SynOp)op) {
    case SYN_OP_PUSH: return "push";
    case SYN_OP_POP: return "pop";
    case SYN_OP_SET: return "set";
    case SYN_OP_EMBED: return "embed";
    default: return "stay";
    }
}

static const char *embed_end_name(u8 end)
{
    switch ((SynEmbedEnd)end) {
    case SYN_EMBED_END_LINE: return "line";
    case SYN_EMBED_END_INLINE: return "inline";
    case SYN_EMBED_END_INLINE_ROOT: return "inline-root";
    case SYN_EMBED_END_LINE_CONTINUATION: return "line-continuation";
    default: return "none";
    }
}

static const char *aux_match_name(u8 match)
{
    switch ((SynAuxMatch)match) {
    case SYN_AUXM_LINE_EQ: return "line_eq";
    case SYN_AUXM_LITERAL: return "literal";
    case SYN_AUXM_FENCE_CLOSE: return "fence_close";
    case SYN_AUXM_INDENT_LT: return "indent_lt";
    default: return "none";
    }
}

static const char *unit_name(u8 flags)
{
    if ((flags & YEW_SYN_CTX_UNIT_ATOM) != 0U)
        return "atom";
    if ((flags & YEW_SYN_CTX_UNIT_SPAN) != 0U)
        return "span";
    return "none";
}

static const char *ctx_name(const SynDef *def, u16 id)
{
    const char *name = yew_syn_ctx_name(def, id);

    return name == NULL ? "<unknown>" : name;
}

static void coverage_reachable(const SynDef *def, bool *reachable)
{
    bool changed = true;

    reachable[def->root] = true;
    while (changed) {
        u16 i;

        changed = false;
        for (i = 0U; i < def->nctxs; i++) {
            const SynCtx *ctx;
            u32 j;

            if (!reachable[i])
                continue;
            ctx = &def->ctxs[i];
            if (ctx->at_eol == SYN_OP_SET && !reachable[ctx->eol_target]) {
                reachable[ctx->eol_target] = true;
                changed = true;
            }
            for (j = 0U; j < ctx->nrules; j++) {
                const SynRule *rule = &def->rules[ctx->first_rule + j];
                u8 k;

                if (rule->op == SYN_OP_SET ||
                    ((rule->op == SYN_OP_PUSH ||
                      rule->op == SYN_OP_EMBED) && rule->npush == 0U)) {
                    if (!reachable[rule->target]) {
                        reachable[rule->target] = true;
                        changed = true;
                    }
                }
                if (rule->op != SYN_OP_PUSH && rule->op != SYN_OP_EMBED)
                    continue;
                for (k = 0U; k < rule->npush; k++) {
                    if (!reachable[rule->push[k]]) {
                        reachable[rule->push[k]] = true;
                        changed = true;
                    }
                }
            }
        }
    }
}

static bool coverage_definition_embeds(const SynDef *def)
{
    u16 i;

    if (def == NULL)
        return false;
    for (i = 0U; i < def->nctxs; i++) {
        if ((def->ctxs[i].flags & YEW_SYN_CTX_EMBED_BRIDGE) != 0U)
            return true;
    }
    return false;
}

static u16 coverage_rule_context(const SynDef *def, u32 rule)
{
    u16 i;

    for (i = 0U; i < def->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];

        if (rule >= ctx->first_rule &&
            rule - ctx->first_rule < ctx->nrules)
            return i;
    }
    return UINT16_MAX;
}

static int coverage_scan_input(SynEngine *engine, const char *path)
{
    Arena arena;
    char *input;
    size_t input_len = 0U;
    size_t lo = 0U;
    u32 entry = YEW_SYN_STATE_ROOT;

    arena_init(&arena);
    input = slurp(path, &input_len, &arena);
    if (input == NULL) {
        arena_free_all(&arena);
        return YEW_EXIT_IO;
    }
    while (lo <= input_len) {
        size_t hi = lo;
        size_t line_len;
        SynLineOut out = {.spans = NULL, .cap = 0U};

        while (hi < input_len && input[hi] != '\n')
            hi++;
        line_len = hi - lo;
        if (line_len != 0U && input[lo + line_len - 1U] == '\r')
            line_len--;
        if (line_len > UINT32_MAX) {
            (void)fprintf(stderr, "yew syn: input line is too long: %s\n",
                          path);
            arena_free_all(&arena);
            return YEW_EXIT_IO;
        }
        yew_syn_line(engine, entry, (const u8 *)input + lo, (u32)line_len,
                     &out);
        entry = out.exit_state;
        if (hi == input_len)
            break;
        lo = hi + 1U;
    }
    arena_free_all(&arena);
    return YEW_EXIT_OK;
}

static int check_coverage(const SynDef *def, const char *const *inputs,
                          u32 ninputs)
{
    SynCoverage coverage;
    SynEngine *engine;
    bool *reachable;
    u32 covered_ctx = 0U;
    u32 total_ctx = 0U;
    u32 covered_rules = 0U;
    u32 total_rules = 0U;
    u32 covered_embeds = 0U;
    u32 total_embeds = 0U;
    u32 i;
    int status = YEW_EXIT_OK;

    if (ninputs == 0U)
        return usage_error("--coverage requires at least one INPUT");
    if (!yew_syn_coverage_init(&coverage, def))
        return YEW_EXIT_BUG;
    engine = yew_syn_engine_new((SynDef *)def);
    yew_syn_engine_set_coverage(engine, &coverage);
    for (i = 0U; i < ninputs; i++) {
        status = coverage_scan_input(engine, inputs[i]);
        if (status != YEW_EXIT_OK)
            break;
    }
    if (status == YEW_EXIT_OK && !coverage_definition_embeds(def)) {
        SynStateTab *states = yew_syn_engine_states(engine);
        u32 nstates = yew_syn_state_count(states);

        for (i = YEW_SYN_STATE_ROOT; i < nstates; i++) {
            const SynState *state = yew_syn_state_get(states, i);

            u8 depth;

            if (state == NULL || state->ndef != 1U) {
                (void)fprintf(stderr,
                              "yew syn: state %u has definition depth %u; "
                              "unexpected embed in non-embedding coverage\n",
                              (unsigned)i,
                              state == NULL ? 0U : (unsigned)state->ndef);
                status = YEW_EXIT_ERR;
                break;
            }
            for (depth = 0U; depth < state->depth; depth++) {
                if (state->f[depth].def != 0U) {
                    (void)fprintf(
                        stderr,
                        "yew syn: state %u frame %u has foreign definition %u in non-embedding coverage\n",
                        (unsigned)i, (unsigned)depth,
                        (unsigned)state->f[depth].def);
                    status = YEW_EXIT_ERR;
                    break;
                }
            }
            if (status != YEW_EXIT_OK)
                break;
        }
    }
    reachable = yew_xcalloc(def->nctxs, sizeof(*reachable));
    coverage_reachable(def, reachable);
    if (status == YEW_EXIT_OK) {
        for (i = 0U; i < def->nctxs; i++) {
            if (!reachable[i])
                continue;
            total_ctx++;
            if (coverage.contexts[i] != 0U) {
                covered_ctx++;
            } else {
                (void)fprintf(stderr, "yew syn: uncovered context %s\n",
                              ctx_name(def, (u16)i));
            }
        }
        for (i = 0U; i < def->nrules; i++) {
            u16 ctx = coverage_rule_context(def, i);
            const char *pattern;

            if (ctx == UINT16_MAX || !reachable[ctx])
                continue;
            total_rules++;
            if (coverage.rules[i] != 0U) {
                covered_rules++;
                continue;
            }
            pattern = yew_syn_rule_pattern(def, i);
            (void)fprintf(stderr,
                          "yew syn: uncovered rule %u context=%s %s=%s\n",
                          (unsigned)i, ctx_name(def, ctx),
                          pattern == NULL ? "aux" : "match",
                          pattern == NULL ? "<dynamic>" : pattern);
        }
        for (i = 0U; i < def->nctxs; i++) {
            if (!reachable[i] ||
                (def->ctxs[i].flags & YEW_SYN_CTX_EMBED_BRIDGE) == 0U)
                continue;
            total_embeds++;
            if (coverage.embeds[i] != 0U) {
                covered_embeds++;
            } else {
                (void)fprintf(stderr,
                              "yew syn: uncovered embed site %s:%s\n",
                              def->name == NULL ? "<unnamed>" : def->name,
                              ctx_name(def, (u16)i));
            }
        }
        (void)printf("coverage: contexts %u/%u, rules %u/%u, "
                     "embed sites %u/%u\n",
                     (unsigned)covered_ctx, (unsigned)total_ctx,
                     (unsigned)covered_rules, (unsigned)total_rules,
                     (unsigned)covered_embeds, (unsigned)total_embeds);
        if (covered_ctx != total_ctx || covered_rules != total_rules ||
            covered_embeds != total_embeds)
            status = YEW_EXIT_ERR;
    }
    free(reachable);
    yew_syn_engine_set_coverage(engine, NULL);
    yew_syn_engine_free(engine);
    yew_syn_coverage_free(&coverage);
    return status;
}

static void quote(Bytebuf *out, const char *s)
{
    const u8 *p = (const u8 *)(s == NULL ? "" : s);

    bytebuf_push_u8(out, (u8)'"');
    while (*p != 0U) {
        switch (*p) {
        case '\\': bytebuf_append(out, "\\\\", 2U); break;
        case '"': bytebuf_append(out, "\\\"", 2U); break;
        case '\n': bytebuf_append(out, "\\n", 2U); break;
        case '\r': bytebuf_append(out, "\\r", 2U); break;
        case '\t': bytebuf_append(out, "\\t", 2U); break;
        default:
            if (*p < 0x20U || *p == 0x7fU)
                bytebuf_printf(out, "\\x%02x", (unsigned)*p);
            else
                bytebuf_push_u8(out, *p);
            break;
        }
        p++;
    }
    bytebuf_push_u8(out, (u8)'"');
}

static void dump_first(Bytebuf *out, const u8 first[32])
{
    u32 i;

    bytebuf_append(out, " first=", 7U);
    for (i = 0U; i < 32U; i++)
        bytebuf_printf(out, "%02x", (unsigned)first[i]);
}

static void dump_rule(Bytebuf *out, const SynDef *def, u32 index)
{
    const SynRule *rule = &def->rules[index];
    u32 i;

    bytebuf_printf(out, "  rule %u match=", (unsigned)index);
    quote(out, yew_syn_rule_pattern(def, index));
    bytebuf_printf(out, " attr=%s op=%s", yew_syn_attr_name(rule->attr),
                   op_name(rule->op));
    if (rule->op == SYN_OP_PUSH || rule->op == SYN_OP_EMBED) {
        bytebuf_append(out, " target=", 8U);
        if (rule->npush != 0U) {
            for (i = 0U; i < rule->npush; i++) {
                if (i != 0U)
                    bytebuf_push_u8(out, (u8)',');
                bytebuf_append(out, ctx_name(def, rule->push[i]),
                               strlen(ctx_name(def, rule->push[i])));
            }
        } else {
            bytebuf_append(out, ctx_name(def, rule->target),
                           strlen(ctx_name(def, rule->target)));
        }
    } else if (rule->op == SYN_OP_SET) {
        bytebuf_printf(out, " target=%s", ctx_name(def, rule->target));
    } else if (rule->op == SYN_OP_POP) {
        bytebuf_printf(out, " count=%u", (unsigned)rule->nop);
    }
    if (rule->op == SYN_OP_EMBED) {
        bytebuf_append(out, " embed.lang=", 12U);
        if (rule->embed.lang_kind == SYN_EMBED_LANG_LITERAL)
            quote(out, yew_intern_str(def->aux, rule->embed.lang));
        else if (rule->embed.lang_kind == SYN_EMBED_LANG_CAPTURE)
            bytebuf_printf(out, "@%u", (unsigned)rule->embed.lang_group);
        else if (rule->embed.lang_kind == SYN_EMBED_LANG_SELF)
            bytebuf_append(out, "@self", 5U);
        else
            bytebuf_append(out, "<invalid>", 9U);
        bytebuf_append(out, " embed.ctx=", 11U);
        if (rule->embed.ctx == 0U)
            bytebuf_append(out, "<root>", 6U);
        else
            quote(out, yew_intern_str(def->aux, rule->embed.ctx));
        bytebuf_printf(out, " embed.end=%s embed.defer=%s embed.fallback=%s",
                       embed_end_name(rule->embed.end),
                       (rule->embed.flags & YEW_SYN_EMBED_DEFER) != 0U ?
                           "true" : "false",
                       yew_syn_attr_name(rule->embed.fallback));
    }
    if (rule->end != 0U)
        bytebuf_append(out, " end=true", 9U);
    bytebuf_printf(out, " consume=%u aux=%s", (unsigned)rule->consume,
                   aux_match_name(rule->aux_match));
    if (rule->flags != 0U)
        bytebuf_printf(out, " flags=0x%02x", (unsigned)rule->flags);
    for (i = 0U; i < YEW_ARRAY_LEN(rule->caps); i++) {
        if (rule->caps[i] != UINT8_MAX)
            bytebuf_printf(out, " capture.%u=%s", (unsigned)i,
                           yew_syn_attr_name(rule->caps[i]));
    }
    if (rule->aux_pre != 0U) {
        bytebuf_append(out, " aux_pre=", 9U);
        quote(out, yew_intern_str(def->aux, rule->aux_pre));
    }
    if (rule->aux_post != 0U) {
        bytebuf_append(out, " aux_post=", 10U);
        quote(out, yew_intern_str(def->aux, rule->aux_post));
    }
    dump_first(out, rule->first);
    bytebuf_push_u8(out, (u8)'\n');
}

static int dump_tables(const SynDef *def)
{
    Bytebuf out;
    u16 i;

    bytebuf_init(&out);
    bytebuf_printf(&out, "language %s\nroot %s\n", def->name,
                   ctx_name(def, def->root));
    for (i = 0U; i < def->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];
        u32 j;

        bytebuf_printf(&out, "context %s default=%s at_eol=%s unit=%s",
                       ctx_name(def, i), yew_syn_attr_name(ctx->dflt_attr),
                       op_name(ctx->at_eol), unit_name(ctx->flags));
        if (ctx->at_eol == SYN_OP_SET)
            bytebuf_printf(&out, " target=%s",
                           ctx_name(def, ctx->eol_target));
        else if (ctx->at_eol == SYN_OP_POP)
            bytebuf_printf(&out, " count=%u", (unsigned)ctx->eol_nop);
        if ((ctx->flags & YEW_SYN_CTX_EMBED_BRIDGE) != 0U) {
            bytebuf_printf(&out, " embed.end=%s embed.ctx=",
                           embed_end_name(ctx->embed.end));
            if (ctx->embed.ctx == 0U)
                bytebuf_append(&out, "<root>", 6U);
            else
                quote(&out, yew_intern_str(def->aux, ctx->embed.ctx));
            bytebuf_printf(&out, " embed.fallback=%s",
                           yew_syn_attr_name(ctx->embed.fallback));
        }
        dump_first(&out, ctx->first);
        bytebuf_push_u8(&out, (u8)'\n');
        for (j = 0U; j < ctx->nrules; j++)
            dump_rule(&out, def, ctx->first_rule + j);
    }
    if (!output(&out)) {
        bytebuf_free(&out);
        return YEW_EXIT_IO;
    }
    bytebuf_free(&out);
    return YEW_EXIT_OK;
}

static const char *qualified_frame(SynEngine *engine, const SynFrame *frame,
                                   char *out, size_t out_cap)
{
    const SynDef *def;
    const char *name;

    if (frame == NULL)
        return "<unknown>";
    def = yew_syn_engine_def_at(engine, frame->def);
    if (def == NULL || def->name == NULL)
        return "<unknown>";
    name = ctx_name(def, frame->ctx);
    if (snprintf(out, out_cap, "%s:%s", def->name, name) < 0)
        return "<unknown>";
    return out;
}

static const char *qualified_context(SynEngine *engine,
                                     const SynState *state, char *out,
                                     size_t out_cap)
{
    if (state == NULL || state->depth == 0U)
        return "<unknown>";
    return qualified_frame(engine, &state->f[state->depth - 1U], out,
                           out_cap);
}

static const char *state_context(SynEngine *engine, u32 state, char *out,
                                 size_t out_cap)
{
    const SynState *value = yew_syn_state_get(yew_syn_engine_states(engine),
                                              state);

    return qualified_context(engine, value, out, out_cap);
}

static int dump_spans(const SynDef *def, const char *path)
{
    Arena input_arena;
    char *input;
    size_t input_len = 0U;
    size_t lo = 0U;
    u32 line_no = 1U;
    u32 entry = YEW_SYN_STATE_ROOT;
    SynEngine *engine;
    SynBuf syn;
    TextBuf *tb;
    Bytebuf text;
    int status = YEW_EXIT_OK;

    arena_init(&input_arena);
    input = slurp(path, &input_len, &input_arena);
    if (input == NULL) {
        arena_free_all(&input_arena);
        return YEW_EXIT_IO;
    }
    engine = yew_syn_engine_new((SynDef *)def);
    tb = yew_textbuf_from_bytes((const u8 *)input, input_len);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, engine);
    {
        u32 lang = yew_syn_lang_named(def->name);
        u32 passes = 0U;
        bool loaded;
        SynSettleReport report;

        yew_syn_attach(&syn, lang == YEW_LANG_NONE ? 1U : lang, tb);
        do {
            yew_syn_settle(&syn, tb, LINENO(0U),
                           LINENO(yew_textbuf_line_count(tb)), INT64_MAX,
                           &report);
            loaded = yew_syn_embed_pump(&syn, engine,
                                        YEW_SYN_EMBED_LOAD_BUDGET_US);
            passes++;
        } while ((loaded || !report.fixpoint) && passes < 64U);
        if (passes == 64U) {
            (void)fputs("yew syn: embed settling did not converge\n", stderr);
            status = YEW_EXIT_BUG;
        }
    }
    bytebuf_init(&text);
    while (status == YEW_EXIT_OK && lo <= input_len) {
        size_t hi = lo;
        size_t line_len;
        SynSpan spans[YEW_SYN_MAX_SPANS];
        SynLineOut out = {.spans = spans, .cap = YEW_ARRAY_LEN(spans)};
        char entry_name[256];
        char exit_name[256];
        u32 i;

        while (hi < input_len && input[hi] != '\n')
            hi++;
        line_len = hi - lo;
        if (line_len != 0U && input[lo + line_len - 1U] == '\r')
            line_len--;
        if (line_len > UINT32_MAX) {
            (void)fprintf(stderr, "yew syn: input line %u is too long\n",
                          (unsigned)line_no);
            status = YEW_EXIT_IO;
            break;
        }
        if ((size_t)(line_no - 1U) < syn.entry.len)
            entry = syn.entry.data[line_no - 1U];
        yew_syn_line(engine, entry, (const u8 *)input + lo, (u32)line_len,
                     &out);
        bytebuf_printf(&text, "line %u entry=%s exit=%s\n",
                       (unsigned)line_no,
                       state_context(engine, entry, entry_name,
                                     sizeof(entry_name)),
                       state_context(engine, out.exit_state, exit_name,
                                     sizeof(exit_name)));
        for (i = 0U; i < out.n; i++) {
            SynState at;
            SynState after;
            char context_name[256];
            const char *context = state_context(engine, entry, context_name,
                                                sizeof(context_name));

            if (yew_syn_stack_at(engine, entry, (const u8 *)input + lo,
                                 (u32)line_len, spans[i].start, &at) &&
                at.depth != 0U) {
                context = qualified_context(engine, &at, context_name,
                                            sizeof(context_name));
                if (at.ndef > 1U &&
                    yew_syn_stack_at(engine, entry,
                                     (const u8 *)input + lo,
                                     (u32)line_len,
                                     spans[i].start + spans[i].len,
                                     &after) &&
                    after.ndef < at.ndef) {
                    u8 depth = at.depth;

                    while (depth != 0U) {
                        depth--;
                        if ((at.f[depth].fl & YEW_SYN_FR_BRIDGE) != 0U) {
                            context = qualified_frame(
                                engine, &at.f[depth], context_name,
                                sizeof(context_name));
                            break;
                        }
                    }
                }
            }
            bytebuf_printf(&text, "  %u:%u-%u attr=%s context=%s",
                           (unsigned)line_no, (unsigned)spans[i].start,
                           (unsigned)(spans[i].start + spans[i].len),
                           yew_syn_attr_name(spans[i].attr), context);
            if ((spans[i].flags & YEW_SPAN_TRUNCATED) != 0U)
                bytebuf_append(&text, " truncated", 10U);
            bytebuf_push_u8(&text, (u8)'\n');
        }
        entry = out.exit_state;
        if (hi == input_len)
            break;
        lo = hi + 1U;
        line_no++;
    }
    if (status == YEW_EXIT_OK && !output(&text))
        status = YEW_EXIT_IO;
    bytebuf_free(&text);
    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    yew_syn_engine_free(engine);
    arena_free_all(&input_arena);
    return status;
}

static int dump_def(const char *definition, const char *mode,
                    const char *input)
{
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    int status;

    arena_init(&arena);
    def = load_def(definition, &arena, &dc, &status);
    if (def != NULL) {
        status = strcmp(mode, "--tables") == 0 ? dump_tables(def) :
                 dump_spans(def, input);
        yew_syn_def_dispose(def);
    }
    arena_free_all(&arena);
    return status;
}

int yew_syn_main(int argc, char **argv, bool clean)
{
    int at = 1;

    if (at < argc && strcmp(argv[at], "--clean") == 0) {
        clean = true;
        at++;
    }
    yew_syn_cache_set_bypass(clean);
    yew_syn_discovery_set_bypass(clean);
    if (at >= argc || strcmp(argv[at], "--help") == 0) {
        (void)fputs(syn_usage, at >= argc ? stderr : stdout);
        return at >= argc ? YEW_EXIT_ERR : YEW_EXIT_OK;
    }
    if (strcmp(argv[at], "list") == 0)
        return at + 1 == argc ? list_defs(clean) :
               usage_error("list takes no arguments");
    if (strcmp(argv[at], "compile") == 0) {
        if (at + 2 != argc)
            return usage_error("compile requires --all or FILE");
        return strcmp(argv[at + 1], "--all") == 0 ? compile_all() :
               compile_one(argv[at + 1]);
    }
    if (strcmp(argv[at], "check") == 0) {
        bool strict = false;
        bool coverage = false;
        bool embed = false;
        const char *path = NULL;
        const char **inputs = yew_xcalloc((size_t)(argc - at),
                                          sizeof(*inputs));
        u32 ninputs = 0U;
        int result;
        int i;

        for (i = at + 1; i < argc; i++) {
            if (strcmp(argv[i], "--strict") == 0 && !strict)
                strict = true;
            else if (strcmp(argv[i], "--coverage") == 0 && !coverage)
                coverage = true;
            else if (strcmp(argv[i], "--embed") == 0 && !embed)
                embed = true;
            else if (path == NULL)
                path = argv[i];
            else if (coverage) {
                inputs[ninputs++] = argv[i];
            } else {
                free(inputs);
                return usage_error("check requires one FILE");
            }
        }
        if (embed) {
            free(inputs);
            if (coverage || path != NULL)
                return usage_error("--embed takes no FILE or coverage inputs");
            return check_embed(strict);
        }
        if (path == NULL) {
            free(inputs);
            return usage_error("check requires FILE");
        }
        if (coverage && ninputs == 0U) {
            free(inputs);
            return usage_error("--coverage requires at least one INPUT");
        }
        result = check_one(path, strict, coverage, inputs, ninputs);
        free(inputs);
        return result;
    }
    if (strcmp(argv[at], "dump") == 0) {
        if (at + 3 == argc && strcmp(argv[at + 2], "--tables") == 0)
            return dump_def(argv[at + 1], argv[at + 2], NULL);
        if (at + 4 == argc && strcmp(argv[at + 2], "--spans") == 0)
            return dump_def(argv[at + 1], argv[at + 2], argv[at + 3]);
        return usage_error("dump requires FILE --tables or FILE --spans INPUT");
    }
    if (strcmp(argv[at], "cache") == 0) {
        if (at + 2 != argc)
            return usage_error("cache requires clear or path");
        if (strcmp(argv[at + 1], "clear") == 0) {
            if (yew_syn_cache_clear())
                return YEW_EXIT_OK;
            else {
                (void)fputs("yew syn: cannot clear syntax cache\n", stderr);
                return YEW_EXIT_IO;
            }
        }
        if (strcmp(argv[at + 1], "path") == 0) {
            char *path = yew_syn_cache_dir();

            if (path == NULL) {
                (void)fputs("yew syn: cannot resolve syntax cache path\n",
                            stderr);
                return YEW_EXIT_IO;
            }
            (void)printf("%s\n", path);
            free(path);
            return ferror(stdout) == 0 ? YEW_EXIT_OK : YEW_EXIT_IO;
        }
        return usage_error("cache requires clear or path");
    }
    return usage_error("unknown command");
}
