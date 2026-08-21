#include "harness.h"

#include <string.h>

#include "mod/ai/prompt.h"
#include "util/buf.h"

static AiBackend prompt_backend(AiKind kind, const char *model, bool fim)
{
    AiBackend backend;

    (void)memset(&backend, 0, sizeof(backend));
    backend.kind = (u8)kind;
    backend.model = model;
    backend.max_tokens = 256;
    backend.temperature = 0.1;
    backend.stream = true;
    backend.fim = fim;
    return backend;
}

static void prompt_json(const AiBackend *backend, const AiCtx *context,
                        Bytebuf *out)
{
    JsonW writer;

    bytebuf_init(out);
    yew_jsonw_init(&writer, out);
    yew_ai_prompt_build(&writer, backend, context);
    bytebuf_push_u8(out, (u8)'\0');
}

void test_ai_prompt_fim_family_table_is_conservative(void)
{
    static const struct {
        const char *model;
        AiTemplate want;
    } cases[] = {
        {"QWEN2.5-CODER:7b", YEW_AI_TPL_FIM},
        {"qwen3-coder:30b", YEW_AI_TPL_FIM},
        {"codellama:13b", YEW_AI_TPL_FIM},
        {"codegemma:7b", YEW_AI_TPL_FIM},
        {"starcoder2:3b", YEW_AI_TPL_FIM},
        {"deepseek-coder:6.7b", YEW_AI_TPL_FIM},
        {"codestral:latest", YEW_AI_TPL_FIM},
        {"llama3.2", YEW_AI_TPL_CHAT},
        {"mistral", YEW_AI_TPL_CHAT},
        {"unknown-coder", YEW_AI_TPL_CHAT}
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        AiBackend backend = prompt_backend(YEW_AI_OLLAMA, cases[i].model,
                                           false);
        YEW_ASSERT_EQ_U64(yew_ai_template_of(&backend), cases[i].want);
    }
    {
        AiBackend openai = prompt_backend(YEW_AI_OPENAI,
                                          "qwen2.5-coder", true);
        YEW_ASSERT_EQ_U64(yew_ai_template_of(&openai), YEW_AI_TPL_CHAT);
    }
}

void test_ai_prompt_fim_json_is_byte_exact(void)
{
    AiBackend backend = prompt_backend(YEW_AI_OLLAMA,
                                       "qwen2.5-coder:7b", false);
    AiCtx context = {
        (const u8 *)"int ", 4U, (const u8 *)";\n", 2U,
        "src/x.c", "c", 1U, false, false
    };
    Bytebuf json;
    const char *want =
        "{\"model\":\"qwen2.5-coder:7b\",\"prompt\":\"int \","
        "\"suffix\":\";\\n\",\"stream\":true,\"raw\":false,"
        "\"options\":{\"num_predict\":256,\"temperature\":0.1,"
        "\"stop\":[\"\\n\\n\",\"\\n}\",\"```\"]}}";

    prompt_json(&backend, &context, &json);
    YEW_ASSERT_EQ_STR((const char *)json.data, want);
    YEW_ASSERT_NULL(strstr((const char *)json.data, "src/x.c"));
    YEW_ASSERT_NULL(strstr((const char *)json.data, "fim_prefix"));
    bytebuf_free(&json);
}

void test_ai_prompt_chat_framing_and_system_are_exact(void)
{
    static const char system[] =
        "You are a code completion engine embedded in a text editor.\n"
        "You are given the text before the cursor and the text after it.\n"
        "Reply with ONLY the code that belongs at the cursor.\n\n"
        "Rules:\n"
        "- Output raw code. No prose, no explanation, no apology.\n"
        "- No Markdown. No ``` fences. No language tag.\n"
        "- Do not repeat any of the text before the cursor.\n"
        "- Do not repeat any of the text after the cursor.\n"
        "- Continue the file's existing indentation, brace style, and naming.\n"
        "- If nothing sensible belongs at the cursor, reply with an empty message.\n"
        "- Reply with at most 8 lines.\n";
    AiBackend backend = prompt_backend(YEW_AI_ANTHROPIC,
                                       "claude-fixture", false);
    AiCtx context = {
        (const u8 *)"int ", 4U, (const u8 *)";", 1U,
        "src/edit/motion.c", "c", 10U, true, false
    };
    Bytebuf json;

    YEW_ASSERT_EQ_STR(yew_ai_chat_system_prompt(), system);
    prompt_json(&backend, &context, &json);
    YEW_ASSERT_NOT_NULL(strstr((const char *)json.data,
        "\"max_tokens\":256"));
    YEW_ASSERT_NOT_NULL(strstr((const char *)json.data,
        "File: src/edit/motion.c\\nLanguage: c\\n\\n<|before|>\\n"));
    YEW_ASSERT_NOT_NULL(strstr((const char *)json.data,
        "[… earlier in the file elided …]\\nint \\n<|cursor|>\\n;"
        "\\n<|after|>"));
    YEW_ASSERT_NOT_NULL(strstr((const char *)json.data,
        "\"stop_sequences\":[\"```\",\"<|before|>\",\"<|after|>\"]"));
    bytebuf_free(&json);
}
