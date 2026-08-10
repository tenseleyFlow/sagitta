#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "unicode/coords.h"

typedef struct {
    const u8 *bytes;
    u64 len;
} UnitFixture;

static const u8 units_ascii[] = "alpha beta\n  gamma\n\nomega";
static const u8 units_combining[] = "a\xCC\x81 b\nc";
static const u8 units_cjk[] =
    "\xE6\xBC\xA2\xE5\xAD\x97\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\n"
    "\xE6\xAC\xA1";
static const u8 units_emoji[] =
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
    "\xF0\x9F\x91\xA7 x\n\xF0\x9F\x87\xA6\xF0\x9F\x87\xBA";
static const u8 units_invalid[] = {0xFFU, (u8)'A', (u8)'\n', 0x80U,
                                   (u8)'B'};
static const u8 units_crlf_tabs[] = "\tleft\r\nright\tend\r\n";

static const UnitFixture unit_fixtures[] = {
    {units_ascii, sizeof(units_ascii) - 1U},
    {units_combining, sizeof(units_combining) - 1U},
    {units_cjk, sizeof(units_cjk) - 1U},
    {units_emoji, sizeof(units_emoji) - 1U},
    {units_invalid, sizeof(units_invalid)},
    {units_crlf_tabs, sizeof(units_crlf_tabs) - 1U},
};

static const UnitOps *const unit_engines[] = {
    &yew_unit_line,
    &yew_unit_word,
    &yew_unit_block,
    &yew_unit_char,
};

typedef struct {
    Buffer buffer;
    Win win;
    UnitCtx unit;
} UnitTestCtx;

static void unit_ctx_init(UnitTestCtx *ctx, const UnitFixture *fixture)
{
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->buffer.tb = yew_textbuf_from_bytes(fixture->bytes, fixture->len);
    ctx->buffer.tabwidth = 4U;
    ctx->win.buf = &ctx->buffer;
    ctx->win.vp.rows = 24U;
    ctx->win.vp.cols = 80U;
    ctx->unit.tb = ctx->buffer.tb;
    ctx->unit.buf = &ctx->buffer;
    ctx->unit.win = &ctx->win;
}

static void unit_ctx_free(UnitTestCtx *ctx)
{
    yew_textbuf_free(ctx->buffer.tb);
}

static ByteOff next_boundary(const TextBuf *tb, ByteOff at)
{
    if (at.v == yew_textbuf_len(tb))
        return at;
    return yew_grapheme_next_boundary(tb, at);
}

static void assert_roundtrip(const UnitOps *ops, size_t fixture, bool alt,
                             ByteOff p, ByteOff next, ByteOff back)
{
    char detail[192];

    if (back.v <= p.v)
        return;
    (void)snprintf(detail, sizeof(detail),
                   "roundtrip engine=%s fixture=%zu alt=%u p=%llu "
                   "next=%llu back=%llu",
                   ops->name, fixture, alt ? 1U : 0U,
                   (unsigned long long)p.v, (unsigned long long)next.v,
                   (unsigned long long)back.v);
    yew_test_fail(__FILE__, __LINE__, detail);
}

void test_units_registered_mode_mapping(void)
{
    YEW_ASSERT(yew_unit_of_mode(YEW_MODE_L) == &yew_unit_line);
    YEW_ASSERT(yew_unit_of_mode(YEW_MODE_W) == &yew_unit_word);
    YEW_ASSERT(yew_unit_of_mode(YEW_MODE_B) == &yew_unit_block);
    YEW_ASSERT(yew_unit_of_mode(YEW_MODE_I) == &yew_unit_char);
    YEW_ASSERT_NULL(yew_unit_of_mode(YEW_MODE_H));
    YEW_ASSERT_NULL(yew_unit_of_mode(YEW_MODE_E));
    YEW_ASSERT_NULL(yew_unit_of_mode(YEW_MODE_F));
}

void test_units_char_alt_projects_codepoints_to_graphemes(void)
{
    static const u8 bytes[] = {
        0xE6U, 0xBCU, 0xA2U,             /* Han */
        (u8)'e', 0xCCU, 0x81U,           /* e + combining acute */
        (u8)'\r', (u8)'\n',
        0xF0U, 0x9FU, 0x87U, 0xA6U,     /* regional indicator A */
        0xF0U, 0x9FU, 0x87U, 0xBAU,     /* regional indicator U */
        0xEDU, 0xA0U, 0x80U, 0xFFU,     /* four escaped bytes */
        (u8)'A',
    };
    static const u64 stops[] = {
        0U, 3U, 6U, 8U, 16U, 17U, 18U, 19U, 20U, 21U,
    };
    static const u64 interiors[] = {1U, 2U, 4U, 5U, 7U, 12U};
    UnitFixture fixture = {bytes, sizeof(bytes)};
    UnitTestCtx ctx;

    unit_ctx_init(&ctx, &fixture);
    for (size_t i = 0U; i + 1U < YEW_ARRAY_LEN(stops); i++) {
        ByteOff at = BYTEOFF(stops[i]);
        ByteOff next = yew_unit_char.next(&ctx.unit, at, true);
        Span alt_span = yew_unit_char.span(&ctx.unit, at, true);
        Span plain_span = yew_unit_char.span(&ctx.unit, at, false);

        YEW_ASSERT_EQ_U64(next.v, stops[i + 1U]);
        YEW_ASSERT_EQ_U64(yew_unit_char.next(&ctx.unit, at, false).v,
                          next.v);
        YEW_ASSERT_EQ_U64(alt_span.lo, stops[i]);
        YEW_ASSERT_EQ_U64(alt_span.hi, stops[i + 1U]);
        YEW_ASSERT_EQ_U64(plain_span.lo, alt_span.lo);
        YEW_ASSERT_EQ_U64(plain_span.hi, alt_span.hi);
        YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, next));
    }
    for (size_t i = YEW_ARRAY_LEN(stops) - 1U; i != 0U; i--) {
        ByteOff at = BYTEOFF(stops[i]);
        ByteOff prev = yew_unit_char.prev(&ctx.unit, at, true);

        YEW_ASSERT_EQ_U64(prev.v, stops[i - 1U]);
        YEW_ASSERT_EQ_U64(yew_unit_char.prev(&ctx.unit, at, false).v,
                          prev.v);
        YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, prev));
    }
    {
        Span end = yew_unit_char.span(&ctx.unit, BYTEOFF(sizeof(bytes)),
                                      true);

        YEW_ASSERT_EQ_U64(end.lo, stops[YEW_ARRAY_LEN(stops) - 2U]);
        YEW_ASSERT_EQ_U64(end.hi, sizeof(bytes));
    }
    for (size_t i = 0U; i < YEW_ARRAY_LEN(interiors); i++)
        YEW_ASSERT(!yew_is_grapheme_boundary(ctx.unit.tb,
                                             BYTEOFF(interiors[i])));
    unit_ctx_free(&ctx);
}

void test_units_next_prev_are_monotone_and_terminate(void)
{
    for (size_t f = 0U; f < YEW_ARRAY_LEN(unit_fixtures); f++) {
        UnitTestCtx ctx;
        u64 len;

        unit_ctx_init(&ctx, &unit_fixtures[f]);
        len = yew_textbuf_len(ctx.unit.tb);
        for (size_t e = 0U; e < YEW_ARRAY_LEN(unit_engines); e++) {
            const UnitOps *ops = unit_engines[e];

            for (u8 alt = 0U; alt < 2U; alt++) {
                ByteOff at = BYTEOFF(0U);
                u64 steps = 0U;

                while (at.v < len) {
                    ByteOff moved = ops->next(&ctx.unit, at, alt != 0U);

                    YEW_ASSERT(moved.v > at.v);
                    YEW_ASSERT(moved.v <= len);
                    at = moved;
                    YEW_ASSERT(++steps <= len + 1U);
                }
                YEW_ASSERT_EQ_U64(at.v, len);

                at = BYTEOFF(len);
                steps = 0U;
                while (at.v != 0U) {
                    ByteOff moved = ops->prev(&ctx.unit, at, alt != 0U);

                    YEW_ASSERT(moved.v < at.v);
                    at = moved;
                    YEW_ASSERT(++steps <= len + 1U);
                }
                YEW_ASSERT_EQ_U64(at.v, 0U);
            }
        }
        unit_ctx_free(&ctx);
    }
}

void test_units_results_are_boundaries_and_spans_obey_law(void)
{
    for (size_t f = 0U; f < YEW_ARRAY_LEN(unit_fixtures); f++) {
        UnitTestCtx ctx;
        ByteOff p = BYTEOFF(0U);
        u64 len;

        unit_ctx_init(&ctx, &unit_fixtures[f]);
        len = yew_textbuf_len(ctx.unit.tb);
        for (;;) {
            for (size_t e = 0U; e < YEW_ARRAY_LEN(unit_engines); e++) {
                const UnitOps *ops = unit_engines[e];

                for (u8 alt = 0U; alt < 2U; alt++) {
                    bool use_alt = alt != 0U;
                    ByteOff home = ops->home(&ctx.unit, p, use_alt);
                    ByteOff end = ops->end(&ctx.unit, p, use_alt);
                    ByteOff next = ops->next(&ctx.unit, p, use_alt);
                    ByteOff prev = ops->prev(&ctx.unit, p, use_alt);
                    Span span = ops->span(&ctx.unit, p, use_alt);

                    YEW_ASSERT(home.v <= p.v);
                    YEW_ASSERT(end.v >= p.v);
                    YEW_ASSERT_EQ_U64(span.lo, home.v);
                    YEW_ASSERT_EQ_U64(span.hi, end.v);
                    YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, home));
                    YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, end));
                    YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, next));
                    YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, prev));
                }
            }
            if (p.v == len)
                break;
            p = next_boundary(ctx.unit.tb, p);
        }
        unit_ctx_free(&ctx);
    }
}

void test_units_roundtrip_and_purity_hold_for_every_engine(void)
{
    for (size_t f = 0U; f < YEW_ARRAY_LEN(unit_fixtures); f++) {
        UnitTestCtx ctx;
        ByteOff p = BYTEOFF(0U);
        u64 len;

        unit_ctx_init(&ctx, &unit_fixtures[f]);
        len = yew_textbuf_len(ctx.unit.tb);
        for (;;) {
            for (size_t e = 0U; e < YEW_ARRAY_LEN(unit_engines); e++) {
                const UnitOps *ops = unit_engines[e];

                for (u8 alt = 0U; alt < 2U; alt++) {
                    Win before = ctx.win;
                    u64 gen = ctx.unit.tb->gen;
                    ByteOff next = ops->next(&ctx.unit, p, alt != 0U);

                    if (next.v > p.v) {
                        ByteOff back =
                            ops->prev(&ctx.unit, next, alt != 0U);

                        yew_test_count_assertion();
                        assert_roundtrip(ops, f, alt != 0U, p, next, back);
                    }
                    (void)ops->home(&ctx.unit, p, alt != 0U);
                    (void)ops->end(&ctx.unit, p, alt != 0U);
                    (void)ops->span(&ctx.unit, p, alt != 0U);
                    YEW_ASSERT_EQ_U64(ctx.unit.tb->gen, gen);
                    YEW_ASSERT_EQ_MEM(&ctx.win, &before, sizeof(before));
                }
            }
            if (p.v == len)
                break;
            p = next_boundary(ctx.unit.tb, p);
        }
        unit_ctx_free(&ctx);
    }
}

static ByteOff broken_fixed_next(UnitCtx *u, ByteOff p, bool alt)
{
    (void)u;
    (void)alt;
    return p;
}

static bool unit_next_contract_holds(const UnitOps *ops, UnitCtx *ctx,
                                     ByteOff p)
{
    u64 len = yew_textbuf_len(ctx->tb);
    ByteOff next = ops->next(ctx, p, false);

    return p.v == len ? next.v == len : next.v > p.v && next.v <= len;
}

void test_units_conformance_rejects_fixed_point_engine(void)
{
    static const UnitOps broken = {
        "broken", broken_fixed_next, broken_fixed_next, broken_fixed_next,
        broken_fixed_next, NULL,
    };
    UnitTestCtx ctx;

    unit_ctx_init(&ctx, &unit_fixtures[0]);
    YEW_ASSERT(!unit_next_contract_holds(&broken, &ctx.unit, BYTEOFF(0U)));
    unit_ctx_free(&ctx);
}
