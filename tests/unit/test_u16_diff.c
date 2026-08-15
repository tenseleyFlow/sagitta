#include "harness.h"

#include <stdlib.h>

#include "unicode/u16.h"
#include "unicode/utf8.h"

static u64 naive_u16_prefix(const u8 *line, size_t len, size_t prefix)
{
    size_t off = 0U;
    u64 units = 0U;

    while (off < prefix) {
        u32 cp;
        size_t n = yew_utf8_decode(line + off, len - off, &cp);

        if (off + n > prefix)
            break;
        units += cp > 0xFFFFU ? 2U : 1U;
        off += n;
    }
    return units;
}

static bool scalar_boundary(const u8 *line, size_t len, size_t target)
{
    size_t off = 0U;

    while (off < target) {
        u32 cp;
        size_t n = yew_utf8_decode(line + off, len - off, &cp);

        (void)cp;
        off += n;
    }
    return off == target;
}

void test_u16_differential_200_lines(void)
{
    static const u8 corpus[] = {
        (u8)'A', 0xC2U, 0xA2U, 0xE6U, 0x97U, 0xA5U,
        0xF0U, 0x9FU, 0x98U, 0x80U, (u8)'e', 0xCCU, 0x81U,
        0xFFU, (u8)'\t'
    };
    const size_t rows = 200U;
    const size_t stride = sizeof(corpus) + 2U;
    const size_t total = rows * stride;
    u8 *fixture = malloc(total);
    TextBuf *tb;
    size_t row;

    YEW_ASSERT_NOT_NULL(fixture);
    for (row = 0U; row < rows; row++) {
        size_t base = row * stride;

        (void)memcpy(fixture + base, corpus, sizeof(corpus));
        fixture[base + sizeof(corpus)] = (u8)'\r';
        fixture[base + sizeof(corpus) + 1U] = (u8)'\n';
    }
    tb = yew_textbuf_from_bytes(fixture, (u64)total);
    for (row = 0U; row < rows; row++) {
        Span line = yew_textbuf_line_span(tb, LINENO(row));
        size_t base = row * stride;
        size_t off;

        for (off = 0U; off <= sizeof(corpus); off++) {
            U16Col got = yew_off_to_u16col(tb, line,
                                           BYTEOFF(base + off));
            u64 want = naive_u16_prefix(corpus, sizeof(corpus), off);

            YEW_ASSERT_EQ_U64(got.v, want);
            /* UTF-8 negotiation is the byte identity in both directions. */
            YEW_ASSERT_EQ_U64((base + off) - line.lo, off);
            YEW_ASSERT_EQ_U64(line.lo + off, base + off);
            if (scalar_boundary(corpus, sizeof(corpus), off))
                YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, line, got).v,
                                  base + off);
        }
        YEW_ASSERT_EQ_U64(yew_u16col_to_off(tb, line,
                                            U16COL(UINT64_MAX)).v,
                          base + sizeof(corpus));
    }
    yew_textbuf_free(tb);
    free(fixture);
}
