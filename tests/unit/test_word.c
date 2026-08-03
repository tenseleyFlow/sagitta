#include "harness.h"

#include <string.h>

#include "edit/word.h"
#include "unicode/coords.h"

typedef struct {
    const u8 *text;
    size_t len;
    const u64 *stops;
    size_t stop_count;
} WordMotionCase;

#define WORD_CASE(s, positions)                                               \
    {(const u8 *)(s), sizeof(s) - 1U, (positions), SAG_ARRAY_LEN(positions)}

static void assert_motion_case(const WordMotionCase *c, bool alt)
{
    TextBuf *tb = sag_textbuf_from_bytes(c->text, c->len);
    UnitCtx u = {tb, NULL, NULL};
    size_t i;

    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT(c->stop_count >= 2U);
    SAG_ASSERT_EQ_U64(c->stops[0], 0U);
    SAG_ASSERT_EQ_U64(c->stops[c->stop_count - 1U], c->len);
    for (i = 1U; i < c->stop_count; i++) {
        ByteOff next = sag_unit_word.next(&u, BYTEOFF(c->stops[i - 1U]),
                                          alt);

        SAG_ASSERT_EQ_U64(next.v, c->stops[i]);
        SAG_ASSERT(sag_is_grapheme_boundary(tb, next));
    }
    sag_textbuf_free(tb);
}

void test_word_motion_reference_rows(void)
{
    static const u64 punct[] = {0U, 3U, 4U, 7U, 8U, 9U};
    static const u64 spaces[] = {0U, 3U, 4U};
    static const u64 blank[] = {0U, 1U, 2U};
    static const u64 cjk[] = {0U, 3U, 6U, 15U};
    static const u64 han[] = {0U, 3U, 6U};
    static const u64 katakana[] = {0U, 9U};
    static const u64 hangul[] = {0U, 15U};
    static const u64 family[] = {0U, 25U, 26U};
    static const u64 flags[] = {0U, 8U, 16U};
    static const u64 apostrophe[] = {0U, 5U};
    static const u64 hyphen[] = {0U, 4U, 5U, 10U};
    static const u64 number[] = {0U, 8U};
    static const u8 invalid_text[] = {0xFFU, 'A'};
    static const u64 invalid[] = {0U, 1U, 2U};
    static const WordMotionCase cases[] = {
        WORD_CASE("foo.bar()", punct),
        WORD_CASE("a  b", spaces),
        WORD_CASE("\n\n", blank),
        WORD_CASE("\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
                  cjk),
        WORD_CASE("\xe6\xbc\xa2\xe5\xad\x97", han),
        WORD_CASE("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88", katakana),
        WORD_CASE("\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94",
                  hangul),
        WORD_CASE("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
                  "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
                  "\xf0\x9f\x91\xa6x",
                  family),
        WORD_CASE("\xf0\x9f\x87\xa6\xf0\x9f\x87\xba"
                  "\xf0\x9f\x87\xa6\xf0\x9f\x87\xba",
                  flags),
        WORD_CASE("don't", apostrophe),
        WORD_CASE("well-known", hyphen),
        WORD_CASE("1,000.50", number),
        {invalid_text, sizeof(invalid_text), invalid,
         SAG_ARRAY_LEN(invalid)},
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++)
        assert_motion_case(&cases[i], false);
}

void test_word_motion_word_variant(void)
{
    static const u8 text[] = "foo.bar  baz-qux\nzap";
    static const u64 stops[] = {0U, 9U, 17U, 20U};
    static const WordMotionCase c = {
        text, sizeof(text) - 1U, stops, SAG_ARRAY_LEN(stops),
    };
    TextBuf *tb;
    UnitCtx u;
    Span span;
    ByteOff at;

    assert_motion_case(&c, true);
    tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    SAG_ASSERT_NOT_NULL(tb);
    u = (UnitCtx){tb, NULL, NULL};
    span = sag_unit_word.span(&u, BYTEOFF(3U), true);
    SAG_ASSERT_EQ_U64(span.lo, 0U);
    SAG_ASSERT_EQ_U64(span.hi, 7U);
    at = sag_unit_word.prev(&u, BYTEOFF(20U), true);
    SAG_ASSERT_EQ_U64(at.v, 17U);
    at = sag_unit_word.prev(&u, at, true);
    SAG_ASSERT_EQ_U64(at.v, 9U);
    at = sag_unit_word.prev(&u, at, true);
    SAG_ASSERT_EQ_U64(at.v, 0U);
    sag_textbuf_free(tb);
}

static void assert_subwords(const u8 *text, size_t len,
                            const u64 *stops, size_t stop_count)
{
    TextBuf *tb = sag_textbuf_from_bytes(text, len);
    UnitCtx u = {tb, NULL, NULL};
    size_t i;

    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT(stop_count >= 2U);
    SAG_ASSERT_EQ_U64(stops[0], 0U);
    SAG_ASSERT_EQ_U64(stops[stop_count - 1U], len);
    for (i = 1U; i < stop_count; i++) {
        ByteOff next = sag_word_sub_next(&u, BYTEOFF(stops[i - 1U]));

        SAG_ASSERT_EQ_U64(next.v, stops[i]);
        SAG_ASSERT(sag_is_grapheme_boundary(tb, next));
    }
    for (i = stop_count - 1U; i > 0U; i--) {
        ByteOff prev = sag_word_sub_prev(&u, BYTEOFF(stops[i]));

        SAG_ASSERT_EQ_U64(prev.v, stops[i - 1U]);
        SAG_ASSERT(sag_is_grapheme_boundary(tb, prev));
    }
    sag_textbuf_free(tb);
}

void test_word_subword_reference_rows(void)
{
    static const u64 camel[] = {0U, 3U, 6U, 9U};
    static const u64 acronym[] = {0U, 4U, 10U};
    static const u64 snake[] = {0U, 6U, 11U, 12U};
    static const u64 kebab[] = {0U, 6U, 10U};
    static const u64 digits[] = {0U, 2U, 8U};
    static const u64 dunder[] = {0U, 2U, 8U};

    assert_subwords((const u8 *)"fooBarBaz", 9U, camel,
                    SAG_ARRAY_LEN(camel));
    assert_subwords((const u8 *)"HTTPServer", 10U, acronym,
                    SAG_ARRAY_LEN(acronym));
    assert_subwords((const u8 *)"snake_case_x", 12U, snake,
                    SAG_ARRAY_LEN(snake));
    assert_subwords((const u8 *)"kebab-case", 10U, kebab,
                    SAG_ARRAY_LEN(kebab));
    assert_subwords((const u8 *)"v2Model3", 8U, digits,
                    SAG_ARRAY_LEN(digits));
    assert_subwords((const u8 *)"__init__", 8U, dunder,
                    SAG_ARRAY_LEN(dunder));
}
