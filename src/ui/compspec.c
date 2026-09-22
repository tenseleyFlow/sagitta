#define _POSIX_C_SOURCE 200809L

#include "ui/compspec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/data.h"
#include "fl/diag.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "ui/message.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/runtime_asset.h"
#include "util/sort.h"
#include "util/xdg.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

enum {
    /* A spec is a few KiB; a megabyte is a mistake, not a spec. */
    SPEC_MAX_BYTES = 1024U * 1024U,
    SPEC_NAME_MAX = 255U,
    SPEC_ERR_MAX = 512U
};

struct YewCompSpec {
    Arena arena;
    const char *origin;
    const char *const *commands;
    u32 n_commands;
    const char *description;
    bool has_precommand;
    YewShWrapper precommand;
    const YewSpecGen *gens;
    u32 n_gens;
    YewSpecNode root;
};

/* ---------------------------------------------------------------- */
/* Reading and validation (§1)                                       */
/* ---------------------------------------------------------------- */

static const char *const builtin_generators[] = {
    "hosts", "signals", "users", "make_targets", "pids"
};

bool yew_compspec_builtin_generator(const char *name)
{
    size_t i;

    if (name == NULL)
        return false;
    for (i = 0U; i < YEW_ARRAY_LEN(builtin_generators); i++) {
        if (strcmp(name, builtin_generators[i]) == 0)
            return true;
    }
    return false;
}

static const char *const arg_kind_names[YEW_SPEC_ARG__N] = {
    "path", "dir", "exec", "command", "var", "user", "host", "pid",
    "signal", "values", "generator", "none"
};

typedef struct SpecRead {
    YewCompSpec *spec;
    Arena *a;
    const char *origin;
    char *err;
    size_t errsz;
    bool failed;
    /* The key path of whatever is being read, for the message. */
    char path[256];
    size_t path_len;
} SpecRead;

static void read_fail(SpecRead *r, const char *fmt, ...)
{
    char msg[SPEC_ERR_MAX];
    va_list ap;

    if (r->failed)
        return; /* the first reason is the one worth reading */
    r->failed = true;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (r->err == NULL || r->errsz == 0U)
        return;
    if (r->path_len == 0U)
        (void)snprintf(r->err, r->errsz, "%s: %s", r->origin, msg);
    else
        (void)snprintf(r->err, r->errsz, "%s: %s (at %s)", r->origin, msg,
                       r->path);
}

/* Append ".seg" (or "[seg]") to the key path; returns the old length for
 * path_pop.  A path longer than the buffer is truncated, never overrun. */
static size_t path_push(SpecRead *r, const char *fmt, ...)
{
    size_t old = r->path_len;
    va_list ap;
    int n;

    if (r->path_len >= sizeof(r->path) - 1U)
        return old;
    va_start(ap, fmt);
    n = vsnprintf(r->path + r->path_len, sizeof(r->path) - r->path_len, fmt,
                  ap);
    va_end(ap);
    if (n > 0) {
        r->path_len += (size_t)n;
        if (r->path_len >= sizeof(r->path))
            r->path_len = sizeof(r->path) - 1U;
    }
    return old;
}

static void path_pop(SpecRead *r, size_t old)
{
    r->path_len = old;
    r->path[old] = '\0';
}

static size_t push_key(SpecRead *r, const char *key)
{
    return path_push(r, r->path_len == 0U ? "%s" : ".%s", key);
}

static bool as_str(FlValue v, const char **s, size_t *n)
{
    const FlStr *str;

    if (v.t != (u8)FL_STR)
        return false;
    str = (const FlStr *)v.as.o;
    if (memchr(str->b, '\0', str->len) != NULL)
        return false;
    *s = str->b;
    *n = str->len;
    return true;
}

static const FlList *as_list(FlValue v)
{
    return v.t == (u8)FL_LIST ? (const FlList *)v.as.o : NULL;
}

static const FlMap *as_map(FlValue v)
{
    return v.t == (u8)FL_MAP ? (const FlMap *)v.as.o : NULL;
}

static bool key_is(const char *k, size_t n, const char *want)
{
    return strlen(want) == n && memcmp(k, want, n) == 0;
}

/* A string field: copied into the spec's arena. */
static const char *read_string(SpecRead *r, FlValue v, const char *key)
{
    const char *s;
    size_t n;
    size_t old = push_key(r, key);
    const char *out = NULL;

    if (!as_str(v, &s, &n))
        read_fail(r, "'%s' must be a string", key);
    else
        out = arena_strndup(r->a, s, n);
    path_pop(r, old);
    return out;
}

static bool read_bool(SpecRead *r, FlValue v, const char *key)
{
    if (v.t != (u8)FL_BOOL) {
        size_t old = push_key(r, key);

        read_fail(r, "'%s' must be true or false", key);
        path_pop(r, old);
        return false;
    }
    return v.as.b;
}

/* A list of strings, NULL-terminated in the arena. */
static const char *const *read_strings(SpecRead *r, FlValue v,
                                       const char *key, u32 *count,
                                       bool allow_single)
{
    const FlList *l = as_list(v);
    const char **out;
    size_t old = push_key(r, key);
    u32 i;

    *count = 0U;
    if (l == NULL) {
        const char *s;
        size_t n;

        if (allow_single && as_str(v, &s, &n)) {
            out = arena_alloc(r->a, 2U * sizeof(*out), sizeof(void *));
            out[0] = arena_strndup(r->a, s, n);
            out[1] = NULL;
            *count = 1U;
            path_pop(r, old);
            return out;
        }
        read_fail(r, allow_single ? "'%s' must be a string or a list of "
                                    "strings"
                                  : "'%s' must be a list of strings",
                  key);
        path_pop(r, old);
        return NULL;
    }
    out = arena_alloc(r->a, ((size_t)l->n + 1U) * sizeof(*out),
                      sizeof(void *));
    for (i = 0U; i < l->n; i++) {
        const char *s;
        size_t n;

        if (!as_str(l->v[i], &s, &n)) {
            size_t at = path_push(r, "[%u]", (unsigned)i);

            read_fail(r, "'%s' must hold only strings", key);
            path_pop(r, at);
            break;
        }
        out[i] = arena_strndup(r->a, s, n);
    }
    out[l->n] = NULL;
    *count = l->n;
    path_pop(r, old);
    return out;
}

static void unknown_key(SpecRead *r, const char *k, size_t n)
{
    char key[64];
    size_t keep = n < sizeof(key) - 1U ? n : sizeof(key) - 1U;

    (void)memcpy(key, k, keep);
    key[keep] = '\0';
    read_fail(r, "unknown key '%s'", key);
}

static const YewSpecGen *find_gen(const YewCompSpec *spec, const char *name)
{
    u32 i;

    for (i = 0U; i < spec->n_gens; i++) {
        if (strcmp(spec->gens[i].name, name) == 0)
            return &spec->gens[i];
    }
    return NULL;
}

static bool read_arg(SpecRead *r, FlValue v, YewSpecArg *out)
{
    const FlMap *m = as_map(v);
    u32 cursor = 0U;
    FlValue k;
    FlValue val;
    bool have_kind = false;
    bool have_ext = false;
    bool have_values = false;
    bool have_gen = false;

    (void)memset(out, 0, sizeof(*out));
    if (m == NULL) {
        read_fail(r, "an argument must be a map with a 'kind'");
        return false;
    }
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const char *key;
        size_t n;

        if (!as_str(k, &key, &n)) {
            read_fail(r, "argument keys must be strings");
            continue;
        }
        if (key_is(key, n, "kind")) {
            const char *s;
            size_t sn;
            u32 i;
            size_t old = push_key(r, "kind");

            have_kind = true;
            if (!as_str(val, &s, &sn)) {
                read_fail(r, "'kind' must be a string");
            } else {
                for (i = 0U; i < (u32)YEW_SPEC_ARG__N; i++) {
                    if (key_is(s, sn, arg_kind_names[i]))
                        break;
                }
                if (i == (u32)YEW_SPEC_ARG__N)
                    read_fail(r, "unknown argument kind '%.*s'",
                              (int)(sn > 32U ? 32U : sn), s);
                else
                    out->kind = (YewSpecArgKind)i;
            }
            path_pop(r, old);
        } else if (key_is(key, n, "ext")) {
            have_ext = true;
            out->ext = read_strings(r, val, "ext", &out->n_ext, false);
        } else if (key_is(key, n, "values")) {
            const FlList *l = as_list(val);
            YewSpecValue *vals;
            size_t old = push_key(r, "values");
            u32 i;

            have_values = true;
            if (l == NULL) {
                read_fail(r, "'values' must be a list");
                path_pop(r, old);
                continue;
            }
            vals = arena_alloc(r->a,
                               ((size_t)l->n + 1U) * sizeof(*vals),
                               sizeof(void *));
            for (i = 0U; i < l->n; i++) {
                const FlMap *vm = as_map(l->v[i]);
                const char *s;
                size_t sn;
                size_t at = path_push(r, "[%u]", (unsigned)i);

                vals[i].value = NULL;
                vals[i].desc = NULL;
                if (as_str(l->v[i], &s, &sn)) {
                    vals[i].value = arena_strndup(r->a, s, sn);
                } else if (vm != NULL) {
                    u32 vc = 0U;
                    FlValue vk;
                    FlValue vv;

                    while (fl_map_iter(vm, &vc, &vk, &vv)) {
                        const char *vkey;
                        size_t vn;

                        if (!as_str(vk, &vkey, &vn))
                            read_fail(r, "value keys must be strings");
                        else if (key_is(vkey, vn, "value"))
                            vals[i].value = read_string(r, vv, "value");
                        else if (key_is(vkey, vn, "desc"))
                            vals[i].desc = read_string(r, vv, "desc");
                        else
                            unknown_key(r, vkey, vn);
                    }
                    if (vals[i].value == NULL)
                        read_fail(r, "a value map needs 'value'");
                } else {
                    read_fail(r, "a value must be a string or a "
                                 "{ value, desc } map");
                }
                path_pop(r, at);
            }
            out->values = vals;
            out->n_values = l->n;
            path_pop(r, old);
        } else if (key_is(key, n, "generator")) {
            have_gen = true;
            out->generator = read_string(r, val, "generator");
        } else if (key_is(key, n, "repeat")) {
            out->repeat = read_bool(r, val, "repeat");
        } else {
            unknown_key(r, key, n);
        }
    }
    if (!have_kind) {
        read_fail(r, "an argument needs 'kind'");
        return false;
    }
    if (have_ext && out->kind != YEW_SPEC_ARG_PATH)
        read_fail(r, "'ext' is only for kind \"path\"");
    if (have_values != (out->kind == YEW_SPEC_ARG_VALUES))
        read_fail(r, have_values ? "'values' is only for kind \"values\""
                                 : "kind \"values\" needs 'values'");
    if (have_gen != (out->kind == YEW_SPEC_ARG_GENERATOR))
        read_fail(r, have_gen ? "'generator' is only for kind \"generator\""
                              : "kind \"generator\" needs 'generator'");
    switch (out->kind) {
    case YEW_SPEC_ARG_HOST:
        out->generator = "hosts";
        break;
    case YEW_SPEC_ARG_PID:
        out->generator = "pids";
        break;
    case YEW_SPEC_ARG_SIGNAL:
        out->generator = "signals";
        break;
    case YEW_SPEC_ARG_USER:
        out->generator = "users";
        break;
    case YEW_SPEC_ARG_GENERATOR:
        if (out->generator == NULL)
            break;
        if (!yew_compspec_builtin_generator(out->generator)) {
            out->gen = find_gen(r->spec, out->generator);
            if (out->gen == NULL) {
                size_t old = push_key(r, "generator");

                read_fail(r, "generator '%s' is neither built in nor in "
                             "'generators'",
                          out->generator);
                path_pop(r, old);
            }
        }
        break;
    default:
        break;
    }
    return !r->failed;
}

static const YewSpecArg *read_arg_at(SpecRead *r, FlValue v,
                                     const char *key)
{
    YewSpecArg *arg = arena_alloc(r->a, sizeof(*arg), sizeof(void *));
    size_t old = push_key(r, key);

    (void)read_arg(r, v, arg);
    path_pop(r, old);
    return arg;
}

static void read_flag(SpecRead *r, FlValue v, YewSpecFlag *out)
{
    const FlMap *m = as_map(v);
    u32 cursor = 0U;
    FlValue k;
    FlValue val;

    (void)memset(out, 0, sizeof(*out));
    if (m == NULL) {
        read_fail(r, "a flag must be a map");
        return;
    }
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const char *key;
        size_t n;

        if (!as_str(k, &key, &n)) {
            read_fail(r, "flag keys must be strings");
            continue;
        }
        if (key_is(key, n, "long")) {
            out->lng = read_string(r, val, "long");
            if (out->lng != NULL &&
                (out->lng[0] == '\0' || out->lng[0] == '-' ||
                 strpbrk(out->lng, "= \t") != NULL)) {
                size_t old = push_key(r, "long");

                read_fail(r, "'long' is the name without dashes, "
                             "e.g. \"release\"");
                path_pop(r, old);
            }
        } else if (key_is(key, n, "short")) {
            out->shrt = read_string(r, val, "short");
            if (out->shrt != NULL &&
                (strlen(out->shrt) != 1U || out->shrt[0] == '-' ||
                 out->shrt[0] == ' ')) {
                size_t old = push_key(r, "short");

                read_fail(r, "'short' is one character without the dash");
                path_pop(r, old);
            }
        } else if (key_is(key, n, "desc")) {
            out->desc = read_string(r, val, "desc");
        } else if (key_is(key, n, "arg")) {
            out->arg = read_arg_at(r, val, "arg");
        } else if (key_is(key, n, "arg_optional")) {
            out->arg_optional = read_bool(r, val, "arg_optional");
        } else if (key_is(key, n, "global")) {
            out->global = read_bool(r, val, "global");
        } else {
            unknown_key(r, key, n);
        }
    }
    if (out->lng == NULL && out->shrt == NULL)
        read_fail(r, "a flag needs 'long' or 'short'");
    if (out->arg_optional && out->arg == NULL)
        read_fail(r, "'arg_optional' needs 'arg'");
}

static bool node_names(const YewSpecNode *n, const char *name)
{
    u32 i;

    if (n->name != NULL && strcmp(n->name, name) == 0)
        return true;
    for (i = 0U; i < n->n_aliases; i++) {
        if (strcmp(n->aliases[i], name) == 0)
            return true;
    }
    return false;
}

static void read_node(SpecRead *r, FlValue v, bool root,
                      const YewSpecNode *parent, YewSpecNode *out);

static bool top_only_key(const char *k, size_t n)
{
    return key_is(k, n, "completion") || key_is(k, n, "command") ||
           key_is(k, n, "description") || key_is(k, n, "precommand") ||
           key_is(k, n, "generators");
}

static void read_node_key(SpecRead *r, const char *key, size_t n,
                          FlValue val, bool root, YewSpecNode *out)
{
    if (key_is(key, n, "name")) {
        if (root) {
            size_t old = push_key(r, "name");

            read_fail(r, "'name' is for subcommands, not the top level");
            path_pop(r, old);
            return;
        }
        out->name = read_string(r, val, "name");
    } else if (key_is(key, n, "aliases")) {
        out->aliases = read_strings(r, val, "aliases", &out->n_aliases,
                                    false);
    } else if (key_is(key, n, "desc")) {
        out->desc = read_string(r, val, "desc");
    } else if (key_is(key, n, "flags")) {
        const FlList *l = as_list(val);
        YewSpecFlag *flags;
        size_t old = push_key(r, "flags");
        u32 i;

        if (l == NULL) {
            read_fail(r, "'flags' must be a list of flag maps");
            path_pop(r, old);
            return;
        }
        flags = arena_alloc(r->a, ((size_t)l->n + 1U) * sizeof(*flags),
                            sizeof(void *));
        for (i = 0U; i < l->n; i++) {
            size_t at = path_push(r, "[%u]", (unsigned)i);

            read_flag(r, l->v[i], &flags[i]);
            path_pop(r, at);
        }
        out->flags = flags;
        out->n_flags = l->n;
        path_pop(r, old);
    } else if (key_is(key, n, "args")) {
        const FlList *l = as_list(val);
        YewSpecArg *args;
        size_t old = push_key(r, "args");
        u32 i;

        if (l == NULL) {
            read_fail(r, "'args' must be a list of argument maps");
            path_pop(r, old);
            return;
        }
        args = arena_alloc(r->a, ((size_t)l->n + 1U) * sizeof(*args),
                           sizeof(void *));
        for (i = 0U; i < l->n; i++) {
            size_t at = path_push(r, "[%u]", (unsigned)i);

            (void)read_arg(r, l->v[i], &args[i]);
            if (args[i].repeat && i + 1U < l->n)
                read_fail(r, "only the last argument may set 'repeat'");
            path_pop(r, at);
        }
        out->args = args;
        out->n_args = l->n;
        path_pop(r, old);
    } else if (key_is(key, n, "subcommands")) {
        const FlList *l = as_list(val);
        YewSpecNode *subs;
        size_t old = push_key(r, "subcommands");
        u32 i;
        u32 j;
        u32 a;

        if (l == NULL) {
            read_fail(r, "'subcommands' must be a list of maps");
            path_pop(r, old);
            return;
        }
        subs = arena_alloc(r->a, ((size_t)l->n + 1U) * sizeof(*subs),
                           sizeof(void *));
        (void)memset(subs, 0, ((size_t)l->n + 1U) * sizeof(*subs));
        for (i = 0U; i < l->n; i++) {
            size_t at = path_push(r, "[%u]", (unsigned)i);

            read_node(r, l->v[i], false, out, &subs[i]);
            path_pop(r, at);
        }
        out->subs = subs;
        out->n_subs = l->n;
        /* Duplicate names or aliases within one node: the second would
         * be unreachable, and which one wins would be an accident. */
        for (i = 0U; i < l->n && !r->failed; i++) {
            for (j = i + 1U; j < l->n && !r->failed; j++) {
                const char *dup = NULL;

                if (subs[j].name != NULL && node_names(&subs[i], subs[j].name))
                    dup = subs[j].name;
                for (a = 0U; dup == NULL && a < subs[j].n_aliases; a++) {
                    if (node_names(&subs[i], subs[j].aliases[a]))
                        dup = subs[j].aliases[a];
                }
                if (dup != NULL)
                    read_fail(r, "duplicate subcommand name or alias '%s'",
                              dup);
            }
            for (a = 0U; a < subs[i].n_aliases && !r->failed; a++) {
                if (subs[i].name != NULL &&
                    strcmp(subs[i].name, subs[i].aliases[a]) == 0)
                    read_fail(r, "duplicate subcommand name or alias '%s'",
                              subs[i].aliases[a]);
                for (j = a + 1U; j < subs[i].n_aliases && !r->failed; j++) {
                    if (strcmp(subs[i].aliases[a], subs[i].aliases[j]) == 0)
                        read_fail(r, "duplicate subcommand name or alias "
                                     "'%s'",
                                  subs[i].aliases[a]);
                }
            }
        }
        path_pop(r, old);
    } else if (key_is(key, n, "dash_values")) {
        out->dash_values = read_arg_at(r, val, "dash_values");
    } else {
        unknown_key(r, key, n);
    }
}

static void read_node(SpecRead *r, FlValue v, bool root,
                      const YewSpecNode *parent, YewSpecNode *out)
{
    const FlMap *m = as_map(v);
    u32 cursor = 0U;
    FlValue k;
    FlValue val;

    out->parent = parent;
    if (m == NULL) {
        read_fail(r, root ? "a spec must be one map"
                          : "a subcommand must be a map");
        return;
    }
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const char *key;
        size_t n;

        if (!as_str(k, &key, &n)) {
            read_fail(r, "keys must be strings");
            continue;
        }
        if (root && top_only_key(key, n))
            continue; /* read_top has them */
        read_node_key(r, key, n, val, root, out);
    }
    if (!root && out->name == NULL)
        read_fail(r, "a subcommand needs 'name'");
}

static void read_generators(SpecRead *r, FlValue v)
{
    const FlMap *m = as_map(v);
    YewSpecGen *gens;
    u32 cursor = 0U;
    FlValue k;
    FlValue val;
    u32 n = 0U;
    size_t old = push_key(r, "generators");

    if (m == NULL) {
        read_fail(r, "'generators' must be a map of name to generator");
        path_pop(r, old);
        return;
    }
    gens = arena_alloc(r->a, ((size_t)m->n + 1U) * sizeof(*gens),
                       sizeof(void *));
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const FlMap *g = as_map(val);
        YewSpecGen *gen = &gens[n];
        const char *name;
        size_t name_len;
        u32 gc = 0U;
        FlValue gk;
        FlValue gv;
        bool have_argv = false;
        size_t at;

        if (!as_str(k, &name, &name_len)) {
            read_fail(r, "generator names must be strings");
            continue;
        }
        (void)memset(gen, 0, sizeof(*gen));
        gen->name = arena_strndup(r->a, name, name_len);
        gen->cache_ms = 2000;
        at = push_key(r, gen->name);
        if (yew_compspec_builtin_generator(gen->name))
            read_fail(r, "generator '%s' shadows a built-in", gen->name);
        if (g == NULL) {
            read_fail(r, "a generator must be a map with 'argv'");
            path_pop(r, at);
            continue;
        }
        while (fl_map_iter(g, &gc, &gk, &gv)) {
            const char *key;
            size_t kn;

            if (!as_str(gk, &key, &kn)) {
                read_fail(r, "generator keys must be strings");
            } else if (key_is(key, kn, "argv")) {
                have_argv = true;
                gen->argv = read_strings(r, gv, "argv", &gen->argc, false);
            } else if (key_is(key, kn, "cache_ms")) {
                if (gv.t != (u8)FL_INT || gv.as.i < 0 ||
                    gv.as.i > 24 * 3600 * 1000) {
                    size_t ck = push_key(r, "cache_ms");

                    read_fail(r, "'cache_ms' must be an integer from 0 "
                                 "to 86400000");
                    path_pop(r, ck);
                } else {
                    gen->cache_ms = gv.as.i;
                }
            } else if (key_is(key, kn, "pass_flags")) {
                gen->pass_flags = read_strings(r, gv, "pass_flags",
                                               &gen->n_pass, false);
            } else {
                unknown_key(r, key, kn);
            }
        }
        if (!have_argv || gen->argc == 0U || gen->argv == NULL ||
            gen->argv[0][0] == '\0')
            read_fail(r, "a generator needs a non-empty 'argv'");
        path_pop(r, at);
        n++;
    }
    r->spec->gens = gens;
    r->spec->n_gens = n;
    path_pop(r, old);
}

static void read_precommand(SpecRead *r, FlValue v)
{
    const FlMap *m = as_map(v);
    u32 cursor = 0U;
    FlValue k;
    FlValue val;
    size_t old = push_key(r, "precommand");
    YewShWrapper *w = &r->spec->precommand;

    r->spec->has_precommand = true;
    (void)memset(w, 0, sizeof(*w));
    if (m == NULL) {
        read_fail(r, "'precommand' must be a map");
        path_pop(r, old);
        return;
    }
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const char *key;
        size_t n;

        if (!as_str(k, &key, &n)) {
            read_fail(r, "precommand keys must be strings");
        } else if (key_is(key, n, "flags_with_args")) {
            u32 count = 0U;

            w->consumes = read_strings(r, val, "flags_with_args", &count,
                                       false);
        } else if (key_is(key, n, "operands")) {
            if (val.t != (u8)FL_INT || val.as.i < 0 || val.as.i > 8) {
                size_t at = push_key(r, "operands");

                read_fail(r, "'operands' must be an integer from 0 to 8");
                path_pop(r, at);
            } else {
                w->operands = (u32)val.as.i;
            }
        } else if (key_is(key, n, "assignments")) {
            w->skips_assign = read_bool(r, val, "assignments");
        } else {
            unknown_key(r, key, n);
        }
    }
    path_pop(r, old);
}

static void read_top(SpecRead *r, FlValue root)
{
    const FlMap *m = as_map(root);
    u32 cursor = 0U;
    FlValue k;
    FlValue val;
    bool have_version = false;
    bool have_command = false;

    if (m == NULL) {
        read_fail(r, "a spec must be one map literal");
        return;
    }
    /* Generators first: an argument names one wherever it sits. */
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const char *key;
        size_t n;

        if (as_str(k, &key, &n) && key_is(key, n, "generators"))
            read_generators(r, val);
    }
    cursor = 0U;
    while (fl_map_iter(m, &cursor, &k, &val)) {
        const char *key;
        size_t n;

        if (!as_str(k, &key, &n))
            continue;
        if (key_is(key, n, "completion")) {
            have_version = true;
            if (val.t != (u8)FL_INT || val.as.i != 1) {
                size_t old = push_key(r, "completion");

                read_fail(r, "'completion' must be 1 (the schema version)");
                path_pop(r, old);
            }
        } else if (key_is(key, n, "command")) {
            have_command = true;
            r->spec->commands = read_strings(r, val, "command",
                                             &r->spec->n_commands, true);
            if (r->spec->commands != NULL && r->spec->n_commands == 0U) {
                size_t old = push_key(r, "command");

                read_fail(r, "'command' names no command");
                path_pop(r, old);
            }
        } else if (key_is(key, n, "description")) {
            r->spec->description = read_string(r, val, "description");
        } else if (key_is(key, n, "precommand")) {
            read_precommand(r, val);
        }
    }
    if (!have_version)
        read_fail(r, "missing required key 'completion'");
    else if (!have_command)
        read_fail(r, "missing required key 'command'");
    read_node(r, root, true, NULL, &r->spec->root);
}

typedef struct DiagFirst {
    char msg[SPEC_ERR_MAX];
    u32 line;
    bool seen;
} DiagFirst;

static void diag_first(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    DiagFirst *d = ctx;

    (void)rendered;
    if (level != FL_DIAG_ERROR || d->seen)
        return;
    d->seen = true;
    d->line = sp.line;
    (void)snprintf(d->msg, sizeof(d->msg), "%s", msg);
}

static u32 parse_count;

u32 yew_compspec_test_parse_count(void)
{
    return parse_count;
}

void yew_compspec_free(YewCompSpec *spec)
{
    Arena arena;

    if (spec == NULL)
        return;
    arena = spec->arena;
    arena_free_all(&arena);
}

YewCompSpec *yew_compspec_load_text(const char *origin, const char *src,
                                    size_t len, char *err, size_t errsz)
{
    Arena arena;
    Arena scratch;
    Interner in;
    FlVm vm;
    DiagCtx dc;
    DiagFirst first;
    FlValue root;
    SpecRead r;
    YewCompSpec *spec;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (origin == NULL || src == NULL)
        return NULL;
    parse_count++;
    arena_init(&arena);
    spec = arena_alloc(&arena, sizeof(*spec), sizeof(void *));
    (void)memset(spec, 0, sizeof(*spec));
    spec->origin = arena_strdup(&arena, origin);
    (void)memset(&r, 0, sizeof(r));
    r.spec = spec;
    r.a = &arena;
    r.origin = spec->origin;
    r.err = err;
    r.errsz = errsz;

    arena_init(&scratch);
    (void)memset(&first, 0, sizeof(first));
    fl_diag_init(&dc, &scratch);
    fl_diag_set_sink(&dc, diag_first, &first);
    (void)fl_diag_add_file(&dc, origin, src, len);
    interner_init(&in, &scratch);
    (void)fl_vm_init(&vm, &scratch, &in, &dc);
    root = fl_data_read(&vm, src, len, &dc);
    if (fl_diag_errors(&dc) != 0U) {
        read_fail(&r, "line %u: %s", (unsigned)first.line,
                  first.seen ? first.msg : "not a Fletch data document");
    } else {
        read_top(&r, root);
    }
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&scratch);
    if (r.failed) {
        arena_free_all(&arena);
        return NULL;
    }
    /* The arena was copied by value into its own first allocation; from
     * here on the spec owns it (yew_compspec_free takes it back out). */
    spec->arena = arena;
    return spec;
}

/* ---------------------------------------------------------------- */
/* Where the files are (§2)                                          */
/* ---------------------------------------------------------------- */

static const char *test_default_root;
static bool test_default_root_set;

void yew_compspec_test_set_default_root(const char *root)
{
    test_default_root = root;
    test_default_root_set = root != NULL;
    yew_compspec_invalidate_all();
}

static char *join3(const char *a, const char *b, const char *c)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    size_t nc = strlen(c);
    char *out = yew_xmalloc(na + nb + nc + 1U);

    (void)memcpy(out, a, na);
    (void)memcpy(out + na, b, nb);
    (void)memcpy(out + na + nb, c, nc + 1U);
    return out;
}

static bool is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * The shipped completions directory on disk, or NULL for the embedded
 * image.  The same precedence the runtime's other consumers use:
 * $YEW_RUNTIME_DIR alone when set; else the installed prefix -- but only
 * if IT has completions/, because an install from before this sprint
 * does not, and an evening has already been lost to exactly that; else
 * the source tree's runtime/ (a build run from the repository root);
 * else the embedded image.
 */
static char *shipped_dir(void)
{
    const char *env = getenv("YEW_RUNTIME_DIR");
    const char *root;
    char *dir;

    if (env != NULL && env[0] != '\0')
        return join3(env, "/", "completions");
    root = test_default_root_set ? test_default_root
                                 : YEW_RUNTIME_DIR_DEFAULT;
    if (root != NULL && root[0] != '\0') {
        dir = join3(root, "/", "completions");
        if (is_dir(dir))
            return dir;
        yew_xfree(dir);
    }
    if (is_dir("runtime/completions"))
        return join3("runtime/", "", "completions");
    return NULL;
}

static bool read_fd_all(int fd, Bytebuf *out)
{
    char chunk[4096];

    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return false;
        if (n == 0)
            return true;
        if (out->len + (size_t)n > SPEC_MAX_BYTES)
            return false;
        bytebuf_append(out, chunk, (size_t)n);
    }
}

static bool read_path(const char *path, Bytebuf *out)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    bool ok;

    if (fd < 0)
        return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return false;
    }
    ok = read_fd_all(fd, out);
    (void)close(fd);
    return ok;
}

/* ---------------------------------------------------------------- */
/* The shipped list, its files and the alias index                    */
/* ---------------------------------------------------------------- */

typedef struct Shipped {
    char *file; /* "git.fl" */
    YewCompSpec *spec;
    char *error;
    bool loaded;
    bool reported;
} Shipped;

typedef struct AliasEntry {
    char *name;
    u32 file;
} AliasEntry;

static struct {
    bool listed;
    bool indexed;
    char *dir; /* NULL: the embedded image */
    Shipped *files;
    u32 n_files;
    AliasEntry *alias;
    u32 n_alias;
} shipped;

static int str_ptr_cmp(const void *a, const void *b, void *ctx)
{
    const char *const *x = a;
    const char *const *y = b;

    (void)ctx;
    return strcmp(*x, *y);
}

static bool spec_file_name(const char *name)
{
    size_t n = strlen(name);

    return n > 3U && strcmp(name + n - 3U, ".fl") == 0 &&
           strchr(name, '/') == NULL && name[0] != '.';
}

static void shipped_list(void)
{
    char **names = NULL;
    size_t n = 0U;
    size_t cap = 0U;
    size_t i;

    if (shipped.listed)
        return;
    shipped.listed = true;
    shipped.dir = shipped_dir();
    if (shipped.dir != NULL) {
        DIR *d = opendir(shipped.dir);
        struct dirent *e;

        while (d != NULL && (e = readdir(d)) != NULL) {
            if (!spec_file_name(e->d_name))
                continue;
            if (n == cap) {
                cap = cap == 0U ? 32U : cap * 2U;
                names = yew_xrealloc(names, cap * sizeof(*names));
            }
            names[n++] = yew_xstrdup(e->d_name);
        }
        if (d != NULL)
            (void)closedir(d);
    } else {
        size_t count = yew_runtime_asset_count();

        for (i = 0U; i < count; i++) {
            const char *asset = yew_runtime_asset_name(i);

            if (asset == NULL || strncmp(asset, "completions/", 12U) != 0 ||
                !spec_file_name(asset + 12U))
                continue;
            if (n == cap) {
                cap = cap == 0U ? 32U : cap * 2U;
                names = yew_xrealloc(names, cap * sizeof(*names));
            }
            names[n++] = yew_xstrdup(asset + 12U);
        }
    }
    if (n > 1U)
        yew_sort_stable(names, n, sizeof(*names), str_ptr_cmp, NULL);
    shipped.files = n == 0U ? NULL : yew_xcalloc(n, sizeof(*shipped.files));
    for (i = 0U; i < n; i++)
        shipped.files[i].file = names[i];
    shipped.n_files = (u32)n;
    yew_xfree(names);
}

size_t yew_compspec_shipped_count(void)
{
    shipped_list();
    return shipped.n_files;
}

const char *yew_compspec_shipped_name(size_t i)
{
    shipped_list();
    return i < shipped.n_files ? shipped.files[i].file : NULL;
}

const char *yew_compspec_shipped_source(void)
{
    shipped_list();
    return shipped.dir != NULL ? "disk" : "embedded";
}

static bool shipped_read(const char *file, Bytebuf *out)
{
    bool ok;

    shipped_list();
    if (shipped.dir != NULL) {
        char *path = join3(shipped.dir, "/", file);

        ok = read_path(path, out);
        yew_xfree(path);
        return ok;
    }
    {
        char *key = join3("completions/", file, "");

        ok = yew_runtime_asset_read(key, out);
        yew_xfree(key);
    }
    return ok;
}

static YewCompSpec *load_bytes(const char *origin, Bytebuf *src,
                               char **error)
{
    char err[SPEC_ERR_MAX];
    YewCompSpec *spec;

    spec = yew_compspec_load_text(origin,
                                  src->data == NULL ? ""
                                                    : (const char *)src->data,
                                  src->len, err, sizeof(err));
    if (spec == NULL)
        *error = yew_xstrdup(err[0] != '\0' ? err : "rejected");
    return spec;
}

static Shipped *shipped_load(u32 i)
{
    Shipped *f = &shipped.files[i];
    Bytebuf src;
    char *origin;

    if (f->loaded)
        return f;
    f->loaded = true;
    origin = join3("completions/", f->file, "");
    bytebuf_init(&src);
    if (!shipped_read(f->file, &src)) {
        char msg[SPEC_ERR_MAX];

        (void)snprintf(msg, sizeof(msg), "%s: cannot read", origin);
        f->error = yew_xstrdup(msg);
    } else {
        f->spec = load_bytes(origin, &src, &f->error);
    }
    if (f->error != NULL)
        yew_log(YEW_LOG_WARN, "completion spec rejected: %s", f->error);
    bytebuf_free(&src);
    yew_xfree(origin);
    return f;
}

bool yew_compspec_check_shipped(const char *file, char *err, size_t errsz)
{
    u32 i;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    shipped_list();
    for (i = 0U; i < shipped.n_files; i++) {
        if (strcmp(shipped.files[i].file, file) == 0) {
            Shipped *f = shipped_load(i);

            if (f->spec == NULL && err != NULL && errsz != 0U)
                (void)snprintf(err, errsz, "%s",
                               f->error == NULL ? "rejected" : f->error);
            return f->spec != NULL;
        }
    }
    if (err != NULL && errsz != 0U)
        (void)snprintf(err, errsz, "%s: not shipped", file);
    return false;
}

static int shipped_find(const char *file)
{
    u32 lo = 0U;
    u32 hi;

    shipped_list();
    hi = shipped.n_files;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        int c = strcmp(file, shipped.files[mid].file);

        if (c == 0)
            return (int)mid;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1U;
    }
    return -1;
}

/* Every shipped file, parsed once; each `command` name maps to the first
 * file (in name order) that lists it. */
static void shipped_index(void)
{
    u32 i;
    u32 c;
    u32 cap = 0U;

    if (shipped.indexed)
        return;
    shipped.indexed = true;
    shipped_list();
    for (i = 0U; i < shipped.n_files; i++) {
        Shipped *f = shipped_load(i);

        if (f->spec == NULL)
            continue;
        for (c = 0U; c < f->spec->n_commands; c++) {
            u32 k;
            bool dup = false;

            for (k = 0U; k < shipped.n_alias; k++) {
                if (strcmp(shipped.alias[k].name, f->spec->commands[c]) == 0)
                    dup = true;
            }
            if (dup)
                continue;
            if (shipped.n_alias == cap) {
                cap = cap == 0U ? 64U : cap * 2U;
                shipped.alias = yew_xrealloc(shipped.alias,
                                             cap * sizeof(*shipped.alias));
            }
            shipped.alias[shipped.n_alias].name =
                yew_xstrdup(f->spec->commands[c]);
            shipped.alias[shipped.n_alias].file = i;
            shipped.n_alias++;
        }
    }
}

static const Shipped *shipped_for(const char *name)
{
    char *file = join3(name, ".fl", "");
    int direct = shipped_find(file);
    u32 k;

    yew_xfree(file);
    if (direct >= 0)
        return shipped_load((u32)direct);
    shipped_index();
    for (k = 0U; k < shipped.n_alias; k++) {
        if (strcmp(shipped.alias[k].name, name) == 0)
            return shipped_load(shipped.alias[k].file);
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Per-name lookup and the user override                              */
/* ---------------------------------------------------------------- */

typedef struct Slot {
    char *name;
    const YewCompSpec *spec; /* borrowed from `user` or a Shipped      */
    YewCompSpec *user;       /* owned when the user's file won         */
    char *user_error;
    bool user_reported;
    /* The user file as last seen: presence, mtime and size. */
    bool user_seen;
    i64 user_mtime_ns;
    i64 user_size;
    u32 epoch;
} Slot;

static struct {
    Slot *v;
    u32 n;
    u32 cap;
    u32 epoch;
} slots;

static void slot_drop_user(Slot *s)
{
    yew_compspec_free(s->user);
    s->user = NULL;
    yew_xfree(s->user_error);
    s->user_error = NULL;
    s->user_reported = false;
}

void yew_compspec_invalidate_all(void)
{
    u32 i;

    for (i = 0U; i < slots.n; i++) {
        slot_drop_user(&slots.v[i]);
        yew_xfree(slots.v[i].name);
    }
    yew_xfree(slots.v);
    (void)memset(&slots, 0, sizeof(slots));
    for (i = 0U; i < shipped.n_files; i++) {
        yew_compspec_free(shipped.files[i].spec);
        yew_xfree(shipped.files[i].error);
        yew_xfree(shipped.files[i].file);
    }
    yew_xfree(shipped.files);
    for (i = 0U; i < shipped.n_alias; i++)
        yew_xfree(shipped.alias[i].name);
    yew_xfree(shipped.alias);
    yew_xfree(shipped.dir);
    (void)memset(&shipped, 0, sizeof(shipped));
}

void yew_compspec_prompt_closed(void)
{
    slots.epoch++;
}

static char *user_path(const char *name)
{
    char *dir = yew_xdg_config_dir();
    char *path;

    if (dir == NULL)
        return NULL;
    path = join3(dir, "/completions/", name);
    yew_xfree(dir);
    {
        char *with = join3(path, ".fl", "");

        yew_xfree(path);
        path = with;
    }
    return path;
}

static i64 stat_mtime_ns(const struct stat *st)
{
#if defined(__APPLE__)
    return (i64)st->st_mtimespec.tv_sec * 1000000000LL +
           (i64)st->st_mtimespec.tv_nsec;
#else
    return (i64)st->st_mtim.tv_sec * 1000000000LL + (i64)st->st_mtim.tv_nsec;
#endif
}

/* (Re)decide a slot: the user's file if there is one, else shipped. */
static void slot_refresh(Slot *s)
{
    char *path = user_path(s->name);
    struct stat st;
    bool seen = path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);

    s->epoch = slots.epoch;
    if (seen && s->user_seen && stat_mtime_ns(&st) == s->user_mtime_ns &&
        (i64)st.st_size == s->user_size) {
        yew_xfree(path);
        return; /* unchanged since it was read */
    }
    slot_drop_user(s);
    s->user_seen = seen;
    s->spec = NULL;
    if (seen) {
        Bytebuf src;

        s->user_mtime_ns = stat_mtime_ns(&st);
        s->user_size = (i64)st.st_size;
        bytebuf_init(&src);
        if (!read_path(path, &src)) {
            char msg[SPEC_ERR_MAX];

            (void)snprintf(msg, sizeof(msg), "%s: cannot read", path);
            s->user_error = yew_xstrdup(msg);
        } else {
            s->user = load_bytes(path, &src, &s->user_error);
        }
        bytebuf_free(&src);
        if (s->user_error != NULL)
            yew_log(YEW_LOG_WARN, "completion spec rejected: %s",
                    s->user_error);
        /* Whole-file replace: a broken override leaves the command with
         * NO spec rather than quietly falling back to the shipped one --
         * the user asked for their file, and half-honouring it would
         * hide the mistake. */
        s->spec = s->user;
    } else {
        const Shipped *f = shipped_for(s->name);

        s->spec = f == NULL ? NULL : f->spec;
    }
    yew_xfree(path);
}

static bool valid_name(const char *name)
{
    size_t n = strlen(name);

    return n != 0U && n <= SPEC_NAME_MAX && strcmp(name, ".") != 0 &&
           strcmp(name, "..") != 0 && strchr(name, '/') == NULL;
}

static void report(Ed *ed, Slot *s)
{
    Shipped *f = NULL;
    u32 i;

    if (ed == NULL)
        return;
    if (s->user_error != NULL && !s->user_reported) {
        s->user_reported = true;
        yew_msg(ed, YEW_MSG_WARN, "completion spec rejected: %s",
                s->user_error);
        return;
    }
    if (s->user_seen)
        return;
    {
        char *file = join3(s->name, ".fl", "");
        int at = shipped_find(file);

        yew_xfree(file);
        if (at >= 0)
            f = &shipped.files[at];
    }
    for (i = 0U; f == NULL && shipped.indexed && i < shipped.n_alias; i++) {
        if (strcmp(shipped.alias[i].name, s->name) == 0)
            f = &shipped.files[shipped.alias[i].file];
    }
    if (f != NULL && f->error != NULL && !f->reported) {
        f->reported = true;
        yew_msg(ed, YEW_MSG_WARN, "completion spec rejected: %s", f->error);
    }
}

const YewCompSpec *yew_compspec_get(Ed *ed, const char *name)
{
    const char *base;
    Slot *s = NULL;
    u32 i;

    if (name == NULL)
        return NULL;
    base = strrchr(name, '/');
    base = base == NULL ? name : base + 1;
    if (!valid_name(base))
        return NULL;
    for (i = 0U; i < slots.n; i++) {
        if (strcmp(slots.v[i].name, base) == 0) {
            s = &slots.v[i];
            break;
        }
    }
    if (s == NULL) {
        if (slots.n == slots.cap) {
            slots.cap = slots.cap == 0U ? 32U : slots.cap * 2U;
            slots.v = yew_xrealloc(slots.v, slots.cap * sizeof(*slots.v));
        }
        s = &slots.v[slots.n++];
        (void)memset(s, 0, sizeof(*s));
        s->name = yew_xstrdup(base);
        slot_refresh(s);
    } else if (s->epoch != slots.epoch) {
        slot_refresh(s);
    }
    report(ed, s);
    return s->spec;
}

const YewSpecNode *yew_compspec_root(const YewCompSpec *spec)
{
    return spec == NULL ? NULL : &spec->root;
}

const char *yew_compspec_description(const YewCompSpec *spec)
{
    return spec == NULL ? NULL : spec->description;
}

const char *yew_compspec_origin(const YewCompSpec *spec)
{
    return spec == NULL ? NULL : spec->origin;
}

bool yew_compspec_precommand(const YewCompSpec *spec, YewShWrapper *out)
{
    if (spec == NULL || !spec->has_precommand)
        return false;
    if (out != NULL)
        *out = spec->precommand;
    return true;
}

const char *yew_compspec_describe(const char *name)
{
    u32 k;

    if (name == NULL)
        return NULL;
    shipped_index();
    for (k = 0U; k < shipped.n_alias; k++) {
        if (strcmp(shipped.alias[k].name, name) == 0) {
            const Shipped *f = &shipped.files[shipped.alias[k].file];

            return f->spec == NULL ? NULL : f->spec->description;
        }
    }
    return NULL;
}

int yew_compspec_wrapper(void *ud, const char *name, YewShWrapper *out)
{
    const YewCompSpec *spec = yew_compspec_get((Ed *)ud, name);

    if (spec == NULL)
        return -1;
    if (!spec->has_precommand)
        return 0;
    *out = spec->precommand;
    return 1;
}

/* ---------------------------------------------------------------- */
/* Resolution (§3)                                                    */
/* ---------------------------------------------------------------- */

const YewSpecArg *yew_compspec_slot(const YewSpecNode *node, u32 positional)
{
    if (node == NULL || node->n_args == 0U)
        return NULL;
    if (positional < node->n_args)
        return &node->args[positional];
    if (node->args[node->n_args - 1U].repeat)
        return &node->args[node->n_args - 1U];
    return NULL;
}

/* A flag of `node`, or a `global` one of an ancestor. */
static const YewSpecFlag *find_long(const YewSpecNode *node,
                                    const char *name, size_t len)
{
    const YewSpecNode *n;
    bool own = true;
    u32 i;

    for (n = node; n != NULL; n = n->parent, own = false) {
        for (i = 0U; i < n->n_flags; i++) {
            const YewSpecFlag *f = &n->flags[i];

            if ((own || f->global) && f->lng != NULL &&
                strlen(f->lng) == len && memcmp(f->lng, name, len) == 0)
                return f;
        }
    }
    return NULL;
}

static const YewSpecFlag *find_short(const YewSpecNode *node, char c)
{
    const YewSpecNode *n;
    bool own = true;
    u32 i;

    for (n = node; n != NULL; n = n->parent, own = false) {
        for (i = 0U; i < n->n_flags; i++) {
            const YewSpecFlag *f = &n->flags[i];

            if ((own || f->global) && f->shrt != NULL && f->shrt[0] == c)
                return f;
        }
    }
    return NULL;
}

static const YewSpecNode *find_child(const YewSpecNode *node,
                                     const char *word)
{
    u32 i;

    for (i = 0U; i < node->n_subs; i++) {
        if (node_names(&node->subs[i], word))
            return &node->subs[i];
    }
    return NULL;
}

/* A precommand's own flag that takes the next word, by exact spelling. */
static bool pre_consumes(const YewCompSpec *spec, const char *word)
{
    u32 i;

    if (!spec->has_precommand || spec->precommand.consumes == NULL)
        return false;
    for (i = 0U; spec->precommand.consumes[i] != NULL; i++) {
        if (strcmp(spec->precommand.consumes[i], word) == 0)
            return true;
    }
    return false;
}

static bool is_assignment(const char *w)
{
    size_t i;

    if (!((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z') ||
          w[0] == '_'))
        return false;
    for (i = 1U; w[i] != '\0' && w[i] != '='; i++) {
        char c = w[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return w[i] == '=';
}

bool yew_compspec_resolve(const YewCompSpec *spec, const YewShCtx *ctx,
                          YewSpecPoint *out)
{
    const YewSpecNode *node;
    const YewSpecFlag *pending = NULL;
    bool pre_pending = false;
    bool ended;
    u32 positional = 0U;
    u32 i;
    const char *stem;

    if (out == NULL)
        return false;
    (void)memset(out, 0, sizeof(*out));
    if (spec == NULL || ctx == NULL || ctx->argv == NULL ||
        ctx->arg_index == 0U || ctx->arg_index > ctx->argc)
        return false;
    node = &spec->root;
    ended = false;
    for (i = 1U; i < ctx->arg_index; i++) {
        const char *w = ctx->argv[i];

        if (w == NULL)
            break;
        /*
         * §3 pitfall: awaiting a value is checked BEFORE "starts with
         * -".  `git log --since -2weeks` gives -2weeks to --since, and
         * reading it as a flag instead would shift every word after it.
         */
        if (pending != NULL || pre_pending) {
            pending = NULL;
            pre_pending = false;
            continue;
        }
        if (!ended && strcmp(w, "--") == 0) {
            ended = true;
            continue;
        }
        if (!ended && w[0] == '-' && w[1] == '-') {
            const char *name = w + 2;
            const char *eq = strchr(name, '=');
            const YewSpecFlag *f;

            if (eq != NULL)
                continue; /* --name=value: consumed whole, known or not */
            f = find_long(node, name, strlen(name));
            if (f != NULL && f->arg != NULL && !f->arg_optional)
                pending = f;
            else if (f == NULL && pre_consumes(spec, w))
                pre_pending = true;
            continue;
        }
        if (!ended && w[0] == '-' && w[1] != '\0') {
            size_t j;
            bool described = false;

            /*
             * A short flag or a bundle: each letter in turn.  The first
             * that takes an argument swallows the rest of the word, or,
             * at the word's end, the NEXT word.  An unknown letter takes
             * nothing -- guessing that it does would eat the subcommand
             * that follows.
             */
            for (j = 1U; w[j] != '\0'; j++) {
                const YewSpecFlag *f = find_short(node, w[j]);

                if (f != NULL)
                    described = true;
                if (f == NULL || f->arg == NULL)
                    continue;
                if (w[j + 1U] == '\0' && !f->arg_optional)
                    pending = f;
                break;
            }
            /* A precommand flag the spec names only in flags_with_args
             * still takes its word. */
            if (!described && pre_consumes(spec, w))
                pre_pending = true;
            continue;
        }
        if (spec->has_precommand) {
            if (spec->precommand.skips_assign && is_assignment(w))
                continue;
            if (positional < spec->precommand.operands) {
                positional++;
                continue;
            }
            out->command_at = i;
            out->node = node;
            return true;
        }
        if (!ended && positional == 0U) {
            const YewSpecNode *child = find_child(node, w);

            if (child != NULL) {
                node = child;
                continue;
            }
        }
        {
            const YewSpecArg *slot = yew_compspec_slot(node, positional);

            if (slot != NULL && slot->kind == YEW_SPEC_ARG_COMMAND) {
                out->command_at = i;
                out->node = node;
                return true;
            }
        }
        if (positional < UINT32_MAX)
            positional++;
    }
    ended = ended || ctx->dashdash;
    out->node = node;
    out->positional = positional;
    out->flags_ended = ended;
    stem = ctx->stem == NULL ? "" : ctx->stem;
    if (pending != NULL) {
        out->pending_flag = pending;
        return true;
    }
    if (pre_pending) {
        /* A precommand flag the spec lists only by spelling: its value
         * is free text.  pending_flag stays NULL; nothing is offered. */
        out->pending_flag = NULL;
        out->positional = UINT32_MAX;
        return true;
    }
    if (!ended && stem[0] == '-' && stem[1] == '-') {
        const char *eq = strchr(stem + 2, '=');

        if (eq != NULL) {
            const YewSpecFlag *f =
                find_long(node, stem + 2, (size_t)(eq - (stem + 2)));

            if (f != NULL && f->arg != NULL) {
                out->pending_flag = f;
                out->after_equals = true;
                out->value_at = (u32)(eq + 1 - stem);
            } else {
                out->positional = UINT32_MAX; /* an unknown --x=: nothing */
            }
            return true;
        }
    }
    if (spec->has_precommand) {
        if ((ended || stem[0] != '-') &&
            positional >= spec->precommand.operands &&
            !(spec->precommand.skips_assign && is_assignment(stem)))
            out->command_at = ctx->arg_index;
        return true;
    }
    out->subcommands_allowed = !ended && positional == 0U &&
                               node->n_subs != 0U;
    {
        const YewSpecArg *slot = yew_compspec_slot(node, positional);

        if (slot != NULL && slot->kind == YEW_SPEC_ARG_COMMAND &&
            !(stem[0] == '-' && !ended))
            out->command_at = ctx->arg_index;
    }
    return true;
}
