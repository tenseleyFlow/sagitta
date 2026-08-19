#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/ai/backend.h"
#include "util/arena.h"
#include "util/buf.h"

static AiText text(const char *value)
{
    AiText out;

    out.bytes = (const u8 *)value;
    out.len = (u32)strlen(value);
    return out;
}

static AiBackend backend(AiKind kind, bool fim)
{
    AiBackend out;

    memset(&out, 0, sizeof(out));
    out.name = "fixture";
    out.kind = (u8)kind;
    out.transport = YEW_AI_TR_HTTP;
    out.model = kind == YEW_AI_OLLAMA ? "qwen2.5-coder:7b" :
                kind == YEW_AI_OPENAI ? "gpt-fixture" : "claude-fixture";
    out.max_tokens = 128;
    out.temperature = 0.1;
    out.stream = true;
    out.fim = fim;
    return out;
}

static AiPrompt prompt(void)
{
    static const AiText stops[] = {
        {(const u8 *)"\n\n", 2U},
        {(const u8 *)"\n}", 2U},
        {(const u8 *)"```", 3U}
    };
    AiPrompt out;

    memset(&out, 0, sizeof(out));
    out.system = text("complete code only");
    out.prefix = text("int ");
    out.suffix = text(";\n");
    out.stops = stops;
    out.nstops = YEW_ARRAY_LEN(stops);
    return out;
}

static void assert_build(const AiAdapter *adapter, const AiBackend *b,
                         const AiPrompt *p, const char *expected)
{
    Bytebuf out;
    JsonW writer;

    bytebuf_init(&out);
    yew_jsonw_init(&writer, &out);
    adapter->build(&writer, b, p);
    YEW_ASSERT_EQ_U64(out.len, strlen(expected));
    YEW_ASSERT_EQ_MEM(out.data, expected, out.len);
    bytebuf_free(&out);
}

void test_ai_backend_paths_and_modes(void)
{
    AiBackend ollama = backend(YEW_AI_OLLAMA, true);

    YEW_ASSERT_EQ_U64(YEW_AI_NKIND, 3U);
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OLLAMA].kind_name, "ollama");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OPENAI].kind_name, "openai");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_ANTHROPIC].kind_name,
                      "anthropic");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OLLAMA].path_gen(&ollama),
                      "/api/generate");
    ollama.fim = false;
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OLLAMA].path_gen(&ollama),
                      "/api/chat");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OLLAMA].path_models(&ollama),
                      "/api/tags");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OPENAI].path_gen(&ollama),
                      "/v1/chat/completions");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_OPENAI].path_models(&ollama),
                      "/v1/models");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_ANTHROPIC].path_gen(&ollama),
                      "/v1/messages");
    YEW_ASSERT_EQ_STR(yew_ai_adapters[YEW_AI_ANTHROPIC].path_models(&ollama),
                      "/v1/models");
    YEW_ASSERT_EQ_U64(yew_ai_adapters[YEW_AI_OLLAMA].stream_mode,
                      YEW_AISTREAM_NDJSON);
    YEW_ASSERT_EQ_U64(yew_ai_adapters[YEW_AI_OPENAI].stream_mode,
                      YEW_AISTREAM_SSE);
    YEW_ASSERT_EQ_U64(yew_ai_adapters[YEW_AI_ANTHROPIC].stream_mode,
                      YEW_AISTREAM_SSE);
}

void test_ai_backend_request_bodies(void)
{
    AiPrompt p = prompt();
    AiBackend b = backend(YEW_AI_OLLAMA, true);

    p.system.len = 0U;
    assert_build(&yew_ai_adapters[YEW_AI_OLLAMA], &b, &p,
        "{\"model\":\"qwen2.5-coder:7b\",\"prompt\":\"int \","
        "\"suffix\":\";\\n\",\"stream\":true,\"options\":{"
        "\"num_predict\":128,\"temperature\":0.10000000000000001,"
        "\"stop\":[\"\\n\\n\",\"\\n}\",\"```\"]}}");

    p = prompt();
    b.fim = false;
    assert_build(&yew_ai_adapters[YEW_AI_OLLAMA], &b, &p,
        "{\"model\":\"qwen2.5-coder:7b\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"complete code only\"},"
        "{\"role\":\"user\",\"content\":\"int \"}],\"stream\":true,"
        "\"options\":{\"num_predict\":128,"
        "\"temperature\":0.10000000000000001,"
        "\"stop\":[\"\\n\\n\",\"\\n}\",\"```\"]}}");

    b = backend(YEW_AI_OPENAI, false);
    assert_build(&yew_ai_adapters[YEW_AI_OPENAI], &b, &p,
        "{\"model\":\"gpt-fixture\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"complete code only\"},"
        "{\"role\":\"user\",\"content\":\"int \"}],\"stream\":true,"
        "\"max_tokens\":128,\"temperature\":0.10000000000000001,"
        "\"stop\":[\"\\n\\n\",\"\\n}\",\"```\"]}");

    b = backend(YEW_AI_ANTHROPIC, false);
    assert_build(&yew_ai_adapters[YEW_AI_ANTHROPIC], &b, &p,
        "{\"model\":\"claude-fixture\","
        "\"system\":\"complete code only\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"int \"}],"
        "\"max_tokens\":128,\"stream\":true,"
        "\"temperature\":0.10000000000000001,"
        "\"stop_sequences\":[\"\\n\\n\",\"\\n}\",\"```\"]}");
}

static Bytebuf read_fixture(const char *path)
{
    Bytebuf out;
    FILE *file;
    u8 chunk[4096];
    size_t n;

    bytebuf_init(&out);
    file = fopen(path, "rb");
    YEW_ASSERT_NOT_NULL(file);
    while ((n = fread(chunk, 1U, sizeof(chunk), file)) != 0U)
        bytebuf_append(&out, chunk, n);
    YEW_ASSERT(!ferror(file));
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    return out;
}

typedef struct {
    const AiAdapter *adapter;
    AiAdapterState state;
    Bytebuf tokens;
    AiErrKind inband_error;
    u32 events;
    u32 deltas;
    u32 terminators;
} FixtureResult;

static void fixture_event(void *ctx, const AiEvent *event)
{
    FixtureResult *result = ctx;
    Arena arena;
    JsonErr error;
    JsonValue *value;
    AiAdapterEvent consumed;
    AiErrKind classified;

    result->events++;
    if (event->terminator) {
        result->terminators++;
    }
    if (event->dlen == 0U) {
        result->adapter->consume(&result->state, event, NULL, &consumed);
        return;
    }
    arena_init(&arena);
    value = yew_json_parse(&arena, event->data, event->dlen, &error);
    YEW_ASSERT_NOT_NULL(value);
    result->adapter->consume(&result->state, event, value, &consumed);
    if (consumed.has_text) {
        result->deltas++;
        bytebuf_append(&result->tokens, consumed.text, consumed.len);
    }
    classified = result->adapter->classify(200U, value);
    if (classified != YEW_AI_OK)
        result->inband_error = classified;
    arena_free_all(&arena);
}

static FixtureResult run_fixture(AiKind kind, const char *path)
{
    FixtureResult result;
    Bytebuf input = read_fixture(path);
    AiStream stream;

    memset(&result, 0, sizeof(result));
    result.adapter = &yew_ai_adapters[kind];
    yew_ai_adapter_state_init(&result.state);
    bytebuf_init(&result.tokens);
    yew_ai_stream_init(&stream, result.adapter->stream_mode);
    yew_ai_stream_feed(&stream, input.data, input.len, true,
                       fixture_event, &result);
    YEW_ASSERT_EQ_STR(stream.err, "");
    yew_ai_stream_free(&stream);
    bytebuf_free(&input);
    return result;
}

static void assert_fixture_tokens(FixtureResult *result)
{
    static const char expected[] = "int answer = 42;";

    YEW_ASSERT_EQ_U64(result->tokens.len, strlen(expected));
    YEW_ASSERT_EQ_MEM(result->tokens.data, expected, strlen(expected));
    bytebuf_free(&result->tokens);
}

static void fixture_result_free(FixtureResult *result)
{
    yew_ai_adapter_state_free(&result->state);
}

void test_ai_backend_recorded_stream_deltas(void)
{
    FixtureResult result;

    result = run_fixture(YEW_AI_OLLAMA,
                         "tests/unit/fixtures/ai/ollama.ndjson");
    assert_fixture_tokens(&result);
    YEW_ASSERT_EQ_U64(result.events, 3U);
    YEW_ASSERT_EQ_U64(result.deltas, 2U);
    YEW_ASSERT_EQ_U64(result.terminators, 1U);
    YEW_ASSERT_EQ_U64(result.inband_error, YEW_AI_OK);
    fixture_result_free(&result);

    result = run_fixture(YEW_AI_OPENAI,
                         "tests/unit/fixtures/ai/openai.sse");
    assert_fixture_tokens(&result);
    YEW_ASSERT_EQ_U64(result.events, 5U);
    YEW_ASSERT_EQ_U64(result.deltas, 2U);
    YEW_ASSERT_EQ_U64(result.terminators, 1U);
    YEW_ASSERT_EQ_U64(result.inband_error, YEW_AI_OK);
    fixture_result_free(&result);

    result = run_fixture(YEW_AI_ANTHROPIC,
                         "tests/unit/fixtures/ai/anthropic_lifecycle.sse");
    assert_fixture_tokens(&result);
    YEW_ASSERT_EQ_U64(result.events, 11U);
    YEW_ASSERT_EQ_U64(result.deltas, 2U);
    YEW_ASSERT_EQ_U64(result.terminators, 0U);
    YEW_ASSERT_EQ_U64(result.inband_error, YEW_AI_OK);
    YEW_ASSERT(result.state.terminal);
    YEW_ASSERT_EQ_U64(result.state.event_type_mismatches, 1U);
    YEW_ASSERT_EQ_I64(result.state.input_tokens, 12);
    YEW_ASSERT_EQ_I64(result.state.output_tokens, 6);
    YEW_ASSERT_EQ_STR(result.state.message_id, "msg_lifecycle");
    YEW_ASSERT_EQ_STR(result.state.model, "claude-fixture");
    YEW_ASSERT_EQ_STR(result.state.stop_reason, "end_turn");
    YEW_ASSERT_EQ_STR(result.state.stop_sequence, "");
    YEW_ASSERT_EQ_U64(result.state.nblocks, 0U);
    fixture_result_free(&result);

    result = run_fixture(YEW_AI_ANTHROPIC,
                         "tests/unit/fixtures/ai/anthropic.sse");
    YEW_ASSERT_EQ_U64(result.inband_error, YEW_AI_ERR_RATELIMIT);
    bytebuf_free(&result.tokens);
    fixture_result_free(&result);
}

static JsonValue *parse(Arena *arena, const char *json)
{
    JsonErr error;
    JsonValue *value = yew_json_parse(arena, (const u8 *)json,
                                      (u64)strlen(json), &error);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

void test_ai_backend_error_classification(void)
{
    static const struct {
        AiKind kind;
        u16 status;
        const char *body;
        AiErrKind want;
    } cases[] = {
        {YEW_AI_OLLAMA, 200U, "{}", YEW_AI_OK},
        {YEW_AI_OLLAMA, 404U,
         "{\"error\":\"model \\\"missing\\\" not found\"}",
         YEW_AI_ERR_MODEL},
        {YEW_AI_OLLAMA, 500U,
         "{\"error\":\"model missing not found\"}", YEW_AI_ERR_PROTOCOL},
        {YEW_AI_OPENAI, 401U,
         "{\"error\":{\"code\":\"model_not_found\"}}", YEW_AI_ERR_AUTH},
        {YEW_AI_OPENAI, 404U,
         "{\"error\":{\"message\":\"That model is not available\"}}",
         YEW_AI_ERR_MODEL},
        {YEW_AI_OPENAI, 200U,
         "{\"error\":{\"type\":\"rate_limit_error\"}}",
         YEW_AI_ERR_RATELIMIT},
        {YEW_AI_OPENAI, 200U,
         "{\"error\":{\"type\":\"authentication_error\"}}",
         YEW_AI_ERR_AUTH},
        {YEW_AI_ANTHROPIC, 200U,
         "{\"type\":\"error\",\"error\":{"
         "\"type\":\"authentication_error\"}}", YEW_AI_ERR_AUTH},
        {YEW_AI_ANTHROPIC, 200U,
         "{\"type\":\"error\",\"error\":{"
         "\"type\":\"overloaded_error\"}}", YEW_AI_ERR_RATELIMIT},
        {YEW_AI_ANTHROPIC, 200U,
         "{\"type\":\"error\",\"error\":{"
         "\"type\":\"api_error\"}}", YEW_AI_ERR_PROTOCOL},
        {YEW_AI_ANTHROPIC, 413U, "{}", YEW_AI_ERR_TOO_LARGE},
        {YEW_AI_ANTHROPIC, 429U, "{}", YEW_AI_ERR_RATELIMIT},
        {YEW_AI_ANTHROPIC, 403U, "{}", YEW_AI_ERR_AUTH},
        {YEW_AI_ANTHROPIC, 503U, "{}", YEW_AI_ERR_PROTOCOL},
        {YEW_AI_ANTHROPIC, 529U,
         "{\"type\":\"error\",\"error\":{"
         "\"type\":\"overloaded_error\"}}", YEW_AI_ERR_RATELIMIT}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        Arena arena;
        JsonValue *body;
        AiErrKind got;

        arena_init(&arena);
        body = parse(&arena, cases[i].body);
        got = yew_ai_adapters[cases[i].kind].classify(cases[i].status, body);
        YEW_ASSERT_EQ_U64(got, cases[i].want);
        arena_free_all(&arena);
    }

    {
        static const AiKind kinds[] = {
            YEW_AI_OLLAMA, YEW_AI_OPENAI, YEW_AI_ANTHROPIC
        };

        for (i = 0U; i < YEW_ARRAY_LEN(kinds); i++) {
            Arena arena;
            const JsonValue *empty;
            const u8 *token = (const u8 *)"sentinel";
            u32 len = 99U;

            arena_init(&arena);
            empty = parse(&arena,
                kinds[i] == YEW_AI_OLLAMA ? "{\"response\":\"\"}" :
                kinds[i] == YEW_AI_OPENAI ?
                    "{\"choices\":[{\"delta\":{\"content\":\"\"}}]}" :
                    "{\"type\":\"content_block_delta\",\"delta\":{"
                    "\"type\":\"text_delta\",\"text\":\"\"}}" );
            YEW_ASSERT(!yew_ai_adapters[kinds[i]].delta(empty, &token,
                                                        &len));
            YEW_ASSERT_NULL(token);
            YEW_ASSERT_EQ_U64(len, 0U);
            arena_free_all(&arena);
        }
    }
}

void test_ai_backend_error_messages(void)
{
    static const AiErrKind kinds[] = {
        YEW_AI_ERR_UNREACHABLE, YEW_AI_ERR_AUTH,
        YEW_AI_ERR_RATELIMIT, YEW_AI_ERR_MODEL,
        YEW_AI_ERR_PROTOCOL, YEW_AI_ERR_TIMEOUT, YEW_AI_ERR_TLS,
        YEW_AI_ERR_TOO_LARGE, YEW_AI_ERR_NO_CURL,
        YEW_AI_ERR_CANCELLED
    };
    AiBackend b = backend(YEW_AI_ANTHROPIC, false);
    AiErr errors[YEW_ARRAY_LEN(kinds)];
    size_t i;
    size_t j;

    b.name = "work";
    b.model = "claude-fixture";
    b.key_env = "ANTHROPIC_API_KEY";
    b.url.host = "api.anthropic.com";
    b.url.port = 443U;
    for (i = 0U; i < YEW_ARRAY_LEN(kinds); i++) {
        yew_ai_err_format(&errors[i], kinds[i], &b, 529U, 21000,
                          "first-byte timeout");
        YEW_ASSERT_EQ_U64(errors[i].kind, kinds[i]);
        YEW_ASSERT_EQ_I64(errors[i].retry_ms, 21000);
        if (kinds[i] == YEW_AI_ERR_CANCELLED)
            YEW_ASSERT_EQ_STR(errors[i].msg, "");
        else
            YEW_ASSERT(errors[i].msg[0] != '\0');
    }
    for (i = 0U; i + 1U < YEW_ARRAY_LEN(kinds) - 1U; i++) {
        for (j = i + 1U; j < YEW_ARRAY_LEN(kinds) - 1U; j++)
            YEW_ASSERT(strcmp(errors[i].msg, errors[j].msg) != 0);
    }
    YEW_ASSERT_NOT_NULL(strstr(errors[YEW_AI_ERR_UNREACHABLE - 1U].msg,
                               "api.anthropic.com:443"));
    YEW_ASSERT_NOT_NULL(strstr(errors[YEW_AI_ERR_AUTH - 1U].msg,
                               "$ANTHROPIC_API_KEY"));
    YEW_ASSERT_NOT_NULL(strstr(errors[YEW_AI_ERR_RATELIMIT - 1U].msg,
                               "21 s"));
    YEW_ASSERT_NOT_NULL(strstr(errors[YEW_AI_ERR_MODEL - 1U].msg,
                               "claude-fixture"));

    {
        AiCooldown cooldown;
        AiErr cooldown_error;

        yew_ai_cooldown_init(&cooldown);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 0U);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 100), 0);

        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_UNREACHABLE, -1,
                             100, 60000);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 1U);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 100),
                          1000);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 1099), 1);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 1100), 0);

        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_RATELIMIT, 500,
                             2000, 60000);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 2U);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 2000),
                          2000);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_RATELIMIT, 9000,
                             5000, 60000);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 3U);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 5000),
                          9000);
        yew_ai_err_format(&cooldown_error, YEW_AI_ERR_RATELIMIT, &b,
                          429U,
                          yew_ai_cooldown_remaining(&cooldown, 5000),
                          NULL);
        YEW_ASSERT_NOT_NULL(strstr(cooldown_error.msg, "9 s"));

        yew_ai_cooldown_note(&cooldown, YEW_AI_OK, -1, 5000, 60000);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 0U);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 5000), 0);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_UNREACHABLE, -1,
                             6000, 2500);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_UNREACHABLE, -1,
                             7000, 2500);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_UNREACHABLE, -1,
                             8000, 2500);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 8000),
                          2500);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_UNREACHABLE, 5000,
                             9000, 2500);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 9000),
                          5000);

        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_PROTOCOL, -1,
                             9000, 2500);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 0U);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, 9000), 0);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_UNREACHABLE, -1,
                             INT64_MAX - 10, INT64_MAX);
        YEW_ASSERT_EQ_I64(cooldown.until_ms, INT64_MAX);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown,
                                                     INT64_MAX - 10),
                          10);
        YEW_ASSERT_EQ_I64(yew_ai_cooldown_remaining(&cooldown, INT64_MAX),
                          0);

        cooldown.consecutive = UINT32_MAX;
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_RATELIMIT, -1,
                             0, INT64_MAX);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, UINT32_MAX);
        YEW_ASSERT_EQ_I64(cooldown.until_ms, INT64_MAX);
        yew_ai_cooldown_note(&cooldown, YEW_AI_ERR_AUTH, -1,
                             0, INT64_MAX);
        YEW_ASSERT_EQ_U64(cooldown.consecutive, 0U);
        YEW_ASSERT_EQ_I64(cooldown.until_ms, 0);
    }
}
