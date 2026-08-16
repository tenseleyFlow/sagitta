#include "harness.h"

#include <string.h>

#include "mod/lsp/features.h"
#include "util/buf.h"

typedef struct SnippetCase {
    const char *in;
    const char *want;
    u32 cursor;
} SnippetCase;

static void snippet_check(const SnippetCase *tc)
{
    Bytebuf out;
    u32 cursor;
    size_t want_len = strlen(tc->want);

    bytebuf_init(&out);
    cursor = yew_lsp_snippet_strip((const u8 *)tc->in,
                                   (u32)strlen(tc->in), &out);
    YEW_ASSERT_EQ_U64(out.len, want_len);
    YEW_ASSERT(out.len == 0U || memcmp(out.data, tc->want, out.len) == 0);
    YEW_ASSERT_EQ_U64(cursor, tc->cursor);
    bytebuf_free(&out);
}

void test_lsp_snippet_strips_all_grammar_forms(void)
{
    static const SnippetCase cases[] = {
        {"plain text", "plain text", 10U},
        {"$1", "", 0U},
        {"$123", "", 0U},
        {"${3}", "", 0U},
        {"a${3}b", "ab", 2U},
        {"${1:int argc}", "int argc", 8U},
        {"x${20:default}y", "xdefaulty", 9U},
        {"${1|red,green,blue|}", "red", 3U},
        {"${2|only|}", "only", 4U},
        {"$TM_FILENAME", "", 0U},
        {"${TM_LINE_NUMBER}", "", 0U},
        {"a$VAR_NAME-b", "a-b", 3U},
        {"${VAR:kept}", "kept", 4U},
        {"${_VAR:under}", "under", 5U},
        {"$0tail", "tail", 0U},
        {"head$0", "head", 4U},
        {"foo($0)", "foo()", 4U},
        {"${0:inside}", "inside", 0U},
        {"a$00b", "ab", 1U},
        {"$01", "", 0U},
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        snippet_check(&cases[i]);
}

void test_lsp_snippet_decodes_escapes_and_choices(void)
{
    static const SnippetCase cases[] = {
        {"\\$", "$", 1U},
        {"\\\\", "\\", 1U},
        {"\\}", "}", 1U},
        {"\\,", ",", 1U},
        {"\\|", "|", 1U},
        {"a\\$b", "a$b", 3U},
        {"${1|a\\,b,c|}", "a,b", 3U},
        {"${1|a\\|b,c|}", "a|b", 3U},
        {"${1|a\\}b,c|}", "a}b", 3U},
        {"${1|a\\\\b,c|}", "a\\b", 3U},
        {"${1:cost \\$5}", "cost $5", 7U},
        {"\\q", "\\q", 2U},
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        snippet_check(&cases[i]);
}

void test_lsp_snippet_recursion_cap_and_malformed_input(void)
{
    static const SnippetCase cases[] = {
        {"${1:${2:x}}", "x", 1U},
        {"${1:${2:${3:${4:${5:${6:${7:${8:x}}}}}}}}", "x", 1U},
        {"${1:${2:${3:${4:${5:${6:${7:${8:${9:x}}}}}}}}}", "", 0U},
        {"${", "${", 2U},
        {"${1", "${1", 3U},
        {"${}", "${}", 3U},
        {"${:x}", "${:x}", 5U},
        {"${1?x}", "${1?x}", 6U},
        {"${1|a,b}", "${1|a,b}", 8U},
        {"${1:${2:x}", "${1:x", 5U},
        {"trailing$", "trailing$", 9U},
        {"$-", "$-", 2U},
        {"$$", "$$", 2U},
        {"before${1:unterminated", "before${1:unterminated", 22U},
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        snippet_check(&cases[i]);
}

void test_lsp_snippet_handles_four_kib_and_appends(void)
{
    u8 input[4096];
    Bytebuf out;
    u32 cursor;
    size_t i;

    (void)memset(input, 'x', sizeof(input));
    input[2047] = '$';
    input[2048] = '0';
    bytebuf_init(&out);
    bytebuf_append(&out, "pre", 3U);
    cursor = yew_lsp_snippet_strip(input, (u32)sizeof(input), &out);
    YEW_ASSERT_EQ_U64(out.len, 4097U);
    YEW_ASSERT_EQ_U64(cursor, 2047U);
    YEW_ASSERT(memcmp(out.data, "pre", 3U) == 0);
    for (i = 3U; i < out.len; i++)
        YEW_ASSERT_EQ_U64(out.data[i], 'x');
    bytebuf_free(&out);

    bytebuf_init(&out);
    YEW_ASSERT_EQ_U64(yew_lsp_snippet_strip(NULL, 0U, &out), 0U);
    YEW_ASSERT_EQ_U64(out.len, 0U);
    bytebuf_free(&out);
}
