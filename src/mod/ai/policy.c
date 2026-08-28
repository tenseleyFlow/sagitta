#define _POSIX_C_SOURCE 200809L

#include "mod/ai/policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/data.h"
#include "fl/gc.h"
#include "fl/vm.h"
#include "mod/ai/ai_int.h"
#include "search/regex.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/runtime_asset.h"
#include "util/xdg.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

typedef struct PolicyDoc {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    Bytebuf bytes;
    const char *path;
} PolicyDoc;

typedef struct PolicyRows {
    AiRedactSpec *v;
    size_t n;
    size_t cap;
} PolicyRows;

static char *policy_copy(const char *s)
{
    size_t n = strlen(s);
    char *copy = yew_xmalloc(n + 1U);

    (void)memcpy(copy, s, n + 1U);
    return copy;
}

static char *policy_join(const char *dir, const char *leaf)
{
    size_t dn = strlen(dir);
    size_t ln = strlen(leaf);
    bool slash = dn != 0U && dir[dn - 1U] != '/';
    char *path = yew_xmalloc(dn + ln + (slash ? 2U : 1U));

    (void)memcpy(path, dir, dn);
    if (slash)
        path[dn++] = '/';
    (void)memcpy(path + dn, leaf, ln + 1U);
    return path;
}

static char *shipped_path(void)
{
    const char *runtime = getenv("YEW_RUNTIME_DIR");
    char *path;

    if (runtime != NULL && runtime[0] != '\0')
        return policy_join(runtime, "ai-deny.fl");
    path = policy_join(YEW_RUNTIME_DIR_DEFAULT, "ai-deny.fl");
    if (access(path, R_OK) == 0)
        return path;
    yew_xfree(path);
    if (access("runtime/ai-deny.fl", R_OK) == 0)
        return policy_copy("runtime/ai-deny.fl");
    path = yew_runtime_asset_resolve("ai-deny.fl");
    if (path != NULL)
        return path;
    return policy_join(YEW_RUNTIME_DIR_DEFAULT, "ai-deny.fl");
}

static char *user_path(void)
{
    char *dir = yew_xdg_config_dir();
    char *path;

    if (dir == NULL)
        return NULL;
    path = policy_join(dir, "ai-deny.fl");
    yew_xfree(dir);
    return path;
}

static bool read_file(const char *path, Bytebuf *out, bool *missing)
{
    u8 chunk[8192];
    int fd;

    *missing = false;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        const char *runtime = getenv("YEW_RUNTIME_DIR");
        int saved = errno;

        if (saved == ENOENT &&
            (runtime == NULL || runtime[0] == '\0') &&
            yew_runtime_asset_read(path, out)) {
            errno = saved;
            return true;
        }
        errno = saved;
        *missing = saved == ENOENT;
        return false;
    }
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof(chunk));

        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return false;
        }
        if ((size_t)got > FL_DATA_MAX_BYTES - out->len) {
            errno = EFBIG;
            (void)close(fd);
            return false;
        }
        bytebuf_append(out, chunk, (size_t)got);
    }
    return close(fd) == 0;
}

static void doc_init(PolicyDoc *doc, const char *path)
{
    (void)memset(doc, 0, sizeof(*doc));
    arena_init(&doc->arena);
    interner_init(&doc->in, &doc->arena);
    fl_diag_init(&doc->dc, &doc->arena);
    (void)fl_vm_init(&doc->vm, &doc->arena, &doc->in, &doc->dc);
    bytebuf_init(&doc->bytes);
    doc->path = path;
}

static void doc_drop(PolicyDoc *doc)
{
    bytebuf_free(&doc->bytes);
    fl_vm_free(&doc->vm);
    interner_free(&doc->in);
    arena_free_all(&doc->arena);
}

static void diag_log(void *ctx, FlDiagLevel level, FlSpan sp,
                     const char *msg, const char *rendered)
{
    const PolicyDoc *doc = ctx;

    (void)rendered;
    yew_log(level == FL_DIAG_WARNING ? YEW_LOG_WARN : YEW_LOG_ERROR,
            "%s:%u:%u: %s", doc->path, sp.line, sp.col, msg);
}

static bool key_eq(FlValue key, const char *name)
{
    const FlStr *s;
    size_t n = strlen(name);

    if (key.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)key.as.o;
    return s->len == n && memcmp(s->b, name, n) == 0;
}

static bool map_get(const FlMap *map, const char *name, FlValue *out)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    while (fl_map_iter(map, &cursor, &key, &value)) {
        if (!key_eq(key, name))
            continue;
        *out = value;
        return true;
    }
    return false;
}

static const char *string_value(FlValue value)
{
    return value.t == (u8)FL_STR ? ((const FlStr *)value.as.o)->b : NULL;
}

static u32 row_line(const u8 *src, size_t len, u32 want)
{
    size_t i;
    u32 line = 1U;
    u32 depth = 0U;
    u32 row = 0U;
    bool string = false;
    bool escape = false;
    bool comment = false;

    for (i = 0U; i < len; i++) {
        u8 c = src[i];

        if (comment) {
            if (c == (u8)'\n')
                comment = false;
        } else if (string) {
            if (escape)
                escape = false;
            else if (c == (u8)'\\')
                escape = true;
            else if (c == (u8)'"')
                string = false;
        } else if (c == (u8)'#') {
            comment = true;
        } else if (c == (u8)'"') {
            string = true;
        } else if (c == (u8)'[') {
            depth++;
        } else if (c == (u8)'{') {
            if (depth == 1U && row++ == want)
                return line;
            depth++;
        } else if ((c == (u8)'}' || c == (u8)']') && depth != 0U) {
            depth--;
        }
        if (c == (u8)'\n')
            line++;
    }
    return line;
}

static bool flags_value(const char *text, u32 *out)
{
    u32 flags = 0U;

    while (*text != '\0') {
        if (*text == 'i')
            flags |= YEW_RE_ICASE;
        else if (*text == 's')
            flags |= YEW_RE_DOTALL;
        else if (*text == 'l')
            flags |= YEW_RE_LITERAL;
        else
            return false;
        text++;
    }
    *out = flags;
    return true;
}

static void rows_push(PolicyRows *rows, AiRedactSpec spec)
{
    if (rows->n == rows->cap) {
        size_t cap = rows->cap == 0U ? 16U : rows->cap * 2U;

        rows->v = yew_xrealloc(rows->v, cap * sizeof(*rows->v));
        rows->cap = cap;
    }
    rows->v[rows->n++] = spec;
}

static void warn_deny_replaced(const PolicyRows *rows)
{
    Bytebuf names;
    size_t i;

    bytebuf_init(&names);
    for (i = 0U; i < rows->n; i++) {
        if (i != 0U)
            bytebuf_append(&names, ", ", 2U);
        bytebuf_append(&names, rows->v[i].name, strlen(rows->v[i].name));
    }
    bytebuf_push_u8(&names, 0U);
    yew_log(YEW_LOG_WARN,
            "ai.deny_replace dropped shipped secret protections: %s",
            names.len == 1U ? "(none loaded)" : (const char *)names.data);
    bytebuf_free(&names);
}

static bool parse_doc(PolicyDoc *doc, bool required, PolicyRows *rows)
{
    bool missing;
    bool clean = true;
    FlValue root;
    const FlList *list;
    u32 i;

    if (!read_file(doc->path, &doc->bytes, &missing)) {
        if (!missing || required)
            yew_log(YEW_LOG_ERROR, "cannot read AI deny policy %s: %s",
                    doc->path, strerror(errno));
        return !required && missing;
    }
    fl_diag_set_sink(&doc->dc, diag_log, doc);
    root = fl_data_read(&doc->vm, (const char *)doc->bytes.data,
                        doc->bytes.len, &doc->dc);
    if (fl_diag_errors(&doc->dc) != 0U || root.t != (u8)FL_LIST) {
        if (fl_diag_errors(&doc->dc) == 0U)
            yew_log(YEW_LOG_ERROR, "%s:1: AI deny policy must be a list",
                    doc->path);
        return !required;
    }
    list = (const FlList *)root.as.o;
    for (i = 0U; i < list->n; i++) {
        FlValue name;
        FlValue re;
        FlValue flags;
        FlValue note;
        AiRedactSpec spec;
        u32 line = row_line(doc->bytes.data, doc->bytes.len, i);

        if (list->v[i].t != (u8)FL_MAP ||
            !map_get((const FlMap *)list->v[i].as.o, "name", &name) ||
            !map_get((const FlMap *)list->v[i].as.o, "re", &re) ||
            !map_get((const FlMap *)list->v[i].as.o, "flags", &flags) ||
            !map_get((const FlMap *)list->v[i].as.o, "note", &note) ||
            string_value(name) == NULL || string_value(re) == NULL ||
            string_value(flags) == NULL || string_value(note) == NULL) {
            yew_log(YEW_LOG_ERROR,
                    "%s:%u: invalid AI deny row; require string name, re, flags, note",
                    doc->path, line);
            clean = false;
            continue;
        }
        (void)memset(&spec, 0, sizeof(spec));
        spec.name = string_value(name);
        spec.re = string_value(re);
        spec.note = string_value(note);
        spec.source = doc->path;
        spec.line_1based = line;
        if (!flags_value(string_value(flags), &spec.flags)) {
            yew_log(YEW_LOG_ERROR, "%s:%u: invalid AI deny flags for '%s'",
                    doc->path, line, spec.name);
            clean = false;
            continue;
        }
        {
            AiRedactError error;
            AiRedactPolicy *probe = yew_ai_redact_policy_new(&spec, 1U,
                                                              true, &error);

            if (error.message != NULL) {
                yew_log(YEW_LOG_ERROR, "%s:%u: invalid AI deny rule '%s': %s",
                        doc->path, line, spec.name, error.message);
                yew_ai_redact_policy_free(probe);
                clean = false;
                continue;
            }
            yew_ai_redact_policy_free(probe);
        }
        rows_push(rows, spec);
    }
    return !required || clean;
}

void yew_ai_policy_bundle_drop(AiPolicyBundle *bundle)
{
    if (bundle == NULL)
        return;
    yew_ai_redact_policy_free(bundle->redact);
    yew_ai_path_policy_free(bundle->paths);
    (void)memset(bundle, 0, sizeof(*bundle));
}

bool yew_ai_policy_load_paths(const char *shipped, const char *user,
                              bool deny_replace, bool exclude_replace,
                              AiPolicyBundle *out)
{
    PolicyDoc shipped_doc;
    PolicyDoc user_doc;
    PolicyRows shipped_rows = {0};
    PolicyRows user_rows = {0};
    PolicyRows *selected;
    bool ok;

    if (shipped == NULL || out == NULL)
        return false;
    (void)memset(out, 0, sizeof(*out));
    doc_init(&shipped_doc, shipped);
    ok = parse_doc(&shipped_doc, true, &shipped_rows);
    if (user != NULL) {
        doc_init(&user_doc, user);
        (void)parse_doc(&user_doc, false, &user_rows);
    } else {
        (void)memset(&user_doc, 0, sizeof(user_doc));
    }
    if (!ok || shipped_rows.n == 0U) {
        yew_log(YEW_LOG_WARN,
                "AI deny policy: shipped file unusable; retaining compiled protection");
        out->redact = yew_ai_redact_policy_new(NULL, 0U, false, NULL);
    } else {
        size_t i;

        if (!deny_replace) {
            for (i = 0U; i < user_rows.n; i++)
                rows_push(&shipped_rows, user_rows.v[i]);
            selected = &shipped_rows;
        } else {
            selected = &user_rows;
            warn_deny_replaced(&shipped_rows);
        }
        out->redact = yew_ai_redact_policy_new(selected->v, selected->n,
                                                true, NULL);
    }
    out->paths = yew_ai_path_policy_new(NULL, 0U, exclude_replace, NULL);
    if (exclude_replace)
        yew_log(YEW_LOG_WARN,
                "ai.exclude_replace dropped shipped path exclusions: "
                ".env*, *secret*, *credential*, .ssh/, *.pem, *.key, "
                "*.p12, *.pfx, *.jks, *.keystore, id_rsa*, id_ecdsa*, "
                "id_ed25519*, .netrc, _netrc, .npmrc, .pypirc, .aws/, "
                ".gnupg/, .docker/config.json, *.kdbx, *.gpg, *.asc");
    yew_xfree(shipped_rows.v);
    yew_xfree(user_rows.v);
    if (user != NULL)
        doc_drop(&user_doc);
    doc_drop(&shipped_doc);
    if (out->redact == NULL || out->paths == NULL) {
        yew_ai_policy_bundle_drop(out);
        return false;
    }
    return true;
}

static bool bool_option(Ed *ed, const char *name)
{
    OptVal value;

    return ed != NULL && ed->opt_globals != NULL &&
           yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name), &value) &&
           value.type == (u8)YEW_OPT_BOOL && value.as.b;
}

static AiPathPolicy *path_policy_from_options(Ed *ed, bool replace)
{
    OptVal value;
    const char **globs = NULL;
    AiPathPolicy *policy;
    u32 i;

    if (ed == NULL || ed->opt_globals == NULL ||
        !yew_opt_get(ed, NULL, NULL, "ai.exclude_paths", 16U, &value) ||
        value.type != (u8)YEW_OPT_STRLIST)
        return yew_ai_path_policy_new(NULL, 0U, replace, NULL);
    if (value.as.list.len != 0U) {
        globs = yew_xcalloc(value.as.list.len, sizeof(*globs));
        for (i = 0U; i < value.as.list.len; i++)
            globs[i] = value.as.list.v[i].s;
    }
    policy = yew_ai_path_policy_new(globs, value.as.list.len, replace, NULL);
    yew_xfree(globs);
    return policy;
}

bool yew_ai_policy_reload(Ed *ed)
{
    char *shipped;
    char *user;
    AiPolicyBundle bundle;

    if (ed == NULL || ed->ai == NULL)
        return false;
    shipped = shipped_path();
    user = user_path();
    if (!yew_ai_policy_load_paths(shipped, user,
                                  bool_option(ed, "ai.deny_replace"),
                                  bool_option(ed, "ai.exclude_replace"),
                                  &bundle)) {
        yew_xfree(user);
        yew_xfree(shipped);
        return false;
    }
    {
        AiPathPolicy *paths = path_policy_from_options(
            ed, bool_option(ed, "ai.exclude_replace"));

        if (paths == NULL) {
            yew_ai_policy_bundle_drop(&bundle);
            yew_xfree(user);
            yew_xfree(shipped);
            return false;
        }
        yew_ai_path_policy_free(bundle.paths);
        bundle.paths = paths;
    }
    yew_ai_redact_policy_free(ed->ai->redact);
    yew_ai_path_policy_free(ed->ai->paths);
    ed->ai->redact = bundle.redact;
    ed->ai->paths = bundle.paths;
    ed->ai->policy_config = ed->config;
    ed->ai->policy_options_loaded = ed->opt_globals != NULL;
    yew_xfree(user);
    yew_xfree(shipped);
    return true;
}

void yew_ai_policy_ensure(Ed *ed)
{
    if (ed != NULL && ed->ai != NULL && ed->opt_globals != NULL &&
        (!ed->ai->policy_options_loaded ||
         ed->ai->policy_config != ed->config))
        (void)yew_ai_policy_reload(ed);
}
