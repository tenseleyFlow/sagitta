/* Sprint 48: owned, insertion-ordered runtime backend registry. */

#include "mod/ai/registry.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/arena.h"
#include "util/base.h"

enum { YEW_AI_KEY_CMD_MAX = 64 };

struct AiOwnedBackend {
    AiBackendEntry pub;
    char **key_cmd;
};

typedef struct AiOwnedBackend OwnedBackend;

static bool registry_error(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL && errsz != 0U) {
        va_start(ap, fmt);
        (void)vsnprintf(err, errsz, fmt, ap);
        va_end(ap);
    }
    return false;
}

static char *copy_bytes(const char *bytes, size_t len)
{
    char *copy = yew_xmalloc(len + 1U);

    (void)memcpy(copy, bytes, len);
    copy[len] = '\0';
    return copy;
}

static char *copy_fl_string(const FlStr *s)
{
    if (s == NULL || memchr(s->b, '\0', s->len) != NULL)
        return NULL;
    return copy_bytes(s->b, s->len);
}

static bool fl_string_eq(const FlStr *s, const char *text)
{
    size_t len = strlen(text);

    return s != NULL && (size_t)s->len == len &&
           memcmp(s->b, text, len) == 0;
}

static bool ascii_prefix_ci(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        unsigned char a = (unsigned char)*text++;
        unsigned char b = (unsigned char)*prefix++;

        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z')
            a = (unsigned char)(a - (unsigned char)'A' +
                                (unsigned char)'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool forbidden_name(const FlStr *name)
{
    return fl_string_eq(name, "key") || fl_string_eq(name, "api_key") ||
           fl_string_eq(name, "token") ||
           fl_string_eq(name, "password") ||
           fl_string_eq(name, "secret");
}

static bool field_find(const FlMap *map, const char *name, FlValue *out)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    while (fl_map_iter(map, &cursor, &key, &value)) {
        if (key.t == (u8)FL_STR &&
            fl_string_eq((const FlStr *)key.as.o, name)) {
            if (out != NULL)
                *out = value;
            return true;
        }
    }
    return false;
}

static void string_list_free(char **list)
{
    u32 i;

    if (list == NULL)
        return;
    for (i = 0U; list[i] != NULL; i++)
        free(list[i]);
    free(list);
}

static void owned_drop(AiBackendRegistry *registry, OwnedBackend *owned)
{
    if (owned == NULL)
        return;
    if (owned->pub.endpoint != NULL && registry->release != NULL)
        registry->release(registry->prepare_ctx, owned->pub.endpoint);
    free((char *)owned->pub.backend.name);
    free((char *)owned->pub.url_text);
    free((char *)owned->pub.backend.url.host);
    free((char *)owned->pub.backend.url.path);
    free((char *)owned->pub.backend.model);
    free((char *)owned->pub.backend.key_env);
    string_list_free(owned->key_cmd);
    (void)memset(owned, 0, sizeof(*owned));
}

static bool value_string(const FlMap *map, const char *field, bool required,
                         char **out, char *err, size_t errsz)
{
    FlValue value;

    *out = NULL;
    if (!field_find(map, field, &value)) {
        if (required)
            return registry_error(err, errsz, "backend field '%s' is required",
                                  field);
        return true;
    }
    if (value.t != (u8)FL_STR)
        return registry_error(err, errsz, "backend field '%s' must be a string",
                              field);
    *out = copy_fl_string((const FlStr *)value.as.o);
    if (*out == NULL || (*out)[0] == '\0') {
        free(*out);
        *out = NULL;
        return registry_error(err, errsz,
                              "backend field '%s' must be a non-empty string",
                              field);
    }
    return true;
}

static bool value_bool(const FlMap *map, const char *field, bool fallback,
                       bool *out, char *err, size_t errsz)
{
    FlValue value;

    *out = fallback;
    if (!field_find(map, field, &value))
        return true;
    if (value.t != (u8)FL_BOOL)
        return registry_error(err, errsz, "backend field '%s' must be bool",
                              field);
    *out = value.as.b;
    return true;
}

static bool value_i64(const FlMap *map, const char *field, i64 fallback,
                      i64 min, i64 max, i64 *out, char *err, size_t errsz)
{
    FlValue value;

    *out = fallback;
    if (!field_find(map, field, &value))
        return true;
    if (value.t != (u8)FL_INT || value.as.i < min || value.as.i > max)
        return registry_error(err, errsz,
                              "backend field '%s' must be an integer from "
                              "%lld to %lld", field, (long long)min,
                              (long long)max);
    *out = value.as.i;
    return true;
}

static bool value_temperature(const FlMap *map, double *out,
                              char *err, size_t errsz)
{
    FlValue value;
    double temperature;

    *out = 0.1;
    if (!field_find(map, "temperature", &value))
        return true;
    if (value.t == (u8)FL_INT)
        temperature = (double)value.as.i;
    else if (value.t == (u8)FL_FLOAT)
        temperature = value.as.f;
    else
        return registry_error(err, errsz,
                              "backend field 'temperature' must be a number");
    if (!(temperature >= 0.0 && temperature <= 2.0))
        return registry_error(err, errsz,
                              "backend field 'temperature' must be from 0 to 2");
    *out = temperature;
    return true;
}

static bool value_key_cmd(const FlMap *map, char ***out,
                          char *err, size_t errsz)
{
    FlValue value;
    const FlList *list;
    char **copy;
    u32 i;

    *out = NULL;
    if (!field_find(map, "key_cmd", &value))
        return true;
    if (value.t != (u8)FL_LIST)
        return registry_error(err, errsz,
                              "backend field 'key_cmd' must be an argv array");
    list = (const FlList *)value.as.o;
    if (list->n == 0U || list->n > YEW_AI_KEY_CMD_MAX)
        return registry_error(err, errsz,
                              "backend field 'key_cmd' must contain 1 to %u arguments",
                              (unsigned)YEW_AI_KEY_CMD_MAX);
    copy = yew_xcalloc((size_t)list->n + 1U, sizeof(*copy));
    for (i = 0U; i < list->n; i++) {
        if (list->v[i].t != (u8)FL_STR ||
            (copy[i] = copy_fl_string((const FlStr *)list->v[i].as.o)) == NULL ||
            copy[i][0] == '\0') {
            string_list_free(copy);
            return registry_error(err, errsz,
                                  "backend field 'key_cmd' must contain only non-empty strings");
        }
    }
    *out = copy;
    return true;
}

static bool parse_kind(const char *text, u8 *out, char *err, size_t errsz)
{
    if (strcmp(text, "ollama") == 0)
        *out = (u8)YEW_AI_OLLAMA;
    else if (strcmp(text, "openai") == 0 ||
             strcmp(text, "openai-compatible") == 0)
        *out = (u8)YEW_AI_OPENAI;
    else if (strcmp(text, "anthropic") == 0)
        *out = (u8)YEW_AI_ANTHROPIC;
    else
        return registry_error(err, errsz,
                              "backend field 'kind' must be ollama, openai, or anthropic");
    return true;
}

static bool parse_transport(const char *text, bool https, u8 *out,
                            char *err, size_t errsz)
{
    if (text == NULL)
        *out = https ? (u8)YEW_AI_TR_CURL : (u8)YEW_AI_TR_HTTP;
    else if (strcmp(text, "http") == 0)
        *out = (u8)YEW_AI_TR_HTTP;
    else if (strcmp(text, "curl") == 0)
        *out = (u8)YEW_AI_TR_CURL;
    else
        return registry_error(err, errsz,
                              "backend field 'transport' must be http or curl");
    if (https && *out != (u8)YEW_AI_TR_CURL)
        return registry_error(
            err, errsz,
            "yew has no TLS: https backends run through curl.\n"
            "set transport: \"curl\" on this backend, or use a http:// endpoint.");
    return true;
}

static bool https_has_explicit_port(const char *url)
{
    const char *authority = url + strlen("https://");
    const char *tail = authority + strcspn(authority, "/?#");

    if (authority < tail && authority[0] == '[') {
        const char *close = memchr(authority, ']', (size_t)(tail - authority));

        return close != NULL && close + 1U < tail && close[1] == ':';
    }
    return memchr(authority, ':', (size_t)(tail - authority)) != NULL;
}

static bool parse_url(const char *url_text, u8 transport, HttpUrl *out,
                      char *err, size_t errsz)
{
    Arena arena;
    HttpUrl parsed;
    char *translated = NULL;
    bool https = ascii_prefix_ci(url_text, "https://");
    bool ok;

    arena_init(&arena);
    if (https) {
        size_t len = strlen(url_text);

        translated = yew_xmalloc(len);
        (void)memcpy(translated, "http://", 7U);
        (void)memcpy(translated + 7U, url_text + 8U, len - 7U);
        ok = yew_http_url_parse(&arena, translated, &parsed, err, errsz);
        if (ok && !https_has_explicit_port(url_text))
            parsed.port = 443U;
    } else {
        ok = yew_http_url_parse(&arena, url_text, &parsed, err, errsz);
    }
    free(translated);
    if (!ok) {
        arena_free_all(&arena);
        return false;
    }
    if (https && transport != (u8)YEW_AI_TR_CURL) {
        arena_free_all(&arena);
        return registry_error(err, errsz,
                              "https backend requires curl transport");
    }
    *out = parsed;
    out->host = copy_bytes(parsed.host, strlen(parsed.host));
    out->path = copy_bytes(parsed.path, strlen(parsed.path));
    arena_free_all(&arena);
    return true;
}

static bool validate_fields(const FlMap *map, char *err, size_t errsz)
{
    static const char *const allowed[] = {
        "kind", "transport", "url", "model", "key_env", "key_cmd",
        "max_tokens", "temperature", "stream", "fim"
    };
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    (void)value;
    while (fl_map_iter(map, &cursor, &key, &value)) {
        const FlStr *name;
        size_t i;
        bool known = false;

        if (key.t != (u8)FL_STR)
            return registry_error(err, errsz,
                                  "backend field names must be strings");
        name = (const FlStr *)key.as.o;
        if (forbidden_name(name))
            return registry_error(err, errsz,
                                  "backend field '%.*s' may not hold a literal API key; use key_env or key_cmd",
                                  (int)name->len, name->b);
        for (i = 0U; i < YEW_ARRAY_LEN(allowed); i++) {
            if (fl_string_eq(name, allowed[i])) {
                known = true;
                break;
            }
        }
        if (!known)
            return registry_error(err, errsz,
                                  "unknown backend field '%.*s'",
                                  (int)name->len, name->b);
    }
    return true;
}

static bool owned_parse(AiBackendRegistry *registry, OwnedBackend *out,
                        const FlStr *name, const FlMap *config,
                        char *err, size_t errsz)
{
    char *kind = NULL;
    char *transport = NULL;
    char *url = NULL;
    char *model = NULL;
    char *key_env = NULL;
    bool https;

    (void)memset(out, 0, sizeof(*out));
    if (name == NULL || config == NULL)
        return registry_error(err, errsz, "invalid backend registration");
    out->pub.backend.name = copy_fl_string(name);
    if (out->pub.backend.name == NULL || out->pub.backend.name[0] == '\0') {
        owned_drop(registry, out);
        return registry_error(err, errsz,
                              "backend name must be a non-empty string");
    }
    if (!validate_fields(config, err, errsz) ||
        !value_string(config, "kind", true, &kind, err, errsz) ||
        !value_string(config, "transport", false, &transport, err, errsz) ||
        !value_string(config, "url", true, &url, err, errsz) ||
        !value_string(config, "model", true, &model, err, errsz) ||
        !value_string(config, "key_env", false, &key_env, err, errsz) ||
        !value_key_cmd(config, &out->key_cmd, err, errsz) ||
        !value_i64(config, "max_tokens", 256, 1, 1048576,
                   &out->pub.backend.max_tokens, err, errsz) ||
        !value_temperature(config, &out->pub.backend.temperature,
                           err, errsz) ||
        !value_bool(config, "stream", true, &out->pub.backend.stream,
                    err, errsz) ||
        !value_bool(config, "fim", false, &out->pub.backend.fim,
                    err, errsz))
        goto fail;
    https = ascii_prefix_ci(url, "https://");
    if (!parse_kind(kind, &out->pub.backend.kind, err, errsz) ||
        !parse_transport(transport, https, &out->pub.backend.transport,
                         err, errsz) ||
        !parse_url(url, out->pub.backend.transport, &out->pub.backend.url,
                   err, errsz))
        goto fail;
    if (key_env != NULL && out->key_cmd != NULL) {
        (void)registry_error(err, errsz,
                             "backend '%s' has both key_env and key_cmd",
                             out->pub.backend.name);
        goto fail;
    }
    out->pub.url_text = url;
    url = NULL;
    out->pub.backend.model = model;
    model = NULL;
    out->pub.backend.key_env = key_env;
    key_env = NULL;
    out->pub.backend.key_cmd = (char *const *)out->key_cmd;
    out->pub.cooldown.until_ms = 0;
    out->pub.cooldown.consecutive = 0U;
    if (registry->prepare != NULL &&
        !registry->prepare(registry->prepare_ctx, out->pub.url_text,
                           &out->pub.backend.url,
                           out->pub.backend.transport, &out->pub.endpoint,
                           err, errsz))
        goto fail;
    free(kind);
    free(transport);
    return true;

fail:
    free(kind);
    free(transport);
    free(url);
    free(model);
    free(key_env);
    owned_drop(registry, out);
    return false;
}

static OwnedBackend *owned_entries(AiBackendRegistry *registry)
{
    return registry->entries;
}

static const OwnedBackend *owned_entries_const(const AiBackendRegistry *registry)
{
    return registry->entries;
}

void yew_ai_registry_init(AiBackendRegistry *registry,
                          AiBackendPrepareFn prepare,
                          AiBackendReleaseFn release, void *prepare_ctx)
{
    if (registry == NULL)
        return;
    (void)memset(registry, 0, sizeof(*registry));
    registry->prepare = prepare;
    registry->release = release;
    registry->prepare_ctx = prepare_ctx;
}

void yew_ai_registry_drop(AiBackendRegistry *registry)
{
    u32 i;
    OwnedBackend *entries;

    if (registry == NULL)
        return;
    entries = owned_entries(registry);
    for (i = 0U; i < registry->len; i++)
        owned_drop(registry, &entries[i]);
    free(entries);
    (void)memset(registry, 0, sizeof(*registry));
}

static i32 registry_index(const AiBackendRegistry *registry,
                          const char *name)
{
    const OwnedBackend *entries = owned_entries_const(registry);
    u32 i;

    for (i = 0U; i < registry->len; i++) {
        if (strcmp(entries[i].pub.backend.name, name) == 0)
            return (i32)i;
    }
    return -1;
}

bool yew_ai_registry_put(AiBackendRegistry *registry, const FlStr *name,
                         const FlMap *config, char *err, size_t errsz)
{
    OwnedBackend parsed;
    OwnedBackend *entries;
    i32 at;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (registry == NULL)
        return registry_error(err, errsz, "invalid backend registry");
    if (!owned_parse(registry, &parsed, name, config, err, errsz))
        return false;
    at = registry_index(registry, parsed.pub.backend.name);
    entries = owned_entries(registry);
    if (at >= 0) {
        owned_drop(registry, &entries[(u32)at]);
        entries[(u32)at] = parsed;
        return true;
    }
    if (registry->len == registry->cap) {
        u32 cap = registry->cap == 0U ? 4U : registry->cap * 2U;

        entries = yew_xreallocarray(entries, cap, sizeof(*entries));
        registry->entries = entries;
        registry->cap = cap;
    }
    entries[registry->len++] = parsed;
    return true;
}

bool yew_ai_registry_reload(AiBackendRegistry *registry,
                            const FlMap *backends, char *err, size_t errsz)
{
    AiBackendRegistry fresh;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (registry == NULL || backends == NULL)
        return registry_error(err, errsz, "invalid backend registry reload");
    yew_ai_registry_init(&fresh, registry->prepare, registry->release,
                         registry->prepare_ctx);
    while (fl_map_iter(backends, &cursor, &key, &value)) {
        if (key.t != (u8)FL_STR || value.t != (u8)FL_MAP) {
            yew_ai_registry_drop(&fresh);
            return registry_error(err, errsz,
                                  "backend registry must map string names to maps");
        }
        if (!yew_ai_registry_put(&fresh, (const FlStr *)key.as.o,
                                 (const FlMap *)value.as.o, err, errsz)) {
            yew_ai_registry_drop(&fresh);
            return false;
        }
    }
    {
        AiBackendRegistry old = *registry;

        *registry = fresh;
        yew_ai_registry_drop(&old);
    }
    return true;
}

bool yew_ai_registry_keep(AiBackendRegistry *registry, const char *name)
{
    OwnedBackend *entries;
    i32 keep;
    u32 i;

    if (registry == NULL || name == NULL ||
        (keep = registry_index(registry, name)) < 0)
        return false;
    entries = owned_entries(registry);
    for (i = 0U; i < registry->len; i++) {
        if (i != (u32)keep)
            owned_drop(registry, &entries[i]);
    }
    if (keep != 0)
        entries[0] = entries[(u32)keep];
    registry->len = 1U;
    return true;
}

const AiBackendEntry *yew_ai_registry_find(const AiBackendRegistry *registry,
                                           const char *name)
{
    i32 at;

    if (registry == NULL || name == NULL)
        return NULL;
    at = registry_index(registry, name);
    if (at < 0)
        return NULL;
    return &owned_entries_const(registry)[(u32)at].pub;
}

AiBackendEntry *yew_ai_registry_find_mut(AiBackendRegistry *registry,
                                         const char *name)
{
    i32 at;

    if (registry == NULL || name == NULL)
        return NULL;
    at = registry_index(registry, name);
    if (at < 0)
        return NULL;
    return &owned_entries(registry)[(u32)at].pub;
}

u32 yew_ai_registry_count(const AiBackendRegistry *registry)
{
    return registry != NULL ? registry->len : 0U;
}

const AiBackendEntry *yew_ai_registry_at(const AiBackendRegistry *registry,
                                         u32 index)
{
    if (registry == NULL || index >= registry->len)
        return NULL;
    return &owned_entries_const(registry)[index].pub;
}
