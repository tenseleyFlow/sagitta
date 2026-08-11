#define _POSIX_C_SOURCE 200809L

#include "syn/defs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/ast.h"
#include "fl/parse.h"
#include "syn/langs_gen.h"
#include "syn/registry.h"
#include "text/file.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/sort.h"
#include "util/xdg.h"

enum {
    SYN_DEF_MAX_CONTEXTS = 4096,
    SYN_DEF_MAX_RULES = 65536,
    SYN_DEF_MAX_DIAGS = 64,
    SYN_DEF_STATIC_DEPTH = 12,
    SYN_DEF_MAX_BYTES = 64 * 1024 * 1024
};

static const char *const attr_names[YEW_ATTR__COUNT] = {
    "text", "keyword", "keyword.control", "keyword.op",
    "keyword.storage", "type", "type.builtin", "constant",
    "constant.builtin", "number", "boolean", "character", "string",
    "string.escape", "string.interp", "string.special", "comment",
    "comment.doc", "comment.todo", "function",
    "function.builtin", "function.macro", "method", "variable",
    "variable.builtin", "variable.param", "variable.member",
    "namespace", "label", "attribute", "preproc", "operator",
    "punct", "punct.bracket", "punct.delim", "tag",
    "tag.attr", "heading", "link", "emphasis", "strong", "code",
    "list", "quote", "diff.add", "diff.del", "error", "warning",
    "whitespace.special", "motion.unit", "motion.arrow", "motion.count",
    "motion.cmd", "ui.invisible"
};

typedef struct AliasSpec {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
    FlSpan sp;
} AliasSpec;

typedef struct CtxSpec {
    const char *name;
    size_t name_len;
    FlNode *node;
    FlNode *rules;
    FlNode *include;
    FlNode *default_node;
    FlNode *at_eol_node;
    FlNode *unit_node;
    bool icase;
    bool include_used;
    bool reachable;
    bool pushed;
    bool embed_set;
    SynEmbed embed;
} CtxSpec;

typedef struct RuleRef {
    FlNode *node;
    u16 owner;
} RuleRef;

typedef struct RuleVec {
    RuleRef *data;
    u32 len;
    u32 cap;
} RuleVec;

typedef struct Compile {
    Arena *arena;
    DiagCtx *dc;
    Interner in;
    Interner *aux;
    const u8 *src;
    size_t src_len;
    u32 file_id;
    u32 errors;
    u32 warnings;
    AliasSpec *aliases;
    u32 naliases;
    CtxSpec *ctxs;
    u32 nctxs;
    u16 root;
} Compile;

typedef struct DefMeta {
    SynDef *def;
    SynLangDesc lang;
    const char **ctx_names;
    const char **patterns;
    YewRe *first_line_re;
    Interner *aux;
    SynEngine *engine;
    bool builtin;
    bool retired;
    struct DefMeta *next;
    struct DefMeta *user_next;
} DefMeta;

typedef struct DiscoveredDef {
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    u32 pins;
    bool retired;
    struct DiscoveredDef *next;
} DiscoveredDef;

static DefMeta *metas;
static DefMeta *user_metas;
static DiscoveredDef *discovered_defs;
static DiscoveredDef *retired_defs;
static u64 compile_count;
static bool cache_bypass;
static bool discovery_done;
static bool discovery_bypass;
static BuiltinRegistry builtin_registry;

static void discover_user_definitions(void);
static bool builtin_name_exists(const char *name);

static void meta_link(DefMeta *meta)
{
    meta->next = metas;
    metas = meta;
    if (!meta->builtin) {
        meta->user_next = user_metas;
        user_metas = meta;
    }
}

static void meta_unlink_user(DefMeta *meta)
{
    DefMeta **link;

    if (meta->builtin)
        return;
    link = &user_metas;
    while (*link != NULL) {
        if (*link == meta) {
            *link = meta->user_next;
            return;
        }
        link = &(*link)->user_next;
    }
}

static DefMeta *meta_for(const SynDef *def)
{
    DefMeta *m;

    for (m = metas; m != NULL; m = m->next) {
        if (m->def == def)
            return m;
    }
    return NULL;
}

static DiscoveredDef *discovered_owned(const SynDef *def,
                                       DiscoveredDef ***link_out)
{
    DiscoveredDef **lists[] = {&discovered_defs, &retired_defs};
    u32 list;

    for (list = 0U; list < YEW_ARRAY_LEN(lists); list++) {
        DiscoveredDef **link = lists[list];

        while (*link != NULL) {
            if ((*link)->def == def) {
                if (link_out != NULL)
                    *link_out = link;
                return *link;
            }
            link = &(*link)->next;
        }
    }
    return NULL;
}

static void discovered_owned_free(DiscoveredDef *owned)
{
    yew_syn_def_dispose(owned->def);
    arena_free_all(&owned->arena);
    free(owned);
}

void yew_syn_def_pin(const SynDef *def)
{
    DiscoveredDef *owned = discovered_owned(def, NULL);

    if (owned != NULL) {
        if (owned->pins == UINT32_MAX)
            YEW_BUG("syntax: discovered definition pin overflow");
        owned->pins++;
    }
}

void yew_syn_def_unpin(const SynDef *def)
{
    DiscoveredDef **link = NULL;
    DiscoveredDef *owned = discovered_owned(def, &link);

    if (owned == NULL)
        return;
    if (owned->pins == 0U)
        YEW_BUG("syntax: discovered definition pin underflow");
    owned->pins--;
    if (owned->pins == 0U && owned->retired && link != NULL) {
        *link = owned->next;
        discovered_owned_free(owned);
    }
}

const char *yew_syn_attr_name(u8 attr)
{
    return attr < YEW_ATTR__COUNT ? attr_names[attr] : NULL;
}

bool yew_syn_attr_id(const char *name, size_t n, u8 *out)
{
    u32 i;

    if (name == NULL)
        return false;
    for (i = 0U; i < YEW_ATTR__COUNT; i++) {
        if (strlen(attr_names[i]) == n && memcmp(attr_names[i], name, n) == 0) {
            if (out != NULL)
                *out = (u8)i;
            return true;
        }
    }
    return false;
}

static void diag(Compile *c, FlDiagLevel level, FlSpan sp,
                 const char *fmt, ...)
{
    va_list ap;

    if (c->errors + c->warnings >= SYN_DEF_MAX_DIAGS)
        return;
    if (level == FL_DIAG_ERROR)
        c->errors++;
    else if (level == FL_DIAG_WARNING)
        c->warnings++;
    va_start(ap, fmt);
    fl_diag_vemit(c->dc, level, sp, fmt, ap);
    va_end(ap);
}

static bool node_is(const FlNode *n, FlAstKind kind)
{
    return n != NULL && n->kind == (u8)kind;
}

static bool lit_is(const FlNode *n, FlLitKind kind)
{
    return node_is(n, FL_A_LIT) && n->as.lit.lit == (u8)kind;
}

static const char *node_str(const Compile *c, const FlNode *n, size_t *len)
{
    if (!lit_is(n, FL_L_STR))
        return NULL;
    if (len != NULL)
        *len = yew_intern_len(&c->in, n->as.lit.v.str_id);
    return yew_intern_str(&c->in, n->as.lit.v.str_id);
}

static bool text_eq(const char *a, size_t an, const char *b)
{
    return strlen(b) == an && (an == 0U || memcmp(a, b, an) == 0);
}

static const char *key_str(const Compile *c, const FlNode *key, size_t *len)
{
    return node_str(c, key, len);
}

static FlNode *map_find(const Compile *c, const FlNode *map,
                        const char *want)
{
    u32 i;

    if (!node_is(map, FL_A_MAP))
        return NULL;
    for (i = 0U; i < map->as.map.n; i++) {
        size_t n = 0U;
        const char *key = key_str(c, map->as.map.keys[i], &n);

        if (key != NULL && text_eq(key, n, want))
            return map->as.map.vals[i];
    }
    return NULL;
}

static u32 edit_distance(const char *a, size_t an, const char *b, size_t bn)
{
    u32 row[96];
    u32 next[96];
    size_t i;
    size_t j;

    if (an >= YEW_ARRAY_LEN(row) || bn >= YEW_ARRAY_LEN(row))
        return UINT32_MAX;
    for (j = 0U; j <= bn; j++)
        row[j] = (u32)j;
    for (i = 0U; i < an; i++) {
        next[0] = (u32)(i + 1U);
        for (j = 0U; j < bn; j++) {
            u32 sub = row[j] + (a[i] == b[j] ? 0U : 1U);
            u32 del = row[j + 1U] + 1U;
            u32 ins = next[j] + 1U;

            next[j + 1U] = sub < del ? sub : del;
            if (ins < next[j + 1U])
                next[j + 1U] = ins;
        }
        (void)memcpy(row, next, (bn + 1U) * sizeof(*row));
    }
    return row[bn];
}

static const char *suggest(const char *key, size_t n,
                           const char *const *known, u32 nknown)
{
    const char *best = NULL;
    u32 best_dist = 3U;
    u32 i;

    for (i = 0U; i < nknown; i++) {
        u32 d = edit_distance(key, n, known[i], strlen(known[i]));
        if (d < best_dist) {
            best = known[i];
            best_dist = d;
        }
    }
    return best;
}

static bool key_known(const char *key, size_t n,
                      const char *const *known, u32 nknown)
{
    u32 i;

    for (i = 0U; i < nknown; i++) {
        if (text_eq(key, n, known[i]))
            return true;
    }
    return false;
}

static void validate_keys(Compile *c, const FlNode *map,
                          const char *const *known, u32 nknown)
{
    u32 i;

    if (!node_is(map, FL_A_MAP))
        return;
    for (i = 0U; i < map->as.map.n; i++) {
        size_t n = 0U;
        FlNode *kn = map->as.map.keys[i];
        const char *key = key_str(c, kn, &n);
        const char *maybe;

        if (key == NULL) {
            diag(c, FL_DIAG_ERROR, kn->sp, "map key must be a string or name");
            continue;
        }
        if (key_known(key, n, known, nknown))
            continue;
        if (text_eq(key, n, "embed")) {
            diag(c, FL_DIAG_ERROR, kn->sp,
                 "'embed' is only valid on a rule");
            continue;
        }
        maybe = suggest(key, n, known, nknown);
        if (maybe != NULL)
            diag(c, FL_DIAG_ERROR, kn->sp,
                 "unknown key '%.*s' (did you mean '%s'?)", (int)n, key,
                 maybe);
        else
            diag(c, FL_DIAG_ERROR, kn->sp, "unknown key '%.*s'", (int)n,
                 key);
    }
}

static bool require_map(Compile *c, FlNode *n, const char *what)
{
    if (node_is(n, FL_A_MAP))
        return true;
    diag(c, FL_DIAG_ERROR, n == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U}
                                     : n->sp,
         "%s must be a map", what);
    return false;
}

static bool require_string(Compile *c, FlNode *n, const char *what,
                           const char **out, size_t *out_len)
{
    const char *s = node_str(c, n, out_len);

    if (s != NULL) {
        if (out != NULL)
            *out = s;
        return true;
    }
    diag(c, FL_DIAG_ERROR, n == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U}
                                     : n->sp,
         "%s must be a string", what);
    return false;
}

static bool require_bool(Compile *c, FlNode *n, const char *what, bool *out)
{
    if (lit_is(n, FL_L_BOOL)) {
        *out = n->as.lit.v.b;
        return true;
    }
    diag(c, FL_DIAG_ERROR, n == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U}
                                     : n->sp,
         "%s must be a boolean", what);
    return false;
}

static bool require_int(Compile *c, FlNode *n, const char *what, i64 *out)
{
    if (lit_is(n, FL_L_INT)) {
        *out = n->as.lit.v.i;
        return true;
    }
    diag(c, FL_DIAG_ERROR, n == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U}
                                     : n->sp,
         "%s must be an integer", what);
    return false;
}

static i32 ctx_index(const Compile *c, const char *name, size_t n)
{
    u32 i;

    for (i = 0U; i < c->nctxs; i++) {
        if (c->ctxs[i].name_len == n &&
            (n == 0U || memcmp(c->ctxs[i].name, name, n) == 0))
            return (i32)i;
    }
    return -1;
}

static bool attr_resolve(Compile *c, FlNode *node, u8 *out)
{
    const char *name;
    size_t n;
    u32 i;
    const char *maybe = NULL;
    u32 best = 3U;

    if (!require_string(c, node, "attr", &name, &n))
        return false;
    for (i = 0U; i < c->naliases; i++) {
        if (c->aliases[i].name_len == n &&
            memcmp(c->aliases[i].name, name, n) == 0) {
            name = c->aliases[i].value;
            n = c->aliases[i].value_len;
            break;
        }
    }
    if (yew_syn_attr_id(name, n, out))
        return true;
    for (i = 0U; i < YEW_ATTR__COUNT; i++) {
        u32 d = edit_distance(name, n, attr_names[i], strlen(attr_names[i]));
        if (d < best) {
            best = d;
            maybe = attr_names[i];
        }
    }
    if (maybe != NULL)
        diag(c, FL_DIAG_ERROR, node->sp,
             "unknown attr '%.*s' (did you mean '%s'?)", (int)n, name,
             maybe);
    else
        diag(c, FL_DIAG_ERROR, node->sp, "unknown attr '%.*s'", (int)n,
             name);
    return false;
}

static void parse_aliases(Compile *c, FlNode *node)
{
    u32 i;

    if (node == NULL)
        return;
    if (!require_map(c, node, "attrs"))
        return;
    c->aliases = arena_alloc(c->arena,
                             (size_t)node->as.map.n * sizeof(*c->aliases),
                             _Alignof(AliasSpec));
    for (i = 0U; i < node->as.map.n; i++) {
        FlNode *kn = node->as.map.keys[i];
        FlNode *vn = node->as.map.vals[i];
        const char *key;
        const char *value;
        size_t key_len;
        size_t value_len;
        u32 j;

        key = key_str(c, kn, &key_len);
        if (key == NULL || !require_string(c, vn, "attr alias value", &value,
                                           &value_len))
            continue;
        for (j = 0U; j < c->naliases; j++) {
            if (c->aliases[j].name_len == key_len &&
                memcmp(c->aliases[j].name, key, key_len) == 0) {
                diag(c, FL_DIAG_ERROR, kn->sp,
                     "duplicate attr alias '%.*s'", (int)key_len, key);
                break;
            }
        }
        if (j != c->naliases)
            continue;
        c->aliases[c->naliases++] =
            (AliasSpec){key, key_len, value, value_len, vn->sp};
    }
    for (i = 0U; i < c->naliases; i++) {
        u8 ignored;

        if (!yew_syn_attr_id(c->aliases[i].value,
                             c->aliases[i].value_len, &ignored)) {
            diag(c, FL_DIAG_ERROR, c->aliases[i].sp,
                 "unknown attr '%.*s'", (int)c->aliases[i].value_len,
                 c->aliases[i].value);
        }
    }
}

static void parse_contexts(Compile *c, FlNode *node)
{
    static const char *const keys[] = {
        "rules", "default", "at_eol", "icase", "unit", "include"
    };
    u32 i;

    if (!require_map(c, node, "contexts"))
        return;
    if (node->as.map.n == 0U) {
        diag(c, FL_DIAG_ERROR, node->sp, "contexts must not be empty");
        return;
    }
    if (node->as.map.n > SYN_DEF_MAX_CONTEXTS) {
        diag(c, FL_DIAG_ERROR, node->sp, "too many contexts (max %u)",
             SYN_DEF_MAX_CONTEXTS);
        return;
    }
    c->ctxs = arena_alloc(c->arena,
                          (size_t)node->as.map.n * sizeof(*c->ctxs),
                          _Alignof(CtxSpec));
    (void)memset(c->ctxs, 0, (size_t)node->as.map.n * sizeof(*c->ctxs));
    for (i = 0U; i < node->as.map.n; i++) {
        FlNode *kn = node->as.map.keys[i];
        FlNode *vn = node->as.map.vals[i];
        const char *name;
        size_t name_len;
        u32 j;
        bool b;

        name = key_str(c, kn, &name_len);
        if (name == NULL || !require_map(c, vn, "context"))
            continue;
        for (j = 0U; j < c->nctxs; j++) {
            if (c->ctxs[j].name_len == name_len &&
                memcmp(c->ctxs[j].name, name, name_len) == 0) {
                diag(c, FL_DIAG_ERROR, kn->sp,
                     "duplicate context '%.*s'", (int)name_len, name);
                break;
            }
        }
        if (j != c->nctxs)
            continue;
        validate_keys(c, vn, keys, (u32)YEW_ARRAY_LEN(keys));
        c->ctxs[c->nctxs].name = name;
        c->ctxs[c->nctxs].name_len = name_len;
        c->ctxs[c->nctxs].node = vn;
        c->ctxs[c->nctxs].rules = map_find(c, vn, "rules");
        c->ctxs[c->nctxs].include = map_find(c, vn, "include");
        c->ctxs[c->nctxs].default_node = map_find(c, vn, "default");
        c->ctxs[c->nctxs].at_eol_node = map_find(c, vn, "at_eol");
        c->ctxs[c->nctxs].unit_node = map_find(c, vn, "unit");
        if (map_find(c, vn, "icase") != NULL &&
            require_bool(c, map_find(c, vn, "icase"), "context icase", &b))
            c->ctxs[c->nctxs].icase = b;
        if (c->ctxs[c->nctxs].rules != NULL &&
            !node_is(c->ctxs[c->nctxs].rules, FL_A_LIST))
            diag(c, FL_DIAG_ERROR, c->ctxs[c->nctxs].rules->sp,
                 "rules must be a list");
        c->nctxs++;
    }
}

static bool parse_string_list(Compile *c, FlNode *node, const char *what,
                              const char ***out, u32 *nout)
{
    const char **list;
    u32 i;

    *out = NULL;
    *nout = 0U;
    if (node == NULL)
        return true;
    if (!node_is(node, FL_A_LIST)) {
        diag(c, FL_DIAG_ERROR, node->sp, "%s must be a list", what);
        return false;
    }
    list = arena_alloc(c->arena,
                       (size_t)node->as.list.n * sizeof(*list),
                       _Alignof(const char *));
    for (i = 0U; i < node->as.list.n; i++) {
        const char *s;
        size_t n;

        if (!require_string(c, node->as.list.items[i], what, &s, &n))
            continue;
        if (memchr(s, '\0', n) != NULL) {
            diag(c, FL_DIAG_ERROR, node->as.list.items[i]->sp,
                 "%s entries may not contain NUL", what);
            continue;
        }
        list[(*nout)++] = s;
    }
    *out = list;
    return true;
}

static void parse_comment(Compile *c, FlNode *node, SynComment *out)
{
    static const char *const keys[] = {"line", "block"};
    FlNode *line;
    FlNode *block;
    size_t ignored;

    if (node == NULL)
        return;
    if (!require_map(c, node, "language.comment"))
        return;
    validate_keys(c, node, keys, (u32)YEW_ARRAY_LEN(keys));
    line = map_find(c, node, "line");
    block = map_find(c, node, "block");
    if (line != NULL)
        (void)require_string(c, line, "comment.line", &out->line, &ignored);
    if (block != NULL) {
        if (!node_is(block, FL_A_LIST) || block->as.list.n != 2U) {
            diag(c, FL_DIAG_ERROR, block->sp,
                 "comment.block must be a two-string list");
        } else {
            (void)require_string(c, block->as.list.items[0],
                                 "comment block opener", &out->block_open,
                                 &ignored);
            (void)require_string(c, block->as.list.items[1],
                                 "comment block closer", &out->block_close,
                                 &ignored);
        }
    }
}

static void parse_language(Compile *c, FlNode *node, SynLangDesc *lang)
{
    static const char *const keys[] = {
        "name", "extensions", "filenames", "shebangs", "first_line",
        "priority", "comment"
    };
    FlNode *name;
    FlNode *first;
    FlNode *priority;
    size_t ignored;
    i64 value;

    if (!require_map(c, node, "language"))
        return;
    validate_keys(c, node, keys, (u32)YEW_ARRAY_LEN(keys));
    name = map_find(c, node, "name");
    if (name == NULL) {
        diag(c, FL_DIAG_ERROR, node->sp, "language.name is required");
    } else {
        if (require_string(c, name, "language.name", &lang->name, &ignored) &&
            (ignored == 0U || strchr(lang->name, '/') != NULL ||
             strcmp(lang->name, ".") == 0 || strcmp(lang->name, "..") == 0)) {
            diag(c, FL_DIAG_ERROR, name->sp,
                 "language.name must be a non-empty cache-safe name");
            lang->name = NULL;
        }
    }
    (void)parse_string_list(c, map_find(c, node, "extensions"),
                            "language.extensions", &lang->extensions,
                            &lang->nextensions);
    (void)parse_string_list(c, map_find(c, node, "filenames"),
                            "language.filenames", &lang->filenames,
                            &lang->nfilenames);
    (void)parse_string_list(c, map_find(c, node, "shebangs"),
                            "language.shebangs", &lang->shebangs,
                            &lang->nshebangs);
    first = map_find(c, node, "first_line");
    if (first != NULL)
        (void)require_string(c, first, "language.first_line",
                             &lang->first_line, &ignored);
    priority = map_find(c, node, "priority");
    if (priority != NULL && require_int(c, priority, "language.priority",
                                        &value)) {
        if (value < INT32_MIN || value > INT32_MAX)
            diag(c, FL_DIAG_ERROR, priority->sp,
                 "language.priority is out of range");
        else
            lang->priority = (i32)value;
    }
    parse_comment(c, map_find(c, node, "comment"), &lang->comment);
}

static bool rulevec_push(Compile *c, RuleVec *v, FlNode *node, u16 owner)
{
    RuleRef *grown;
    u32 cap;

    if (v->len == SYN_DEF_MAX_RULES) {
        diag(c, FL_DIAG_ERROR, node->sp, "too many expanded rules (max %u)",
             SYN_DEF_MAX_RULES);
        return false;
    }
    if (v->len == v->cap) {
        cap = v->cap == 0U ? 16U : v->cap * 2U;
        if (cap > SYN_DEF_MAX_RULES)
            cap = SYN_DEF_MAX_RULES;
        grown = arena_alloc(c->arena, (size_t)cap * sizeof(*grown),
                            _Alignof(RuleRef));
        if (v->len != 0U)
            (void)memcpy(grown, v->data,
                         (size_t)v->len * sizeof(*grown));
        v->data = grown;
        v->cap = cap;
    }
    v->data[v->len++] = (RuleRef){node, owner};
    return true;
}

static bool include_name(Compile *c, RuleVec *out, const char *name, size_t n,
                         FlSpan sp, u8 *visiting);

static bool expand_context(Compile *c, RuleVec *out, u16 id, FlSpan via,
                           u8 *visiting)
{
    CtxSpec *ctx;
    u32 i;

    if (id >= c->nctxs)
        return false;
    if (visiting[id] != 0U) {
        diag(c, FL_DIAG_ERROR, via, "include cycle reaches '%.*s'",
             (int)c->ctxs[id].name_len, c->ctxs[id].name);
        return false;
    }
    visiting[id] = 1U;
    ctx = &c->ctxs[id];
    if (ctx->include != NULL) {
        if (lit_is(ctx->include, FL_L_STR)) {
            size_t n;
            const char *name = node_str(c, ctx->include, &n);

            (void)include_name(c, out, name, n, ctx->include->sp, visiting);
        } else if (node_is(ctx->include, FL_A_LIST)) {
            for (i = 0U; i < ctx->include->as.list.n; i++) {
                FlNode *item = ctx->include->as.list.items[i];
                size_t n;
                const char *name = node_str(c, item, &n);

                if (name == NULL)
                    diag(c, FL_DIAG_ERROR, item->sp,
                         "context include entries must be strings");
                else
                    (void)include_name(c, out, name, n, item->sp, visiting);
            }
        } else {
            diag(c, FL_DIAG_ERROR, ctx->include->sp,
                 "context include must be a string or list");
        }
    }
    if (node_is(ctx->rules, FL_A_LIST)) {
        for (i = 0U; i < ctx->rules->as.list.n; i++) {
            FlNode *item = ctx->rules->as.list.items[i];

            if (node_is(item, FL_A_MAP)) {
                (void)rulevec_push(c, out, item, id);
            } else if (lit_is(item, FL_L_STR)) {
                size_t n;
                const char *text = node_str(c, item, &n);

                if (n > 8U && memcmp(text, "include:", 8U) == 0)
                    (void)include_name(c, out, text + 8U, n - 8U,
                                       item->sp, visiting);
                else
                    diag(c, FL_DIAG_ERROR, item->sp,
                         "rule-list string must be 'include:NAME'");
            } else {
                diag(c, FL_DIAG_ERROR, item->sp,
                     "rule must be a map or 'include:NAME'");
            }
        }
    }
    visiting[id] = 0U;
    return true;
}

static bool include_name(Compile *c, RuleVec *out, const char *name, size_t n,
                         FlSpan sp, u8 *visiting)
{
    i32 id = ctx_index(c, name, n);

    if (id < 0) {
        diag(c, FL_DIAG_ERROR, sp, "no context named '%.*s'", (int)n, name);
        return false;
    }
    c->ctxs[id].include_used = true;
    return expand_context(c, out, (u16)id, sp, visiting);
}

static bool context_target(Compile *c, FlNode *node, const char *what,
                           u16 *out)
{
    const char *name;
    size_t n;
    i32 id;

    if (!require_string(c, node, what, &name, &n))
        return false;
    id = ctx_index(c, name, n);
    if (id < 0) {
        diag(c, FL_DIAG_ERROR, node->sp, "no context named '%.*s'",
             (int)n, name);
        return false;
    }
    *out = (u16)id;
    return true;
}

static void first_union(u8 dst[32], const u8 src[32])
{
    u32 i;

    for (i = 0U; i < 32U; i++)
        dst[i] |= src[i];
}

static bool parse_pop(Compile *c, FlNode *node, u8 *out)
{
    i64 value;

    if (lit_is(node, FL_L_BOOL)) {
        if (!node->as.lit.v.b) {
            diag(c, FL_DIAG_ERROR, node->sp,
                 "pop: false is not an operation; omit pop instead");
            return false;
        }
        *out = 1U;
        return true;
    }
    if (!require_int(c, node, "pop", &value))
        return false;
    if (value < 1 || value > 4) {
        diag(c, FL_DIAG_ERROR, node->sp, "pop count %lld is out of range (1-4)",
             (long long)value);
        return false;
    }
    *out = (u8)value;
    return true;
}

static void compile_captures(Compile *c, FlNode *node, SynRule *rule,
                             u32 groups)
{
    u32 i;

    if (node == NULL)
        return;
    if (!node_is(node, FL_A_MAP)) {
        diag(c, FL_DIAG_ERROR, node->sp, "captures must be a map");
        return;
    }
    for (i = 0U; i < node->as.map.n; i++) {
        FlNode *key = node->as.map.keys[i];
        i64 group;
        u8 attr;

        if (!require_int(c, key, "capture group", &group))
            continue;
        if (group < 0 || group > 7) {
            diag(c, FL_DIAG_ERROR, key->sp,
                 "capture group %lld is out of range (0-7)",
                 (long long)group);
            continue;
        }
        if ((u64)group >= groups) {
            diag(c, FL_DIAG_ERROR, key->sp,
                 "capture group %lld but the pattern has %u capture groups",
                 (long long)group, groups == 0U ? 0U : groups - 1U);
            continue;
        }
        if (attr_resolve(c, node->as.map.vals[i], &attr))
            rule->caps[group] = attr;
    }
}

static u8 aux_kind(Compile *c, FlNode *node)
{
    const char *name;
    size_t n;

    if (!require_string(c, node, "aux", &name, &n))
        return SYN_AUXM_NONE;
    if (text_eq(name, n, "line_eq"))
        return SYN_AUXM_LINE_EQ;
    if (text_eq(name, n, "literal"))
        return SYN_AUXM_LITERAL;
    if (text_eq(name, n, "fence_close"))
        return SYN_AUXM_FENCE_CLOSE;
    if (text_eq(name, n, "indent_lt"))
        return SYN_AUXM_INDENT_LT;
    if (text_eq(name, n, "line_empty"))
        return SYN_AUXM_LINE_EMPTY;
    if (text_eq(name, n, "line_start"))
        return SYN_AUXM_LINE_START;
    diag(c, FL_DIAG_ERROR, node->sp, "unknown aux matcher '%.*s'", (int)n,
         name);
    return SYN_AUXM_NONE;
}

static bool embed_boundary_same(const SynEmbed *a, const SynEmbed *b)
{
    return a->ctx == b->ctx && a->end == b->end &&
           a->fallback == b->fallback;
}

static void compile_embed(Compile *c, FlNode *node, FlNode *push,
                          u16 target, u32 groups, SynEmbed *out)
{
    static const char *const keys[] = {
        "lang", "ctx", "end", "defer", "fallback", "interleave"
    };
    SynEmbed desc;
    FlNode *lang;
    FlNode *ctx;
    FlNode *end;
    FlNode *defer;
    FlNode *fallback;
    FlNode *interleave;
    const char *text;
    size_t n;
    u32 errors_before = c->errors;

    (void)memset(&desc, 0, sizeof(desc));
    (void)memset(out, 0, sizeof(*out));
    if (!require_map(c, node, "embed"))
        return;
    validate_keys(c, node, keys, (u32)YEW_ARRAY_LEN(keys));
    lang = map_find(c, node, "lang");
    ctx = map_find(c, node, "ctx");
    end = map_find(c, node, "end");
    defer = map_find(c, node, "defer");
    fallback = map_find(c, node, "fallback");
    interleave = map_find(c, node, "interleave");
    if (interleave != NULL)
        diag(c, FL_DIAG_ERROR, interleave->sp,
             "embed.interleave is deferred until after 1.0");

    if (push == NULL) {
        diag(c, FL_DIAG_ERROR, node->sp,
             "embed requires exactly one string 'push' bridge target");
    } else if (!lit_is(push, FL_L_STR)) {
        diag(c, FL_DIAG_ERROR, push->sp,
             "embed bridge 'push' must be one context name, not a list");
    }

    if (require_string(c, lang, "embed.lang", &text, &n)) {
        if (n == 0U || memchr(text, '\0', n) != NULL) {
            diag(c, FL_DIAG_ERROR, lang->sp,
                 "embed.lang must be a non-empty string without NUL");
        } else if (text_eq(text, n, "@self")) {
            desc.lang_kind = SYN_EMBED_LANG_SELF;
        } else if (text[0] == '@') {
            u32 group = 0U;
            size_t i;

            if (n == 1U) {
                group = UINT32_MAX;
            } else {
                for (i = 1U; i < n; i++) {
                    if (text[i] < '0' || text[i] > '9' ||
                        group > (UINT32_MAX - 9U) / 10U) {
                        group = UINT32_MAX;
                        break;
                    }
                    group = group * 10U + (u32)(text[i] - '0');
                }
            }
            if (group == 0U || group == UINT32_MAX) {
                diag(c, FL_DIAG_ERROR, lang->sp,
                     "embed.lang reference must be '@self' or '@N'");
            } else if (group > 7U || group >= groups) {
                diag(c, FL_DIAG_ERROR, lang->sp,
                     "embed.lang: @%u but the pattern has %u capture groups",
                     group, groups == 0U ? 0U : groups - 1U);
            } else {
                desc.lang_kind = SYN_EMBED_LANG_CAPTURE;
                desc.lang_group = (u8)group;
            }
        } else {
            desc.lang_kind = SYN_EMBED_LANG_LITERAL;
            desc.lang = yew_intern(c->aux, text, n);
        }
    }
    if (ctx != NULL && require_string(c, ctx, "embed.ctx", &text, &n)) {
        if (n == 0U || memchr(text, '\0', n) != NULL)
            diag(c, FL_DIAG_ERROR, ctx->sp,
                 "embed.ctx must be a non-empty string without NUL");
        else
            desc.ctx = yew_intern(c->aux, text, n);
    }
    if (require_string(c, end, "embed.end", &text, &n)) {
        if (text_eq(text, n, "line"))
            desc.end = SYN_EMBED_END_LINE;
        else if (text_eq(text, n, "inline"))
            desc.end = SYN_EMBED_END_INLINE;
        else if (text_eq(text, n, "inline-root"))
            desc.end = SYN_EMBED_END_INLINE_ROOT;
        else if (text_eq(text, n, "line-continuation"))
            desc.end = SYN_EMBED_END_LINE_CONTINUATION;
        else
            diag(c, FL_DIAG_ERROR, end->sp,
                 "embed.end must be 'line', 'inline', 'inline-root', or 'line-continuation'");
    }
    if (defer != NULL) {
        bool enabled = false;

        if (require_bool(c, defer, "embed.defer", &enabled) && enabled)
            desc.flags |= YEW_SYN_EMBED_DEFER;
    }
    if (fallback != NULL)
        (void)attr_resolve(c, fallback, &desc.fallback);
    else if (target < c->nctxs && c->ctxs[target].default_node != NULL)
        (void)attr_resolve(c, c->ctxs[target].default_node, &desc.fallback);
    else
        desc.fallback = YEW_ATTR_TEXT;

    if (c->errors != errors_before || push == NULL ||
        !lit_is(push, FL_L_STR) || target >= c->nctxs)
        return;
    if (node_str(c, push, &n) == NULL ||
        ctx_index(c, node_str(c, push, NULL), n) < 0)
        return;
    *out = desc;
    /* The bridge owns one normalized exit policy.  Language selection and
     * defer remain on the opener rule, so compatible openers may share it. */
    if (c->ctxs[target].embed_set) {
        if (!embed_boundary_same(&c->ctxs[target].embed, &desc))
            diag(c, FL_DIAG_ERROR, node->sp,
                 "context '%.*s' has incompatible embed bridge descriptors",
                 (int)c->ctxs[target].name_len, c->ctxs[target].name);
        return;
    }
    c->ctxs[target].embed.ctx = desc.ctx;
    c->ctxs[target].embed.end = desc.end;
    c->ctxs[target].embed.fallback = desc.fallback;
    c->ctxs[target].embed_set = true;
}

static void compile_rule(Compile *c, FlNode *node, u16 ctx_id, SynRule *rule,
                         const char **pattern_out, u32 rule_index)
{
    static const char *const keys[] = {
        "match", "attr", "captures", "consume", "push", "pop", "set",
        "icase", "set_aux", "strip", "aux", "aux_pre", "aux_post",
        "aux_int", "aux_add", "aux_add_capture", "value", "if_value",
        "first_line", "embed", "end"
    };
    FlNode *match = map_find(c, node, "match");
    FlNode *aux = map_find(c, node, "aux");
    FlNode *push = map_find(c, node, "push");
    FlNode *pop = map_find(c, node, "pop");
    FlNode *set = map_find(c, node, "set");
    FlNode *embed = map_find(c, node, "embed");
    FlNode *end_node = map_find(c, node, "end");
    FlNode *consume = map_find(c, node, "consume");
    FlNode *set_aux = map_find(c, node, "set_aux");
    FlNode *strip = map_find(c, node, "strip");
    FlNode *aux_int = map_find(c, node, "aux_int");
    FlNode *aux_add = map_find(c, node, "aux_add");
    FlNode *aux_add_capture = map_find(c, node, "aux_add_capture");
    FlNode *value_node = map_find(c, node, "value");
    FlNode *if_value_node = map_find(c, node, "if_value");
    FlNode *icase_node = map_find(c, node, "icase");
    FlNode *first_line_node = map_find(c, node, "first_line");
    bool icase = c->ctxs[ctx_id].icase;
    u32 errors_before_keys = c->errors;
    u32 groups = 0U;
    u32 nops = (push != NULL ? 1U : 0U) + (pop != NULL ? 1U : 0U) +
               (set != NULL ? 1U : 0U);
    i64 integer;

    (void)rule_index;
    (void)memset(rule, 0, sizeof(*rule));
    (void)memset(rule->caps, 0xff, sizeof(rule->caps));
    rule->attr = YEW_ATTR_TEXT;
    validate_keys(c, node, keys, (u32)YEW_ARRAY_LEN(keys));
    if (match == NULL && aux == NULL && c->errors == errors_before_keys)
        diag(c, FL_DIAG_ERROR, node->sp,
             "rule requires 'match' or one aux matcher");
    if (match != NULL && aux != NULL)
        diag(c, FL_DIAG_ERROR, aux->sp,
             "rule has both 'match' and 'aux'; choose one matcher");
    if (embed == NULL && nops > 1U) {
        const char *a = push != NULL ? "push" : "pop";
        const char *b = set != NULL ? "set" : "pop";

        diag(c, FL_DIAG_ERROR, node->sp,
             "rule has both '%s' and '%s'; a rule performs exactly one state op",
             a, b);
    }
    if (embed != NULL && (pop != NULL || set != NULL))
        diag(c, FL_DIAG_ERROR, node->sp,
             "embed is a state op; a rule performs exactly one");
    if (icase_node != NULL)
        (void)require_bool(c, icase_node, "rule icase", &icase);
    if (first_line_node != NULL) {
        bool enabled = false;

        if (require_bool(c, first_line_node, "rule first_line", &enabled) &&
            enabled)
            rule->flags |= YEW_SYN_RULE_FIRST_LINE;
    }
    if (match != NULL) {
        const char *pattern;
        size_t pattern_len;
        YewReErr err = {0U, NULL};

        if (require_string(c, match, "match", &pattern, &pattern_len)) {
            rule->re = yew_re_compile(c->arena, pattern, pattern_len,
                                      icase ? YEW_RE_ICASE : 0U, &err);
            *pattern_out = pattern;
            if (rule->re == NULL) {
                diag(c, FL_DIAG_ERROR, match->sp,
                     "invalid pattern at offset %u: %s", err.off,
                     err.msg == NULL ? "compile failed" : err.msg);
            } else {
                groups = yew_re_group_count(rule->re);
                if (yew_re_min_len(rule->re) == 0U)
                    diag(c, FL_DIAG_ERROR, match->sp,
                         "pattern may match empty; use a sanctioned zero-width aux matcher");
                yew_re_first_bytes(rule->re, rule->first);
            }
        }
    } else if (aux != NULL) {
        rule->aux_match = aux_kind(c, aux);
        (void)memset(rule->first, 0xff, sizeof(rule->first));
        if (rule->aux_match == SYN_AUXM_INDENT_LT ||
            rule->aux_match == SYN_AUXM_LINE_START)
            rule->flags |= YEW_SYN_RULE_ZERO_TRANSITION;
    }
    if (map_find(c, node, "attr") != NULL)
        (void)attr_resolve(c, map_find(c, node, "attr"), &rule->attr);
    else if (ctx_id < c->nctxs && c->ctxs[ctx_id].default_node != NULL)
        (void)attr_resolve(c, c->ctxs[ctx_id].default_node, &rule->attr);
    compile_captures(c, map_find(c, node, "captures"), rule, groups);
    if (consume != NULL && require_int(c, consume, "consume", &integer)) {
        if (integer < 0 || integer > 7) {
            diag(c, FL_DIAG_ERROR, consume->sp,
                 "consume group %lld is out of range (0-7)",
                 (long long)integer);
        } else if ((u64)integer >= groups) {
            diag(c, FL_DIAG_ERROR, consume->sp,
                 "consume: %lld but the pattern has %u capture groups",
                 (long long)integer, groups == 0U ? 0U : groups - 1U);
        } else {
            rule->consume = (u8)integer;
        }
    }
    if (push != NULL) {
        rule->op = SYN_OP_PUSH;
        if (lit_is(push, FL_L_STR)) {
            if (context_target(c, push, "push", &rule->target))
                c->ctxs[rule->target].pushed = true;
        } else if (node_is(push, FL_A_LIST)) {
            u32 i;

            if (push->as.list.n == 0U || push->as.list.n > 4U) {
                diag(c, FL_DIAG_ERROR, push->sp,
                     "push list has %u entries (limit 1-4)", push->as.list.n);
            } else {
                rule->npush = (u8)push->as.list.n;
                for (i = 0U; i < push->as.list.n; i++) {
                    if (context_target(c, push->as.list.items[i], "push",
                                       &rule->push[i]))
                        c->ctxs[rule->push[i]].pushed = true;
                }
            }
        } else {
            diag(c, FL_DIAG_ERROR, push->sp,
                 "push must be a context name or list");
        }
    } else if (pop != NULL) {
        rule->op = SYN_OP_POP;
        (void)parse_pop(c, pop, &rule->nop);
    } else if (set != NULL) {
        rule->op = SYN_OP_SET;
        (void)context_target(c, set, "set", &rule->target);
        if (rule->target < c->nctxs)
            c->ctxs[rule->target].pushed = true;
    }
    if (embed != NULL) {
        compile_embed(c, embed, push, rule->target, groups, &rule->embed);
        rule->op = SYN_OP_EMBED;
    }
    if (end_node != NULL) {
        bool enabled = false;

        if (require_bool(c, end_node, "rule end", &enabled))
            rule->end = enabled ? 1U : 0U;
    }
    if (set_aux != NULL && require_int(c, set_aux, "set_aux", &integer)) {
        if (integer < 0 || integer > 7 || (u64)integer >= groups) {
            diag(c, FL_DIAG_ERROR, set_aux->sp,
                 "set_aux: %lld but the pattern has %u capture groups",
                 (long long)integer, groups == 0U ? 0U : groups - 1U);
        } else {
            rule->flags |= YEW_SYN_RULE_SET_AUX;
            rule->aux_group = (u8)integer;
        }
    }
    if (aux_int != NULL) {
        bool enabled = false;

        if (require_bool(c, aux_int, "rule aux_int", &enabled) && enabled) {
            if (set_aux == NULL)
                diag(c, FL_DIAG_ERROR, aux_int->sp,
                     "aux_int requires set_aux on the same rule");
            rule->flags |= YEW_SYN_RULE_AUX_INT;
        }
    }
    if (aux_add != NULL && require_int(c, aux_add, "aux_add", &integer)) {
        if (integer < 0 || integer > UINT8_MAX) {
            diag(c, FL_DIAG_ERROR, aux_add->sp,
                 "aux_add %lld is out of range (0-255)",
                 (long long)integer);
        } else {
            if (aux_int == NULL)
                diag(c, FL_DIAG_ERROR, aux_add->sp,
                     "aux_add requires aux_int on the same rule");
            rule->aux_add = (u8)integer;
        }
    }
    if (aux_add_capture != NULL &&
        require_int(c, aux_add_capture, "aux_add_capture", &integer)) {
        if (integer < 1 || integer > 7 || (u64)integer >= groups) {
            diag(c, FL_DIAG_ERROR, aux_add_capture->sp,
                 "aux_add_capture: %lld but the pattern has %u capture groups",
                 (long long)integer, groups == 0U ? 0U : groups - 1U);
        } else {
            if (aux_int == NULL)
                diag(c, FL_DIAG_ERROR, aux_add_capture->sp,
                     "aux_add_capture requires aux_int on the same rule");
            rule->aux_add_group = (u8)integer;
        }
    }
    if (strip != NULL) {
        bool enabled = false;

        if (require_bool(c, strip, "strip", &enabled) && enabled) {
            if (set_aux == NULL)
                diag(c, FL_DIAG_ERROR, strip->sp,
                     "strip requires set_aux on the same rule");
            rule->flags |= YEW_SYN_RULE_STRIP;
        }
    }
    if (value_node != NULL) {
        bool enabled = false;

        if (require_bool(c, value_node, "value", &enabled))
            rule->flags |= enabled ? YEW_SYN_RULE_SET_VALUE
                                   : YEW_SYN_RULE_CLR_VALUE;
    }
    if (if_value_node != NULL) {
        bool enabled = false;

        if (require_bool(c, if_value_node, "rule if_value", &enabled))
            rule->value_pred = enabled ? SYN_VALUE_SET : SYN_VALUE_CLEAR;
    }
    if (map_find(c, node, "aux_pre") != NULL) {
        const char *s;
        size_t n;

        if (require_string(c, map_find(c, node, "aux_pre"), "aux_pre", &s,
                           &n))
            rule->aux_pre = yew_intern(c->aux, s, n);
    }
    if (map_find(c, node, "aux_post") != NULL) {
        const char *s;
        size_t n;

        if (require_string(c, map_find(c, node, "aux_post"), "aux_post", &s,
                           &n))
            rule->aux_post = yew_intern(c->aux, s, n);
    }
    if ((map_find(c, node, "aux_pre") != NULL ||
         map_find(c, node, "aux_post") != NULL) &&
        rule->aux_match != SYN_AUXM_LITERAL)
        diag(c, FL_DIAG_ERROR, node->sp,
             "aux_pre and aux_post require aux: 'literal'");
    if (rule->aux_match == SYN_AUXM_INDENT_LT && rule->op != SYN_OP_POP)
        diag(c, FL_DIAG_ERROR, node->sp,
             "indent_lt is zero-width and must pop");
    if (rule->aux_match == SYN_AUXM_LINE_START && rule->op == SYN_OP_STAY)
        diag(c, FL_DIAG_ERROR, node->sp,
             "line_start is zero-width and requires a state operation");
}

static void compile_eol(Compile *c, u16 id, SynCtx *out)
{
    FlNode *node = c->ctxs[id].at_eol_node;
    const char *text;
    size_t n;

    out->at_eol = SYN_OP_STAY;
    if (node == NULL)
        return;
    if (!require_string(c, node, "at_eol", &text, &n))
        return;
    if (text_eq(text, n, "stay"))
        return;
    if (text_eq(text, n, "pop")) {
        out->at_eol = SYN_OP_POP;
        out->eol_nop = 1U;
        return;
    }
    if (n > 4U && memcmp(text, "pop:", 4U) == 0) {
        unsigned long value;
        char tail[16];
        char *end;

        if (n - 4U >= sizeof(tail)) {
            diag(c, FL_DIAG_ERROR, node->sp, "invalid at_eol '%.*s'",
                 (int)n, text);
            return;
        }
        (void)memcpy(tail, text + 4U, n - 4U);
        tail[n - 4U] = '\0';
        errno = 0;
        value = strtoul(tail, &end, 10);
        if (errno != 0 || *tail == '\0' || *end != '\0' || value < 1U ||
            value > 4U) {
            diag(c, FL_DIAG_ERROR, node->sp,
                 "at_eol pop count is out of range (1-4)");
            return;
        }
        out->at_eol = SYN_OP_POP;
        out->eol_nop = (u8)value;
        return;
    }
    if (n > 4U && memcmp(text, "set:", 4U) == 0) {
        i32 target = ctx_index(c, text + 4U, n - 4U);

        if (target < 0)
            diag(c, FL_DIAG_ERROR, node->sp, "no context named '%.*s'",
                 (int)(n - 4U), text + 4U);
        else {
            out->at_eol = SYN_OP_SET;
            out->eol_target = (u16)target;
            c->ctxs[target].pushed = true;
        }
        return;
    }
    diag(c, FL_DIAG_ERROR, node->sp, "invalid at_eol '%.*s'", (int)n, text);
}

static void compile_unit(Compile *c, u16 id, SynCtx *out)
{
    FlNode *node = c->ctxs[id].unit_node;
    const char *text;
    size_t n;

    if (node == NULL)
        return;
    if (!require_string(c, node, "unit", &text, &n))
        return;
    if (text_eq(text, n, "span"))
        out->flags |= YEW_SYN_CTX_UNIT_SPAN;
    else if (text_eq(text, n, "atom"))
        out->flags |= YEW_SYN_CTX_UNIT_ATOM;
    else
        diag(c, FL_DIAG_ERROR, node->sp,
             "unit must be 'span' or 'atom'");
}

static void mark_reachable(const SynDef *def, CtxSpec *ctxs, u16 id)
{
    const SynCtx *ctx;
    u32 i;

    if (id >= def->nctxs || ctxs[id].reachable)
        return;
    ctxs[id].reachable = true;
    ctx = &def->ctxs[id];
    for (i = 0U; i < ctx->nrules; i++) {
        const SynRule *rule = &def->rules[ctx->first_rule + i];
        u32 j;

        if (rule->op == SYN_OP_PUSH || rule->op == SYN_OP_EMBED) {
            if (rule->npush == 0U)
                mark_reachable(def, ctxs, rule->target);
            else {
                for (j = 0U; j < rule->npush; j++)
                    mark_reachable(def, ctxs, rule->push[j]);
            }
        } else if (rule->op == SYN_OP_SET) {
            mark_reachable(def, ctxs, rule->target);
        }
    }
    if (ctx->at_eol == SYN_OP_SET)
        mark_reachable(def, ctxs, ctx->eol_target);
}

static bool context_reduces(const SynDef *def, u16 id)
{
    const SynCtx *ctx = &def->ctxs[id];
    u32 i;

    if (ctx->at_eol == SYN_OP_POP || ctx->at_eol == SYN_OP_SET)
        return true;
    for (i = 0U; i < ctx->nrules; i++) {
        u8 op = def->rules[ctx->first_rule + i].op;

        if (op == SYN_OP_POP || op == SYN_OP_SET)
            return true;
    }
    return false;
}

static u32 context_depth(const SynDef *def, u16 id, u8 *visiting)
{
    const SynCtx *ctx;
    u32 max = 1U;
    u32 i;

    if (id >= def->nctxs || visiting[id] != 0U)
        return 1U;
    visiting[id] = 1U;
    ctx = &def->ctxs[id];
    for (i = 0U; i < ctx->nrules; i++) {
        const SynRule *rule = &def->rules[ctx->first_rule + i];
        u32 d = 1U;
        u32 j;

        if (rule->op != SYN_OP_PUSH && rule->op != SYN_OP_EMBED)
            continue;
        if (rule->npush == 0U) {
            d += context_depth(def, rule->target, visiting);
        } else {
            d = 1U;
            for (j = 0U; j < rule->npush; j++) {
                u32 child = context_depth(def, rule->push[j], visiting);
                u32 candidate = 1U + j + child;

                if (candidate > d)
                    d = candidate;
            }
        }
        if (d > max)
            max = d;
    }
    visiting[id] = 0U;
    return max;
}

static bool first_is_superset(const u8 a[32], const u8 b[32])
{
    u32 i;

    for (i = 0U; i < 32U; i++) {
        if ((a[i] | b[i]) != a[i])
            return false;
    }
    return true;
}

static void validate_compiled(Compile *c, SynDef *def,
                              const char *const *patterns,
                              const FlSpan *rule_spans)
{
    u8 *visiting = arena_alloc(c->arena, c->nctxs, 1U);
    u32 i;

    (void)memset(visiting, 0, c->nctxs);
    mark_reachable(def, c->ctxs, def->root);
    for (i = 0U; i < c->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];
        bool has_end = false;
        u32 j;

        if (!c->ctxs[i].reachable && !c->ctxs[i].include_used)
            diag(c, FL_DIAG_WARNING, c->ctxs[i].node->sp,
                 "context '%.*s' is unreachable", (int)c->ctxs[i].name_len,
                 c->ctxs[i].name);
        if (i != def->root && c->ctxs[i].reachable && c->ctxs[i].pushed &&
            !context_reduces(def, (u16)i))
            diag(c, FL_DIAG_ERROR, c->ctxs[i].node->sp,
                 "context '%.*s' can never be popped",
                 (int)c->ctxs[i].name_len, c->ctxs[i].name);
        if (c->ctxs[i].include_used && !c->ctxs[i].pushed && i != def->root &&
            (c->ctxs[i].default_node != NULL ||
             c->ctxs[i].at_eol_node != NULL ||
             c->ctxs[i].unit_node != NULL))
            diag(c, FL_DIAG_WARNING, c->ctxs[i].node->sp,
                 "only 'rules' is used at an include site");
        if (ctx->dflt_attr == YEW_ATTR_ERROR)
            diag(c, FL_DIAG_WARNING, c->ctxs[i].default_node->sp,
                 "default: 'error' paints every unmatched byte red; did you mean 'text'?");
        for (j = 0U; j < ctx->nrules; j++) {
            u32 index = ctx->first_rule + j;
            u32 k;

            if (def->rules[index].end != 0U) {
                has_end = true;
                if ((ctx->flags & YEW_SYN_CTX_EMBED_BRIDGE) == 0U)
                    diag(c, FL_DIAG_ERROR, rule_spans[index],
                         "'end' marks a rule that returns from an embedded language");
            }

            if (def->rules[index].aux_match == SYN_AUXM_INDENT_LT && j != 0U)
                diag(c, FL_DIAG_ERROR, c->ctxs[i].rules->sp,
                     "indent_lt must be the first rule in its context");
            if (patterns[index] == NULL)
                continue;
            for (k = 0U; k < j; k++) {
                u32 earlier = ctx->first_rule + k;

                if (patterns[earlier] != NULL &&
                    strcmp(patterns[earlier], patterns[index]) == 0) {
                    diag(c, FL_DIAG_WARNING, rule_spans[index],
                         "rule %u is unreachable: rule %u has the same pattern",
                         j + 1U, k + 1U);
                    break;
                }
                if (def->rules[earlier].re != NULL &&
                    yew_re_is_simple_catch_all(def->rules[earlier].re) &&
                    first_is_superset(def->rules[earlier].first,
                                      def->rules[index].first)) {
                    diag(c, FL_DIAG_WARNING, rule_spans[index],
                         "rule %u is unreachable: rule %u (line %u) matches everything it could match",
                         j + 1U, k + 1U, rule_spans[earlier].line);
                    break;
                }
            }
        }
        if ((ctx->flags & YEW_SYN_CTX_EMBED_BRIDGE) != 0U && !has_end &&
            ctx->at_eol != SYN_OP_POP && ctx->at_eol != SYN_OP_SET)
            diag(c, FL_DIAG_ERROR, c->ctxs[i].node->sp,
                 "context '%.*s' embeds a language it can never leave",
                 (int)c->ctxs[i].name_len, c->ctxs[i].name);
    }
    (void)memset(visiting, 0, c->nctxs);
    {
        u32 depth = context_depth(def, def->root, visiting);

        if (depth > SYN_DEF_STATIC_DEPTH)
            diag(c, FL_DIAG_ERROR, c->ctxs[def->root].node->sp,
                 "context nesting can reach depth %u; the cap is 16 with 4 levels reserved for runtime recursion (see YEW_SYN_DEPTH_MAX)",
                 depth);
    }
}

static u32 language_id(const char *name)
{
    DefMeta *m;
    size_t i;
    u32 max = 0U;

    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        if (strcmp(yew_syn_builtin_langs[i].name, name) == 0)
            return yew_syn_builtin_langs[i].id;
        if (yew_syn_builtin_langs[i].id > max)
            max = yew_syn_builtin_langs[i].id;
    }
    for (m = metas; m != NULL; m = m->next) {
        if (strcmp(m->lang.name, name) == 0)
            return m->lang.id;
        if (m->lang.id > max)
            max = m->lang.id;
    }
    return max + 1U;
}

static void register_meta(Compile *c, SynDef *def, SynLangDesc *lang,
                          FlSpan first_line_sp, const char **ctx_names,
                          const char **patterns)
{
    DefMeta *m = yew_xcalloc(1U, sizeof(*m));

    lang->id = language_id(lang->name);
    m->def = def;
    m->lang = *lang;
    m->builtin = builtin_name_exists(lang->name);
    m->ctx_names = ctx_names;
    m->patterns = patterns;
    m->aux = c->aux;
    if (lang->first_line != NULL) {
        YewReErr err = {0U, NULL};

        m->first_line_re = yew_re_compile(c->arena, lang->first_line,
                                          strlen(lang->first_line), 0U,
                                          &err);
        if (m->first_line_re == NULL)
            diag(c, FL_DIAG_ERROR, first_line_sp,
                 "invalid first_line pattern at offset %u: %s", err.off,
                 err.msg == NULL ? "compile failed" : err.msg);
    }
    meta_link(m);
}

SynDef *yew_syn_def_compile(Arena *a, DiagCtx *dc, const u8 *src, size_t n,
                            u32 file_id, u32 *n_err, u32 *n_warn)
{
    static const char *const top_keys[] = {
        "syntax", "language", "contexts", "root", "attrs"
    };
    Compile c;
    FlNode *top;
    FlNode *syntax;
    FlNode *language;
    FlNode *contexts;
    FlNode *root;
    SynLangDesc lang;
    SynDef *def = NULL;
    RuleVec *expanded = NULL;
    const char **ctx_names = NULL;
    const char **patterns = NULL;
    FlSpan *rule_spans = NULL;
    FlSpan first_line_sp;
    u32 total = 0U;
    u32 i;
    i64 version;

    if (n_err != NULL)
        *n_err = 0U;
    if (n_warn != NULL)
        *n_warn = 0U;
    if (a == NULL || dc == NULL || (src == NULL && n != 0U))
        return NULL;
    (void)memset(&c, 0, sizeof(c));
    (void)memset(&lang, 0, sizeof(lang));
    first_line_sp = (FlSpan){file_id, 1U, 1U, 1U};
    c.arena = a;
    c.dc = dc;
    c.src = src;
    c.src_len = n;
    c.file_id = file_id;
    interner_init(&c.in, a);
    c.aux = arena_alloc(a, sizeof(*c.aux), _Alignof(Interner));
    interner_init(c.aux, a);
    compile_count++;
    top = fl_parse_literal(a, dc, &c.in, (const char *)src, n, file_id);
    if (top == NULL) {
        c.errors = 1U;
        goto done;
    }
    if (!require_map(&c, top, "syntax definition"))
        goto done;
    validate_keys(&c, top, top_keys, (u32)YEW_ARRAY_LEN(top_keys));
    syntax = map_find(&c, top, "syntax");
    language = map_find(&c, top, "language");
    contexts = map_find(&c, top, "contexts");
    root = map_find(&c, top, "root");
    if (syntax == NULL) {
        diag(&c, FL_DIAG_ERROR, top->sp,
             "missing schema version (this build understands 1)");
    } else if (require_int(&c, syntax, "syntax", &version) && version != 1) {
        diag(&c, FL_DIAG_ERROR, syntax->sp,
             "unknown schema version %lld (this build understands 1)",
             (long long)version);
    }
    if (language == NULL)
        diag(&c, FL_DIAG_ERROR, top->sp, "language is required");
    else {
        parse_language(&c, language, &lang);
        if (map_find(&c, language, "first_line") != NULL)
            first_line_sp = map_find(&c, language, "first_line")->sp;
    }
    parse_aliases(&c, map_find(&c, top, "attrs"));
    if (contexts == NULL)
        diag(&c, FL_DIAG_ERROR, top->sp, "contexts is required");
    else
        parse_contexts(&c, contexts);
    if (c.nctxs == 0U || lang.name == NULL)
        goto done;
    c.root = 0U;
    if (root != NULL) {
        const char *name;
        size_t name_len;
        i32 id;

        if (require_string(&c, root, "root", &name, &name_len)) {
            id = ctx_index(&c, name, name_len);
            if (id < 0)
                diag(&c, FL_DIAG_ERROR, root->sp,
                     "no context named '%.*s'", (int)name_len, name);
            else
                c.root = (u16)id;
        }
    } else {
        i32 id = ctx_index(&c, "main", 4U);

        if (id < 0)
            diag(&c, FL_DIAG_ERROR, contexts->sp,
                 "no context named 'main'");
        else
            c.root = (u16)id;
    }
    expanded = arena_alloc(a, (size_t)c.nctxs * sizeof(*expanded),
                           _Alignof(RuleVec));
    (void)memset(expanded, 0, (size_t)c.nctxs * sizeof(*expanded));
    for (i = 0U; i < c.nctxs; i++) {
        u8 *visiting = arena_alloc(a, c.nctxs, 1U);

        (void)memset(visiting, 0, c.nctxs);
        (void)expand_context(&c, &expanded[i], (u16)i,
                             c.ctxs[i].node->sp, visiting);
        if (UINT32_MAX - total < expanded[i].len) {
            diag(&c, FL_DIAG_ERROR, c.ctxs[i].node->sp,
                 "expanded rule count overflow");
            goto done;
        }
        total += expanded[i].len;
    }
    def = arena_alloc(a, sizeof(*def), _Alignof(SynDef));
    def->name = lang.name;
    def->root = c.root;
    def->nctxs = (u16)c.nctxs;
    def->nrules = total;
    def->ctxs = arena_alloc(a, (size_t)c.nctxs * sizeof(*def->ctxs),
                            _Alignof(SynCtx));
    def->rules = arena_alloc(a, (size_t)total * sizeof(*def->rules),
                             _Alignof(SynRule));
    def->aux = c.aux;
    (void)memset(def->ctxs, 0, (size_t)c.nctxs * sizeof(*def->ctxs));
    if (total != 0U)
        (void)memset(def->rules, 0, (size_t)total * sizeof(*def->rules));
    ctx_names = arena_alloc(a, (size_t)c.nctxs * sizeof(*ctx_names),
                            _Alignof(const char *));
    patterns = arena_alloc(a, (size_t)total * sizeof(*patterns),
                           _Alignof(const char *));
    rule_spans = arena_alloc(a, (size_t)total * sizeof(*rule_spans),
                             _Alignof(FlSpan));
    if (total != 0U)
        (void)memset(patterns, 0, (size_t)total * sizeof(*patterns));
    total = 0U;
    for (i = 0U; i < c.nctxs; i++) {
        SynCtx *ctx = &def->ctxs[i];
        u32 j;

        ctx_names[i] = c.ctxs[i].name;
        ctx->first_rule = total;
        ctx->nrules = expanded[i].len;
        ctx->dflt_attr = YEW_ATTR_TEXT;
        if (c.ctxs[i].default_node != NULL)
            (void)attr_resolve(&c, c.ctxs[i].default_node,
                               &ctx->dflt_attr);
        compile_eol(&c, (u16)i, ctx);
        compile_unit(&c, (u16)i, ctx);
        for (j = 0U; j < expanded[i].len; j++) {
            FlNode *match = map_find(&c, expanded[i].data[j].node, "match");

            rule_spans[total] = match == NULL ?
                                expanded[i].data[j].node->sp : match->sp;
            compile_rule(&c, expanded[i].data[j].node, (u16)i,
                         &def->rules[total], &patterns[total], total);
            first_union(ctx->first, def->rules[total].first);
            total++;
        }
    }
    for (i = 0U; i < c.nctxs; i++) {
        def->ctxs[i].embed = c.ctxs[i].embed;
        if (c.ctxs[i].embed_set)
            def->ctxs[i].flags |= YEW_SYN_CTX_EMBED_BRIDGE;
    }
    validate_compiled(&c, def, patterns, rule_spans);
    if (c.errors == 0U) {
        register_meta(&c, def, &lang, first_line_sp, ctx_names, patterns);
        if (c.errors != 0U) {
            DefMeta *m = metas;

            metas = m->next;
            free(m);
        }
    }

done:
    if (n_err != NULL)
        *n_err = c.errors;
    if (n_warn != NULL)
        *n_warn = c.warnings;
    interner_free(&c.in);
    if (c.errors != 0U) {
        interner_free(c.aux);
        def = NULL;
    }
    return def;
}

void yew_syn_def_dispose(SynDef *def)
{
    DefMeta **link = &metas;

    while (*link != NULL) {
        DefMeta *m = *link;

        if (m->def != def) {
            link = &m->next;
            continue;
        }
        *link = m->next;
        meta_unlink_user(m);
        if (m->aux != NULL)
            interner_free(m->aux);
        yew_syn_engine_free(m->engine);
        free(m);
        return;
    }
}

const char *yew_syn_ctx_name(const SynDef *def, u16 ctx)
{
    DefMeta *m = meta_for(def);

    return m != NULL && ctx < def->nctxs ? m->ctx_names[ctx] : NULL;
}

const char *yew_syn_rule_pattern(const SynDef *def, u32 rule)
{
    DefMeta *m = meta_for(def);

    return m != NULL && rule < def->nrules ? m->patterns[rule] : NULL;
}

u64 yew_syn_compile_count(void)
{
    return compile_count;
}

void yew_syn_compile_count_reset(void)
{
    compile_count = 0U;
}

static void discovery_diag_discard(void *ctx, FlDiagLevel level, FlSpan sp,
                                   const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static int discovery_name_cmp(const void *left, const void *right, void *ctx)
{
    const char *const *a = left;
    const char *const *b = right;

    (void)ctx;
    return strcmp(*a, *b);
}

static bool discovery_filename(const char *name)
{
    size_t len = strlen(name);

    return len > 3U && strcmp(name + len - 3U, ".fl") == 0;
}

static char *discovery_path(const char *dir, const char *name)
{
    size_t len = strlen(dir) + 1U + strlen(name);
    char *path = yew_xmalloc(len + 1U);

    (void)snprintf(path, len + 1U, "%s/%s", dir, name);
    return path;
}

static bool discovered_name_exists(const char *name)
{
    DiscoveredDef *owned;

    for (owned = discovered_defs; owned != NULL; owned = owned->next) {
        if (strcmp(owned->def->name, name) == 0)
            return true;
    }
    return false;
}

static bool builtin_name_exists(const char *name)
{
    size_t i;

    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        if (strcmp(yew_syn_builtin_langs[i].name, name) == 0)
            return true;
    }
    return false;
}

static void discover_user_definitions(void)
{
    char *config;
    char *dir;
    DIR *stream;
    struct dirent *entry;
    char **names = NULL;
    size_t nnames = 0U;
    size_t i;
    int read_error;

    if (discovery_done)
        return;
    /* Set this before loading: registration and cache unpacking consult the
     * same public registry and must not recursively rescan the directory. */
    discovery_done = true;
    if (discovery_bypass)
        return;
    config = yew_xdg_config_dir();
    if (config == NULL)
        return;
    dir = discovery_path(config, "syntax");
    free(config);
    stream = opendir(dir);
    if (stream == NULL) {
        if (errno != ENOENT)
            yew_log(YEW_LOG_WARN, "cannot scan syntax definitions: %s",
                    dir);
        free(dir);
        return;
    }
    errno = 0;
    while ((entry = readdir(stream)) != NULL) {
        char *copy;

        if (!discovery_filename(entry->d_name))
            continue;
        copy = yew_xmalloc(strlen(entry->d_name) + 1U);
        (void)memcpy(copy, entry->d_name, strlen(entry->d_name) + 1U);
        names = yew_xreallocarray(names, nnames + 1U, sizeof(*names));
        names[nnames++] = copy;
    }
    read_error = errno;
    if (closedir(stream) != 0 && read_error == 0)
        read_error = errno;
    if (read_error != 0)
        yew_log(YEW_LOG_WARN, "cannot finish scanning syntax definitions: %s",
                dir);
    yew_sort_stable(names, nnames, sizeof(*names), discovery_name_cmp, NULL);
    for (i = 0U; i < nnames; i++) {
        DiscoveredDef *owned = yew_xcalloc(1U, sizeof(*owned));
        char *path = discovery_path(dir, names[i]);

        arena_init(&owned->arena);
        fl_diag_init(&owned->dc, &owned->arena);
        fl_diag_set_sink(&owned->dc, discovery_diag_discard, NULL);
        owned->def = yew_syn_def_load(&owned->arena, &owned->dc, path);
        if (owned->def != NULL &&
            (builtin_name_exists(owned->def->name) ||
             discovered_name_exists(owned->def->name))) {
            yew_log(YEW_LOG_WARN,
                    "ignoring duplicate syntax language '%s': %s",
                    owned->def->name, path);
            yew_syn_def_dispose(owned->def);
            arena_free_all(&owned->arena);
            free(owned);
        } else if (owned->def == NULL) {
            yew_log(YEW_LOG_WARN, "ignoring invalid syntax definition: %s",
                    path);
            arena_free_all(&owned->arena);
            free(owned);
        } else {
            owned->next = discovered_defs;
            discovered_defs = owned;
        }
        free(path);
        free(names[i]);
    }
    free(names);
    free(dir);
}

void yew_syn_discovery_reset(void)
{
    while (discovered_defs != NULL) {
        DiscoveredDef *owned = discovered_defs;
        DefMeta *meta = meta_for(owned->def);

        discovered_defs = owned->next;
        if (owned->pins != 0U) {
            owned->retired = true;
            owned->next = retired_defs;
            retired_defs = owned;
            if (meta != NULL)
                meta->retired = true;
        } else {
            discovered_owned_free(owned);
        }
    }
    discovery_done = false;
}

void yew_syn_discovery_set_bypass(bool bypass)
{
    discovery_bypass = bypass;
}

static void builtin_registry_init(void)
{
    if (!builtin_registry.ready) {
        yew_syn_builtin_registry_build(&builtin_registry,
                                       yew_syn_builtin_langs,
                                       yew_syn_builtin_langs_len);
    }
}

static const SynLangDesc *builtin_desc_at(size_t i)
{
    builtin_registry_init();
    return yew_syn_builtin_registry_desc_at(&builtin_registry, i);
}

const SynLangDesc *yew_syn_lang_desc(u32 lang)
{
    DefMeta *m;
    size_t i;

    discover_user_definitions();
    for (m = metas; m != NULL; m = m->next) {
        if (!m->retired && m->lang.id == lang)
            return &m->lang;
    }
    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        if (yew_syn_builtin_langs[i].id == lang)
            return builtin_desc_at(i);
    }
    return NULL;
}

u32 yew_syn_lang_named(const char *name)
{
    DefMeta *m;
    size_t i;

    if (name == NULL)
        return YEW_LANG_NONE;
    discover_user_definitions();
    for (m = metas; m != NULL; m = m->next) {
        if (!m->retired && strcmp(m->lang.name, name) == 0)
            return m->lang.id;
    }
    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        if (strcmp(yew_syn_builtin_langs[i].name, name) == 0)
            return yew_syn_builtin_langs[i].id;
    }
    return YEW_LANG_NONE;
}

u32 yew_syn_lang_by_name(const u8 *name, u32 len)
{
    size_t i;

    if (name == NULL)
        return YEW_LANG_NONE;
    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        const char *candidate = yew_syn_builtin_langs[i].name;
        size_t candidate_len = strlen(candidate);

        if (candidate_len == len &&
            (len == 0U || memcmp(candidate, name, len) == 0))
            return yew_syn_builtin_langs[i].id;
    }
    return YEW_LANG_NONE;
}

u32 yew_syn_lang_snapshot(const char **names, u32 *langs, u32 cap)
{
    DefMeta *m;
    u32 n = 0U;
    size_t i;

    discover_user_definitions();
    for (m = metas; m != NULL; m = m->next) {
        if (m->retired)
            continue;
        if (n < cap && names != NULL && langs != NULL) {
            names[n] = m->lang.name;
            langs[n] = m->lang.id;
        }
        n++;
    }
    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        if (n < cap && names != NULL && langs != NULL) {
            names[n] = yew_syn_builtin_langs[i].name;
            langs[n] = yew_syn_builtin_langs[i].id;
        }
        n++;
    }
    return n;
}

u32 yew_syn_lang_count(void)
{
    DefMeta *m;
    u32 count = (u32)yew_syn_builtin_langs_len;

    discover_user_definitions();
    for (m = metas; m != NULL; m = m->next) {
        size_t i;

        if (m->retired)
            continue;

        for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
            if (m->lang.id == yew_syn_builtin_langs[i].id)
                break;
        }
        if (i == yew_syn_builtin_langs_len)
            count++;
    }
    return count;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8U);
    p[2] = (u8)(v >> 16U);
    p[3] = (u8)(v >> 24U);
}

static void put64(u8 *p, u64 v)
{
    u32 i;

    for (i = 0U; i < 8U; i++)
        p[i] = (u8)(v >> (i * 8U));
}

static u32 get32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8U | (u32)p[2] << 16U |
           (u32)p[3] << 24U;
}

static u64 get64(const u8 *p)
{
    u64 value = 0U;
    u32 i;

    for (i = 0U; i < 8U; i++)
        value |= (u64)p[i] << (i * 8U);
    return value;
}

static void blob_u32(Bytebuf *out, u32 value)
{
    u8 bytes[4];

    put32(bytes, value);
    bytebuf_append(out, bytes, sizeof(bytes));
}

static void blob_u16(Bytebuf *out, u16 value)
{
    u8 bytes[2] = {(u8)value, (u8)(value >> 8U)};

    bytebuf_append(out, bytes, sizeof(bytes));
}

static bool blob_string_n(Bytebuf *out, const char *value, size_t len)
{
    if (value == NULL) {
        blob_u32(out, UINT32_MAX);
        return true;
    }
    if (len > UINT32_MAX)
        return false;
    blob_u32(out, (u32)len);
    bytebuf_append(out, value, len);
    return true;
}

static bool blob_string(Bytebuf *out, const char *value)
{
    return blob_string_n(out, value, value == NULL ? 0U : strlen(value));
}

static bool blob_string_array(Bytebuf *out, const char *const *values,
                              u32 count)
{
    u32 i;

    blob_u32(out, count);
    for (i = 0U; i < count; i++) {
        if (!blob_string(out, values[i]))
            return false;
    }
    return true;
}

typedef struct BlobReader {
    const u8 *data;
    size_t len;
    size_t at;
    bool ok;
} BlobReader;

static u8 blob_read_u8(BlobReader *r)
{
    if (!r->ok || r->at == r->len) {
        r->ok = false;
        return 0U;
    }
    return r->data[r->at++];
}

static u16 blob_read_u16(BlobReader *r)
{
    u16 value;

    if (!r->ok || r->len - r->at < 2U) {
        r->ok = false;
        return 0U;
    }
    value = (u16)((u16)r->data[r->at] |
                  (u16)((u16)r->data[r->at + 1U] << 8U));
    r->at += 2U;
    return value;
}

static u32 blob_read_u32(BlobReader *r)
{
    u32 value;

    if (!r->ok || r->len - r->at < 4U) {
        r->ok = false;
        return 0U;
    }
    value = get32(r->data + r->at);
    r->at += 4U;
    return value;
}

static const u8 *blob_read_bytes(BlobReader *r, size_t len)
{
    const u8 *value;

    if (!r->ok || len > r->len - r->at) {
        r->ok = false;
        return NULL;
    }
    value = r->data + r->at;
    r->at += len;
    return value;
}

static char *blob_read_string(BlobReader *r, Arena *arena, size_t *len_out)
{
    u32 len = blob_read_u32(r);
    const u8 *bytes;

    if (!r->ok || len == UINT32_MAX) {
        if (len_out != NULL)
            *len_out = 0U;
        return NULL;
    }
    bytes = blob_read_bytes(r, len);
    if (bytes == NULL)
        return NULL;
    if (len_out != NULL)
        *len_out = len;
    return arena_strndup(arena, (const char *)bytes, len);
}

static const char **blob_read_string_array(BlobReader *r, Arena *arena,
                                           u32 *count_out)
{
    u32 count = blob_read_u32(r);
    const char **values;
    u32 i;

    if (!r->ok || count > SYN_DEF_MAX_RULES) {
        r->ok = false;
        return NULL;
    }
    values = arena_alloc(arena, (size_t)count * sizeof(*values),
                         _Alignof(const char *));
    for (i = 0U; i < count; i++) {
        values[i] = blob_read_string(r, arena, NULL);
        if (!r->ok || values[i] == NULL) {
            r->ok = false;
            return NULL;
        }
    }
    *count_out = count;
    return values;
}

static const u8 syn_blob_magic[8] = {'Y', 'E', 'W', 'S', 'Y', 'N', '2', 0};

static bool syn_blob_pack(const SynDef *def, const char *source, Bytebuf *out)
{
    DefMeta *m = meta_for(def);
    u32 i;

    if (def == NULL || m == NULL || source == NULL || out == NULL ||
        def->nctxs == 0U ||
        def->root >= def->nctxs || m->aux == NULL)
        return false;
    bytebuf_init(out);
    bytebuf_append(out, syn_blob_magic, sizeof(syn_blob_magic));
    if (!blob_string(out, source))
        goto fail;
    blob_u32(out, def->root);
    blob_u32(out, def->nctxs);
    blob_u32(out, def->nrules);
    blob_u32(out, m->lang.id);
    blob_u32(out, (u32)m->lang.priority);
    if (!blob_string(out, def->name) ||
        !blob_string_array(out, m->lang.extensions, m->lang.nextensions) ||
        !blob_string_array(out, m->lang.filenames, m->lang.nfilenames) ||
        !blob_string_array(out, m->lang.shebangs, m->lang.nshebangs) ||
        !blob_string(out, m->lang.first_line) ||
        !blob_string(out, m->lang.comment.line) ||
        !blob_string(out, m->lang.comment.block_open) ||
        !blob_string(out, m->lang.comment.block_close))
        goto fail;
    {
        size_t len_at = out->len;
        size_t re_at;

        blob_u32(out, 0U);
        re_at = out->len;
        if (m->first_line_re != NULL &&
            (!yew_re_pack(m->first_line_re, out) ||
             out->len - re_at > UINT32_MAX))
            goto fail;
        put32(out->data + len_at, (u32)(out->len - re_at));
    }
    if (yew_intern_count(m->aux) > UINT32_MAX)
        goto fail;
    blob_u32(out, (u32)yew_intern_count(m->aux));
    for (i = 1U; i <= yew_intern_count(m->aux); i++) {
        if (!blob_string_n(out, yew_intern_str(m->aux, i),
                           yew_intern_len(m->aux, i)))
            goto fail;
    }
    for (i = 0U; i < def->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];

        blob_u32(out, ctx->first_rule);
        blob_u32(out, ctx->nrules);
        bytebuf_push_u8(out, ctx->dflt_attr);
        bytebuf_push_u8(out, ctx->at_eol);
        bytebuf_push_u8(out, ctx->eol_nop);
        bytebuf_push_u8(out, ctx->flags);
        blob_u16(out, ctx->eol_target);
        blob_u32(out, ctx->embed.lang);
        blob_u32(out, ctx->embed.ctx);
        bytebuf_push_u8(out, ctx->embed.lang_kind);
        bytebuf_push_u8(out, ctx->embed.lang_group);
        bytebuf_push_u8(out, ctx->embed.end);
        bytebuf_push_u8(out, ctx->embed.fallback);
        bytebuf_push_u8(out, ctx->embed.flags);
        bytebuf_append(out, ctx->first, sizeof(ctx->first));
        if (!blob_string(out, m->ctx_names[i]))
            goto fail;
    }
    for (i = 0U; i < def->nrules; i++) {
        const SynRule *rule = &def->rules[i];
        u32 j;
        size_t len_at;
        size_t re_at;

        bytebuf_push_u8(out, rule->attr);
        bytebuf_push_u8(out, rule->op);
        bytebuf_push_u8(out, rule->nop);
        bytebuf_push_u8(out, rule->aux_match);
        blob_u16(out, rule->target);
        bytebuf_push_u8(out, rule->consume);
        bytebuf_push_u8(out, rule->flags);
        bytebuf_push_u8(out, rule->value_pred);
        bytebuf_push_u8(out, rule->aux_add);
        bytebuf_push_u8(out, rule->aux_add_group);
        bytebuf_append(out, rule->caps, sizeof(rule->caps));
        bytebuf_push_u8(out, rule->aux_group);
        bytebuf_push_u8(out, rule->end);
        bytebuf_push_u8(out, rule->npush);
        blob_u32(out, rule->embed.lang);
        blob_u32(out, rule->embed.ctx);
        bytebuf_push_u8(out, rule->embed.lang_kind);
        bytebuf_push_u8(out, rule->embed.lang_group);
        bytebuf_push_u8(out, rule->embed.end);
        bytebuf_push_u8(out, rule->embed.fallback);
        bytebuf_push_u8(out, rule->embed.flags);
        blob_u32(out, rule->aux_pre);
        blob_u32(out, rule->aux_post);
        for (j = 0U; j < YEW_ARRAY_LEN(rule->push); j++)
            blob_u16(out, rule->push[j]);
        bytebuf_append(out, rule->first, sizeof(rule->first));
        if (!blob_string(out, m->patterns[i]))
            goto fail;
        len_at = out->len;
        blob_u32(out, 0U);
        re_at = out->len;
        if (rule->re != NULL &&
            (!yew_re_pack(rule->re, out) || out->len - re_at > UINT32_MAX))
            goto fail;
        put32(out->data + len_at, (u32)(out->len - re_at));
    }
    return out->len <= UINT32_MAX;

fail:
    bytebuf_free(out);
    return false;
}

static bool syn_rule_valid(const SynRule *rule, const SynDef *def,
                           size_t naux)
{
    u32 i;

    if (rule->attr >= YEW_ATTR__COUNT || rule->op > SYN_OP_EMBED ||
        rule->nop > 4U || rule->aux_match > SYN_AUXM_LINE_START ||
        rule->consume > 7U || rule->npush > 4U ||
        rule->value_pred > SYN_VALUE_SET ||
        rule->aux_group > 7U || rule->end > 1U ||
        rule->aux_add_group > 7U ||
        rule->aux_pre >= naux ||
        rule->aux_post >= naux)
        return false;
    if (((rule->op == SYN_OP_PUSH || rule->op == SYN_OP_EMBED) &&
         rule->npush == 0U) ||
        rule->op == SYN_OP_SET) {
        if (rule->target >= def->nctxs)
            return false;
    }
    for (i = 0U; i < rule->npush; i++) {
        if (rule->push[i] >= def->nctxs)
            return false;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(rule->caps); i++) {
        if (rule->caps[i] != UINT8_MAX && rule->caps[i] >= YEW_ATTR__COUNT)
            return false;
    }
    if (rule->aux_add_group != 0U &&
        (((rule->flags & YEW_SYN_RULE_AUX_INT) == 0U) || rule->re == NULL ||
         rule->aux_add_group >= yew_re_group_count(rule->re)))
        return false;
    if (rule->op == SYN_OP_EMBED) {
        if (rule->embed.lang_kind == SYN_EMBED_LANG_NONE ||
            rule->embed.lang_kind > SYN_EMBED_LANG_SELF ||
            rule->embed.end == SYN_EMBED_END_NONE ||
            rule->embed.end > SYN_EMBED_END_LINE_CONTINUATION ||
            rule->embed.fallback >= YEW_ATTR__COUNT ||
            (rule->embed.flags & (u8)~YEW_SYN_EMBED_DEFER) != 0U ||
            rule->embed.lang >= naux || rule->embed.ctx >= naux)
            return false;
        if (rule->embed.lang_kind == SYN_EMBED_LANG_LITERAL) {
            if (rule->embed.lang == 0U || rule->embed.lang_group != 0U)
                return false;
        } else if (rule->embed.lang_kind == SYN_EMBED_LANG_CAPTURE) {
            if (rule->embed.lang != 0U || rule->embed.lang_group < 1U ||
                rule->embed.lang_group > 7U || rule->re == NULL ||
                rule->embed.lang_group >= yew_re_group_count(rule->re))
                return false;
        } else if (rule->embed.lang != 0U ||
                   rule->embed.lang_group != 0U) {
            return false;
        }
    } else if (rule->embed.lang != 0U || rule->embed.ctx != 0U ||
               rule->embed.lang_kind != SYN_EMBED_LANG_NONE ||
               rule->embed.lang_group != 0U ||
               rule->embed.end != SYN_EMBED_END_NONE ||
               rule->embed.fallback != 0U || rule->embed.flags != 0U) {
        return false;
    }
    return true;
}

static bool syn_embed_boundary_valid(const SynEmbed *embed, bool active,
                                     size_t naux)
{
    if (!active)
        return embed->lang == 0U && embed->ctx == 0U &&
               embed->lang_group == 0U && embed->end == SYN_EMBED_END_NONE &&
               embed->fallback == 0U && embed->flags == 0U;
    return embed->lang == 0U && embed->ctx < naux &&
           embed->lang_kind == SYN_EMBED_LANG_NONE &&
           embed->lang_group == 0U && embed->end != SYN_EMBED_END_NONE &&
           embed->end <= SYN_EMBED_END_LINE_CONTINUATION &&
           embed->fallback < YEW_ATTR__COUNT && embed->flags == 0U;
}

static SynDef *syn_blob_unpack(Arena *arena, const u8 *data, size_t len,
                               const char *source, const char *expected_name)
{
    BlobReader r = {data, len, 0U, true};
    SynDef *def = NULL;
    DefMeta *m = NULL;
    Interner *aux = NULL;
    const u8 *magic = blob_read_bytes(&r, sizeof(syn_blob_magic));
    u32 root;
    u32 nctxs;
    u32 cached_id;
    u32 priority;
    u32 naux;
    u32 i;
    char *packed_source;

    if (magic == NULL || memcmp(magic, syn_blob_magic,
                                sizeof(syn_blob_magic)) != 0)
        return NULL;
    packed_source = blob_read_string(&r, arena, NULL);
    if (!r.ok || packed_source == NULL || strcmp(packed_source, source) != 0)
        return NULL;
    def = arena_alloc(arena, sizeof(*def), _Alignof(SynDef));
    (void)memset(def, 0, sizeof(*def));
    root = blob_read_u32(&r);
    nctxs = blob_read_u32(&r);
    def->nrules = blob_read_u32(&r);
    cached_id = blob_read_u32(&r);
    priority = blob_read_u32(&r);
    if (!r.ok || nctxs == 0U || nctxs > SYN_DEF_MAX_CONTEXTS ||
        root >= nctxs || def->nrules > SYN_DEF_MAX_RULES)
        return NULL;
    def->root = (u16)root;
    def->nctxs = (u16)nctxs;
    m = yew_xcalloc(1U, sizeof(*m));
    def->name = blob_read_string(&r, arena, NULL);
    m->lang.name = def->name;
    m->builtin = builtin_name_exists(def->name);
    m->lang.extensions = blob_read_string_array(&r, arena,
                                                &m->lang.nextensions);
    m->lang.filenames = blob_read_string_array(&r, arena,
                                               &m->lang.nfilenames);
    m->lang.shebangs = blob_read_string_array(&r, arena,
                                              &m->lang.nshebangs);
    m->lang.first_line = blob_read_string(&r, arena, NULL);
    m->lang.comment.line = blob_read_string(&r, arena, NULL);
    m->lang.comment.block_open = blob_read_string(&r, arena, NULL);
    m->lang.comment.block_close = blob_read_string(&r, arena, NULL);
    m->lang.priority = (i32)priority;
    m->lang.source = arena_strdup(arena, source);
    {
        u32 re_len = blob_read_u32(&r);
        size_t used = 0U;
        const u8 *packed = blob_read_bytes(&r, re_len);

        if (re_len != 0U) {
            m->first_line_re = yew_re_unpack(arena, packed, re_len, &used);
            if (m->first_line_re == NULL || used != re_len)
                r.ok = false;
        }
    }
    naux = blob_read_u32(&r);
    if (!r.ok || naux >= UINT32_MAX || naux > SYN_DEF_MAX_RULES)
        goto fail;
    aux = arena_alloc(arena, sizeof(*aux), _Alignof(Interner));
    interner_init(aux, arena);
    for (i = 1U; i <= naux; i++) {
        size_t value_len;
        char *value = blob_read_string(&r, arena, &value_len);

        if (!r.ok || value == NULL || yew_intern(aux, value, value_len) != i) {
            r.ok = false;
            goto fail;
        }
    }
    def->aux = aux;
    m->aux = aux;
    def->ctxs = arena_alloc(arena, (size_t)def->nctxs * sizeof(*def->ctxs),
                            _Alignof(SynCtx));
    m->ctx_names = arena_alloc(arena,
                               (size_t)def->nctxs * sizeof(*m->ctx_names),
                               _Alignof(const char *));
    for (i = 0U; i < def->nctxs; i++) {
        SynCtx *ctx = &def->ctxs[i];
        const u8 *first;

        (void)memset(ctx, 0, sizeof(*ctx));
        ctx->first_rule = blob_read_u32(&r);
        ctx->nrules = blob_read_u32(&r);
        ctx->dflt_attr = blob_read_u8(&r);
        ctx->at_eol = blob_read_u8(&r);
        ctx->eol_nop = blob_read_u8(&r);
        ctx->flags = blob_read_u8(&r);
        ctx->eol_target = blob_read_u16(&r);
        ctx->embed.lang = blob_read_u32(&r);
        ctx->embed.ctx = blob_read_u32(&r);
        ctx->embed.lang_kind = blob_read_u8(&r);
        ctx->embed.lang_group = blob_read_u8(&r);
        ctx->embed.end = blob_read_u8(&r);
        ctx->embed.fallback = blob_read_u8(&r);
        ctx->embed.flags = blob_read_u8(&r);
        first = blob_read_bytes(&r, sizeof(ctx->first));
        (void)first;
        m->ctx_names[i] = blob_read_string(&r, arena, NULL);
        if (!r.ok || m->ctx_names[i] == NULL ||
            ctx->first_rule > def->nrules ||
            ctx->nrules > def->nrules - ctx->first_rule ||
            ctx->dflt_attr >= YEW_ATTR__COUNT || ctx->at_eol > SYN_OP_SET ||
            ctx->eol_nop > 4U ||
            (ctx->flags & (u8)~(YEW_SYN_CTX_UNIT_SPAN |
                                YEW_SYN_CTX_UNIT_ATOM |
                                YEW_SYN_CTX_EMBED_BRIDGE)) != 0U ||
            !syn_embed_boundary_valid(
                &ctx->embed,
                (ctx->flags & YEW_SYN_CTX_EMBED_BRIDGE) != 0U,
                (size_t)naux + 1U) ||
            (ctx->at_eol == SYN_OP_SET && ctx->eol_target >= def->nctxs))
            goto fail;
    }
    def->rules = arena_alloc(arena,
                             (size_t)def->nrules * sizeof(*def->rules),
                             _Alignof(SynRule));
    m->patterns = arena_alloc(arena,
                              (size_t)def->nrules * sizeof(*m->patterns),
                              _Alignof(const char *));
    for (i = 0U; i < def->nrules; i++) {
        SynRule *rule = &def->rules[i];
        const u8 *bytes;
        u32 j;
        u32 re_len;
        size_t used = 0U;

        (void)memset(rule, 0, sizeof(*rule));
        rule->attr = blob_read_u8(&r);
        rule->op = blob_read_u8(&r);
        rule->nop = blob_read_u8(&r);
        rule->aux_match = blob_read_u8(&r);
        rule->target = blob_read_u16(&r);
        rule->consume = blob_read_u8(&r);
        rule->flags = blob_read_u8(&r);
        rule->value_pred = blob_read_u8(&r);
        rule->aux_add = blob_read_u8(&r);
        rule->aux_add_group = blob_read_u8(&r);
        bytes = blob_read_bytes(&r, sizeof(rule->caps));
        if (bytes != NULL)
            (void)memcpy(rule->caps, bytes, sizeof(rule->caps));
        rule->aux_group = blob_read_u8(&r);
        rule->end = blob_read_u8(&r);
        rule->npush = blob_read_u8(&r);
        rule->embed.lang = blob_read_u32(&r);
        rule->embed.ctx = blob_read_u32(&r);
        rule->embed.lang_kind = blob_read_u8(&r);
        rule->embed.lang_group = blob_read_u8(&r);
        rule->embed.end = blob_read_u8(&r);
        rule->embed.fallback = blob_read_u8(&r);
        rule->embed.flags = blob_read_u8(&r);
        rule->aux_pre = blob_read_u32(&r);
        rule->aux_post = blob_read_u32(&r);
        for (j = 0U; j < YEW_ARRAY_LEN(rule->push); j++)
            rule->push[j] = blob_read_u16(&r);
        bytes = blob_read_bytes(&r, sizeof(rule->first));
        m->patterns[i] = blob_read_string(&r, arena, NULL);
        re_len = blob_read_u32(&r);
        bytes = blob_read_bytes(&r, re_len);
        if (re_len != 0U) {
            rule->re = yew_re_unpack(arena, bytes, re_len, &used);
            if (rule->re == NULL || used != re_len)
                r.ok = false;
        }
        if (rule->re != NULL)
            yew_re_first_bytes(rule->re, rule->first);
        else if (rule->aux_match != SYN_AUXM_NONE)
            (void)memset(rule->first, 0xff, sizeof(rule->first));
        if (!r.ok || !syn_rule_valid(rule, def, (size_t)naux + 1U) ||
            (rule->aux_match == SYN_AUXM_NONE) != (rule->re != NULL))
            goto fail;
    }
    for (i = 0U; i < def->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];
        bool bridge = (ctx->flags & YEW_SYN_CTX_EMBED_BRIDGE) != 0U;
        bool has_end = false;
        u32 j;

        for (j = 0U; j < ctx->nrules; j++) {
            const SynRule *rule = &def->rules[ctx->first_rule + j];

            if (rule->end != 0U) {
                if (!bridge)
                    goto fail;
                has_end = true;
            }
            if (rule->op == SYN_OP_EMBED) {
                const SynCtx *target = &def->ctxs[rule->target];

                if ((target->flags & YEW_SYN_CTX_EMBED_BRIDGE) == 0U ||
                    !embed_boundary_same(&target->embed, &rule->embed))
                    goto fail;
            }
        }
        if (bridge && !has_end && ctx->at_eol != SYN_OP_POP &&
            ctx->at_eol != SYN_OP_SET)
            goto fail;
    }
    if (!r.ok || r.at != r.len || def->name == NULL ||
        expected_name == NULL || strcmp(def->name, expected_name) != 0)
        goto fail;
    for (i = 0U; i < def->nctxs; i++) {
        SynCtx *ctx = &def->ctxs[i];
        u32 j;

        (void)memset(ctx->first, 0, sizeof(ctx->first));
        for (j = 0U; j < ctx->nrules; j++)
            first_union(ctx->first, def->rules[ctx->first_rule + j].first);
    }
    m->def = def;
    m->lang.id = language_id(def->name);
    (void)cached_id;
    meta_link(m);
    return def;

fail:
    if (aux != NULL)
        interner_free(aux);
    free(m);
    return NULL;
}

static u64 fnv64(const u8 *p, size_t n)
{
    u64 hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0U; i < n; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static u32 crc32_bytes(const u8 *p, size_t n)
{
    u32 crc = UINT32_MAX;
    size_t i;

    for (i = 0U; i < n; i++) {
        u32 bit;

        crc ^= p[i];
        for (bit = 0U; bit < 8U; bit++)
            crc = (crc >> 1U) ^
                  (UINT32_C(0xedb88320) & (u32)-(i32)(crc & 1U));
    }
    return ~crc;
}

static u32 abi_tag(void)
{
    const u16 one = 1U;
    const u8 little = *(const u8 *)&one;
    u32 tag = UINT32_C(2166136261);
    const u32 values[] = {
        (u32)sizeof(void *), (u32)sizeof(SynCtx), (u32)_Alignof(SynCtx),
        (u32)sizeof(SynRule), (u32)_Alignof(SynRule),
        (u32)sizeof(SynDef), (u32)_Alignof(SynDef), (u32)little
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(values); i++) {
        tag ^= values[i];
        tag *= UINT32_C(16777619);
    }
    return tag;
}

static bool fd_stat_definition(int fd, struct stat *st)
{
    if (fstat(fd, st) != 0)
        return false;
    if (!S_ISREG(st->st_mode)) {
        errno = EINVAL;
        return false;
    }
    if (st->st_size < 0 || (u64)st->st_size > SYN_DEF_MAX_BYTES) {
        errno = EFBIG;
        return false;
    }
    return true;
}

static bool read_whole(const char *path, Bytebuf *out, struct stat *st)
{
    int fd;
    u8 buf[16384];

    bytebuf_init(out);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    if (!fd_stat_definition(fd, st)) {
        int saved = errno;

        (void)close(fd);
        errno = saved;
        return false;
    }
    for (;;) {
        ssize_t got = read(fd, buf, sizeof(buf));

        if (got > 0) {
            if ((size_t)got > SYN_DEF_MAX_BYTES - out->len) {
                (void)close(fd);
                bytebuf_free(out);
                errno = EFBIG;
                return false;
            }
            bytebuf_append(out, buf, (size_t)got);
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            (void)close(fd);
            bytebuf_free(out);
            return false;
        }
        break;
    }
    if (!fd_stat_definition(fd, st)) {
        int saved = errno;

        (void)close(fd);
        bytebuf_free(out);
        errno = saved;
        return false;
    }
    if (close(fd) != 0) {
        bytebuf_free(out);
        return false;
    }
    return true;
}

static bool source_stat(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY);
    int saved;

    if (fd < 0)
        return false;
    if (fd_stat_definition(fd, st)) {
        if (close(fd) == 0)
            return true;
        return false;
    }
    saved = errno;
    (void)close(fd);
    errno = saved;
    return false;
}

static char *runtime_definition_path(const SynLangSeed *seed);

char *yew_syn_cache_dir(void)
{
    char *root = yew_xdg_cache_dir();
    char *path;
    size_t n;

    if (root == NULL)
        return NULL;
    n = strlen(root) + strlen("/syn");
    path = yew_xmalloc(n + 1U);
    (void)snprintf(path, n + 1U, "%s/syn", root);
    free(root);
    return path;
}

char *yew_syn_cache_path(const char *name)
{
    char *dir;
    char *path;
    size_t n;

    if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL)
        return NULL;
    dir = yew_syn_cache_dir();
    if (dir == NULL)
        return NULL;
    n = strlen(dir) + 1U + strlen(name) + strlen(".stab");
    path = yew_xmalloc(n + 1U);
    (void)snprintf(path, n + 1U, "%s/%s.stab", dir, name);
    free(dir);
    return path;
}

void yew_syn_cache_set_bypass(bool bypass)
{
    cache_bypass = bypass;
}

bool yew_syn_cache_clear(void)
{
    char *dir = yew_syn_cache_dir();
    DIR *stream;
    struct dirent *entry;
    bool ok = true;

    if (dir == NULL)
        return false;
    stream = opendir(dir);
    if (stream == NULL) {
        ok = errno == ENOENT;
        free(dir);
        return ok;
    }
    errno = 0;
    while ((entry = readdir(stream)) != NULL) {
        size_t len = strlen(entry->d_name);

        if (len <= 5U || strcmp(entry->d_name + len - 5U, ".stab") != 0)
            continue;
        {
            size_t n = strlen(dir) + 1U + len;
            char *path = yew_xmalloc(n + 1U);

            (void)snprintf(path, n + 1U, "%s/%s", dir, entry->d_name);
            if (unlink(path) != 0 && errno != ENOENT)
                ok = false;
            free(path);
        }
    }
    if (errno != 0)
        ok = false;
    if (closedir(stream) != 0)
        ok = false;
    free(dir);
    return ok;
}

static bool cache_header_ok(const u8 *p, size_t n, size_t *blob_len)
{
    u64 version_hash = fnv64((const u8 *)YEW_VERSION, strlen(YEW_VERSION));
    u32 len;

    if (n < YEW_SYN_CACHE_HEADER_SIZE ||
        memcmp(p, YEW_SYN_CACHE_MAGIC, 8U) != 0 ||
        get32(p + 8U) != YEW_SYN_TABLE_VERSION ||
        get32(p + 12U) != abi_tag() || get64(p + 16U) != version_hash)
        return false;
    len = get32(p + 56U);
    if ((size_t)len != n - YEW_SYN_CACHE_HEADER_SIZE ||
        crc32_bytes(p + YEW_SYN_CACHE_HEADER_SIZE, len) != get32(p + 60U))
        return false;
    *blob_len = len;
    return true;
}

static bool cache_write(const char *path, const struct stat *st,
                        const u8 *src, size_t src_len, const u8 *blob,
                        size_t blob_len)
{
    Bytebuf bytes;
    char *dir = yew_syn_cache_dir();
    u8 header[YEW_SYN_CACHE_HEADER_SIZE];
    bool ok;

    if (path == NULL || dir == NULL || blob_len > UINT32_MAX) {
        free(dir);
        return false;
    }
    if (!yew_mkdirs(dir, 0700U)) {
        free(dir);
        return false;
    }
    free(dir);
    (void)memset(header, 0, sizeof(header));
    (void)memcpy(header, YEW_SYN_CACHE_MAGIC, 8U);
    put32(header + 8U, YEW_SYN_TABLE_VERSION);
    put32(header + 12U, abi_tag());
    put64(header + 16U, fnv64((const u8 *)YEW_VERSION, strlen(YEW_VERSION)));
    put64(header + 24U, (u64)st->st_mtim.tv_sec);
    put64(header + 32U, (u64)st->st_mtim.tv_nsec);
    put64(header + 40U, (u64)st->st_size);
    put64(header + 48U, fnv64(src, src_len));
    put32(header + 56U, (u32)blob_len);
    put32(header + 60U, crc32_bytes(blob, blob_len));
    bytebuf_init(&bytes);
    bytebuf_append(&bytes, header, sizeof(header));
    bytebuf_append(&bytes, blob, blob_len);
    ok = yew_file_write_atomic(path, bytes.data, bytes.len, 0600U) ==
         YEW_SAVE_OK;
    bytebuf_free(&bytes);
    return ok;
}

static char *builtin_language_for_source(const char *source)
{
    size_t i;

    for (i = 0U; i < yew_syn_builtin_langs_len; i++) {
        char *resolved;
        bool matches;
        const char *builtin_name = yew_syn_builtin_langs[i].name;

        if (strcmp(source, yew_syn_builtin_langs[i].source) == 0) {
            char *name = yew_xmalloc(strlen(builtin_name) + 1U);

            (void)memcpy(name, builtin_name, strlen(builtin_name) + 1U);
            return name;
        }
        resolved = runtime_definition_path(&yew_syn_builtin_langs[i]);
        matches = strcmp(source, resolved) == 0;
        free(resolved);
        if (matches) {
            char *name = yew_xmalloc(strlen(builtin_name) + 1U);

            (void)memcpy(name, builtin_name, strlen(builtin_name) + 1U);
            return name;
        }
    }
    return NULL;
}

static char *definition_language_name(const u8 *src, size_t len)
{
    Arena arena;
    DiagCtx dc;
    Compile c;
    FlNode *top;
    FlNode *language;
    FlNode *name;
    const char *value;
    size_t value_len;
    char *copy = NULL;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, discovery_diag_discard, NULL);
    (void)memset(&c, 0, sizeof(c));
    c.arena = &arena;
    c.dc = &dc;
    interner_init(&c.in, &arena);
    top = fl_parse_literal(&arena, &dc, &c.in, (const char *)src, len, 0U);
    language = map_find(&c, top, "language");
    name = map_find(&c, language, "name");
    value = node_str(&c, name, &value_len);
    if (value != NULL && value_len != 0U && strchr(value, '/') == NULL &&
        !(value_len == 1U && value[0] == '.') &&
        !(value_len == 2U && value[0] == '.' && value[1] == '.')) {
        copy = yew_xmalloc(value_len + 1U);
        (void)memcpy(copy, value, value_len);
        copy[value_len] = '\0';
    }
    interner_free(&c.in);
    arena_free_all(&arena);
    return copy;
}

SynDef *yew_syn_def_load(Arena *a, DiagCtx *dc, const char *path)
{
    Bytebuf source;
    Bytebuf cached;
    Bytebuf packed;
    struct stat src_st;
    struct stat cache_st;
    char *expected_name = NULL;
    char *cache_path;
    size_t blob_len = 0U;
    bool valid_cache = false;
    bool bypass;
    bool cache_warned = false;
    bool source_loaded = false;
    const char *no_cache;
    u32 file_id;
    u32 errors = 0U;
    u32 warnings = 0U;
    SynDef *def = NULL;
    char *owned;

    if (a == NULL || dc == NULL || path == NULL)
        return NULL;
    bytebuf_init(&source);
    bytebuf_init(&cached);
    bytebuf_init(&packed);
    if (!source_stat(path, &src_st)) {
        file_id = fl_diag_add_file(dc, path, "", 0U);
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){file_id, 1U, 1U, 1U},
                     "cannot read syntax definition: %s", strerror(errno));
        return NULL;
    }
    expected_name = builtin_language_for_source(path);
    if (expected_name == NULL) {
        if (!read_whole(path, &source, &src_st)) {
            file_id = fl_diag_add_file(dc, path, "", 0U);
            fl_diag_emit(dc, FL_DIAG_ERROR,
                         (FlSpan){file_id, 1U, 1U, 1U},
                         "cannot read syntax definition: %s",
                         strerror(errno));
            return NULL;
        }
        source_loaded = true;
        expected_name = definition_language_name(source.data, source.len);
    }
    cache_path = yew_syn_cache_path(expected_name);
    no_cache = getenv("YEW_NO_SYN_CACHE");
    bypass = cache_bypass || (no_cache != NULL && strcmp(no_cache, "1") == 0);
    if (!bypass && cache_path != NULL) {
        bool exists = access(cache_path, F_OK) == 0;

        if (read_whole(cache_path, &cached, &cache_st)) {
            valid_cache = cache_header_ok(cached.data, cached.len, &blob_len);
        } else if (exists) {
            yew_log(YEW_LOG_WARN, "syntax cache unreadable; recompiling %s",
                    path);
            cache_warned = true;
        }
        if (cached.len != 0U && !valid_cache) {
            yew_log(YEW_LOG_WARN, "syntax cache corrupt; recompiling %s",
                    path);
            cache_warned = true;
        } else if (valid_cache && !source_loaded &&
                   get64(cached.data + 24U) == (u64)src_st.st_mtim.tv_sec &&
                   get64(cached.data + 32U) == (u64)src_st.st_mtim.tv_nsec &&
                   get64(cached.data + 40U) == (u64)src_st.st_size) {
            file_id = fl_diag_add_file(dc, path, "", 0U);
            (void)file_id;
            def = syn_blob_unpack(a,
                                  cached.data + YEW_SYN_CACHE_HEADER_SIZE,
                                  blob_len, path, expected_name);
            if (def != NULL)
                goto done;
            yew_log(YEW_LOG_WARN,
                    "syntax cache tables invalid; recompiling %s", path);
            cache_warned = true;
            valid_cache = false;
        }
    }
    if (!source_loaded && !read_whole(path, &source, &src_st)) {
        file_id = fl_diag_add_file(dc, path, "", 0U);
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){file_id, 1U, 1U, 1U},
                     "cannot read syntax definition: %s", strerror(errno));
        goto done;
    }
    source_loaded = true;
    if (valid_cache &&
        get64(cached.data + 48U) == fnv64(source.data, source.len)) {
        def = syn_blob_unpack(a, cached.data + YEW_SYN_CACHE_HEADER_SIZE,
                              blob_len, path, expected_name);
        if (def != NULL) {
            if (!cache_write(cache_path, &src_st, source.data, source.len,
                             cached.data + YEW_SYN_CACHE_HEADER_SIZE,
                             blob_len))
                yew_log(YEW_LOG_WARN,
                        "syntax cache metadata update failed: %s", cache_path);
            goto done;
        }
        if (!cache_warned)
            yew_log(YEW_LOG_WARN,
                    "syntax cache tables invalid; recompiling %s", path);
    }
    owned = arena_strndup(a, (const char *)source.data, source.len);
    file_id = fl_diag_add_file(dc, path, owned, source.len);
    def = yew_syn_def_compile(a, dc, (const u8 *)owned, source.len, file_id,
                              &errors, &warnings);
    if (def != NULL && expected_name != NULL &&
        strcmp(def->name, expected_name) != 0) {
        fl_diag_emit(dc, FL_DIAG_ERROR,
                     (FlSpan){file_id, 1U, 1U, 1U},
                     "syntax language name '%s' does not match expected '%s'",
                     def->name, expected_name);
        yew_syn_def_dispose(def);
        def = NULL;
    }
    if (def != NULL) {
        DefMeta *m = meta_for(def);

        if (m != NULL)
            m->lang.source = arena_strdup(a, path);
        if (!bypass && cache_path != NULL) {
            if (!syn_blob_pack(def, path, &packed) ||
                !cache_write(cache_path, &src_st, (const u8 *)owned,
                             source.len, packed.data, packed.len))
                yew_log(YEW_LOG_WARN, "syntax cache write failed: %s",
                        cache_path);
        }
    }
    (void)errors;
    (void)warnings;

done:
    free(expected_name);
    free(cache_path);
    bytebuf_free(&source);
    bytebuf_free(&cached);
    bytebuf_free(&packed);
    return def;
}

static bool first_has(const u8 first[32], u8 byte)
{
    return (first[byte >> 3U] & (u8)(1U << (byte & 7U))) != 0U;
}

bool yew_syn_def_firstbyte_check(const SynDef *def, u32 *bad_rule,
                                 u8 *bad_byte)
{
    static const u8 tails[][12] = {
        "", "a", "0", " ", "true", "=value", "]", "\"text\""
    };
    u32 i;

    if (def == NULL)
        return false;
    for (i = 0U; i < def->nrules; i++) {
        const SynRule *rule = &def->rules[i];
        u32 byte;

        if (rule->re == NULL)
            continue;
        for (byte = 0U; byte < 256U; byte++) {
            u32 t;

            for (t = 0U; t < YEW_ARRAY_LEN(tails); t++) {
                u8 sample[16];
                size_t tail_len = strlen((const char *)tails[t]);
                YewReInput in;
                YewReMatch match;

                sample[0] = (u8)byte;
                (void)memcpy(sample + 1U, tails[t], tail_len);
                in = yew_re_input_bytes(sample, 1U + tail_len);
                if (yew_re_match_at(rule->re, &in, BYTEOFF(0U), &match) &&
                    !first_has(rule->first, (u8)byte)) {
                    if (bad_rule != NULL)
                        *bad_rule = i;
                    if (bad_byte != NULL)
                        *bad_byte = (u8)byte;
                    return false;
                }
            }
        }
    }
    return true;
}

static bool ascii_equal_fold(const char *a, size_t an, const char *b,
                             size_t bn)
{
    size_t i;

    if (an != bn)
        return false;
    for (i = 0U; i < an; i++) {
        u8 ac = (u8)a[i];
        u8 bc = (u8)b[i];

        if (ac >= (u8)'A' && ac <= (u8)'Z')
            ac = (u8)(ac + ((u8)'a' - (u8)'A'));
        if (bc >= (u8)'A' && bc <= (u8)'Z')
            bc = (u8)(bc + ((u8)'a' - (u8)'A'));
        if (ac != bc)
            return false;
    }
    return true;
}

static const char *path_base(const char *path)
{
    const char *slash;

    if (path == NULL)
        return "";
    slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool one_star_glob(const char *pat, const char *text)
{
    const char *star = strchr(pat, '*');
    size_t pn;
    size_t tn;
    size_t prefix;
    size_t suffix;

    if (star == NULL || strchr(star + 1, '*') != NULL)
        return false;
    pn = strlen(pat);
    tn = strlen(text);
    prefix = (size_t)(star - pat);
    suffix = pn - prefix - 1U;
    return tn >= prefix + suffix && memcmp(pat, text, prefix) == 0 &&
           memcmp(star + 1, text + tn - suffix, suffix) == 0;
}

static bool better(const SynLangDesc *a, const SynLangDesc *b)
{
    return b == NULL || a->priority > b->priority ||
           (a->priority == b->priority && strcmp(a->name, b->name) < 0);
}

typedef bool (*LangPred)(const SynLangDesc *, void *);

static const SynLangDesc *builtin_desc_for_id(u32 lang)
{
    size_t ordinal;

    if (lang == YEW_LANG_NONE)
        return NULL;
    ordinal = (size_t)lang - 1U;
    if (ordinal >= yew_syn_builtin_langs_len ||
        yew_syn_builtin_langs[ordinal].id != lang)
        return NULL;
    return builtin_desc_at(ordinal);
}

static const SynLangDesc *best_detect_run(SynDetectRun run)
{
    const SynLangDesc *best = NULL;
    size_t i;

    for (i = 0U; i < run.len; i++) {
        const SynLangDesc *lang = builtin_desc_for_id(run.entry[i].lang);

        if (lang != NULL && better(lang, best))
            best = lang;
    }
    return best;
}

static const SynLangDesc *best_user_language(LangPred pred, void *ctx,
                                              const SynLangDesc *best)
{
    DefMeta *m;

    for (m = user_metas; m != NULL; m = m->user_next) {
        if (!m->retired && pred(&m->lang, ctx) && better(&m->lang, best))
            best = &m->lang;
    }
    return best;
}

static size_t language_extension_len(const SynLangDesc *lang,
                                     const char *base)
{
    size_t base_len = strlen(base);
    size_t longest = 0U;
    u32 i;

    for (i = 0U; i < lang->nextensions; i++) {
        const char *ext = lang->extensions[i];
        size_t n = strlen(ext);

        if (n <= longest || base_len <= n || base[base_len - n - 1U] != '.')
            continue;
        if (ascii_equal_fold(base + base_len - n, n, ext, n))
            longest = n;
    }
    return longest;
}

typedef struct PathMatch {
    const char *base;
    const SynLangDesc *exact;
    const SynLangDesc *glob;
    const SynLangDesc *extension;
    size_t extension_len;
} PathMatch;

static void path_match_consider(PathMatch *match, const SynLangDesc *lang)
{
    size_t extension_len;
    u32 i;

    for (i = 0U; i < lang->nfilenames; i++) {
        const char *pattern = lang->filenames[i];
        bool glob = strchr(pattern, '*') != NULL;

        if (!glob && strcmp(pattern, match->base) == 0 &&
            better(lang, match->exact))
            match->exact = lang;
        if (glob && one_star_glob(pattern, match->base) &&
            better(lang, match->glob))
            match->glob = lang;
    }
    extension_len = language_extension_len(lang, match->base);
    if (extension_len > match->extension_len ||
        (extension_len != 0U && extension_len == match->extension_len &&
         better(lang, match->extension))) {
        match->extension_len = extension_len;
        match->extension = lang;
    }
}

static const SynLangDesc *path_language(const char *base)
{
    PathMatch match = {base, NULL, NULL, NULL, 0U};
    SynDetectRun run;
    DefMeta *m;
    size_t i;
    size_t base_len = strlen(base);
    const char *dot;

    run = yew_syn_detect_find(yew_syn_builtin_detect_index.exact,
                              yew_syn_builtin_detect_index.nexact,
                              base, base_len, false);
    match.exact = best_detect_run(run);
    for (i = 0U; i < yew_syn_builtin_detect_index.nglobs; i++) {
        const SynDetectEntry *entry =
            &yew_syn_builtin_detect_index.globs[i];
        const SynLangDesc *lang;

        size_t prefix = entry->split;
        size_t suffix = entry->key_len - prefix - 1U;

        if (base_len < prefix + suffix ||
            memcmp(entry->key, base, prefix) != 0 ||
            memcmp(entry->key + prefix + 1U,
                   base + base_len - suffix, suffix) != 0)
            continue;
        lang = builtin_desc_for_id(entry->lang);
        if (lang != NULL && better(lang, match.glob))
            match.glob = lang;
    }
    dot = strchr(base, '.');
    while (dot != NULL) {
        const char *extension = dot + 1;
        size_t extension_len = (size_t)(base + base_len - extension);
        const SynLangDesc *lang;

        run = yew_syn_detect_find(yew_syn_builtin_detect_index.extensions,
                                  yew_syn_builtin_detect_index.nextensions,
                                  extension, extension_len, true);
        lang = best_detect_run(run);
        if (lang != NULL &&
            (extension_len > match.extension_len ||
             (extension_len == match.extension_len &&
              better(lang, match.extension)))) {
            match.extension_len = extension_len;
            match.extension = lang;
        }
        dot = strchr(dot + 1, '.');
    }
    for (m = user_metas; m != NULL; m = m->user_next) {
        if (!m->retired)
            path_match_consider(&match, &m->lang);
    }
    if (match.exact != NULL)
        return match.exact;
    if (match.glob != NULL)
        return match.glob;
    return match.extension;
}

typedef struct ShebangMatch {
    const char *name;
    size_t len;
} ShebangMatch;

static bool shebang_pred(const SynLangDesc *lang, void *opaque)
{
    ShebangMatch *m = opaque;
    u32 i;

    for (i = 0U; i < lang->nshebangs; i++) {
        if (ascii_equal_fold(lang->shebangs[i], strlen(lang->shebangs[i]),
                             m->name, m->len))
            return true;
    }
    return false;
}

static bool parse_shebang(const u8 *line, u32 len, ShebangMatch *out)
{
    u32 at = 2U;
    u32 start;
    u32 end;
    u32 slash;

    if (line == NULL || len < 3U || line[0] != '#' || line[1] != '!')
        return false;
    while (at < len && (line[at] == ' ' || line[at] == '\t'))
        at++;
    start = at;
    while (at < len && line[at] != ' ' && line[at] != '\t' &&
           line[at] != '\r' && line[at] != '\n')
        at++;
    end = at;
    slash = end;
    while (slash > start && line[slash - 1U] != '/')
        slash--;
    start = slash;
    if (ascii_equal_fold((const char *)line + start, end - start, "env", 3U)) {
        do {
            while (at < len && (line[at] == ' ' || line[at] == '\t'))
                at++;
            start = at;
            while (at < len && line[at] != ' ' && line[at] != '\t' &&
                   line[at] != '\r' && line[at] != '\n')
                at++;
            end = at;
        } while (start < end && line[start] == '-');
        slash = end;
        while (slash > start && line[slash - 1U] != '/')
            slash--;
        start = slash;
    }
    if (start == end)
        return false;
    out->name = (const char *)line + start;
    out->len = end - start;
    return true;
}

static YewRe *detection_re(const SynLangDesc *lang)
{
    DefMeta *m;
    size_t i;

    for (m = user_metas; m != NULL; m = m->user_next) {
        if (!m->retired && m->lang.id == lang->id &&
            m->first_line_re != NULL)
            return m->first_line_re;
    }
    builtin_registry_init();
    for (i = 0U; i < builtin_registry.len; i++) {
        if (builtin_registry.desc[i].id != lang->id)
            continue;
        return yew_syn_builtin_registry_first_line(&builtin_registry, i);
    }
    return NULL;
}

typedef struct FirstLineMatch {
    const u8 *line;
    u32 len;
} FirstLineMatch;

static bool first_line_pred(const SynLangDesc *lang, void *opaque)
{
    FirstLineMatch *m = opaque;
    YewRe *re;
    YewReInput in;
    YewReMatch match;

    if (lang->first_line == NULL)
        return false;
    re = detection_re(lang);
    if (re == NULL)
        return false;
    in = yew_re_input_bytes(m->line, m->len);
    return yew_re_match_at(re, &in, BYTEOFF(0U), &match);
}

u32 yew_syn_lang_for(const char *path, const u8 *line1, u32 l1_len)
{
    const char *base = path_base(path);
    const SynLangDesc *found;
    ShebangMatch shebang;
    FirstLineMatch first = {line1, l1_len};

    discover_user_definitions();
    found = path_language(base);
    if (found != NULL)
        return found->id;
    if (parse_shebang(line1, l1_len, &shebang)) {
        SynDetectRun run = yew_syn_detect_find(
            yew_syn_builtin_detect_index.shebangs,
            yew_syn_builtin_detect_index.nshebangs,
            shebang.name, shebang.len, true);

        found = best_detect_run(run);
        found = best_user_language(shebang_pred, &shebang, found);
        if (found != NULL)
            return found->id;
    }
    if (line1 != NULL) {
        size_t i;

        found = NULL;
        for (i = 0U; i < yew_syn_builtin_detect_index.nfirst_lines; i++) {
            const SynLangDesc *lang = builtin_desc_for_id(
                yew_syn_builtin_detect_index.first_lines[i].lang);

            if (lang != NULL && first_line_pred(lang, &first) &&
                better(lang, found))
                found = lang;
        }
        found = best_user_language(first_line_pred, &first, found);
        if (found != NULL)
            return found->id;
    }
    return YEW_LANG_NONE;
}

static u32 fortran_lang(SynFortranForm form)
{
    return yew_syn_lang_named(form == YEW_FORTRAN_FIXED ?
                              "fortran-fixed" : "fortran");
}

u32 yew_syn_lang_for_scored(const char *path, const u8 *line1, u32 l1_len,
                            const SynFortranScore *fortran,
                            SynFortranForm override, bool sniff_legacy)
{
    SynFortranForm scored;
    u32 detected;

    if (override == YEW_FORTRAN_FREE || override == YEW_FORTRAN_FIXED)
        return fortran_lang(override);
    scored = yew_syn_fortran_score_result(fortran);
    if (sniff_legacy && yew_syn_fortran_legacy_path(path) &&
        fortran != NULL)
        return fortran_lang(fortran->fixed_form > fortran->free_form ?
                            YEW_FORTRAN_FIXED : YEW_FORTRAN_FREE);
    detected = yew_syn_lang_for(path, line1, l1_len);
    if (detected != YEW_LANG_NONE)
        return detected;
    if (!yew_syn_fortran_ambiguous_path(path) || fortran == NULL)
        return YEW_LANG_NONE;
    if (fortran->signals == 0U &&
        (path == NULL || strchr(path_base(path), '.') == NULL))
        return YEW_LANG_NONE;
    if (scored == YEW_FORTRAN_AUTO)
        scored = YEW_FORTRAN_FREE;
    return fortran_lang(scored);
}

static char *runtime_definition_path(const SynLangSeed *seed)
{
    const char *root = getenv("YEW_RUNTIME_DIR");
    const char *relative = seed->source;
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
    root = YEW_RUNTIME_DIR_DEFAULT;
    n = strlen(root) + 1U + strlen(relative);
    path = yew_xmalloc(n + 1U);
    (void)snprintf(path, n + 1U, "%s/%s", root, relative);
    if (access(path, R_OK) == 0)
        return path;
    free(path);
    path = yew_xmalloc(strlen(seed->source) + 1U);
    (void)memcpy(path, seed->source, strlen(seed->source) + 1U);
    return path;
}

static SynDef *load_builtin_definition(Arena *arena, DiagCtx *dc,
                                       const SynLangSeed *seed, void *ctx)
{
    char *path;
    SynDef *def;

    (void)ctx;
    path = runtime_definition_path(seed);
    def = yew_syn_def_load(arena, dc, path);
    free(path);
    return def;
}

const SynDef *yew_syn_def_for(u32 lang)
{
    DefMeta *m;

    if (lang == YEW_LANG_NONE)
        return NULL;
    discover_user_definitions();
    for (m = metas; m != NULL; m = m->next) {
        if (!m->retired && m->lang.id == lang)
            return m->def;
    }
    builtin_registry_init();
    return yew_syn_builtin_registry_load(
        &builtin_registry, yew_syn_builtin_langs,
        yew_syn_builtin_langs_len, lang, load_builtin_definition, NULL);
}

SynEngine *yew_syn_engine_for(u32 lang)
{
    const SynDef *def = yew_syn_def_for(lang);
    DefMeta *m;

    if (def == NULL)
        return NULL;
    m = meta_for(def);
    if (m == NULL)
        return NULL;
    if (m->engine == NULL)
        m->engine = yew_syn_engine_new(m->def);
    return m->engine;
}
