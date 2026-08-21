#include "harness.h"

#include <string.h>

#include "mod/ai/prompt.h"

static void assert_trim(const char *input, const char *prefix, u32 max_lines,
                        const char *want)
{
    u8 bytes[512];
    AiCtx context = {0};
    u32 len = (u32)strlen(input);
    u32 trimmed;
    u32 again;

    (void)memcpy(bytes, input, len);
    context.prefix = (const u8 *)prefix;
    context.plen = (u32)strlen(prefix);
    trimmed = yew_ai_response_trim(bytes, len, &context, max_lines);
    YEW_ASSERT_EQ_U64(trimmed, strlen(want));
    YEW_ASSERT_EQ_MEM(bytes, want, trimmed);
    again = yew_ai_response_trim(bytes, trimmed, &context, max_lines);
    YEW_ASSERT_EQ_U64(again, trimmed);
    YEW_ASSERT_EQ_MEM(bytes, want, again);
}

void test_ai_trim_applies_the_five_ordered_rules(void)
{
    assert_trim("```c\nvalue();\n```", "", 8U, "value();");
    assert_trim("\nvalue();", "", 8U, "value();");
    assert_trim("    retreturn x;", "if (ok)\n    ret", 8U,
                "return x;");
    assert_trim("abcabcvalue", "abc", 8U, "value");
    assert_trim("a\nb\nc\nd", "", 3U, "a\nb\nc");
    assert_trim("a  \n  b  \t", "", 8U, "a  \n  b");
}

void test_ai_trim_partial_fences_do_not_flicker(void)
{
    static const char response[] = "```c\nanswer();\n```";
    u32 i;

    for (i = 1U; i < sizeof(response) - 1U; i++) {
        u8 bytes[sizeof(response)];
        AiCtx context = {0};
        u32 trimmed;

        (void)memcpy(bytes, response, i);
        trimmed = yew_ai_response_trim(bytes, i, &context, 8U);
        if (i < sizeof(response) - 2U)
            YEW_ASSERT(trimmed != strlen("answer();") ||
                       memcmp(bytes, "answer();", trimmed) != 0);
        YEW_ASSERT_EQ_U64(yew_ai_response_trim(bytes, trimmed, &context, 8U),
                          trimmed);
    }
}

void test_ai_trim_is_idempotent_over_corpus(void)
{
    u32 seed;

    for (seed = 0U; seed < 200U; seed++) {
        u8 bytes[128];
        u8 once[128];
        AiCtx context = {(const u8 *)"prefix", 6U, NULL, 0U,
                         "", "", 1U, false, false};
        u32 len = 16U + seed % 80U;
        u32 i;
        u32 first;
        u32 second;

        for (i = 0U; i < len; i++) {
            u32 value = seed * 33U + i * 17U;
            bytes[i] = value % 19U == 0U ? (u8)'\n' :
                       value % 13U == 0U ? (u8)' ' :
                       (u8)('a' + value % 26U);
        }
        first = yew_ai_response_trim(bytes, len, &context, 8U);
        (void)memcpy(once, bytes, first);
        second = yew_ai_response_trim(bytes, first, &context, 8U);
        YEW_ASSERT_EQ_U64(second, first);
        YEW_ASSERT_EQ_MEM(bytes, once, first);
    }
}
