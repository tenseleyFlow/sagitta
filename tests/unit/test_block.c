#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/block.h"
#include "edit/ed.h"

typedef struct {
    Buffer buffer;
    UnitCtx unit;
} BlockFixture;

static void block_fixture_init(BlockFixture *fixture, const u8 *bytes,
                               u64 len)
{
    (void)memset(fixture, 0, sizeof(*fixture));
    fixture->buffer.tb = yew_textbuf_from_bytes(bytes, len);
    fixture->buffer.tabwidth = 4U;
    fixture->unit.tb = fixture->buffer.tb;
    fixture->unit.buf = &fixture->buffer;
}

static void block_fixture_text(BlockFixture *fixture, const char *text)
{
    block_fixture_init(fixture, (const u8 *)text, strlen(text));
}

static void block_fixture_free(BlockFixture *fixture)
{
    yew_textbuf_free(fixture->buffer.tb);
}

static u64 text_off(const char *text, const char *needle)
{
    const char *found = strstr(text, needle);

    YEW_ASSERT_NOT_NULL(found);
    return (u64)(found - text);
}

static Span block_level(BlockFixture *fixture, u64 at, u32 level)
{
    Span span = {UINT64_MAX, UINT64_MAX};

    YEW_ASSERT(yew_block_level(&fixture->unit, BYTEOFF(at), level, &span));
    return span;
}

static void assert_span(Span actual, u64 lo, u64 hi)
{
    YEW_ASSERT_EQ_U64(actual.lo, lo);
    YEW_ASSERT_EQ_U64(actual.hi, hi);
}

static void assert_scan_budget(i64 elapsed)
{
    char detail[128];

    if (getenv("YEW_TEST_INSTRUMENTED") != NULL)
        return;
    yew_test_count_assertion();
    if (elapsed < INT64_C(1000000))
        return;
    (void)snprintf(detail, sizeof(detail),
                   "block scan elapsed=%lldns budget=1000000ns",
                   (long long)elapsed);
    yew_test_fail(__FILE__, __LINE__, detail);
}

void test_block_paragraph_text_and_blank_runs_are_units(void)
{
    static const char text[] = "first\nsecond\n\n \t\nthird\n";
    BlockFixture fixture;
    Span first;
    Span gap;
    Span third;

    block_fixture_text(&fixture, text);
    first = block_level(&fixture, text_off(text, "second"), 0U);
    gap = block_level(&fixture, text_off(text, " \t"), 0U);
    third = block_level(&fixture, text_off(text, "third"), 0U);
    assert_span(first, 0U, text_off(text, "\n\n") + 1U);
    assert_span(gap, text_off(text, "\n\n") + 1U,
                text_off(text, "third"));
    assert_span(third, text_off(text, "third"), strlen(text));
    block_fixture_free(&fixture);
}

void test_block_indent_absorbs_interior_not_trailing_blank_lines(void)
{
    static const char text[] =
        "def f():\n"
        "\n"
        "    one = 1\n"
        "\n"
        "    two = 2\n"
        "\n"
        "next = 3\n";
    BlockFixture fixture;
    Span statement;
    Span suite;
    u64 one = text_off(text, "    one");
    u64 trailing_blank = text_off(text, "\n\nnext") + 1U;

    block_fixture_text(&fixture, text);
    statement = block_level(&fixture, text_off(text, "one"), 0U);
    suite = block_level(&fixture, text_off(text, "one"), 1U);
    YEW_ASSERT(statement.lo >= one);
    YEW_ASSERT(suite.lo <= one);
    YEW_ASSERT(suite.hi > text_off(text, "two = 2"));
    YEW_ASSERT_EQ_U64(suite.hi, trailing_blank);
    YEW_ASSERT(((const u8 *)text)[suite.hi - 1U] != (u8)' ');
    YEW_ASSERT(suite.hi <= text_off(text, "next = 3"));
    block_fixture_free(&fixture);
}

void test_block_scope_quote_and_comment_suppression(void)
{
    static const char text[] =
        "don't [live]\n"
        "x = 'a[hidden'\n"
        "x = 'odd[live]\n"
        "x = 1; // [hidden]\n"
        "# [hidden]\n"
        "-- [hidden]\n"
        "; [hidden]\n"
        "/* [hidden] */ [live2]\n";
    BlockFixture fixture;
    Span span;
    u64 live = text_off(text, "live]");
    u64 quoted = text_off(text, "hidden'");
    u64 odd = text_off(text, "live]\n");
    u64 comment = text_off(text, "hidden]\n#");
    u64 live2 = text_off(text, "live2");
    ByteOff match;

    block_fixture_text(&fixture, text);
    span = block_level(&fixture, live, 0U);
    assert_span(span, live - 1U, live + strlen("live") + 1U);
    span = block_level(&fixture, quoted, 0U);
    YEW_ASSERT(span.lo < text_off(text, "x = 'a"));
    YEW_ASSERT(span.hi > quoted);
    YEW_ASSERT(!yew_block_match(&fixture.unit, BYTEOFF(quoted), false,
                                &match));
    span = block_level(&fixture, odd, 0U);
    assert_span(span, odd - 1U, odd + strlen("live") + 1U);
    YEW_ASSERT(yew_block_match(&fixture.unit, BYTEOFF(odd), false, &match));
    YEW_ASSERT_EQ_U64(match.v, odd - 1U);
    span = block_level(&fixture, comment, 0U);
    YEW_ASSERT(span.lo < comment - 1U);
    YEW_ASSERT(!yew_block_match(&fixture.unit, BYTEOFF(comment), false,
                                &match));
    span = block_level(&fixture, live2, 0U);
    assert_span(span, live2 - 1U, live2 + strlen("live2") + 1U);
    block_fixture_free(&fixture);
}

void test_block_c_fixture_has_four_hand_computed_levels(void)
{
    static const char text[] =
        "intro\n"
        "\n"
        "void f() {\n"
        "{\n"
        "value;\n"
        "}\n"
        "}\n"
        "\n"
        "outro\n";
    BlockFixture fixture;
    Span levels[4];
    u64 inner_lo = text_off(text, "{\nvalue");
    u64 inner_hi = text_off(text, "}\n}\n") + 1U;
    u64 outer_lo = text_off(text, "void f() {") + strlen("void f() ");
    u64 outer_hi = text_off(text, "}\n\n") + 1U;
    u64 function_lo = text_off(text, "void f()");
    u64 function_hi = text_off(text, "\n\noutro") + 1U;
    u64 at = inner_lo;

    block_fixture_text(&fixture, text);
    for (u32 i = 0U; i < YEW_ARRAY_LEN(levels); i++)
        levels[i] = block_level(&fixture, at, i);
    assert_span(levels[0], inner_lo, inner_hi);
    assert_span(levels[1], outer_lo, outer_hi);
    assert_span(levels[2], function_lo, function_hi);
    assert_span(levels[3], 0U, strlen(text));
    for (u32 i = 1U; i < YEW_ARRAY_LEN(levels); i++) {
        YEW_ASSERT(levels[i].lo <= levels[i - 1U].lo);
        YEW_ASSERT(levels[i].hi >= levels[i - 1U].hi);
        YEW_ASSERT(levels[i].lo < levels[i - 1U].lo ||
                   levels[i].hi > levels[i - 1U].hi);
    }
    block_fixture_free(&fixture);
}

void test_block_plain_text_has_three_distinct_containment_levels(void)
{
    static const char text[] =
        "heading\n"
        "\n"
        "  item\n"
        "    detail\n"
        "  tail\n"
        "\n"
        "\n"
        "ending\n";
    BlockFixture fixture;
    Span levels[3];
    u64 at = text_off(text, "detail");

    block_fixture_text(&fixture, text);
    for (u32 i = 0U; i < YEW_ARRAY_LEN(levels); i++)
        levels[i] = block_level(&fixture, at, i);
    for (u32 i = 1U; i < YEW_ARRAY_LEN(levels); i++) {
        YEW_ASSERT(levels[i].lo <= levels[i - 1U].lo);
        YEW_ASSERT(levels[i].hi >= levels[i - 1U].hi);
        YEW_ASSERT(levels[i].lo < levels[i - 1U].lo ||
                   levels[i].hi > levels[i - 1U].hi);
    }
    YEW_ASSERT(levels[0].lo > 0U);
    YEW_ASSERT(levels[2].hi <= strlen(text));
    block_fixture_free(&fixture);
}

void test_block_scan_budget_falls_through_before_distant_pair(void)
{
    enum { LINES = 100000, CENTER = 50000, DISTANCE = 3000 };
    u8 *text = malloc((size_t)LINES * 8U);
    u64 len = 0U;
    u64 target_lo = 0U;
    u64 target_hi = 0U;
    BlockFixture fixture;
    struct timespec start;
    struct timespec end;
    Span span;
    i64 elapsed;

    YEW_ASSERT_NOT_NULL(text);
    for (u64 line = 0U; line < LINES; line++) {
        if (line == CENTER - DISTANCE) {
            text[len++] = (u8)'{';
        } else if (line == CENTER) {
            target_lo = len;
            (void)memcpy(text + len, "target", 6U);
            len += 6U;
            target_hi = len + 1U;
        } else if (line == CENTER + DISTANCE) {
            text[len++] = (u8)'}';
        }
        text[len++] = (u8)'\n';
    }
    block_fixture_init(&fixture, text, len);
    YEW_ASSERT_EQ_I64(clock_gettime(CLOCK_MONOTONIC, &start), 0);
    span = block_level(&fixture, target_lo + 1U, 0U);
    YEW_ASSERT_EQ_I64(clock_gettime(CLOCK_MONOTONIC, &end), 0);
    elapsed = (i64)(end.tv_sec - start.tv_sec) * INT64_C(1000000000) +
              (i64)end.tv_nsec - (i64)start.tv_nsec;
    assert_span(span, target_lo, target_hi);
    YEW_ASSERT(elapsed >= 0);
    assert_scan_budget(elapsed);
    block_fixture_free(&fixture);
    free(text);
}

void test_block_matching_delimiters_ignores_nested_and_suppressed_pairs(void)
{
    static const char text[] =
        "{ outer [inner] tail }\n"
        "x = '[hidden]' // [comment]\n";
    BlockFixture fixture;
    ByteOff match;
    u64 inner = text_off(text, "inner");
    u64 open = inner - 1U;
    u64 close = inner + strlen("inner");
    u64 hidden = text_off(text, "hidden");

    block_fixture_text(&fixture, text);
    YEW_ASSERT(yew_block_match(&fixture.unit, BYTEOFF(inner), false,
                               &match));
    YEW_ASSERT_EQ_U64(match.v, open);
    YEW_ASSERT(yew_block_match(&fixture.unit, BYTEOFF(inner), true,
                               &match));
    YEW_ASSERT_EQ_U64(match.v, close);
    YEW_ASSERT(!yew_block_match(&fixture.unit, BYTEOFF(hidden), false,
                                &match));
    block_fixture_free(&fixture);
}

void test_block_selection_chain_saturates_at_buffer_for_stack_replay(void)
{
    static const char text[] = "one\n\n  two\n    three\n";
    BlockFixture fixture;
    SelStack stack = {{{0U, 0U}}, 0U};
    Span next;
    u64 at = text_off(text, "three");

    block_fixture_text(&fixture, text);
    for (u32 level = 0U; level < YEW_SEL_DEPTH; level++) {
        next = block_level(&fixture, at, level);
        if (stack.n != 0U &&
            next.lo == stack.s[stack.n - 1U].lo &&
            next.hi == stack.s[stack.n - 1U].hi)
            break;
        stack.s[stack.n++] = next;
    }
    YEW_ASSERT(stack.n >= 3U);
    YEW_ASSERT(stack.n <= YEW_SEL_DEPTH);
    YEW_ASSERT_EQ_U64(stack.s[stack.n - 1U].lo, 0U);
    YEW_ASSERT_EQ_U64(stack.s[stack.n - 1U].hi, strlen(text));
    next = block_level(&fixture, at, stack.n);
    assert_span(next, stack.s[stack.n - 1U].lo,
                stack.s[stack.n - 1U].hi);
    while (stack.n != 0U) {
        Span replay = stack.s[--stack.n];

        YEW_ASSERT(replay.lo <= at);
        YEW_ASSERT(replay.hi >= at);
    }
    YEW_ASSERT_EQ_U64(stack.n, 0U);
    block_fixture_free(&fixture);
}

void test_block_syntax_install_accepts_disabled_provider(void)
{
    static const char text[] = "one\n\n  two\n    three\n";
    BlockFixture fixture;
    Span before;
    Span after;

    block_fixture_text(&fixture, text);
    before = block_level(&fixture, text_off(text, "three"), 0U);
    yew_block_provider_syntax_install(NULL);
    after = block_level(&fixture, text_off(text, "three"), 0U);
    YEW_ASSERT_EQ_U64(after.lo, before.lo);
    YEW_ASSERT_EQ_U64(after.hi, before.hi);
    block_fixture_free(&fixture);
}
