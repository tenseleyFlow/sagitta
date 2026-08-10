#include "harness.h"

#include "../fuzz/oracle.h"

typedef struct {
    const char *input;
    const char *expected;
    u64 lines;
} OracleCase;

static void assert_oracle(const OracleCase *test)
{
    Oracle oracle;
    Bytebuf materialized;
    size_t len = strlen(test->input);
    u64 line;
    u64 off;

    oracle_init(&oracle, (const u8 *)test->input, (u64)len);
    bytebuf_init(&materialized);
    oracle_materialize(&oracle, &materialized);
    YEW_ASSERT_EQ_U64(oracle_len(&oracle), len);
    YEW_ASSERT_EQ_U64(oracle_line_count(&oracle), test->lines);
    YEW_ASSERT_EQ_U64(materialized.len, strlen(test->expected));
    YEW_ASSERT_EQ_MEM(materialized.data, test->expected, materialized.len);
    for (line = 0U; line < test->lines; line++) {
        u64 start = oracle_line_start(&oracle, line);
        YEW_ASSERT(start <= (u64)len);
        YEW_ASSERT_EQ_U64(oracle_line_of(&oracle, start), line);
        if (line != 0U)
            YEW_ASSERT_EQ_U64((u8)test->input[start - 1U], (u8)'\n');
    }
    for (off = 0U; off <= (u64)len; off++) {
        line = oracle_line_of(&oracle, off);
        YEW_ASSERT(line < test->lines);
        YEW_ASSERT(oracle_line_start(&oracle, line) <= off);
    }
    bytebuf_free(&materialized);
    oracle_free(&oracle);
}

void test_oracle_hand_cases(void)
{
    static const OracleCase cases[] = {
        {"", "", 1U}, {"a", "a", 1U}, {"ab", "ab", 1U},
        {"\n", "\n", 2U}, {"\n\n", "\n\n", 3U},
        {"a\n", "a\n", 2U}, {"a\nb", "a\nb", 2U},
        {"a\nb\n", "a\nb\n", 3U}, {"\na", "\na", 2U},
        {"\na\n", "\na\n", 3U}, {"a\n\nb", "a\n\nb", 3U},
        {"abc\ndef", "abc\ndef", 2U}, {"abc\ndef\n", "abc\ndef\n", 3U},
        {"\r", "\r", 1U}, {"\r\n", "\r\n", 2U},
        {"a\r\nb", "a\r\nb", 2U}, {"a\rb", "a\rb", 1U},
        {"\t", "\t", 1U}, {"\t\n", "\t\n", 2U},
        {"one\ntwo\nthree", "one\ntwo\nthree", 3U},
        {"1\n2\n3\n4", "1\n2\n3\n4", 4U},
        {"1\n2\n3\n4\n", "1\n2\n3\n4\n", 5U},
        {" \n ", " \n ", 2U}, {"x\n\n\n", "x\n\n\n", 4U},
        {"\n\n\nx", "\n\n\nx", 4U}, {"long line", "long line", 1U},
        {"long line\n", "long line\n", 2U},
        {"alpha\nbeta\ngamma\ndelta", "alpha\nbeta\ngamma\ndelta", 4U},
        {"A\nB\nC\nD\nE\n", "A\nB\nC\nD\nE\n", 6U},
        {"é", "é", 1U}, {"é\n", "é\n", 2U},
        {"🙂\nx", "🙂\nx", 2U}, {"中\n文", "中\n文", 2U},
        {"a\r\n\r\nb", "a\r\n\r\nb", 3U},
        {"\r\n\r\n", "\r\n\r\n", 3U},
        {"0123456789", "0123456789", 1U},
        {"0\n12\n345\n6789", "0\n12\n345\n6789", 4U},
        {"\\n", "\\n", 1U}, {"#\n#", "#\n#", 2U},
        {"final\nnewline\n", "final\nnewline\n", 3U}
    };
    size_t i;

    YEW_ASSERT_EQ_U64(YEW_ARRAY_LEN(cases), 40U);
    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        assert_oracle(&cases[i]);
}

static void assert_content(Oracle *oracle, const u8 *expected, size_t len,
                           u64 lines)
{
    Bytebuf out;

    bytebuf_init(&out);
    oracle_materialize(oracle, &out);
    YEW_ASSERT_EQ_U64(out.len, len);
    YEW_ASSERT_EQ_MEM(out.data, expected, len);
    YEW_ASSERT_EQ_U64(oracle_len(oracle), len);
    YEW_ASSERT_EQ_U64(oracle_line_count(oracle), lines);
    bytebuf_free(&out);
}

void test_oracle_edit_cases(void)
{
    static const u8 binary[] = {'a', 0U, 'b', '\n', 0xffU};
    static const u8 inserted[] = {'\n', 0x80U, '\r'};
    static const u8 expected_insert[] = {'a', '\n', 0x80U, '\r', 0U,
                                         'b', '\n', 0xffU};
    static const u8 expected_delete[] = {'a', '\n', 0xffU};
    Oracle oracle;

    oracle_init(&oracle, binary, sizeof(binary));
    assert_content(&oracle, binary, sizeof(binary), 2U);
    oracle_insert(&oracle, 1U, inserted, sizeof(inserted));
    assert_content(&oracle, expected_insert, sizeof(expected_insert), 3U);
    oracle_delete(&oracle, 2U, 7U);
    assert_content(&oracle, expected_delete, sizeof(expected_delete), 2U);
    oracle_insert(&oracle, oracle_len(&oracle), (const u8 *)"\n", 1U);
    YEW_ASSERT_EQ_U64(oracle_line_count(&oracle), 3U);
    oracle_delete(&oracle, 1U, oracle_len(&oracle));
    assert_content(&oracle, (const u8 *)"a", 1U, 1U);
    oracle_delete(&oracle, 0U, 1U);
    assert_content(&oracle, NULL, 0U, 1U);
    oracle_insert(&oracle, 0U, (const u8 *)"\n\n\n", 3U);
    assert_content(&oracle, (const u8 *)"\n\n\n", 3U, 4U);
    oracle_free(&oracle);
}
