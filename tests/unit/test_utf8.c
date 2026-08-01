#include "harness.h"

#include "unicode/utf8.h"

static void assert_round_trip(const u8 *input, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        u8 encoded[SAG_UTF8_MAX];
        u32 cp;
        size_t consumed = sag_utf8_decode(input + pos, len - pos, &cp);
        size_t produced = sag_utf8_encode(cp, encoded);

        if (consumed == 0 || consumed > len - pos ||
            produced > sizeof(encoded) || produced != consumed) {
            SAG_ASSERT(false);
            return;
        }
        SAG_ASSERT_EQ_U64(consumed, produced);
        SAG_ASSERT_EQ_MEM(input + pos, encoded, produced);
        pos += consumed;
    }
    SAG_ASSERT_EQ_U64(pos, len);
}

static void assert_decodes(const u8 *input, size_t len, u32 expected,
                           size_t consumed)
{
    u32 actual;

    SAG_ASSERT_EQ_U64(sag_utf8_decode(input, len, &actual), consumed);
    SAG_ASSERT_EQ_U64(actual, expected);
}

void test_utf8_valid_decode(void)
{
    static const struct {
        u8 bytes[4];
        u8 len;
        u32 cp;
    } cases[] = {
        {{0x00, 0, 0, 0}, 1, 0x0000},
        {{0x7F, 0, 0, 0}, 1, 0x007F},
        {{0xC2, 0x80, 0, 0}, 2, 0x0080},
        {{0xDF, 0xBF, 0, 0}, 2, 0x07FF},
        {{0xE0, 0xA0, 0x80, 0}, 3, 0x0800},
        {{0xED, 0x9F, 0xBF, 0}, 3, 0xD7FF},
        {{0xEE, 0x80, 0x80, 0}, 3, 0xE000},
        {{0xEF, 0xBF, 0xBF, 0}, 3, 0xFFFF},
        {{0xF0, 0x90, 0x80, 0x80}, 4, 0x10000},
        {{0xF4, 0x8F, 0xBF, 0xBF}, 4, 0x10FFFF},
    };
    size_t i;

    for (i = 0; i < SAG_ARRAY_LEN(cases); i++) {
        assert_decodes(cases[i].bytes, cases[i].len, cases[i].cp,
                       cases[i].len);
        assert_round_trip(cases[i].bytes, cases[i].len);
    }
}

void test_utf8_reject_classes(void)
{
    static const struct {
        u8 bytes[4];
        u8 len;
    } cases[] = {
        {{0xC0, 0x80, 0, 0}, 2},
        {{0xE0, 0x80, 0x80, 0}, 3},
        {{0xED, 0xA0, 0x80, 0}, 3},
        {{0xED, 0xBF, 0xBF, 0}, 3},
        {{0xF0, 0x80, 0x80, 0x80}, 4},
        {{0xF4, 0x90, 0x80, 0x80}, 4},
        {{0xF5, 0x80, 0x80, 0x80}, 4},
        {{0x80, 0, 0, 0}, 1},
        {{0xFF, 0, 0, 0}, 1},
    };
    size_t i;

    for (i = 0; i < SAG_ARRAY_LEN(cases); i++) {
        u32 cp;

        SAG_ASSERT_EQ_U64(sag_utf8_decode(cases[i].bytes, cases[i].len,
                                          &cp),
                          1);
        SAG_ASSERT_EQ_U64(cp, sag_utf8_escape_of(cases[i].bytes[0]));
        assert_round_trip(cases[i].bytes, cases[i].len);
    }
}

void test_utf8_truncated_decode(void)
{
    static const u8 two[] = {0xC2};
    static const u8 three1[] = {0xE1};
    static const u8 three2[] = {0xE1, 0x80};
    static const u8 four1[] = {0xF1};
    static const u8 four2[] = {0xF1, 0x80};
    static const u8 four3[] = {0xF1, 0x80, 0x80};

    assert_round_trip(two, sizeof(two));
    assert_round_trip(three1, sizeof(three1));
    assert_round_trip(three2, sizeof(three2));
    assert_round_trip(four1, sizeof(four1));
    assert_round_trip(four2, sizeof(four2));
    assert_round_trip(four3, sizeof(four3));
}

void test_utf8_incremental_valid(void)
{
    static const u8 bytes[] = {0x24, 0xC2, 0xA2, 0xE2, 0x82,
                               0xAC, 0xF0, 0x90, 0x8D, 0x88};
    static const u32 expected[] = {0x24, 0xA2, 0x20AC, 0x10348};
    SagU8Dec dec;
    size_t i;
    size_t out_pos = 0;

    sag_utf8_dec_init(&dec);
    for (i = 0; i < sizeof(bytes); i++) {
        u8 n = sag_utf8_push(&dec, bytes[i]);

        SAG_ASSERT(n <= 1);
        if (n != 0) {
            SAG_ASSERT_EQ_U64(dec.out[0], expected[out_pos]);
            out_pos++;
        }
    }
    SAG_ASSERT_EQ_U64(sag_utf8_finish(&dec), 0);
    SAG_ASSERT_EQ_U64(out_pos, SAG_ARRAY_LEN(expected));
}

void test_utf8_incremental_recovery(void)
{
    SagU8Dec dec;

    sag_utf8_dec_init(&dec);
    SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, 0xE1), 0);
    SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, 0x41), 2);
    SAG_ASSERT_EQ_U64(dec.out[0], sag_utf8_escape_of(0xE1));
    SAG_ASSERT_EQ_U64(dec.out[1], 0x41);

    sag_utf8_dec_init(&dec);
    SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, 0xF0), 0);
    SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, 0x90), 0);
    SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, 0x80), 0);
    SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, 0x41), 4);
    SAG_ASSERT_EQ_U64(dec.out[0], sag_utf8_escape_of(0xF0));
    SAG_ASSERT_EQ_U64(dec.out[1], sag_utf8_escape_of(0x90));
    SAG_ASSERT_EQ_U64(dec.out[2], sag_utf8_escape_of(0x80));
    SAG_ASSERT_EQ_U64(dec.out[3], 0x41);
}

void test_utf8_incremental_finish(void)
{
    static const u8 bytes[] = {0xF1, 0x80, 0x80};
    SagU8Dec dec;
    size_t i;

    sag_utf8_dec_init(&dec);
    for (i = 0; i < sizeof(bytes); i++)
        SAG_ASSERT_EQ_U64(sag_utf8_push(&dec, bytes[i]), 0);
    SAG_ASSERT_EQ_U64(sag_utf8_finish(&dec), sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++)
        SAG_ASSERT_EQ_U64(dec.out[i], sag_utf8_escape_of(bytes[i]));
    SAG_ASSERT_EQ_U64(sag_utf8_finish(&dec), 0);
}

void test_utf8_decode_prev(void)
{
    static const u8 bytes[] = {0x00, 0x41, 0xC2, 0xA2, 0xE2, 0x82,
                               0xAC, 0xF0, 0x90, 0x8D, 0x88, 0xFF};
    static const size_t ends[] = {2, 4, 7, 11, 12};
    static const size_t lens[] = {1, 2, 3, 4, 1};
    static const u32 cps[] = {0x41, 0xA2, 0x20AC, 0x10348, 0xDCFF};
    size_t i;

    for (i = 0; i < SAG_ARRAY_LEN(ends); i++) {
        u32 cp;

        SAG_ASSERT_EQ_U64(sag_utf8_decode_prev(bytes, 1, ends[i], &cp),
                          lens[i]);
        SAG_ASSERT_EQ_U64(cp, cps[i]);
    }
    {
        u32 cp = 1;

        SAG_ASSERT_EQ_U64(sag_utf8_decode_prev(bytes, 1, 1, &cp), 0);
        SAG_ASSERT_EQ_U64(cp, 0);
    }
}

void test_utf8_encode_edges(void)
{
    static const u32 cps[] = {0x00, 0x7F, 0x80, 0x7FF, 0x800,
                              0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF};
    size_t i;

    for (i = 0; i < SAG_ARRAY_LEN(cps); i++) {
        u8 bytes[SAG_UTF8_MAX];
        u32 decoded;
        size_t n = sag_utf8_encode(cps[i], bytes);

        SAG_ASSERT(n != 0);
        SAG_ASSERT_EQ_U64(sag_utf8_len(cps[i]), n);
        SAG_ASSERT_EQ_U64(sag_utf8_decode(bytes, n, &decoded), n);
        SAG_ASSERT_EQ_U64(decoded, cps[i]);
    }
    {
        u8 bytes[SAG_UTF8_MAX];

        SAG_ASSERT_EQ_U64(sag_utf8_encode(0x110000, bytes), 0);
        SAG_ASSERT_EQ_U64(sag_utf8_len(0xFFFFFFFF), 0);
    }
}

void test_utf8_escape_helpers(void)
{
    unsigned int b;

    SAG_ASSERT(!sag_utf8_is_escape(SAG_CP_ESC_LO - 1));
    SAG_ASSERT(!sag_utf8_is_escape(SAG_CP_ESC_HI + 1));
    for (b = 0x80; b <= 0xFF; b++) {
        u32 cp = sag_utf8_escape_of((u8)b);

        SAG_ASSERT(sag_utf8_is_escape(cp));
        SAG_ASSERT_EQ_U64(sag_utf8_escape_byte(cp), b);
    }
}

void test_utf8_validate(void)
{
    static const u8 valid[] = {0x41, 0xC2, 0xA2, 0xF4, 0x8F, 0xBF, 0xBF};
    static const u8 invalid[] = {0x41, 0xE1, 0x41};
    static const u8 truncated[] = {0x41, 0xE1, 0x80};

    SAG_ASSERT_EQ_U64(sag_utf8_validate(NULL, 0), 0);
    SAG_ASSERT_EQ_U64(sag_utf8_validate(valid, sizeof(valid)),
                      sizeof(valid));
    SAG_ASSERT_EQ_U64(sag_utf8_validate(invalid, sizeof(invalid)), 1);
    SAG_ASSERT_EQ_U64(sag_utf8_validate(truncated, sizeof(truncated)), 1);
}

void test_utf8_boundaries(void)
{
    static const u8 bytes[] = {0x41, 0xE2, 0x82, 0xAC, 0x80, 0x42};
    static const bool expected[] = {true, true, false, false,
                                    true, true, true};
    size_t i;

    for (i = 0; i < SAG_ARRAY_LEN(expected); i++)
        SAG_ASSERT(sag_utf8_is_boundary(bytes, sizeof(bytes), i) ==
                   expected[i]);
    SAG_ASSERT(!sag_utf8_is_boundary(bytes, sizeof(bytes),
                                     sizeof(bytes) + 1));
}

void test_utf8_exhaustive_scalars(void)
{
    u32 cp;

    for (cp = 0; cp <= 0x10FFFF; cp++) {
        u8 bytes[SAG_UTF8_MAX];
        u32 decoded;
        size_t n;

        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        n = sag_utf8_encode(cp, bytes);
        SAG_ASSERT(n != 0);
        SAG_ASSERT_EQ_U64(sag_utf8_len(cp), n);
        SAG_ASSERT_EQ_U64(sag_utf8_decode(bytes, n, &decoded), n);
        SAG_ASSERT_EQ_U64(decoded, cp);
    }
}

void test_utf8_exhaustive_surrogates(void)
{
    u32 cp;

    for (cp = 0xD800; cp <= 0xDFFF; cp++) {
        u8 bytes[SAG_UTF8_MAX];
        size_t n = sag_utf8_encode(cp, bytes);

        if (sag_utf8_is_escape(cp)) {
            SAG_ASSERT_EQ_U64(n, 1);
            SAG_ASSERT_EQ_U64(bytes[0], cp - 0xDC00);
        } else {
            SAG_ASSERT_EQ_U64(n, 0);
        }
    }
}

void test_utf8_exhaustive_short_strings(void)
{
    unsigned int a;
    unsigned int b;

    for (a = 0; a <= 0xFF; a++) {
        u8 one[1];

        one[0] = (u8)a;
        assert_round_trip(one, sizeof(one));
    }
    for (a = 0; a <= 0xFF; a++) {
        for (b = 0; b <= 0xFF; b++) {
            u8 two[2];

            two[0] = (u8)a;
            two[1] = (u8)b;
            assert_round_trip(two, sizeof(two));
        }
    }
}
