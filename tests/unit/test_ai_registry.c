#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/ai/registry.h"

typedef struct PrepareProbe {
    u32 prepared;
    u32 released;
    u32 next_token;
    char last_url[128];
} PrepareProbe;

static FlStr *test_string(const char *text)
{
    size_t len = strlen(text);
    FlStr *s = calloc(1U, sizeof(*s) + len + 1U);

    YEW_ASSERT_NOT_NULL(s);
    s->h.t = (u8)FL_STR;
    s->len = (u32)len;
    (void)memcpy(s->b, text, len + 1U);
    return s;
}

static FlValue test_strv(const char *text)
{
    return FL_OBJ_V(FL_STR, test_string(text));
}

static FlMap *test_map(void)
{
    FlMap *map = calloc(1U, sizeof(*map));

    YEW_ASSERT_NOT_NULL(map);
    map->h.t = (u8)FL_MAP;
    return map;
}

static void test_map_put(FlMap *map, const char *name, FlValue value)
{
    FlMapEnt *grown = realloc(map->ent,
                              ((size_t)map->n + 1U) * sizeof(*grown));

    YEW_ASSERT_NOT_NULL(grown);
    map->ent = grown;
    map->ent[map->n].k = test_strv(name);
    map->ent[map->n].v = value;
    map->ent[map->n].dead = false;
    map->n++;
}

static FlList *test_list(const char *a, const char *b, const char *c)
{
    const char *items[3] = {a, b, c};
    FlList *list = calloc(1U, sizeof(*list));
    u32 i;

    YEW_ASSERT_NOT_NULL(list);
    list->h.t = (u8)FL_LIST;
    while (list->n < 3U && items[list->n] != NULL)
        list->n++;
    list->v = calloc(list->n, sizeof(*list->v));
    YEW_ASSERT(list->n == 0U || list->v != NULL);
    for (i = 0U; i < list->n; i++)
        list->v[i] = test_strv(items[i]);
    return list;
}

static void test_value_drop(FlValue value)
{
    u32 i;

    if (value.t == (u8)FL_STR) {
        free(value.as.o);
    } else if (value.t == (u8)FL_LIST) {
        FlList *list = (FlList *)value.as.o;

        for (i = 0U; i < list->n; i++)
            test_value_drop(list->v[i]);
        free(list->v);
        free(list);
    } else if (value.t == (u8)FL_MAP) {
        FlMap *map = (FlMap *)value.as.o;

        for (i = 0U; i < map->n; i++) {
            test_value_drop(map->ent[i].k);
            test_value_drop(map->ent[i].v);
        }
        free(map->ent);
        free(map);
    }
}

static FlMap *minimal_config(const char *kind, const char *url,
                             const char *model)
{
    FlMap *map = test_map();

    test_map_put(map, "kind", test_strv(kind));
    test_map_put(map, "url", test_strv(url));
    test_map_put(map, "model", test_strv(model));
    return map;
}

static bool prepare_endpoint(void *ctx, const char *url_text, HttpUrl *url,
                             u8 transport, void **endpoint,
                             char *err, size_t errsz)
{
    PrepareProbe *probe = ctx;
    u32 *token = malloc(sizeof(*token));

    (void)transport;
    (void)err;
    (void)errsz;
    YEW_ASSERT_NOT_NULL(token);
    *token = ++probe->next_token;
    *endpoint = token;
    probe->prepared++;
    (void)snprintf(probe->last_url, sizeof(probe->last_url), "%s", url_text);
    if (strcmp(url->host, "127.0.0.1") == 0)
        url->loopback = true;
    return true;
}

static void release_endpoint(void *ctx, void *endpoint)
{
    PrepareProbe *probe = ctx;

    probe->released++;
    free(endpoint);
}

void test_ai_registry_owns_runtime_values_and_key_argv(void)
{
    AiBackendRegistry registry;
    PrepareProbe probe = {0};
    FlStr *name = test_string("work");
    FlMap *config = minimal_config("anthropic",
                                   "https://api.anthropic.com/v1/messages",
                                   "claude-test");
    const AiBackendEntry *entry;
    char err[256];

    test_map_put(config, "transport", test_strv("curl"));
    test_map_put(config, "key_cmd",
                 FL_OBJ_V(FL_LIST, test_list("pass", "show", "ai/work")));
    test_map_put(config, "max_tokens", FL_INT_V(512));
    test_map_put(config, "temperature", FL_FLOAT_V(0.25));
    test_map_put(config, "stream", FL_BOOL_V(false));
    test_map_put(config, "fim", FL_BOOL_V(true));

    yew_ai_registry_init(&registry, prepare_endpoint, release_endpoint, &probe);
    YEW_ASSERT(yew_ai_registry_put(&registry, name, config, err, sizeof(err)));
    (void)memset(name->b, 'x', name->len);
    (void)memset(((FlStr *)config->ent[0].v.as.o)->b, 'x',
                 ((FlStr *)config->ent[0].v.as.o)->len);
    free(name);
    test_value_drop(FL_OBJ_V(FL_MAP, config));

    entry = yew_ai_registry_find(&registry, "work");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_STR(entry->backend.name, "work");
    YEW_ASSERT_EQ_U64(entry->backend.kind, YEW_AI_ANTHROPIC);
    YEW_ASSERT_EQ_U64(entry->backend.transport, YEW_AI_TR_CURL);
    YEW_ASSERT_EQ_STR(entry->url_text,
                      "https://api.anthropic.com/v1/messages");
    YEW_ASSERT_EQ_STR(entry->backend.url.host, "api.anthropic.com");
    YEW_ASSERT_EQ_U64(entry->backend.url.port, 443U);
    YEW_ASSERT_EQ_STR(entry->backend.url.path, "/v1/messages");
    YEW_ASSERT_EQ_STR(entry->backend.model, "claude-test");
    YEW_ASSERT_EQ_STR(entry->backend.key_cmd[0], "pass");
    YEW_ASSERT_EQ_STR(entry->backend.key_cmd[2], "ai/work");
    YEW_ASSERT_NULL(entry->backend.key_cmd[3]);
    YEW_ASSERT_EQ_I64(entry->backend.max_tokens, 512);
    YEW_ASSERT(entry->backend.temperature == 0.25);
    YEW_ASSERT(!entry->backend.stream);
    YEW_ASSERT(entry->backend.fim);
    YEW_ASSERT_EQ_U64(probe.prepared, 1U);

    yew_ai_registry_drop(&registry);
    YEW_ASSERT_EQ_U64(probe.released, 1U);
}

void test_ai_registry_replacement_keeps_order_and_resets_cooldown(void)
{
    AiBackendRegistry registry;
    PrepareProbe probe = {0};
    FlStr *one = test_string("one");
    FlStr *two = test_string("two");
    FlMap *a = minimal_config("ollama", "http://127.0.0.1:11434", "a");
    FlMap *b = minimal_config("openai", "http://127.0.0.1:8080", "b");
    FlMap *replacement = minimal_config("ollama",
                                        "http://127.0.0.1:11434", "new");
    AiBackendEntry *mutable;
    char err[256];

    yew_ai_registry_init(&registry, prepare_endpoint, release_endpoint, &probe);
    YEW_ASSERT(yew_ai_registry_put(&registry, one, a, err, sizeof(err)));
    YEW_ASSERT(yew_ai_registry_put(&registry, two, b, err, sizeof(err)));
    mutable = yew_ai_registry_find_mut(&registry, "one");
    YEW_ASSERT_NOT_NULL(mutable);
    mutable->cooldown.until_ms = 9000;
    mutable->cooldown.consecutive = 4U;
    YEW_ASSERT(yew_ai_registry_put(&registry, one, replacement,
                                   err, sizeof(err)));

    YEW_ASSERT_EQ_U64(yew_ai_registry_count(&registry), 2U);
    YEW_ASSERT_EQ_STR(yew_ai_registry_at(&registry, 0U)->backend.name, "one");
    YEW_ASSERT_EQ_STR(yew_ai_registry_at(&registry, 0U)->backend.model, "new");
    YEW_ASSERT_EQ_I64(yew_ai_registry_at(&registry, 0U)->cooldown.until_ms, 0);
    YEW_ASSERT_EQ_U64(yew_ai_registry_at(&registry, 0U)->cooldown.consecutive,
                      0U);
    YEW_ASSERT_EQ_STR(yew_ai_registry_at(&registry, 1U)->backend.name, "two");
    YEW_ASSERT_EQ_U64(probe.released, 1U);

    free(one);
    free(two);
    test_value_drop(FL_OBJ_V(FL_MAP, a));
    test_value_drop(FL_OBJ_V(FL_MAP, b));
    test_value_drop(FL_OBJ_V(FL_MAP, replacement));
    yew_ai_registry_drop(&registry);
    YEW_ASSERT_EQ_U64(probe.released, 3U);
}

void test_ai_registry_defaults_and_defensive_errors(void)
{
    AiBackendRegistry registry;
    FlStr *name = test_string("local");
    FlMap *config = minimal_config("ollama", "http://127.0.0.1:11434", "qwen");
    const AiBackendEntry *entry;
    char err[256];

    yew_ai_registry_init(&registry, NULL, NULL, NULL);
    YEW_ASSERT(yew_ai_registry_put(&registry, name, config, err, sizeof(err)));
    entry = yew_ai_registry_find(&registry, "local");
    YEW_ASSERT_EQ_U64(entry->backend.transport, YEW_AI_TR_HTTP);
    YEW_ASSERT_EQ_I64(entry->backend.max_tokens, 256);
    YEW_ASSERT(entry->backend.temperature == 0.1);
    YEW_ASSERT(entry->backend.stream);
    YEW_ASSERT(!entry->backend.fim);

    {
        FlMap *bad = minimal_config("ollama", "http://u:p@host/", "x");

        YEW_ASSERT(!yew_ai_registry_put(&registry, name, bad, err, sizeof(err)));
        YEW_ASSERT(strstr(err, "userinfo") != NULL);
        test_value_drop(FL_OBJ_V(FL_MAP, bad));
    }
    {
        FlMap *bad = minimal_config("openai", "https://example.com", "x");

        test_map_put(bad, "transport", test_strv("http"));
        YEW_ASSERT(!yew_ai_registry_put(&registry, name, bad, err, sizeof(err)));
        YEW_ASSERT(strstr(err, "https backends run through curl") != NULL);
        test_value_drop(FL_OBJ_V(FL_MAP, bad));
    }
    {
        FlMap *bad = minimal_config("anthropic", "https://example.com", "x");

        test_map_put(bad, "key_env", test_strv("AI_KEY"));
        test_map_put(bad, "key_cmd",
                     FL_OBJ_V(FL_LIST, test_list("pass", NULL, NULL)));
        YEW_ASSERT(!yew_ai_registry_put(&registry, name, bad, err, sizeof(err)));
        YEW_ASSERT(strstr(err, "both key_env and key_cmd") != NULL);
        test_value_drop(FL_OBJ_V(FL_MAP, bad));
    }
    {
        FlMap *bad = minimal_config("anthropic", "https://example.com", "x");

        test_map_put(bad, "secret", test_strv("not-even-key-shaped"));
        YEW_ASSERT(!yew_ai_registry_put(&registry, name, bad, err, sizeof(err)));
        YEW_ASSERT(strstr(err, "may not hold a literal API key") != NULL);
        test_value_drop(FL_OBJ_V(FL_MAP, bad));
    }

    YEW_ASSERT_EQ_STR(yew_ai_registry_find(&registry, "local")->backend.model,
                      "qwen");
    free(name);
    test_value_drop(FL_OBJ_V(FL_MAP, config));
    yew_ai_registry_drop(&registry);
}

void test_ai_registry_reload_is_transactional_and_deterministic(void)
{
    AiBackendRegistry left;
    AiBackendRegistry right;
    FlMap *outer = test_map();
    FlMap *bad = test_map();
    const AiBackendEntry *a;
    const AiBackendEntry *b;
    u32 i;
    char err[256];

    test_map_put(outer, "zeta",
                 FL_OBJ_V(FL_MAP, minimal_config("ollama",
                                                "http://127.0.0.1:11434",
                                                "z")));
    test_map_put(outer, "alpha",
                 FL_OBJ_V(FL_MAP, minimal_config("openai",
                                                "https://example.com/v1",
                                                "a")));
    yew_ai_registry_init(&left, NULL, NULL, NULL);
    yew_ai_registry_init(&right, NULL, NULL, NULL);
    YEW_ASSERT(yew_ai_registry_reload(&left, outer, err, sizeof(err)));
    YEW_ASSERT(yew_ai_registry_reload(&right, outer, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(yew_ai_registry_count(&left), 2U);
    for (i = 0U; i < yew_ai_registry_count(&left); i++) {
        a = yew_ai_registry_at(&left, i);
        b = yew_ai_registry_at(&right, i);
        YEW_ASSERT_EQ_STR(a->backend.name, b->backend.name);
        YEW_ASSERT_EQ_STR(a->url_text, b->url_text);
        YEW_ASSERT_EQ_STR(a->backend.model, b->backend.model);
    }
    YEW_ASSERT_EQ_STR(yew_ai_registry_at(&left, 0U)->backend.name, "zeta");
    YEW_ASSERT_EQ_STR(yew_ai_registry_at(&left, 1U)->backend.name, "alpha");

    test_map_put(bad, "broken", FL_INT_V(1));
    YEW_ASSERT(!yew_ai_registry_reload(&left, bad, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(yew_ai_registry_count(&left), 2U);
    YEW_ASSERT_EQ_STR(yew_ai_registry_at(&left, 0U)->backend.name, "zeta");

    test_value_drop(FL_OBJ_V(FL_MAP, outer));
    test_value_drop(FL_OBJ_V(FL_MAP, bad));
    yew_ai_registry_drop(&left);
    yew_ai_registry_drop(&right);
}
