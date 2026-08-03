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
    &sag_unit_line,
    &sag_unit_word,
    &sag_unit_block,
    &sag_unit_char,
};

typedef struct {
    Buffer buffer;
    Win win;
    UnitCtx unit;
} UnitTestCtx;

static void unit_ctx_init(UnitTestCtx *ctx, const UnitFixture *fixture)
{
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->buffer.tb = sag_textbuf_from_bytes(fixture->bytes, fixture->len);
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
    sag_textbuf_free(ctx->buffer.tb);
}

static ByteOff next_boundary(const TextBuf *tb, ByteOff at)
{
    if (at.v == sag_textbuf_len(tb))
        return at;
    return sag_grapheme_next_boundary(tb, at);
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
    sag_test_fail(__FILE__, __LINE__, detail);
}

void test_units_registered_mode_mapping(void)
{
    SAG_ASSERT(sag_unit_of_mode(SAG_MODE_L) == &sag_unit_line);
    SAG_ASSERT(sag_unit_of_mode(SAG_MODE_W) == &sag_unit_word);
    SAG_ASSERT(sag_unit_of_mode(SAG_MODE_B) == &sag_unit_block);
    SAG_ASSERT(sag_unit_of_mode(SAG_MODE_I) == &sag_unit_char);
    SAG_ASSERT_NULL(sag_unit_of_mode(SAG_MODE_H));
    SAG_ASSERT_NULL(sag_unit_of_mode(SAG_MODE_E));
    SAG_ASSERT_NULL(sag_unit_of_mode(SAG_MODE_F));
}

void test_units_next_prev_are_monotone_and_terminate(void)
{
    for (size_t f = 0U; f < SAG_ARRAY_LEN(unit_fixtures); f++) {
        UnitTestCtx ctx;
        u64 len;

        unit_ctx_init(&ctx, &unit_fixtures[f]);
        len = sag_textbuf_len(ctx.unit.tb);
        for (size_t e = 0U; e < SAG_ARRAY_LEN(unit_engines); e++) {
            const UnitOps *ops = unit_engines[e];

            for (u8 alt = 0U; alt < 2U; alt++) {
                ByteOff at = BYTEOFF(0U);
                u64 steps = 0U;

                while (at.v < len) {
                    ByteOff moved = ops->next(&ctx.unit, at, alt != 0U);

                    SAG_ASSERT(moved.v > at.v);
                    SAG_ASSERT(moved.v <= len);
                    at = moved;
                    SAG_ASSERT(++steps <= len + 1U);
                }
                SAG_ASSERT_EQ_U64(at.v, len);

                at = BYTEOFF(len);
                steps = 0U;
                while (at.v != 0U) {
                    ByteOff moved = ops->prev(&ctx.unit, at, alt != 0U);

                    SAG_ASSERT(moved.v < at.v);
                    at = moved;
                    SAG_ASSERT(++steps <= len + 1U);
                }
                SAG_ASSERT_EQ_U64(at.v, 0U);
            }
        }
        unit_ctx_free(&ctx);
    }
}

void test_units_results_are_boundaries_and_spans_obey_law(void)
{
    for (size_t f = 0U; f < SAG_ARRAY_LEN(unit_fixtures); f++) {
        UnitTestCtx ctx;
        ByteOff p = BYTEOFF(0U);
        u64 len;

        unit_ctx_init(&ctx, &unit_fixtures[f]);
        len = sag_textbuf_len(ctx.unit.tb);
        for (;;) {
            for (size_t e = 0U; e < SAG_ARRAY_LEN(unit_engines); e++) {
                const UnitOps *ops = unit_engines[e];

                for (u8 alt = 0U; alt < 2U; alt++) {
                    bool use_alt = alt != 0U;
                    ByteOff home = ops->home(&ctx.unit, p, use_alt);
                    ByteOff end = ops->end(&ctx.unit, p, use_alt);
                    ByteOff next = ops->next(&ctx.unit, p, use_alt);
                    ByteOff prev = ops->prev(&ctx.unit, p, use_alt);
                    Span span = ops->span(&ctx.unit, p, use_alt);

                    SAG_ASSERT(home.v <= p.v);
                    SAG_ASSERT(end.v >= p.v);
                    SAG_ASSERT_EQ_U64(span.lo, home.v);
                    SAG_ASSERT_EQ_U64(span.hi, end.v);
                    SAG_ASSERT(sag_is_grapheme_boundary(ctx.unit.tb, home));
                    SAG_ASSERT(sag_is_grapheme_boundary(ctx.unit.tb, end));
                    SAG_ASSERT(sag_is_grapheme_boundary(ctx.unit.tb, next));
                    SAG_ASSERT(sag_is_grapheme_boundary(ctx.unit.tb, prev));
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
    for (size_t f = 0U; f < SAG_ARRAY_LEN(unit_fixtures); f++) {
        UnitTestCtx ctx;
        ByteOff p = BYTEOFF(0U);
        u64 len;

        unit_ctx_init(&ctx, &unit_fixtures[f]);
        len = sag_textbuf_len(ctx.unit.tb);
        for (;;) {
            for (size_t e = 0U; e < SAG_ARRAY_LEN(unit_engines); e++) {
                const UnitOps *ops = unit_engines[e];

                for (u8 alt = 0U; alt < 2U; alt++) {
                    Win before = ctx.win;
                    u64 gen = ctx.unit.tb->gen;
                    ByteOff next = ops->next(&ctx.unit, p, alt != 0U);

                    if (next.v > p.v) {
                        ByteOff back =
                            ops->prev(&ctx.unit, next, alt != 0U);

                        sag_test_count_assertion();
                        assert_roundtrip(ops, f, alt != 0U, p, next, back);
                    }
                    (void)ops->home(&ctx.unit, p, alt != 0U);
                    (void)ops->end(&ctx.unit, p, alt != 0U);
                    (void)ops->span(&ctx.unit, p, alt != 0U);
                    SAG_ASSERT_EQ_U64(ctx.unit.tb->gen, gen);
                    SAG_ASSERT_EQ_MEM(&ctx.win, &before, sizeof(before));
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
    u64 len = sag_textbuf_len(ctx->tb);
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
    SAG_ASSERT(!unit_next_contract_holds(&broken, &ctx.unit, BYTEOFF(0U)));
    unit_ctx_free(&ctx);
}
