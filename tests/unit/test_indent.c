#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/indent.h"
#include "edit/option.h"

typedef struct {
    Buffer buffer;
} IndentFixture;

static void indent_fixture(IndentFixture *fx, const char *text)
{
    (void)memset(fx, 0, sizeof(*fx));
    fx->buffer.tb = yew_textbuf_from_bytes((const u8 *)text,
                                           (u64)strlen(text));
    fx->buffer.tabwidth = 4U;
}

static void indent_fixture_free(IndentFixture *fx)
{
    yew_textbuf_free(fx->buffer.tb);
    fx->buffer.tb = NULL;
}

static Span indent_line(const IndentFixture *fx, u64 line)
{
    return yew_textbuf_line_span(fx->buffer.tb, LINENO(line));
}

static IndentInfo indent_of(const IndentFixture *fx, u64 line)
{
    IndentInfo info;

    YEW_ASSERT(yew_indent_info(fx->buffer.tb, indent_line(fx, line),
                               fx->buffer.tabwidth, &info));
    return info;
}

void test_indent_info_classifies_tabs_spaces_mixed_and_blank(void)
{
    static const char text[] =
        "alpha\n"
        "    four\n"
        "\ttab\n"
        " \tmixed\n"
        "        \n"
        "\n";
    IndentFixture fx;
    IndentInfo info;
    Span line;

    indent_fixture(&fx, text);

    line = indent_line(&fx, 0U);
    info = indent_of(&fx, 0U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT(!info.tabs);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo);
    YEW_ASSERT_EQ_U64(info.width.v, 0U);

    line = indent_line(&fx, 1U);
    info = indent_of(&fx, 1U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT(!info.tabs);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo + 4U);
    YEW_ASSERT_EQ_U64(info.width.v, 4U);

    line = indent_line(&fx, 2U);
    info = indent_of(&fx, 2U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT(info.tabs);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo + 1U);
    YEW_ASSERT_EQ_U64(info.width.v, 4U);

    /* One space then a tab still lands on the first tab stop. */
    line = indent_line(&fx, 3U);
    info = indent_of(&fx, 3U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT(!info.tabs);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo + 2U);
    YEW_ASSERT_EQ_U64(info.width.v, 4U);

    /* A whitespace-only line reports blank, and `first` is its content
     * end rather than the EOL bytes. */
    line = indent_line(&fx, 4U);
    info = indent_of(&fx, 4U);
    YEW_ASSERT(info.blank);
    YEW_ASSERT_EQ_U64(info.first.v, line.hi - 1U);
    YEW_ASSERT_EQ_U64(info.width.v, 8U);

    line = indent_line(&fx, 5U);
    info = indent_of(&fx, 5U);
    YEW_ASSERT(info.blank);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo);
    YEW_ASSERT_EQ_U64(info.width.v, 0U);

    indent_fixture_free(&fx);
}

void test_indent_info_handles_crlf_and_non_ascii_leading_runs(void)
{
    /* U+00A0 NO-BREAK SPACE and U+3000 IDEOGRAPHIC SPACE lead lines the
     * ASCII fast path must hand to the grapheme walk. */
    static const char text[] =
        "  crlf\r\n"
        "   \r\n"
        "\xc2\xa0nbsp\n"
        "\xe3\x80\x80wide\n";
    IndentFixture fx;
    IndentInfo info;
    Span line;

    indent_fixture(&fx, text);

    line = indent_line(&fx, 0U);
    info = indent_of(&fx, 0U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo + 2U);
    YEW_ASSERT_EQ_U64(info.width.v, 2U);

    /* The CRLF is not content: the blank line's `first` stops before it. */
    line = indent_line(&fx, 1U);
    info = indent_of(&fx, 1U);
    YEW_ASSERT(info.blank);
    YEW_ASSERT_EQ_U64(info.first.v, line.hi - 2U);
    YEW_ASSERT_EQ_U64(info.width.v, 3U);

    line = indent_line(&fx, 2U);
    info = indent_of(&fx, 2U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT(!info.tabs);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo + 2U);
    YEW_ASSERT_EQ_U64(info.width.v, 1U);

    line = indent_line(&fx, 3U);
    info = indent_of(&fx, 3U);
    YEW_ASSERT(!info.blank);
    YEW_ASSERT_EQ_U64(info.first.v, line.lo + 3U);
    YEW_ASSERT_EQ_U64(info.width.v, 2U);

    indent_fixture_free(&fx);
}

void test_indent_last_nonwhite_finds_openers_and_skips_eol(void)
{
    static const char text[] =
        "if (x) {\n"
        "    plain\n"
        "   \n";
    IndentFixture fx;
    u8 byte = 0U;

    indent_fixture(&fx, text);
    YEW_ASSERT(yew_indent_last_nonwhite(fx.buffer.tb, indent_line(&fx, 0U),
                                        &byte));
    YEW_ASSERT_EQ_U64(byte, (u64)(u8)'{');
    YEW_ASSERT(yew_indent_last_nonwhite(fx.buffer.tb, indent_line(&fx, 1U),
                                        &byte));
    YEW_ASSERT_EQ_U64(byte, (u64)(u8)'n');
    YEW_ASSERT(!yew_indent_last_nonwhite(fx.buffer.tb,
                                         indent_line(&fx, 2U), &byte));
    indent_fixture_free(&fx);
}

void test_indent_unit_follows_expandtab_and_tabwidth(void)
{
    static const char text[] = "x\n";
    OptVal on = {(u8)YEW_OPT_BOOL, {0}};
    OptVal off = {(u8)YEW_OPT_BOOL, {0}};
    u8 unit[YEW_INDENT_UNIT_MAX];
    const char *err = NULL;
    Ed ed;
    Buffer *doc;

    on.as.b = true;
    off.as.b = false;
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    doc = yew_ed_doc(&ed);
    YEW_ASSERT_NOT_NULL(doc);
    (void)text;

    /* Default: expandtab is clear, so one level is one tab byte. */
    YEW_ASSERT_EQ_U64(yew_indent_unit(doc, unit, (u32)sizeof(unit)), 1U);
    YEW_ASSERT_EQ_U64(unit[0], (u64)(u8)'\t');

    YEW_ASSERT(yew_opt_set_for(&ed, doc, NULL, YEW_OPT_SCOPE_DECLARED,
                               "expandtab", 9U, &on, &err));
    YEW_ASSERT_NULL(err);
    YEW_ASSERT_EQ_U64(yew_indent_unit(doc, unit, (u32)sizeof(unit)), 4U);
    YEW_ASSERT_EQ_MEM(unit, "    ", 4U);

    doc->tabwidth = 8U;
    YEW_ASSERT_EQ_U64(yew_indent_unit(doc, unit, (u32)sizeof(unit)), 8U);
    YEW_ASSERT_EQ_MEM(unit, "        ", 8U);
    /* A buffer too small emits nothing rather than a short indent. */
    YEW_ASSERT_EQ_U64(yew_indent_unit(doc, unit, 4U), 0U);

    YEW_ASSERT(yew_opt_set_for(&ed, doc, NULL, YEW_OPT_SCOPE_DECLARED,
                               "expandtab", 9U, &off, &err));
    YEW_ASSERT_NULL(err);
    YEW_ASSERT_EQ_U64(yew_indent_unit(doc, unit, (u32)sizeof(unit)), 1U);
    YEW_ASSERT_EQ_U64(unit[0], (u64)(u8)'\t');
    YEW_ASSERT_EQ_U64(yew_indent_unit(NULL, unit, (u32)sizeof(unit)), 1U);
    yew_ed_free(&ed);
}

void test_indent_back_lands_on_the_previous_tab_stop(void)
{
    static const char text[] =
        "        eight\n"
        "     five\n"
        "\t\ttabs\n"
        "word here\n";
    IndentFixture fx;
    Span line;

    indent_fixture(&fx, text);

    line = indent_line(&fx, 0U);
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 4U,
                                      BYTEOFF(line.lo + 8U)).v,
                      line.lo + 4U);
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 4U,
                                      BYTEOFF(line.lo + 4U)).v,
                      line.lo);
    /* A ragged indent snaps back to the stop below it, not one byte. */
    line = indent_line(&fx, 1U);
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 4U,
                                      BYTEOFF(line.lo + 5U)).v,
                      line.lo + 4U);
    /* One tab byte is one level whatever tabwidth says. */
    line = indent_line(&fx, 2U);
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 4U,
                                      BYTEOFF(line.lo + 2U)).v,
                      line.lo + 1U);
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 8U,
                                      BYTEOFF(line.lo + 2U)).v,
                      line.lo + 1U);
    /* Past the first non-blank byte it is exactly one grapheme. */
    line = indent_line(&fx, 3U);
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 4U,
                                      BYTEOFF(line.lo + 4U)).v,
                      line.lo + 3U);
    /* At a line start it crosses the EOL exactly as Backspace always did. */
    YEW_ASSERT_EQ_U64(yew_indent_back(fx.buffer.tb, line, 4U,
                                      BYTEOFF(line.lo)).v,
                      line.lo - 1U);
    indent_fixture_free(&fx);
}
