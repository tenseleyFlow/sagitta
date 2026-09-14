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

/*
 * Vertical motion lands as far RIGHT as the target line allows, and the
 * last line of a buffer is not an exception.
 *
 * FIELD REPORT: arrowing down from a long column onto a line holding
 * only a closing brace put the caret BEFORE the brace, while every other
 * short line put it after. Only the final line of a buffer lacks the
 * trailing newline that gives the caret somewhere past the text to rest,
 * so `yew_gcol_to_off` answered with the brace itself — the character AT
 * that column, which is its documented job. The caret wants the position
 * after it, and that is this layer's business to ask for.
 */
void test_units_line_motion_clamps_to_the_content_end(void)
{
    static const u8 no_eol[] = "int f(void) { return 50; }\n    }";
    static const u8 with_eol[] = "int f(void) { return 50; }\n    }\n";
    static const UnitFixture cases[] = {
        {no_eol, sizeof(no_eol) - 1U},
        {with_eol, sizeof(with_eol) - 1U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        UnitTestCtx ctx;
        Cursor cursor;
        ByteOff landed;

        unit_ctx_init(&ctx, &cases[i]);
        (void)memset(&cursor, 0, sizeof(cursor));
        /* Column 24 on line 0, deep inside `return 50; }`. */
        cursor.pos = BYTEOFF(24U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (CCol){24U};
        ctx.win.cs.curs.data = &cursor;
        ctx.win.cs.curs.len = 1U;
        ctx.win.cs.primary = 0U;

        landed = yew_unit_line.next(&ctx.unit, cursor.pos, false);
        /* `    }` is line 1 at offset 27; its content ends at 32, after
         * the brace. Both spellings must agree. */
        YEW_ASSERT_EQ_U64(landed.v, 32U);

        ctx.win.cs.curs.data = NULL;
        ctx.win.cs.curs.len = 0U;
        unit_ctx_free(&ctx);
    }
}

/*
 * FIELD REPORT: "I have a goal column on the v in `var acc = zero`, and I
 * arrow up to `fn total`, but the cursor lands before the goal column
 * should be."
 *
 * A goal column is the SCREEN column the caret is trying to hold, and a
 * tab is one grapheme but four cells.  Counting the goal in graphemes
 * makes a tab-indented line and a space-indented line disagree about
 * where a given screen column is, so the caret slides left by three
 * cells for every tab the source line's indent contains.
 *
 * The fixture is the report's own shape: a space-indented line above a
 * tab-indented one, then an indent that mixes both, then a wide space
 * indent.  Every `v`-column position below sits at screen cell 4.
 */
static const u8 units_tab_indent[] =
    "    fn total\n"     /* [0,13)  cell 4 is `f` at 4          */
    "\tvar acc = zero\n" /* [13,29) cell 4 is `v` at 14         */
    "  \tmixed\n"        /* [29,38) cell 4 is `m` at 32         */
    "        wide";      /* [38,50) cell 4 is a space at 42     */

static void tab_ctx_cursor(UnitTestCtx *ctx, Cursor *cursor, u64 pos,
                           u64 goal)
{
    (void)memset(cursor, 0, sizeof(*cursor));
    cursor->pos = BYTEOFF(pos);
    cursor->anchor = cursor->pos;
    cursor->goal_col.v = goal;
    ctx->win.cs.curs.data = cursor;
    ctx->win.cs.curs.len = 1U;
    ctx->win.cs.primary = 0U;
}

static void tab_ctx_release(UnitTestCtx *ctx)
{
    ctx->win.cs.curs.data = NULL;
    ctx->win.cs.curs.len = 0U;
    unit_ctx_free(ctx);
}

void test_units_vertical_goal_keeps_the_screen_column_over_tabs(void)
{
    static const UnitFixture fixture = {units_tab_indent,
                                        sizeof(units_tab_indent) - 1U};
    UnitTestCtx ctx;
    Cursor cursor;

    /* Up from the `v` of `var` lands under it, on the `f` of `fn`. */
    unit_ctx_init(&ctx, &fixture);
    tab_ctx_cursor(&ctx, &cursor, 14U, 4U);
    YEW_ASSERT_EQ_U64(yew_unit_line.prev(&ctx.unit, cursor.pos, false).v,
                      4U);
    tab_ctx_release(&ctx);

    /* And down from the `f` of `fn` lands on the `v` of `var`. */
    unit_ctx_init(&ctx, &fixture);
    tab_ctx_cursor(&ctx, &cursor, 4U, 4U);
    YEW_ASSERT_EQ_U64(yew_unit_line.next(&ctx.unit, cursor.pos, false).v,
                      14U);
    tab_ctx_release(&ctx);

    /* Two spaces then a tab still reach cell 4: the `m` of `mixed`. */
    unit_ctx_init(&ctx, &fixture);
    tab_ctx_cursor(&ctx, &cursor, 14U, 4U);
    YEW_ASSERT_EQ_U64(yew_unit_line.next(&ctx.unit, cursor.pos, false).v,
                      32U);
    tab_ctx_release(&ctx);

    /* And so does a plain eight-space indent. */
    unit_ctx_init(&ctx, &fixture);
    tab_ctx_cursor(&ctx, &cursor, 32U, 4U);
    YEW_ASSERT_EQ_U64(yew_unit_line.next(&ctx.unit, cursor.pos, false).v,
                      42U);
    tab_ctx_release(&ctx);
}

/*
 * A goal column that falls INSIDE a tab's render width has no character
 * of its own.  Rounding left onto the tab is the only answer that stays
 * on a grapheme boundary, which invariant 2 requires of every cursor
 * position, and it must round the same way on every line.
 */
void test_units_vertical_goal_rounds_left_inside_a_tab(void)
{
    static const UnitFixture fixture = {units_tab_indent,
                                        sizeof(units_tab_indent) - 1U};
    static const struct {
        u64 pos;
        u64 goal;
        u64 want;
    } cases[] = {
        /* Cell 2 on the tab-indented line is the tab's third cell. */
        {4U, 2U, 13U},
        /* Cell 3 on `  \t` is the tab's second cell; the tab is at 31. */
        {14U, 3U, 31U},
        /* Cell 1 there is the second space, a cell of its own. */
        {14U, 1U, 30U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        UnitTestCtx ctx;
        Cursor cursor;
        ByteOff landed;

        unit_ctx_init(&ctx, &fixture);
        tab_ctx_cursor(&ctx, &cursor, cases[i].pos, cases[i].goal);
        landed = yew_unit_line.next(&ctx.unit, cursor.pos, false);
        YEW_ASSERT_EQ_U64(landed.v, cases[i].want);
        YEW_ASSERT(yew_is_grapheme_boundary(ctx.unit.tb, landed));
        tab_ctx_release(&ctx);
    }
}

/*
 * The screen column depends on the buffer's own tab width, so a goal
 * column must be measured with `Buffer.tabwidth` and not a constant.
 * Cell 8 on `  \tmixed` is the `m` when a tab is eight cells wide and the
 * `d` when it is four, so the two widths must disagree here.
 */
void test_units_vertical_goal_honours_the_buffer_tab_width(void)
{
    static const UnitFixture fixture = {units_tab_indent,
                                        sizeof(units_tab_indent) - 1U};
    static const struct {
        u32 tabwidth;
        u64 want;
    } cases[] = {
        {8U, 32U},
        {4U, 36U},
        /* A zero tab width is the documented stand-in for the default. */
        {0U, 36U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        UnitTestCtx ctx;
        Cursor cursor;

        unit_ctx_init(&ctx, &fixture);
        ctx.buffer.tabwidth = cases[i].tabwidth;
        /* The `w` of `wide` is cell 8 under any tab width. */
        tab_ctx_cursor(&ctx, &cursor, 46U, 8U);
        YEW_ASSERT_EQ_U64(
            yew_unit_line.prev(&ctx.unit, cursor.pos, false).v,
            cases[i].want);
        tab_ctx_release(&ctx);
    }
}

/*
 * A goal column past the target line's end keeps the behaviour HEAD
 * landed for grapheme columns: the caret rests AFTER the last character,
 * on every line including the buffer's final one, which is the only line
 * without a trailing newline to stand on.
 */
void test_units_vertical_goal_past_the_end_still_clamps_after_the_last(void)
{
    static const UnitFixture fixture = {units_tab_indent,
                                        sizeof(units_tab_indent) - 1U};
    UnitTestCtx ctx;
    Cursor cursor;

    /* Up from the final line to `    fn total`, whose content ends at 12. */
    unit_ctx_init(&ctx, &fixture);
    tab_ctx_cursor(&ctx, &cursor, 42U, 99U);
    YEW_ASSERT_EQ_U64(yew_unit_line.prev(&ctx.unit, cursor.pos, true).v,
                      12U);
    tab_ctx_release(&ctx);

    /* Down onto the final line, which ends at the buffer's end. */
    unit_ctx_init(&ctx, &fixture);
    tab_ctx_cursor(&ctx, &cursor, 32U, 99U);
    YEW_ASSERT_EQ_U64(yew_unit_line.next(&ctx.unit, cursor.pos, false).v,
                      50U);
    tab_ctx_release(&ctx);
}

/*
 * FIELD REPORT, second sighting: with
 *
 *     fn main() -> !int {
 *         0
 *     }
 *
 * the caret after the `0` and Down pressed, the caret lands BEFORE the
 * closing brace instead of after it.
 *
 * Both vertical paths now clamp through yew_ccol_to_off_padded, so this
 * pins the LINE unit and the char unit against the same shape at once.
 * Line 1 is `    0` (offsets 20..25), line 2 is `}` (26..27); after the
 * `0` is offset 25, and the answer on the brace line must be 27 -- past
 * the brace, where the caret can rest.
 */
void test_units_down_from_a_short_line_lands_after_the_brace(void)
{
    static const u8 body[] = "fn main() -> !int {\n    0\n}\n";
    static const UnitFixture fixture = {body, sizeof(body) - 1U};
    UnitTestCtx ctx;
    Cursor cursor;

    unit_ctx_init(&ctx, &fixture);
    (void)memset(&cursor, 0, sizeof(cursor));
    cursor.pos = BYTEOFF(25U);          /* just after the 0 */
    cursor.anchor = cursor.pos;
    cursor.goal_col = (CCol){YEW_CCOL_HERE};    /* lazily measured, as typing leaves it */
    ctx.win.cs.curs.data = &cursor;
    ctx.win.cs.curs.len = 1U;
    ctx.win.cs.primary = 0U;

    YEW_ASSERT_EQ_U64(yew_unit_line.next(&ctx.unit, cursor.pos, false).v,
                      27U);

    ctx.win.cs.curs.data = NULL;
    ctx.win.cs.curs.len = 0U;
    unit_ctx_free(&ctx);
}


/*
 * The reported file, byte for byte.
 *
 * ~/scratch/wolf/ch5/fold.lu ends `\t0\n}` with NO trailing newline, and
 * that final detail is the whole bug: the earlier fixture for this shape
 * ended `}\n`, so the case that actually ships was never covered. A line
 * terminated by a newline has somewhere past its text for the caret to
 * rest; the last line of a file does not, and only the padded clamp
 * supplies it.
 *
 * Caret after the `0` is offset 62 (tab, zero, newline at 60..62). The
 * brace line is a single byte at 63, so Down must land at 64 -- past the
 * brace -- not on it.
 */
void test_units_down_onto_an_unterminated_brace_line(void)
{
    static const u8 body[] =
        "//! check: run(exit=0)\n//! phase: run\n\n"
        "fn main() -> !int {\n\n\t0\n}";
    static const UnitFixture fixture = {body, sizeof(body) - 1U};
    UnitTestCtx ctx;
    Cursor cursor;

    YEW_ASSERT_EQ_U64(sizeof(body) - 1U, 64U);
    unit_ctx_init(&ctx, &fixture);
    (void)memset(&cursor, 0, sizeof(cursor));
    cursor.pos = BYTEOFF(62U);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (CCol){YEW_CCOL_HERE};
    ctx.win.cs.curs.data = &cursor;
    ctx.win.cs.curs.len = 1U;
    ctx.win.cs.primary = 0U;

    /* The line unit, which L mode's Down uses. */
    YEW_ASSERT_EQ_U64(yew_unit_line.next(&ctx.unit, cursor.pos, false).v,
                      64U);

    /* And the cursor path, which W and I modes use. */
    cursor.pos = BYTEOFF(62U);
    cursor.goal_col = (CCol){YEW_CCOL_HERE};
    yew_cursor_down(ctx.buffer.tb, &cursor, 4U);
    YEW_ASSERT_EQ_U64(cursor.pos.v, 64U);

    ctx.win.cs.curs.data = NULL;
    ctx.win.cs.curs.len = 0U;
    unit_ctx_free(&ctx);
}
