#include "harness.h"

#include <stdlib.h>

#include "unicode/grapheme.h"
#include "unicode/u16.h"

static void assert_boundary_roundtrips(const u8 *bytes, size_t len)
{
    TextBuf *tb = yew_textbuf_from_bytes(bytes, (u64)len);
    Span line = yew_textbuf_line_span(tb, LINENO(0U));
    size_t off = 0U;

    for (;;) {
        U16Col col = yew_off_to_u16col(tb, line, BYTEOFF((u64)off));
        YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, line, col).v, off);
        if (off == len)
            break;
        off = yew_gb_next_bytes(bytes, len, off);
    }
    yew_textbuf_free(tb);
}

void test_u16_code_unit_table(void)
{
    static const u8 bytes[] = {
        0x00U,                         /* U+0000 */
        0xC2U, 0xA2U,                 /* U+00A2 */
        0xE2U, 0x82U, 0xACU,          /* U+20AC */
        0xF0U, 0x9FU, 0x98U, 0x80U,  /* U+1F600 */
        0xFFU,                         /* lossless escape */
        0x09U                          /* TAB */
    };
    static const u64 offsets[] = {0U, 1U, 3U, 6U, 10U, 11U, 12U};
    static const u64 cols[] = {0U, 1U, 2U, 3U, 5U, 6U, 7U};
    TextBuf *tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    Span line = yew_textbuf_line_span(tb, LINENO(0U));
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(offsets); i++) {
        YEW_ASSERT_EQ_U64(yew_off_to_u16col(tb, line,
                                            BYTEOFF(offsets[i])).v,
                          cols[i]);
        YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, line,
                                            U16COL(cols[i])).v,
                          offsets[i]);
    }
    yew_textbuf_free(tb);
}

void test_u16_grapheme_corpus_roundtrips(void)
{
    static const u8 ascii[] = "plain ASCII";
    static const u8 cjk[] = "\xE6\x97\xA5\xE6\x9C\xAC";
    static const u8 emoji[] = "\xF0\x9F\x98\x80";
    static const u8 family[] =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA7";
    static const u8 flag[] = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    static const u8 keycap[] = "1\xEF\xB8\x8F\xE2\x83\xA3";
    static const u8 combining[] = "e\xCC\x81\xCC\xA7";
    static const u8 invalid[] = {0xFFU, (u8)'x'};

    assert_boundary_roundtrips(ascii, sizeof(ascii) - 1U);
    assert_boundary_roundtrips(cjk, sizeof(cjk) - 1U);
    assert_boundary_roundtrips(emoji, sizeof(emoji) - 1U);
    assert_boundary_roundtrips(family, sizeof(family) - 1U);
    assert_boundary_roundtrips(flag, sizeof(flag) - 1U);
    assert_boundary_roundtrips(keycap, sizeof(keycap) - 1U);
    assert_boundary_roundtrips(combining, sizeof(combining) - 1U);
    assert_boundary_roundtrips(invalid, sizeof(invalid));
}

void test_u16_clamps_surrogate_and_line_end(void)
{
    static const u8 bytes[] = {
        (u8)'a', 0xF0U, 0x9FU, 0x98U, 0x80U, (u8)'b',
        (u8)'\r', (u8)'\n', (u8)'z'
    };
    TextBuf *tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    Span first = yew_textbuf_line_span(tb, LINENO(0U));

    YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, first, U16COL(2U)).v, 1U);
    YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, first, U16COL(99U)).v, 6U);
    YEW_ASSERT_EQ_U64(yew_off_to_u16col(tb, first, BYTEOFF(7U)).v, 4U);
    YEW_ASSERT_EQ_U64(yew_off_to_u16col(tb, first, BYTEOFF(99U)).v, 4U);
    YEW_ASSERT_EQ_U64(yew_off_to_u16col(tb, first, BYTEOFF(3U)).v, 1U);
    yew_textbuf_free(tb);
}

void test_u16_decodes_across_piece_seams(void)
{
    static const u8 lead[] = {(u8)'a', 0xF0U, 0x9FU};
    static const u8 tail[] = {0x98U, 0x80U, (u8)'b'};
    TextBuf *tb = yew_textbuf_from_bytes(lead, sizeof(lead));
    Span line;

    yew_textbuf_insert(tb, BYTEOFF(sizeof(lead)), tail, sizeof(tail));
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_u16col(tb, line, BYTEOFF(5U)).v, 3U);
    YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, line, U16COL(3U)).v, 5U);
    yew_textbuf_free(tb);
}

void test_u16_large_astral_line_uses_u64(void)
{
    const size_t count = 100000U;
    const size_t len = count * 4U;
    u8 *bytes = malloc(len);
    TextBuf *tb;
    Span line;
    size_t i;

    YEW_ASSERT_NOT_NULL(bytes);
    for (i = 0U; i < count; i++) {
        bytes[i * 4U + 0U] = 0xF0U;
        bytes[i * 4U + 1U] = 0x9FU;
        bytes[i * 4U + 2U] = 0x98U;
        bytes[i * 4U + 3U] = 0x80U;
    }
    tb = yew_textbuf_from_bytes(bytes, (u64)len);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_u16col(tb, line, BYTEOFF(len)).v,
                      count * 2U);
    YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, line,
                                        U16COL(count * 2U)).v,
                      len);
    yew_textbuf_free(tb);
    free(bytes);
}
