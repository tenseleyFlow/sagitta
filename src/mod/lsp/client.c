#define _XOPEN_SOURCE 700

#include "mod/lsp/client.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "fl/flconf.h"
#include "fl/value.h"
#include "mod/lsp/diag.h"
#include "ui/message.h"
#include "util/arena.h"
#include "util/log.h"

enum {
    YEW_LSP_MAX_SERVERS = 16,
    YEW_LSP_MAX_ARGS = 14,
    YEW_LSP_MAX_ROOTS = 32,
    YEW_LSP_MAX_INIT_TIMEOUT_MS = 600000,
    YEW_LSP_RESTART_WINDOW_MS = 300000
};
enum { YEW_LSP_STDERR_TAIL = 8U * 1024U };

typedef struct LspOwnedCfg {
    LspServerCfg pub;
    char **args;
    char **roots;
} LspOwnedCfg;

struct LspClient {
    LspServer *server[YEW_LSP_MAX_SERVERS];
    u32 len;
    u32 next_id;
    LspOwnedCfg cfg[YEW_LSP_MAX_SERVERS];
    u32 cfg_len;
    const void *config_token;
    bool config_loaded;
    bool replaces_defaults;
};

static const char *const clangd_args[] = {
    "--background-index", "--clang-tidy", "--offset-encoding=utf-8", NULL
};
static const char *const clangd_roots[] = {
    "compile_commands.json", ".clangd", "compile_flags.txt", ".git", NULL
};
static const char *const rust_roots[] = {"Cargo.toml", ".git", NULL};
static const char *const py_args[] = {"--stdio", NULL};
static const char *const py_roots[] = {
    "pyproject.toml", "setup.py", "setup.cfg", ".git", NULL
};
static const char *const go_roots[] = {"go.mod", ".git", NULL};
static const char *const ts_args[] = {"--stdio", NULL};
static const char *const ts_roots[] = {
    "tsconfig.json", "package.json", ".git", NULL
};
static const char *const fort_roots[] = {".fortls", ".git", NULL};
static const char *const sh_args[] = {"start", NULL};
static const char *const git_roots[] = {".git", NULL};

#define CFG(lang_, id_, cmd_, args_, roots_)                                  \
    {id_, lang_, cmd_, args_, roots_, NULL, YEW_RPC_INIT_TIMEOUT_MS}

static const LspServerCfg default_cfgs[] = {
    CFG("c", "clangd", "clangd", clangd_args, clangd_roots),
    CFG("cpp", "clangd", "clangd", clangd_args, clangd_roots),
    CFG("objc", "clangd", "clangd", clangd_args, clangd_roots),
    CFG("rust", "rust-analyzer", "rust-analyzer", NULL, rust_roots),
    CFG("python", "pyright", "pyright-langserver", py_args, py_roots),
    CFG("go", "gopls", "gopls", NULL, go_roots),
    CFG("js", "tsserver", "typescript-language-server", ts_args, ts_roots),
    CFG("ts", "tsserver", "typescript-language-server", ts_args, ts_roots),
    CFG("jsx", "tsserver", "typescript-language-server", ts_args, ts_roots),
    CFG("tsx", "tsserver", "typescript-language-server", ts_args, ts_roots),
    CFG("fortran", "fortls", "fortls", NULL, fort_roots),
    CFG("sh", "bashls", "bash-language-server", sh_args, git_roots)
};

const LspServerCfg *yew_lsp_default_cfg(const char *lang)
{
    size_t i;

    if (lang == NULL)
        return NULL;
    if (strncmp(lang, "fortran", 7U) == 0)
        lang = "fortran";
    for (i = 0U; i < YEW_ARRAY_LEN(default_cfgs); i++)
        if (strcmp(default_cfgs[i].lang, lang) == 0)
            return &default_cfgs[i];
    return NULL;
}

static char *copy_string(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *copy = yew_xmalloc(n);

    (void)memcpy(copy, s, n);
    return copy;
}

static char *copy_fl_string(const FlStr *s)
{
    char *copy;

    if (s == NULL || memchr(s->b, '\0', s->len) != NULL)
        return NULL;
    copy = yew_xmalloc((size_t)s->len + 1U);
    (void)memcpy(copy, s->b, s->len);
    copy[s->len] = '\0';
    return copy;
}

static bool fl_map_name(const FlMap *map, const char *name, FlValue *out)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;
    size_t len = strlen(name);

    while (fl_map_iter(map, &cursor, &key, &value)) {
        const FlStr *s;

        if (key.t != (u8)FL_STR)
            continue;
        s = (const FlStr *)key.as.o;
        if ((size_t)s->len == len && memcmp(s->b, name, len) == 0) {
            if (out != NULL)
                *out = value;
            return true;
        }
    }
    return false;
}

static void free_string_list(char **list)
{
    size_t i;

    if (list == NULL)
        return;
    for (i = 0U; list[i] != NULL; i++)
        free(list[i]);
    free(list);
}

static void owned_cfg_free(LspOwnedCfg *cfg)
{
    free((char *)cfg->pub.id);
    free((char *)cfg->pub.lang);
    free((char *)cfg->pub.cmd);
    free((char *)cfg->pub.init_options);
    free_string_list(cfg->args);
    free_string_list(cfg->roots);
    (void)memset(cfg, 0, sizeof(*cfg));
}

static void client_cfg_free(LspClient *client)
{
    u32 i;

    for (i = 0U; i < client->cfg_len; i++)
        owned_cfg_free(&client->cfg[i]);
    client->cfg_len = 0U;
}

static bool cfg_string(const FlMap *map, const char *name, bool required,
                       char **out)
{
    FlValue value;

    *out = NULL;
    if (!fl_map_name(map, name, &value))
        return !required;
    if (value.t != (u8)FL_STR)
        return false;
    *out = copy_fl_string((const FlStr *)value.as.o);
    return *out != NULL;
}

static bool cfg_string_list(const FlMap *map, const char *name, u32 max,
                            char ***out)
{
    FlValue value;
    const FlList *list;
    char **copy;
    u32 i;

    *out = NULL;
    if (!fl_map_name(map, name, &value) || value.t == (u8)FL_NIL)
        return true;
    if (value.t != (u8)FL_LIST)
        return false;
    list = (const FlList *)value.as.o;
    if (list->n > max)
        return false;
    copy = yew_xcalloc((size_t)list->n + 1U, sizeof(*copy));
    for (i = 0U; i < list->n; i++) {
        if (list->v[i].t != (u8)FL_STR ||
            (copy[i] = copy_fl_string((const FlStr *)list->v[i].as.o)) == NULL) {
            free_string_list(copy);
            return false;
        }
    }
    *out = copy;
    return true;
}

static bool cfg_init_options(const FlMap *map, char **out)
{
    FlValue value;
    const FlStr *text;
    Arena arena;
    JsonErr err;
    JsonValue *json;

    *out = NULL;
    if (!fl_map_name(map, "init_options", &value) || value.t == (u8)FL_NIL)
        return true;
    if (value.t != (u8)FL_STR)
        return false;
    text = (const FlStr *)value.as.o;
    if (memchr(text->b, '\0', text->len) != NULL)
        return false;
    arena_init(&arena);
    json = yew_json_parse(&arena, (const u8 *)text->b, text->len, &err);
    if (json == NULL || json->kind != YEW_JS_OBJ) {
        arena_free_all(&arena);
        return false;
    }
    arena_free_all(&arena);
    *out = copy_fl_string(text);
    return *out != NULL;
}

static bool cfg_timeout(const FlMap *map, i32 *out)
{
    FlValue value;

    *out = YEW_RPC_INIT_TIMEOUT_MS;
    if (!fl_map_name(map, "init_timeout_ms", &value))
        return true;
    if (value.t != (u8)FL_INT || value.as.i <= 0 ||
        value.as.i > YEW_LSP_MAX_INIT_TIMEOUT_MS)
        return false;
    *out = (i32)value.as.i;
    return true;
}

static bool owned_cfg_parse(LspOwnedCfg *out, const FlStr *lang,
                            const FlMap *row)
{
    char *id = NULL;
    char *cmd = NULL;
    char *init_options = NULL;

    (void)memset(out, 0, sizeof(*out));
    out->pub.lang = copy_fl_string(lang);
    if (out->pub.lang == NULL ||
        !cfg_string(row, "id", true, &id) ||
        !cfg_string(row, "cmd", true, &cmd) ||
        !cfg_string_list(row, "args", YEW_LSP_MAX_ARGS, &out->args) ||
        !cfg_string_list(row, "roots", YEW_LSP_MAX_ROOTS, &out->roots) ||
        !cfg_init_options(row, &init_options) ||
        !cfg_timeout(row, &out->pub.init_timeout_ms)) {
        free(id);
        free(cmd);
        free(init_options);
        owned_cfg_free(out);
        return false;
    }
    out->pub.id = id;
    out->pub.cmd = cmd;
    out->pub.init_options = init_options;
    if (out->pub.id[0] == '\0' || out->pub.cmd[0] == '\0' ||
        out->pub.lang[0] == '\0') {
        owned_cfg_free(out);
        return false;
    }
    out->pub.args = (const char *const *)out->args;
    out->pub.roots = (const char *const *)out->roots;
    return true;
}

static void cfg_warn(Ed *ed, const FlStr *lang, const char *why)
{
    yew_log(YEW_LOG_WARN, "LSP config: ignoring server %.*s: %s",
            (int)lang->len, lang->b, why);
    yew_msg(ed, YEW_MSG_WARN, "LSP config: ignoring server %.*s: %s",
            (int)lang->len, lang->b, why);
}

static void client_cfg_load(Ed *ed, LspClient *client)
{
    FlValue lsp;
    FlValue servers;
    const FlMap *table;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    client->config_token = ed->config;
    client->config_loaded = true;
    if (!yew_config_get_global(ed, "lsp", 3U, &lsp) ||
        lsp.t != (u8)FL_MAP ||
        !fl_map_name((const FlMap *)lsp.as.o, "servers", &servers))
        return;
    client->replaces_defaults = true;
    if (servers.t != (u8)FL_MAP) {
        yew_log(YEW_LOG_WARN, "LSP config: lsp.servers must be a map");
        yew_msg(ed, YEW_MSG_WARN, "LSP config: lsp.servers must be a map");
        return;
    }
    table = (const FlMap *)servers.as.o;
    while (fl_map_iter(table, &cursor, &key, &value)) {
        const FlStr *lang;

        if (key.t != (u8)FL_STR)
            continue;
        lang = (const FlStr *)key.as.o;
        if (client->cfg_len == YEW_LSP_MAX_SERVERS) {
            cfg_warn(ed, lang, "server table exceeds 16 entries");
            continue;
        }
        if (value.t != (u8)FL_MAP ||
            !owned_cfg_parse(&client->cfg[client->cfg_len], lang,
                             (const FlMap *)value.as.o)) {
            cfg_warn(ed, lang, "invalid row or field type");
            continue;
        }
        client->cfg_len++;
    }
}

static char *canonical_dir(const char *path)
{
    char *real = realpath(path, NULL);

    return real != NULL ? real : NULL;
}

static char *path_dir(const char *path)
{
    char *copy;
    char *slash;

    if (path == NULL || path[0] == '\0')
        return NULL;
    copy = copy_string(path);
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        return copy_string(".");
    }
    if (slash == copy)
        slash[1] = '\0';
    else
        *slash = '\0';
    return copy;
}

static bool under_root(const char *dir, const char *root)
{
    size_t n = strlen(root);

    if (strncmp(dir, root, n) != 0)
        return false;
    return dir[n] == '\0' || (n == 1U && root[0] == '/') || dir[n] == '/';
}

static bool has_marker(const char *dir, const char *name)
{
    Bytebuf path;
    struct stat st;
    bool found;

    bytebuf_init(&path);
    bytebuf_append(&path, dir, strlen(dir));
    if (path.len == 0U || path.data[path.len - 1U] != '/')
        bytebuf_push_u8(&path, '/');
    bytebuf_append(&path, name, strlen(name));
    bytebuf_push_u8(&path, 0U);
    found = stat((const char *)path.data, &st) == 0;
    bytebuf_free(&path);
    return found;
}

char *yew_lsp_resolve_root(const LspServerCfg *cfg, const char *buffer_path,
                           const char *workspace_root)
{
    char *raw_dir;
    char *dir;
    char *root;

    if (cfg == NULL || buffer_path == NULL || workspace_root == NULL)
        return NULL;
    root = canonical_dir(workspace_root);
    raw_dir = path_dir(buffer_path);
    dir = raw_dir == NULL ? NULL : canonical_dir(raw_dir);
    free(raw_dir);
    if (root == NULL || dir == NULL || !under_root(dir, root)) {
        free(dir);
        return root;
    }
    for (;;) {
        const char *const *marker;

        for (marker = cfg->roots; marker != NULL && *marker != NULL; marker++) {
            if (has_marker(dir, *marker)) {
                free(root);
                return dir;
            }
        }
        if (strcmp(dir, root) == 0)
            break;
        {
            char *parent = path_dir(dir);

            if (parent == NULL || strcmp(parent, dir) == 0) {
                free(parent);
                break;
            }
            free(dir);
            dir = parent;
        }
    }
    free(dir);
    return root;
}

static bool uri_safe(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~' || c == '/';
}

void yew_lsp_uri_of_path(Bytebuf *out, const u8 *path, u32 n)
{
    static const char hex[] = "0123456789ABCDEF";
    u32 i;

    if (out == NULL || (path == NULL && n != 0U))
        return;
    bytebuf_append(out, "file://", 7U);
    for (i = 0U; i < n; i++) {
        if (uri_safe(path[i]))
            bytebuf_push_u8(out, path[i]);
        else {
            bytebuf_push_u8(out, '%');
            bytebuf_push_u8(out, (u8)hex[path[i] >> 4]);
            bytebuf_push_u8(out, (u8)hex[path[i] & 15U]);
        }
    }
}

static int hexval(u8 c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    return -1;
}

bool yew_lsp_path_of_uri(Bytebuf *out, const u8 *uri, u32 n)
{
    u32 i;

    if (out == NULL || uri == NULL || n < 8U ||
        memcmp(uri, "file://", 7U) != 0 || uri[7] != '/')
        return false;
    for (i = 7U; i < n; i++) {
        if (uri[i] == '%') {
            int hi;
            int lo;

            if (i + 2U >= n || (hi = hexval(uri[i + 1U])) < 0 ||
                (lo = hexval(uri[i + 2U])) < 0)
                return false;
            bytebuf_push_u8(out, (u8)((hi << 4) | lo));
            i += 2U;
        } else {
            bytebuf_push_u8(out, uri[i]);
        }
    }
    return true;
}

static bool capability(const JsonValue *caps, const char *name)
{
    const JsonValue *v = yew_json_get(caps, name);

    return v != NULL && v->kind != YEW_JS_NULL &&
           !(v->kind == YEW_JS_BOOL && !v->b);
}

static void append_string_array(char *out, size_t cap, const JsonValue *arr)
{
    u32 i;
    size_t used = 0U;

    if (cap == 0U || arr == NULL || arr->kind != YEW_JS_ARR)
        return;
    for (i = 0U; i < arr->arr.n; i++) {
        u32 n = 0U;
        const u8 *s = yew_json_str(arr->arr.v[i], &n);

        if (s == NULL || n == 0U || n > cap - 1U - used)
            continue;
        (void)memcpy(out + used, s, n);
        used += n;
    }
    out[used] = '\0';
}

void yew_lsp_caps_parse(LspCaps *out, const JsonValue *initialize_result,
                        u8 *pos_enc, bool *unknown_encoding)
{
    static const struct { const char *key; u32 bit; } fields[] = {
        {"completionProvider", YEW_LSPC_COMPLETION},
        {"hoverProvider", YEW_LSPC_HOVER},
        {"signatureHelpProvider", YEW_LSPC_SIGNATURE},
        {"definitionProvider", YEW_LSPC_DEFINITION},
        {"declarationProvider", YEW_LSPC_DECLARATION},
        {"typeDefinitionProvider", YEW_LSPC_TYPE_DEFINITION},
        {"implementationProvider", YEW_LSPC_IMPLEMENTATION},
        {"referencesProvider", YEW_LSPC_REFERENCES},
        {"documentHighlightProvider", YEW_LSPC_DOCUMENT_HIGHLIGHT},
        {"documentSymbolProvider", YEW_LSPC_DOCUMENT_SYMBOL},
        {"renameProvider", YEW_LSPC_RENAME},
        {"workspaceSymbolProvider", YEW_LSPC_WORKSPACE_SYMBOL}
    };
    const JsonValue *caps = yew_json_get(initialize_result, "capabilities");
    const JsonValue *sync;
    const JsonValue *completion;
    const JsonValue *sig;
    const JsonValue *enc;
    size_t i;

    (void)memset(out, 0, sizeof(*out));
    *pos_enc = YEW_POSENC_UTF16;
    *unknown_encoding = false;
    if (caps == NULL || caps->kind != YEW_JS_OBJ)
        return;
    for (i = 0U; i < YEW_ARRAY_LEN(fields); i++)
        if (capability(caps, fields[i].key)) out->bits |= fields[i].bit;
    sync = yew_json_get(caps, "textDocumentSync");
    if (sync != NULL && sync->kind == YEW_JS_INT)
        out->sync_kind = sync->i >= 0 && sync->i <= 2 ? (u8)sync->i : 0U;
    else if (sync != NULL && sync->kind == YEW_JS_OBJ) {
        i64 change = yew_json_int(yew_json_get(sync, "change"), 0);
        const JsonValue *save = yew_json_get(sync, "save");

        out->sync_kind = change >= 0 && change <= 2 ? (u8)change : 0U;
        out->save_supported = save != NULL && save->kind != YEW_JS_NULL &&
            !(save->kind == YEW_JS_BOOL && !save->b);
        if (save != NULL && save->kind == YEW_JS_OBJ)
            out->save_include_text = yew_json_bool(
                yew_json_get(save, "includeText"), false);
    }
    completion = yew_json_get(caps, "completionProvider");
    if (completion != NULL && completion->kind == YEW_JS_OBJ) {
        out->resolve_completion = yew_json_bool(
            yew_json_get(completion, "resolveProvider"), false);
        append_string_array(out->trigger_chars, sizeof(out->trigger_chars),
                            yew_json_get(completion, "triggerCharacters"));
    }
    sig = yew_json_get(caps, "signatureHelpProvider");
    if (sig != NULL && sig->kind == YEW_JS_OBJ)
        append_string_array(out->sig_trigger, sizeof(out->sig_trigger),
                            yew_json_get(sig, "triggerCharacters"));
    enc = yew_json_get(caps, "positionEncoding");
    if (enc == NULL)
        return;
    if (yew_json_streq(enc, "utf-8"))
        *pos_enc = YEW_POSENC_UTF8;
    else if (!yew_json_streq(enc, "utf-16"))
        *unknown_encoding = true;
}

bool yew_lsp_has(const LspServer *s, u32 cap)
{
    return s != NULL && (s->caps.bits & cap) != 0U;
}

void yew_lsp_initialize_params(Bytebuf *out, const LspServerCfg *cfg,
                               const char *root, i64 process_id)
{
    Bytebuf uri;
    JsonW w;
    const char *name;

    bytebuf_init(&uri);
    yew_lsp_uri_of_path(&uri, (const u8 *)root, (u32)strlen(root));
    name = strrchr(root, '/');
    name = name == NULL ? root : name + 1;
    yew_jsonw_init(&w, out);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "processId"); yew_jsonw_int(&w, process_id);
    yew_jsonw_key(&w, "clientInfo"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "name"); yew_jsonw_cstr(&w, "yew");
    yew_jsonw_key(&w, "version"); yew_jsonw_cstr(&w, YEW_VERSION);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "rootUri"); yew_jsonw_str(&w, uri.data, (u32)uri.len);
    yew_jsonw_key(&w, "workspaceFolders"); yew_jsonw_arr(&w);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "uri"); yew_jsonw_str(&w, uri.data, (u32)uri.len);
    yew_jsonw_key(&w, "name"); yew_jsonw_cstr(&w, name);
    yew_jsonw_obj_end(&w); yew_jsonw_arr_end(&w);
    yew_jsonw_key(&w, "initializationOptions");
    if (cfg->init_options != NULL)
        yew_jsonw_raw(&w, (const u8 *)cfg->init_options,
                      (u32)strlen(cfg->init_options));
    else { yew_jsonw_obj(&w); yew_jsonw_obj_end(&w); }
    yew_jsonw_key(&w, "capabilities"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "general"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "positionEncodings"); yew_jsonw_arr(&w);
    yew_jsonw_cstr(&w, "utf-8"); yew_jsonw_cstr(&w, "utf-16");
    yew_jsonw_arr_end(&w); yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "textDocument"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "synchronization"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "dynamicRegistration"); yew_jsonw_bool(&w, false);
    yew_jsonw_key(&w, "willSave"); yew_jsonw_bool(&w, false);
    yew_jsonw_key(&w, "willSaveWaitUntil"); yew_jsonw_bool(&w, false);
    yew_jsonw_key(&w, "didSave"); yew_jsonw_bool(&w, true);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "completion"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "contextSupport"); yew_jsonw_bool(&w, true);
    yew_jsonw_key(&w, "completionItem"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "snippetSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_key(&w, "documentationFormat"); yew_jsonw_arr(&w);
    yew_jsonw_cstr(&w, "plaintext"); yew_jsonw_arr_end(&w);
    yew_jsonw_key(&w, "insertReplaceSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_key(&w, "resolveSupport"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "properties"); yew_jsonw_arr(&w);
    yew_jsonw_cstr(&w, "documentation"); yew_jsonw_cstr(&w, "detail");
    yew_jsonw_arr_end(&w); yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "completionItemKind"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "valueSet"); yew_jsonw_arr(&w);
    { i64 k; for (k = 1; k <= 25; k++) yew_jsonw_int(&w, k); }
    yew_jsonw_arr_end(&w); yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w);
#define SIMPLE_CAP(name_)                                                     \
    do { yew_jsonw_key(&w, name_); yew_jsonw_obj(&w);                         \
         yew_jsonw_obj_end(&w); } while (0)
    yew_jsonw_key(&w, "hover"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "contentFormat"); yew_jsonw_arr(&w);
    yew_jsonw_cstr(&w, "plaintext"); yew_jsonw_arr_end(&w);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "signatureHelp"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "signatureInformation"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "documentationFormat"); yew_jsonw_arr(&w);
    yew_jsonw_cstr(&w, "plaintext"); yew_jsonw_arr_end(&w);
    yew_jsonw_key(&w, "parameterInformation"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "labelOffsetSupport"); yew_jsonw_bool(&w, true);
    yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "definition"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "linkSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "declaration"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "linkSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "typeDefinition"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "linkSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "implementation"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "linkSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_obj_end(&w);
    SIMPLE_CAP("references"); SIMPLE_CAP("documentHighlight");
    yew_jsonw_key(&w, "documentSymbol"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "hierarchicalDocumentSymbolSupport");
    yew_jsonw_bool(&w, true); yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "rename"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "prepareSupport"); yew_jsonw_bool(&w, false);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "publishDiagnostics"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "relatedInformation"); yew_jsonw_bool(&w, true);
    yew_jsonw_key(&w, "versionSupport"); yew_jsonw_bool(&w, true);
    yew_jsonw_key(&w, "tagSupport"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "valueSet"); yew_jsonw_arr(&w);
    yew_jsonw_int(&w, 1); yew_jsonw_int(&w, 2); yew_jsonw_arr_end(&w);
    yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "workspace"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "applyEdit"); yew_jsonw_bool(&w, false);
    yew_jsonw_key(&w, "configuration"); yew_jsonw_bool(&w, true);
    yew_jsonw_key(&w, "workspaceFolders"); yew_jsonw_bool(&w, true);
    yew_jsonw_key(&w, "workspaceEdit"); yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "documentChanges"); yew_jsonw_bool(&w, true);
    yew_jsonw_key(&w, "failureHandling"); yew_jsonw_cstr(&w, "abort");
    yew_jsonw_key(&w, "resourceOperations"); yew_jsonw_arr(&w);
    yew_jsonw_arr_end(&w); yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "window"); yew_jsonw_obj(&w);
    SIMPLE_CAP("showMessage");
    yew_jsonw_key(&w, "workDoneProgress"); yew_jsonw_bool(&w, false);
    yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w); yew_jsonw_obj_end(&w);
#undef SIMPLE_CAP
    bytebuf_free(&uri);
}

void yew_lsp_server_init(LspServer *s, u32 id,
                         const LspServerCfg *cfg, char *root)
{
    (void)memset(s, 0, sizeof(*s));
    s->id = id;
    s->cfg = cfg;
    s->root = root;
    s->state = YEW_LSP_SPAWNING;
    s->pos_enc = YEW_POSENC_UTF16;
    bytebuf_init(&s->stderr_tail);
}

void yew_lsp_server_dispose(LspServer *s)
{
    size_t i;

    if (s->rpc_live)
        yew_rpc_conn_free(&s->rpc);
    for (i = 0U; i < s->docv.len; i++) {
        yew_lsp_doc_free(&s->docv.data[i]);
    }
    VecLspDoc_free(&s->docv);
    VecU32Lsp_free(&s->docs);
    bytebuf_free(&s->stderr_tail);
    free(s->root);
    (void)memset(s, 0, sizeof(*s));
}

bool yew_lsp_server_initialized(LspServer *s, const JsonValue *result)
{
    bool unknown;

    if (s == NULL || s->state != YEW_LSP_INITIALIZING || result == NULL)
        return false;
    yew_lsp_caps_parse(&s->caps, result, &s->pos_enc, &unknown);
    if (unknown && s->owner != NULL)
        yew_msg(s->owner, YEW_MSG_WARN,
                "%s returned unknown positionEncoding; using utf-16",
                s->cfg->id);
    if (s->caps.sync_kind == 0U) {
        s->state = YEW_LSP_DEAD;
        return false;
    }
    s->state = YEW_LSP_READY;
    return true;
}

bool yew_lsp_server_crashed(LspServer *s, i64 now_ms,
                            const char *last_stderr, Bytebuf *message)
{
    static const i64 delay[] = {250, 1000, 4000, 16000};

    if (s->restarts == 0U ||
        now_ms - s->first_restart_ms >= YEW_LSP_RESTART_WINDOW_MS) {
        s->first_restart_ms = now_ms;
        s->restarts = 0U;
    }
    s->restarts++;
    s->state = YEW_LSP_DEAD;
    if (s->restarts >= 5U) {
        s->gave_up = true;
        bytebuf_printf(message,
            "%s crashed 5 times in 5 minutes; LSP is off for this workspace.\n"
            "last error: %s\nrun :lsp.restart to try again, or :lsp.log "
            "to read the server's output.", s->cfg->id,
            last_stderr != NULL && last_stderr[0] != '\0' ? last_stderr :
            "(no stderr output)");
        return false;
    }
    s->next_try_ms = now_ms + delay[s->restarts - 1U];
    if (s->restarts == 2U)
        bytebuf_printf(message, "%s restarted", s->cfg->id);
    else if (s->restarts > 2U)
        bytebuf_printf(message, "%s restarted (%u)", s->cfg->id,
                       s->restarts);
    return true;
}

void yew_lsp_server_restart_reset(LspServer *s)
{
    s->restarts = 0U;
    s->first_restart_ms = 0;
    s->next_try_ms = 0;
    s->gave_up = false;
}

static void framed_destroy(void *owner)
{
    LspServer *s = owner;

    if (s->rpc_live) {
        yew_rpc_conn_free(&s->rpc);
        s->rpc_live = false;
    }
}

static bool framed_feed(void *owner, const u8 *bytes, u64 n)
{ return yew_rpc_feed_stdout(&((LspServer *)owner)->rpc, bytes, n); }
static bool framed_finish(void *owner)
{ return yew_rpc_finish_stdout(&((LspServer *)owner)->rpc); }
static u64 framed_view(void *owner, const u8 **bytes)
{ return yew_rpc_tx_view(&((LspServer *)owner)->rpc, bytes); }
static void framed_consume(void *owner, u64 n)
{ yew_rpc_tx_consume(&((LspServer *)owner)->rpc, n); }
static i64 framed_deadline(const void *owner)
{ return yew_rpc_job_deadline(&((const LspServer *)owner)->rpc); }
static void framed_tick(void *owner, Ed *ed, i64 now_ms)
{ yew_rpc_job_tick(&((LspServer *)owner)->rpc, ed, now_ms); }

static const YewJobFramedOps framed_ops = {
    framed_feed, framed_finish, framed_view, framed_consume,
    framed_deadline, framed_tick, framed_destroy
};

static void initialize_done(Ed *ed, void *ctx, const JsonValue *result,
                            const JsonValue *error)
{
    LspServer *s = ctx;
    size_t i;

    if (error != NULL || !yew_lsp_server_initialized(s, result)) {
        s->state = YEW_LSP_DEAD;
        s->gave_up = true;
        (void)yew_job_signal(ed, s->job, SIGTERM);
        return;
    }
    yew_rpc_notify(&s->rpc, "initialized", (const u8 *)"{}", 2U);
    for (i = 0U; i < s->docv.len; i++) {
        LspDoc *doc = &s->docv.data[i];
        Buffer *buffer = yew_ws_buf_by_id(ed, doc->buf_id);

        if (buffer != NULL && buffer->tb != NULL)
            (void)yew_lsp_doc_open(&s->rpc, doc, buffer, buffer->lang);
    }
}

bool yew_lsp_dispatch_response(LspServer *s, const JsonValue *msg)
{
    const JsonValue *id;
    const RpcPending *pending;
    u64 number;

    if (s == NULL || msg == NULL)
        return false;
    id = yew_json_get(msg, "id");
    if (!yew_rpc_id_u64(id, &number))
        return false;
    pending = yew_rpc_pending(&s->rpc, number);
    if (pending != NULL && pending->buf_id != 0U) {
        Buffer *buffer = yew_ws_buf_by_id(s->owner, pending->buf_id);

        if (buffer == NULL || buffer->tb == NULL ||
            buffer->tb->gen != pending->gen) {
            if (yew_rpc_drop(&s->rpc, number))
                s->dropped_stale++;
            return false;
        }
    }
    return yew_rpc_dispatch(&s->rpc, s->owner, msg);
}

static void reply_configuration(LspServer *s, const JsonValue *id,
                                const JsonValue *params)
{
    const JsonValue *items = yew_json_get(params, "items");
    Bytebuf result;
    JsonW w;
    u32 i;

    bytebuf_init(&result);
    yew_jsonw_init(&w, &result);
    yew_jsonw_arr(&w);
    for (i = 0U; items != NULL && items->kind == YEW_JS_ARR &&
         i < items->arr.n; i++)
        yew_jsonw_null(&w);
    yew_jsonw_arr_end(&w);
    yew_rpc_reply(&s->rpc, id, result.data, (u32)result.len);
    bytebuf_free(&result);
}

void yew_lsp_server_dispatch_value(LspServer *s, const JsonValue *msg)
{
    RpcMsgKind kind;
    const JsonValue *method;
    const JsonValue *params;

    if (s == NULL || msg == NULL)
        return;
    kind = yew_rpc_classify(msg);
    if (kind == YEW_RPC_MALFORMED)
        return;
    if (kind == YEW_RPC_RESPONSE || kind == YEW_RPC_ERROR) {
        (void)yew_lsp_dispatch_response(s, msg);
        return;
    }
    method = yew_json_get(msg, "method");
    params = yew_json_get(msg, "params");
    if (yew_json_streq(method, "textDocument/publishDiagnostics")) {
        const JsonValue *uri = yew_json_get(params, "uri");
        const JsonValue *items = yew_json_get(params, "diagnostics");
        i64 version = yew_json_int(yew_json_get(params, "version"), -1);
        u32 nuri = 0U;
        const u8 *puri = yew_json_str(uri, &nuri);
        size_t i;

        for (i = 0U; puri != NULL && i < s->docv.len; i++) {
            LspDoc *doc = &s->docv.data[i];

            if (strlen(doc->uri) == nuri && memcmp(doc->uri, puri, nuri) == 0) {
                Buffer *buffer = yew_ws_buf_by_id(s->owner, doc->buf_id);

                if (buffer != NULL && items != NULL &&
                    items->kind == YEW_JS_ARR &&
                    (version < 0 || version >= doc->version))
                    yew_diag_replace(s->owner, buffer, s->id, items, version);
                break;
            }
        }
        return;
    }
    if (kind == YEW_RPC_SRV_REQUEST) {
        const JsonValue *id = yew_json_get(msg, "id");

        if (yew_json_streq(method, "workspace/configuration"))
            reply_configuration(s, id, params);
        else if (yew_json_streq(method,
                                "window/workDoneProgress/create"))
            yew_rpc_reply(&s->rpc, id, (const u8 *)"null", 4U);
        else
            yew_rpc_reply_error(&s->rpc, id, -32601,
                                "method not supported by yew");
        return;
    }
    if (yew_json_streq(method, "$/progress") ||
        yew_json_streq(method, "window/logMessage"))
        return;
    if (yew_json_streq(method, "window/showMessage")) {
        u32 n = 0U;
        const u8 *text = yew_json_str(yew_json_get(params, "message"), &n);

        if (text != NULL)
            yew_msg(s->owner, YEW_MSG_INFO, "%.*s", (int)n,
                    (const char *)text);
    }
}

static void on_rpc_value(void *ctx, const JsonValue *msg)
{
    yew_lsp_server_dispatch_value(ctx, msg);
}

LspClient *yew_lsp_client_new(void)
{
    LspClient *client = yew_xcalloc(1U, sizeof(*client));

    client->next_id = 1U;
    return client;
}

void yew_lsp_client_refresh_config(Ed *ed)
{
    if (ed == NULL)
        return;
    if (ed->lsp != NULL && ed->lsp->config_loaded &&
        ed->lsp->config_token != ed->config)
        yew_lsp_client_free(ed);
    if (ed->lsp == NULL)
        ed->lsp = yew_lsp_client_new();
    if (!ed->lsp->config_loaded)
        client_cfg_load(ed, ed->lsp);
}

const LspServerCfg *yew_lsp_client_cfg(Ed *ed, const char *lang)
{
    u32 i;

    if (ed == NULL || lang == NULL)
        return NULL;
    yew_lsp_client_refresh_config(ed);
    if (!ed->lsp->replaces_defaults)
        return yew_lsp_default_cfg(lang);
    if (strncmp(lang, "fortran", 7U) == 0)
        lang = "fortran";
    for (i = 0U; i < ed->lsp->cfg_len; i++)
        if (strcmp(ed->lsp->cfg[i].pub.lang, lang) == 0)
            return &ed->lsp->cfg[i].pub;
    return NULL;
}

static LspServer *find_server(const LspClient *c, const char *id,
                              const char *root)
{
    u32 i;

    for (i = 0U; c != NULL && i < c->len; i++)
        if (strcmp(c->server[i]->cfg->id, id) == 0 &&
            strcmp(c->server[i]->root, root) == 0)
            return c->server[i];
    return NULL;
}

static bool spawn_server(Ed *ed, LspServer *s)
{
    char *argv[16];
    u32 argc = 0U;
    const char *const *arg;
    YewJobSpec spec;
    RpcPending pending;
    Bytebuf params;
    char err[160];

    argv[argc++] = (char *)s->cfg->cmd;
    for (arg = s->cfg->args; arg != NULL && *arg != NULL; arg++) {
        if (argc + 1U >= YEW_ARRAY_LEN(argv))
            return false;
        argv[argc++] = (char *)*arg;
    }
    argv[argc] = NULL;
    yew_rpc_conn_init(&s->rpc);
    s->rpc_live = true;
    s->owner = ed;
    yew_rpc_set_handler(&s->rpc, on_rpc_value, s);
    (void)memset(&spec, 0, sizeof(spec));
    spec.argv = argv;
    spec.cwd = s->root;
    spec.sink = YEW_SINK_FRAMED;
    spec.display = s->cfg->id;
    spec.framed_owner = s;
    spec.framed_ops = &framed_ops;
    s->job = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (s->job == 0U) {
        framed_destroy(s);
        s->state = YEW_LSP_DEAD;
        /* A missing optional tool is disabled for this session; retry only
         * after an explicit :lsp.restart. */
        s->gave_up = true;
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        return false;
    }
    bytebuf_init(&params);
    yew_lsp_initialize_params(&params, s->cfg, s->root, (i64)getpid());
    (void)memset(&pending, 0, sizeof(pending));
    pending.cb = initialize_done;
    pending.ctx = s;
    pending.sent_ms = ed->now_ms;
    pending.deadline_ms = ed->now_ms + s->cfg->init_timeout_ms;
    (void)yew_rpc_call(&s->rpc, "initialize", params.data, (u32)params.len,
                       &pending);
    bytebuf_free(&params);
    s->state = YEW_LSP_INITIALIZING;
    return true;
}

static LspDoc *attach_doc(LspServer *s, const Buffer *b)
{
    Bytebuf uri;
    LspDoc doc;
    const char *path;

    for (size_t i = 0U; i < s->docv.len; i++)
        if (s->docv.data[i].buf_id == b->id)
            return &s->docv.data[i];
    path = b->meta.realpath != NULL ? b->meta.realpath : b->path;
    bytebuf_init(&uri);
    yew_lsp_uri_of_path(&uri, (const u8 *)path, (u32)strlen(path));
    bytebuf_push_u8(&uri, 0U);
    yew_lsp_doc_init(&doc, b->id, (const char *)uri.data);
    doc.server = s;
    VecLspDoc_push(&s->docv, doc);
    VecU32Lsp_push(&s->docs, b->id);
    bytebuf_free(&uri);
    return &s->docv.data[s->docv.len - 1U];
}

bool yew_lsp_client_start_cfg(Ed *ed, Buffer *b, const LspServerCfg *cfg)
{
    char *root;
    LspServer *s;

    if (ed == NULL || b == NULL || cfg == NULL || b->path == NULL ||
        cfg->id == NULL || cfg->lang == NULL || cfg->cmd == NULL)
        return false;
    if (ed->lsp == NULL)
        ed->lsp = yew_lsp_client_new();
    root = yew_lsp_resolve_root(cfg, b->path, yew_ws_root(ed));
    if (root == NULL)
        return false;
    s = find_server(ed->lsp, cfg->id, root);
    if (s != NULL) {
        LspDoc *doc;

        free(root);
        doc = attach_doc(s, b);
        if (s->state == YEW_LSP_READY && !doc->open)
            (void)yew_lsp_doc_open(&s->rpc, doc, b, b->lang);
        return !s->gave_up;
    }
    if (ed->lsp->len >= YEW_LSP_MAX_SERVERS) {
        free(root);
        yew_msg(ed, YEW_MSG_ERROR, "LSP server limit reached");
        return false;
    }
    s = yew_xcalloc(1U, sizeof(*s));
    yew_lsp_server_init(s, ed->lsp->next_id++, cfg, root);
    ed->lsp->server[ed->lsp->len++] = s;
    (void)attach_doc(s, b);
    return spawn_server(ed, s);
}

bool yew_lsp_client_start(Ed *ed, Buffer *b)
{
    const LspServerCfg *cfg;

    if (b == NULL || b->lang == NULL)
        return false;
    cfg = yew_lsp_client_cfg(ed, b->lang);
    return cfg != NULL && yew_lsp_client_start_cfg(ed, b, cfg);
}

static void shutdown_done(Ed *ed, void *ctx, const JsonValue *result,
                          const JsonValue *error)
{
    LspServer *s = ctx;

    (void)result;
    (void)error;
    yew_rpc_notify(&s->rpc, "exit", (const u8 *)"null", 4U);
    s->exit_sent = true;
    s->next_try_ms = 0;
    (void)ed;
}

void yew_lsp_client_stop(Ed *ed, LspServer *s, bool graceful)
{
    if (ed == NULL || s == NULL || s->state == YEW_LSP_DEAD)
        return;
    s->state = YEW_LSP_SHUTTING_DOWN;
    if (graceful && s->rpc_live) {
        RpcPending p;

        (void)memset(&p, 0, sizeof(p));
        p.cb = shutdown_done;
        p.ctx = s;
        p.sent_ms = ed->now_ms;
        (void)yew_rpc_call(&s->rpc, "shutdown", NULL, 0U, &p);
    } else {
        (void)yew_job_signal(ed, s->job, SIGTERM);
    }
}

void yew_lsp_client_close_buffer(Ed *ed, Buffer *b)
{
    u32 i;

    if (b == NULL)
        return;
    yew_diag_store_free(b);
    if (ed == NULL || ed->lsp == NULL)
        return;
    for (i = 0U; i < ed->lsp->len; i++) {
        LspServer *s = ed->lsp->server[i];
        size_t d;

        for (d = 0U; d < s->docv.len; d++) {
            if (s->docv.data[d].buf_id == b->id) {
                size_t k;

                if (s->state == YEW_LSP_READY)
                    yew_lsp_doc_close(&s->rpc, &s->docv.data[d]);
                yew_lsp_doc_free(&s->docv.data[d]);
                if (d + 1U < s->docv.len)
                    (void)memmove(&s->docv.data[d], &s->docv.data[d + 1U],
                        (s->docv.len - d - 1U) * sizeof(s->docv.data[0]));
                s->docv.len--;
                for (k = 0U; k < s->docs.len; k++) {
                    if (s->docs.data[k] != b->id)
                        continue;
                    if (k + 1U < s->docs.len)
                        (void)memmove(&s->docs.data[k], &s->docs.data[k + 1U],
                            (s->docs.len - k - 1U) * sizeof(s->docs.data[0]));
                    s->docs.len--;
                    break;
                }
                return;
            }
        }
    }
}

static void reset_server_session(Ed *ed, LspServer *s)
{
    size_t i;

    (void)memset(&s->caps, 0, sizeof(s->caps));
    s->pos_enc = YEW_POSENC_UTF16;
    s->exit_sent = false;
    for (i = 0U; i < s->docv.len; i++) {
        LspDoc *doc = &s->docv.data[i];
        Buffer *buffer = yew_ws_buf_by_id(ed, doc->buf_id);

        doc->version = 0;
        doc->sent_gen = 0U;
        doc->open = false;
        doc->full_sync = false;
        doc->insert_waiting = false;
        doc->pending.len = 0U;
        arena_free_all(&doc->changes);
        arena_init(&doc->changes);
        if (buffer != NULL)
            yew_diag_store_free(buffer);
    }
}

bool yew_lsp_client_restart(Ed *ed, Buffer *b)
{
    LspDoc *doc;
    LspServer *s;
    size_t i;

    if (ed == NULL || b == NULL)
        return false;
    doc = yew_lsp_doc_for_buffer(ed, b);
    s = yew_lsp_server_for_doc(ed, doc);
    if (s == NULL)
        return yew_lsp_client_start(ed, b);
    for (i = 0U; i < s->docv.len; i++) {
        if (s->state == YEW_LSP_READY)
            yew_lsp_doc_close(&s->rpc, &s->docv.data[i]);
    }
    reset_server_session(ed, s);
    yew_lsp_server_restart_reset(s);
    (void)yew_job_signal(ed, s->job, SIGTERM);
    s->state = YEW_LSP_DEAD;
    s->next_try_ms = ed->now_ms;
    return true;
}

void yew_lsp_client_pump(Ed *ed)
{
    u32 i;

    if (ed == NULL || ed->lsp == NULL)
        return;
    for (i = 0U; i < ed->lsp->len; i++) {
        LspServer *s = ed->lsp->server[i];
        YewJob *j = yew_job_find(ed, s->job);

        if (j != NULL && j->framed_err.len > YEW_LSP_STDERR_TAIL) {
            size_t keep = YEW_LSP_STDERR_TAIL;

            (void)memmove(j->framed_err.data,
                          j->framed_err.data + j->framed_err.len - keep,
                          keep);
            j->framed_err.len = keep;
        }

        if (j != NULL && j->state == YEW_JOB_EXECFAIL) {
            s->state = YEW_LSP_DEAD;
            s->gave_up = true;
            yew_msg(ed, YEW_MSG_ERROR,
                    "%s: %s — install %s or set lsp.servers.%s.cmd",
                    s->cfg->id, strerror(j->exec_errno), s->cfg->cmd,
                    s->cfg->lang);
            continue;
        }
        if (j != NULL && j->state == YEW_JOB_RUNNING)
        {
            if (s->state == YEW_LSP_READY) {
                size_t d;

                for (d = 0U; d < s->docv.len; d++) {
                    Buffer *buffer = yew_ws_buf_by_id(ed,
                        s->docv.data[d].buf_id);

                    if (buffer != NULL && buffer->tb != NULL)
                        (void)yew_lsp_doc_flush(&s->rpc, &s->docv.data[d],
                            s->caps.sync_kind, buffer->tb);
                }
            } else if (s->state == YEW_LSP_SHUTTING_DOWN && s->exit_sent) {
                if (s->next_try_ms == 0 && s->rpc.tx.pending.len == 0U) {
                    if (j->in_fd >= 0) {
                        (void)close(j->in_fd);
                        j->in_fd = -1;
                    }
                    s->next_try_ms = ed->now_ms + YEW_JOB_TERM_GRACE_MS;
                } else if (s->next_try_ms != 0 &&
                           ed->now_ms >= s->next_try_ms) {
                    (void)yew_job_signal(ed, s->job, SIGTERM);
                    s->next_try_ms = 0;
                    s->exit_sent = false;
                }
            }
            continue;
        }
        if (s->state != YEW_LSP_DEAD && s->state != YEW_LSP_SHUTTING_DOWN) {
            Bytebuf msg;
            Bytebuf tail;
            size_t tail_end;

            bytebuf_init(&msg);
            bytebuf_init(&tail);
            reset_server_session(ed, s);
            tail_end = j == NULL ? 0U : j->framed_err.len;
            while (tail_end != 0U &&
                   (j->framed_err.data[tail_end - 1U] == '\n' ||
                    j->framed_err.data[tail_end - 1U] == '\r'))
                tail_end--;
            if (j != NULL && tail_end != 0U) {
                size_t start = tail_end;

                while (start != 0U && j->framed_err.data[start - 1U] != '\n')
                    start--;
                bytebuf_append(&tail, j->framed_err.data + start,
                               tail_end - start);
            }
            bytebuf_push_u8(&tail, 0U);
            if (!yew_lsp_server_crashed(s, ed->now_ms,
                                        (const char *)tail.data, &msg) ||
                msg.len != 0U) {
                bytebuf_push_u8(&msg, 0U);
                yew_msg(ed, s->gave_up ? YEW_MSG_ERROR : YEW_MSG_WARN,
                        "%s", msg.data);
            }
            bytebuf_free(&tail);
            bytebuf_free(&msg);
        }
        if (!s->gave_up && s->state == YEW_LSP_DEAD &&
            ed->now_ms >= s->next_try_ms) {
            if (j != NULL && !yew_job_pending(j))
                yew_job_release(ed, j);
            (void)spawn_server(ed, s);
        }
    }
}

void yew_lsp_client_free(Ed *ed)
{
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    i64 deadline;
    u32 i;

    if (ed == NULL || ed->lsp == NULL)
        return;
    for (i = 0U; i < ed->lsp->len; i++)
        yew_lsp_client_stop(ed, ed->lsp->server[i], true);
    deadline = yew_now_ms() + 500;
    for (;;) {
        bool pending = false;
        i64 now = yew_now_ms();
        u32 n = 0U;
        int timeout;

        for (i = 0U; i < ed->lsp->len; i++) {
            YewJob *j = yew_job_find(ed, ed->lsp->server[i]->job);

            if (j != NULL && yew_job_pending(j)) {
                pending = true;
                break;
            }
        }
        if (!pending || now >= deadline)
            break;
        yew_job_collect_fds(ed, pfd, &n);
        timeout = (int)(deadline - now);
        if (timeout > 10)
            timeout = 10;
        (void)poll(pfd, (nfds_t)n, timeout);
        now = yew_now_ms();
        ed->now_ms = now;
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, now);
        yew_job_settle(ed);
        yew_lsp_client_pump(ed);
    }
    for (i = 0U; i < ed->lsp->len; i++) {
        YewJob *j = yew_job_find(ed, ed->lsp->server[i]->job);

        if (j != NULL && yew_job_pending(j))
            (void)yew_job_signal(ed, j->id, SIGKILL);
    }
    for (i = 0U; i < ed->lsp->len; i++) {
        LspServer *s = ed->lsp->server[i];
        YewJob *j = yew_job_find(ed, s->job);

        if (s->state != YEW_LSP_DEAD)
            (void)yew_job_signal(ed, s->job, SIGTERM);
        /* Detach the framed owner before releasing it.  yew_ed_free may
         * dispose the optional client before the generic job table. */
        if (j != NULL && j->framed_owner == s) {
            framed_destroy(s);
            j->framed_owner = NULL;
            j->framed_ops = NULL;
            j->framed_destroyed = true;
        }
        yew_lsp_server_dispose(s);
        free(s);
    }
    client_cfg_free(ed->lsp);
    free(ed->lsp);
    ed->lsp = NULL;
}

LspDoc *yew_lsp_doc_find(const Ed *ed, u32 buf_id,
                         const LspServer **server)
{
    u32 i;

    if (server != NULL) *server = NULL;
    for (i = 0U; ed != NULL && ed->lsp != NULL && i < ed->lsp->len; i++) {
        LspServer *s = ed->lsp->server[i];
        size_t j;

        for (j = 0U; j < s->docv.len; j++) {
            if (s->docv.data[j].buf_id == buf_id) {
                if (server != NULL) *server = s;
                return &s->docv.data[j];
            }
        }
    }
    return NULL;
}

LspDoc *yew_lsp_doc_for_buffer(Ed *ed, const Buffer *b)
{
    return b == NULL ? NULL : yew_lsp_doc_find(ed, b->id, NULL);
}

LspServer *yew_lsp_server_for_doc(Ed *ed, const LspDoc *doc)
{
    (void)ed;
    return doc == NULL ? NULL : doc->server;
}

bool yew_lsp_server_pos_enc(const Ed *ed, u32 server, u8 *out)
{
    u32 i;

    for (i = 0U; ed != NULL && ed->lsp != NULL && i < ed->lsp->len; i++) {
        if (ed->lsp->server[i]->id == server) {
            if (out != NULL) *out = ed->lsp->server[i]->pos_enc;
            return true;
        }
    }
    return false;
}
