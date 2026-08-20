#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/registry.h"

void test_ai_fletch_backend_registers_and_replaces(void)
{
    static const char first[] =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", model: \"qwen\"})\n";
    static const char replacement[] =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", model: \"coder\", "
        "max_tokens: 512, fim: true})\n";
    Ed ed;
    const AiBackendEntry *entry;

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, first, sizeof(first) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    entry = yew_ai_backend_at(&ed, 0U);
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_STR(entry->backend.name, "local");
    YEW_ASSERT_EQ_STR(entry->backend.model, "qwen");
    YEW_ASSERT(entry->backend.url.loopback);

    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, replacement,
                                  sizeof(replacement) - 1U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    entry = yew_ai_backend_at(&ed, 0U);
    YEW_ASSERT_EQ_STR(entry->backend.model, "coder");
    YEW_ASSERT_EQ_I64(entry->backend.max_tokens, 512);
    YEW_ASSERT(entry->backend.fim);
    yew_ed_free(&ed);
}

void test_ai_fletch_backend_rejects_credentials_transactionally(void)
{
    static const char good[] =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", model: \"qwen\"})\n";
    static const char bad[] =
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", model: \"bad\", "
        "secret: \"credential\"})\n";
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, good, sizeof(good) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT(yew_fl_eval(&ed, bad, sizeof(bad) - 1U) != YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    YEW_ASSERT_EQ_STR(yew_ai_backend_at(&ed, 0U)->backend.model, "qwen");
    yew_ed_free(&ed);
}
