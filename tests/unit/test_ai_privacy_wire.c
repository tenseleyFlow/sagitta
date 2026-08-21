#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/ai/http.h"
#include "mod/ai/prompt.h"
#include "mod/lsp/json.h"
#include "util/arena.h"
#include "util/buf.h"

/* Pinned to docs/ai-privacy.md, "What is sent, exactly".  The JSON keys
 * below are provider protocol structure; the sentinel assertions inventory
 * the distinct yew data fields promised by that table.  Keeping both checks
 * means a new provider field cannot hide inside an otherwise valid body. */
static const char wire_prefix[] = "YEW_WIRE_PREFIX_7F91";
static const char wire_suffix[] = "YEW_WIRE_SUFFIX_2C48";
static const char wire_path[] = "src/wire_fixture.c";
static const char wire_language[] = "wirelang";
static const char wire_model[] = "wire-model-5D63";
static const char wire_api_key[] = "YEW_WIRE_API_KEY_4E72";

enum WireDocField {
    WIRE_DOC_PREFIX = 1U << 0,
    WIRE_DOC_SUFFIX = 1U << 1,
    WIRE_DOC_PATH = 1U << 2,
    WIRE_DOC_LANGUAGE = 1U << 3,
    WIRE_DOC_GENERATION = 1U << 4,
    WIRE_DOC_USER_AGENT = 1U << 5,
    WIRE_DOC_API_KEY = 1U << 6,
    WIRE_DOC_ANYTHING_ELSE = 1U << 7
};

typedef struct WireDocInventory {
    u32 rows;
    u32 local;
    u32 cloud;
} WireDocInventory;

static bool wire_cell_eq(const char *lo, const char *hi, const char *text)
{
    size_t len;

    while (lo < hi && (*lo == ' ' || *lo == '\t' || *lo == '`'))
        lo++;
    while (hi > lo && (hi[-1] == ' ' || hi[-1] == '\t' ||
                       hi[-1] == '`' || hi[-1] == '\r' ||
                       hi[-1] == '\n'))
        hi--;
    len = (size_t)(hi - lo);
    return strlen(text) == len && memcmp(lo, text, len) == 0;
}

static u32 wire_doc_field(const char *lo, const char *hi)
{
    if (wire_cell_eq(lo, hi, "prefix"))
        return WIRE_DOC_PREFIX;
    if (wire_cell_eq(lo, hi, "suffix"))
        return WIRE_DOC_SUFFIX;
    if (wire_cell_eq(lo, hi, "path"))
        return WIRE_DOC_PATH;
    if (wire_cell_eq(lo, hi, "language"))
        return WIRE_DOC_LANGUAGE;
    if (wire_cell_eq(lo, hi, "generation settings"))
        return WIRE_DOC_GENERATION;
    if (wire_cell_eq(lo, hi, "User-Agent"))
        return WIRE_DOC_USER_AGENT;
    if (wire_cell_eq(lo, hi, "API key"))
        return WIRE_DOC_API_KEY;
    if (wire_cell_eq(lo, hi, "anything else"))
        return WIRE_DOC_ANYTHING_ELSE;
    return 0U;
}

static bool wire_doc_yes(const char *lo, const char *hi)
{
    YEW_ASSERT(wire_cell_eq(lo, hi, "yes") ||
               wire_cell_eq(lo, hi, "no") ||
               wire_cell_eq(lo, hi, "n/a"));
    return wire_cell_eq(lo, hi, "yes");
}

static WireDocInventory wire_doc_inventory(void)
{
    static const u32 all_rows = (1U << 8) - 1U;
    FILE *file = fopen("docs/ai-privacy.md", "rb");
    WireDocInventory inventory = {0U, 0U, 0U};
    char line[4096];
    bool in_table = false;

    YEW_ASSERT_NOT_NULL(file);
    while (fgets(line, sizeof(line), file) != NULL) {
        const char *field_end;
        const char *value_end;
        const char *local_end;
        const char *cloud_end;
        u32 field;

        if (!in_table) {
            if (strcmp(line, "## What is sent, exactly\n") == 0)
                in_table = true;
            continue;
        }
        if (strncmp(line, "## ", 3U) == 0)
            break;
        if (line[0] != '|')
            continue;
        field_end = strchr(line + 1, '|');
        YEW_ASSERT_NOT_NULL(field_end);
        value_end = strchr(field_end + 1, '|');
        YEW_ASSERT_NOT_NULL(value_end);
        local_end = strchr(value_end + 1, '|');
        YEW_ASSERT_NOT_NULL(local_end);
        cloud_end = strchr(local_end + 1, '|');
        YEW_ASSERT_NOT_NULL(cloud_end);
        field = wire_doc_field(line + 1, field_end);
        if (field == 0U) {
            YEW_ASSERT(wire_cell_eq(line + 1, field_end, "Field") ||
                       line[1] == '-');
            continue;
        }
        YEW_ASSERT((inventory.rows & field) == 0U);
        inventory.rows |= field;
        if (wire_doc_yes(value_end + 1, local_end))
            inventory.local |= field;
        if (wire_doc_yes(local_end + 1, cloud_end))
            inventory.cloud |= field;
    }
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    YEW_ASSERT(in_table);
    YEW_ASSERT_EQ_U64(inventory.rows, all_rows);
    return inventory;
}

static u32 wire_occurrences(const u8 *bytes, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;
    u32 count = 0U;

    if (nlen == 0U || nlen > len)
        return 0U;
    for (i = 0U; i <= len - nlen; i++)
        if (memcmp(bytes + i, needle, nlen) == 0)
            count++;
    return count;
}

static AiBackend wire_backend(AiKind kind)
{
    AiBackend backend;

    (void)memset(&backend, 0, sizeof(backend));
    backend.kind = (u8)kind;
    backend.model = wire_model;
    backend.max_tokens = 173;
    backend.temperature = 0.375;
    backend.stream = true;
    /* Exercise Ollama's FIM adapter: this is the default local-preset shape,
     * so its privacy inventory must not silently differ from chat adapters. */
    backend.fim = kind == YEW_AI_OLLAMA;
    backend.fim_policy = backend.fim ? YEW_AI_FIM_ON : YEW_AI_FIM_OFF;
    return backend;
}

static AiCtx wire_context(void)
{
    AiCtx context;

    (void)memset(&context, 0, sizeof(context));
    context.prefix = (const u8 *)wire_prefix;
    context.plen = (u32)(sizeof(wire_prefix) - 1U);
    context.suffix = (const u8 *)wire_suffix;
    context.slen = (u32)(sizeof(wire_suffix) - 1U);
    context.path = wire_path;
    context.lang = wire_language;
    context.line_1based = 19U;
    return context;
}

static JsonValue *wire_body(const AiBackend *backend, const AiCtx *context,
                            Arena *arena, Bytebuf *bytes)
{
    JsonW writer;
    JsonErr error;
    JsonValue *root;

    bytebuf_init(bytes);
    yew_jsonw_init(&writer, bytes);
    yew_ai_prompt_build(&writer, backend, context);
    root = yew_json_parse(arena, bytes->data, bytes->len, &error);
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_U64(root->kind, YEW_JS_OBJ);
    return root;
}

static void wire_exact_keys(const JsonValue *root,
                            const char *const *expected, u32 nexpected)
{
    u32 i;

    YEW_ASSERT_EQ_U64(root->obj.n, nexpected);
    for (i = 0U; i < nexpected; i++) {
        const JsonMember *member = &root->obj.m[i];

        YEW_ASSERT_EQ_U64(member->klen, strlen(expected[i]));
        YEW_ASSERT_EQ_MEM(member->key, expected[i], member->klen);
    }
}

static void wire_generation_fields(const JsonValue *root, AiKind kind)
{
    static const char *const fim_stops[] = {"\n\n", "\n}", "```"};
    static const char *const chat_stops[] = {
        "```", "<|before|>", "<|after|>"
    };
    const char *const *expected;
    const JsonValue *options;
    const JsonValue *stops;
    u32 i;

    YEW_ASSERT(yew_json_streq(yew_json_get(root, "model"), wire_model));
    if (kind == YEW_AI_OLLAMA) {
        options = yew_json_get(root, "options");
        YEW_ASSERT_NOT_NULL(options);
        YEW_ASSERT_EQ_I64(yew_json_int(yew_json_get(options, "num_predict"),
                                       -1), 173);
        YEW_ASSERT(yew_json_real(yew_json_get(options, "temperature"),
                                 -1.0) == 0.375);
        stops = yew_json_get(options, "stop");
    } else {
        YEW_ASSERT_EQ_I64(yew_json_int(yew_json_get(root, "max_tokens"),
                                       -1), 173);
        YEW_ASSERT(yew_json_real(yew_json_get(root, "temperature"),
                                 -1.0) == 0.375);
        stops = yew_json_get(root, kind == YEW_AI_ANTHROPIC ?
                            "stop_sequences" : "stop");
    }
    YEW_ASSERT_NOT_NULL(stops);
    YEW_ASSERT_EQ_U64(stops->kind, YEW_JS_ARR);
    expected = kind == YEW_AI_OLLAMA ? fim_stops : chat_stops;
    YEW_ASSERT_EQ_U64(yew_json_len(stops), 3U);
    for (i = 0U; i < 3U; i++)
        YEW_ASSERT(yew_json_streq(yew_json_at(stops, i), expected[i]));
}

void test_ai_privacy_wire_body_fields_match_documented_sent_rows(void)
{
    static const u32 body_fields = WIRE_DOC_PREFIX | WIRE_DOC_SUFFIX |
                                   WIRE_DOC_PATH | WIRE_DOC_LANGUAGE |
                                   WIRE_DOC_GENERATION;
    static const char *const ollama_keys[] = {
        "model", "prompt", "suffix", "system", "stream", "raw", "options"
    };
    static const char *const openai_keys[] = {
        "model", "messages", "stream", "max_tokens", "temperature", "stop"
    };
    static const char *const anthropic_keys[] = {
        "model", "system", "messages", "max_tokens", "stream",
        "temperature", "stop_sequences"
    };
    static const char *const forbidden[] = {
        "/home/private-user/project/src/wire_fixture.c",
        "private-user", "private-host", "private-os", "private-branch",
        "private-remote", "private-stats", "private-telemetry"
    };
    WireDocInventory documented = wire_doc_inventory();
    u32 kind;

    for (kind = 0U; kind < (u32)YEW_AI_NKIND; kind++) {
        AiBackend backend = wire_backend((AiKind)kind);
        AiCtx context = wire_context();
        Arena arena;
        Bytebuf body;
        JsonValue *root;
        u32 observed = 0U;
        u32 i;

        arena_init(&arena);
        root = wire_body(&backend, &context, &arena, &body);
        if (kind == (u32)YEW_AI_OLLAMA)
            wire_exact_keys(root, ollama_keys, YEW_ARRAY_LEN(ollama_keys));
        else if (kind == (u32)YEW_AI_OPENAI)
            wire_exact_keys(root, openai_keys, YEW_ARRAY_LEN(openai_keys));
        else
            wire_exact_keys(root, anthropic_keys,
                            YEW_ARRAY_LEN(anthropic_keys));

        YEW_ASSERT_EQ_U64(wire_occurrences(body.data, body.len, wire_prefix),
                          1U);
        observed |= WIRE_DOC_PREFIX;
        YEW_ASSERT_EQ_U64(wire_occurrences(body.data, body.len, wire_suffix),
                          1U);
        observed |= WIRE_DOC_SUFFIX;
        YEW_ASSERT_EQ_U64(wire_occurrences(body.data, body.len, wire_path),
                          1U);
        observed |= WIRE_DOC_PATH;
        YEW_ASSERT_EQ_U64(wire_occurrences(body.data, body.len,
                                           wire_language), 1U);
        observed |= WIRE_DOC_LANGUAGE;
        YEW_ASSERT_EQ_U64(wire_occurrences(body.data, body.len, wire_model),
                          1U);
        wire_generation_fields(root, (AiKind)kind);
        observed |= WIRE_DOC_GENERATION;
        YEW_ASSERT_EQ_U64(observed,
                          (kind == (u32)YEW_AI_OLLAMA ? documented.local :
                                                       documented.cloud) &
                              body_fields);
        for (i = 0U; i < YEW_ARRAY_LEN(forbidden); i++)
            YEW_ASSERT_EQ_U64(wire_occurrences(body.data, body.len,
                                               forbidden[i]), 0U);
        bytebuf_free(&body);
        arena_free_all(&arena);
    }
}

static void wire_request(AiKind kind, Bytebuf *request)
{
    static const u8 body[] = "{}";
    HttpHdr headers[2];
    HttpUrl url;
    HttpReq http;
    char auth[sizeof("Bearer ") + sizeof(wire_api_key)];
    char error[128];
    u32 nheaders = 0U;

    url.host = kind == YEW_AI_OLLAMA ? "localhost" :
               kind == YEW_AI_OPENAI ? "api.openai.com" :
                                       "api.anthropic.com";
    url.port = kind == YEW_AI_OLLAMA ? 11434U : 443U;
    url.path = kind == YEW_AI_OLLAMA ? "/api/generate" :
               kind == YEW_AI_OPENAI ? "/v1/chat/completions" :
                                       "/v1/messages";
    url.loopback = kind == YEW_AI_OLLAMA;
    if (kind == YEW_AI_OPENAI) {
        (void)memcpy(auth, "Bearer ", sizeof("Bearer ") - 1U);
        (void)memcpy(auth + sizeof("Bearer ") - 1U, wire_api_key,
                     sizeof(wire_api_key));
        headers[nheaders++] = (HttpHdr){"authorization", auth};
    } else if (kind == YEW_AI_ANTHROPIC) {
        headers[nheaders++] =
            (HttpHdr){"anthropic-version", "2023-06-01"};
        headers[nheaders++] = (HttpHdr){"x-api-key", wire_api_key};
    }
    http.method = "POST";
    http.path = url.path;
    http.hdrs = headers;
    http.nhdr = nheaders;
    http.body = body;
    http.blen = sizeof(body) - 1U;
    http.keepalive = true;
    bytebuf_init(request);
    YEW_ASSERT(yew_http_req_build(request, &url, &http, error,
                                  sizeof(error)));
}

void test_ai_privacy_wire_headers_match_documented_sent_rows(void)
{
    static const u32 header_fields = WIRE_DOC_USER_AGENT |
                                     WIRE_DOC_API_KEY;
    static const char *const expected[] = {
        "POST /api/generate HTTP/1.1\r\n"
        "Host: localhost:11434\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n\r\n{}",
        "POST /v1/chat/completions HTTP/1.1\r\n"
        "Host: api.openai.com:443\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n"
        "authorization: Bearer YEW_WIRE_API_KEY_4E72\r\n\r\n{}",
        "POST /v1/messages HTTP/1.1\r\n"
        "Host: api.anthropic.com:443\r\n"
        "User-Agent: yew/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "x-api-key: YEW_WIRE_API_KEY_4E72\r\n\r\n{}"
    };
    WireDocInventory documented = wire_doc_inventory();
    u32 kind;

    for (kind = 0U; kind < (u32)YEW_AI_NKIND; kind++) {
        Bytebuf request;
        u32 observed = 0U;

        wire_request((AiKind)kind, &request);
        YEW_ASSERT_EQ_U64(request.len, strlen(expected[kind]));
        YEW_ASSERT_EQ_MEM(request.data, expected[kind], request.len);
        YEW_ASSERT_EQ_U64(wire_occurrences(request.data, request.len,
                                           "User-Agent: yew/1.0.0\r\n"),
                          1U);
        observed |= WIRE_DOC_USER_AGENT;
        YEW_ASSERT_EQ_U64(wire_occurrences(request.data, request.len,
                                           wire_api_key),
                          kind == (u32)YEW_AI_OLLAMA ? 0U : 1U);
        YEW_ASSERT_EQ_U64(wire_occurrences(request.data, request.len,
                                           "authorization:"),
                          kind == (u32)YEW_AI_OPENAI ? 1U : 0U);
        YEW_ASSERT_EQ_U64(wire_occurrences(request.data, request.len,
                                           "x-api-key:"),
                          kind == (u32)YEW_AI_ANTHROPIC ? 1U : 0U);
        if (kind != (u32)YEW_AI_OLLAMA)
            observed |= WIRE_DOC_API_KEY;
        YEW_ASSERT_EQ_U64(observed,
                          (kind == (u32)YEW_AI_OLLAMA ? documented.local :
                                                       documented.cloud) &
                              header_fields);
        bytebuf_free(&request);
    }
}
