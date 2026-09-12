#include "harness.h"

#include <stdlib.h>

#if YEW_WITH_LSP
#include "mod/lsp/sync.h"
#endif
#include "unicode/u16.h"

/* Deliberately independent of the product UTF-8 decoder.  The audit corpus
 * is made only from these well-formed scalar shapes plus raw-byte escapes. */
static size_t ref_decode(const u8 *s, size_t len, u32 *cp)
{
    if (len == 0U)
        return 0U;
    if (s[0] < 0x80U) {
        *cp = s[0];
        return 1U;
    }
    if (s[0] >= 0xC2U && s[0] <= 0xDFU && len >= 2U) {
        *cp = ((u32)(s[0] & 0x1FU) << 6) | (u32)(s[1] & 0x3FU);
        return 2U;
    }
    if (s[0] >= 0xE0U && s[0] <= 0xEFU && len >= 3U) {
        *cp = ((u32)(s[0] & 0x0FU) << 12) |
              ((u32)(s[1] & 0x3FU) << 6) | (u32)(s[2] & 0x3FU);
        return 3U;
    }
    if (s[0] >= 0xF0U && s[0] <= 0xF4U && len >= 4U) {
        *cp = ((u32)(s[0] & 0x07U) << 18) |
              ((u32)(s[1] & 0x3FU) << 12) |
              ((u32)(s[2] & 0x3FU) << 6) | (u32)(s[3] & 0x3FU);
        return 4U;
    }
    *cp = 0xDC00U + s[0];
    return 1U;
}

static u64 ref_u16_prefix(const u8 *line, size_t len, size_t prefix,
                          bool wrong_astral)
{
    size_t off = 0U;
    u64 units = 0U;

    while (off < prefix) {
        u32 cp;
        size_t n = ref_decode(line + off, len - off, &cp);

        if (off + n > prefix)
            break;
        units += !wrong_astral && cp > 0xFFFFU ? 2U : 1U;
        off += n;
    }
    return units;
}

static size_t ref_scalar_start(const u8 *line, size_t len, size_t target)
{
    size_t off = 0U;

    while (off < target) {
        u32 cp;
        size_t n = ref_decode(line + off, len - off, &cp);

        (void)cp;
        if (off + n > target)
            break;
        off += n;
    }
    return off;
}

static void audit_line(const TextBuf *tb, const u8 *bytes, size_t len,
                       size_t row, size_t base, bool *wrong_witness)
{
    Span span = yew_textbuf_line_span(tb, LINENO(row));
    size_t off;

    YEW_ASSERT_EQ_U64(span.lo, base);
    for (off = 0U; off <= len; off++) {
#if YEW_WITH_LSP
        i64 line8;
        i64 col8;
        i64 line16;
        i64 col16;
#endif
        u64 want16 = ref_u16_prefix(bytes, len, off, false);
        u64 wrong16 = ref_u16_prefix(bytes, len, off, true);
        size_t scalar = ref_scalar_start(bytes, len, off);

#if YEW_WITH_LSP
        yew_lsp_pos_of_off(YEW_POSENC_UTF8, tb, BYTEOFF(base + off),
                           &line8, &col8);
        yew_lsp_pos_of_off(YEW_POSENC_UTF16, tb, BYTEOFF(base + off),
                           &line16, &col16);
        YEW_ASSERT_EQ_I64(line8, (i64)row);
        YEW_ASSERT_EQ_I64(line16, (i64)row);
        YEW_ASSERT_EQ_I64(col8, (i64)off);
        YEW_ASSERT_EQ_I64(col16, (i64)want16);
        YEW_ASSERT_EQ_U64(yew_off_to_u16col(
            tb, span, BYTEOFF(base + off)).v, want16);
        YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
            YEW_POSENC_UTF8, tb, LINENO(row), (u64)col8).v, base + off);
        YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
            YEW_POSENC_UTF16, tb, LINENO(row), (u64)col16).v,
            base + scalar);
#else
        /* UTF-8 protocol coordinates are byte offsets within the line. */
        YEW_ASSERT_EQ_U64(base + off, span.lo + off);
#endif
        YEW_ASSERT_EQ_U64(yew_u16col_to_off(
            tb, span, U16COL(want16)).v, base + scalar);
        if (want16 != wrong16)
            *wrong_witness = true;
    }
#if YEW_WITH_LSP
    YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
        YEW_POSENC_UTF8, tb, LINENO(row), UINT64_MAX).v, base + len);
    YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
        YEW_POSENC_UTF16, tb, LINENO(row), UINT64_MAX).v, base + len);
#endif
}

void test_u16_differential_200_lines(void)
{
    static const u8 corpus[] = {
        (u8)'A', 0xC2U, 0xA2U, 0xE6U, 0x97U, 0xA5U,
        0xF0U, 0x9FU, 0x98U, 0x80U, (u8)'e', 0xCCU, 0x81U,
        0xFFU, (u8)'\t'
    };
    enum { ROWS = 200, LONG_LEN = 4096 };
    const size_t stride = sizeof(corpus) + 2U;
    const size_t long_ascii = LONG_LEN - sizeof(corpus);
    const size_t total = ROWS * stride + LONG_LEN + 2U;
    u8 *fixture = malloc(total);
    TextBuf *tb;
    size_t row;
    bool wrong_witness = false;

    YEW_ASSERT_NOT_NULL(fixture);
    for (row = 0U; row < ROWS; row++) {
        size_t base = row * stride;

        (void)memcpy(fixture + base, corpus, sizeof(corpus));
        fixture[base + sizeof(corpus)] = (u8)'\r';
        fixture[base + sizeof(corpus) + 1U] = (u8)'\n';
    }
    (void)memset(fixture + ROWS * stride, 'q', long_ascii);
    (void)memcpy(fixture + ROWS * stride + long_ascii,
                 corpus, sizeof(corpus));
    fixture[total - 2U] = (u8)'\r';
    fixture[total - 1U] = (u8)'\n';

    tb = yew_textbuf_from_bytes(fixture, (u64)total);
    for (row = 0U; row < ROWS; row++)
        audit_line(tb, corpus, sizeof(corpus), row, row * stride,
                   &wrong_witness);
    audit_line(tb, fixture + ROWS * stride, LONG_LEN, ROWS,
               ROWS * stride, &wrong_witness);
    /* If astral scalars are incorrectly counted as one UTF-16 unit, this
     * matrix must contain a concrete disagreement. */
    YEW_ASSERT(wrong_witness);

    yew_textbuf_free(tb);
    free(fixture);
}
