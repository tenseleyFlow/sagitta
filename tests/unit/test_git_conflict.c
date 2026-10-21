#include "harness.h"

#include <string.h>

#include "mod/git/conflict.h"

static const char ordinary[] =
    "before\n<<<<<<< HEAD\ncurrent\n=======\nincoming\n>>>>>>> branch\nafter\n";

static void parse(GitConflictParse *p, const char *s, u32 width)
{
    size_t steps = 0U;
    yew_git_conflict_parse_start(p, (const u8 *)s, strlen(s), width);
    while (yew_git_conflict_parse_step(p, 3U) == YEW_CONFLICT_PENDING)
        YEW_ASSERT(++steps <= strlen(s) + 1U);
}

static void span_is(const char *s, Span span, const char *want)
{
    YEW_ASSERT_EQ_U64(span.hi - span.lo, strlen(want));
    YEW_ASSERT(memcmp(s + span.lo, want, strlen(want)) == 0);
}

void test_git_conflict_parser_ordinary_and_all_choices(void)
{
    GitConflictParse p = {0};
    Bytebuf choice = {0};
    const GitConflictRange *r;
    const char *const expected[] = {"current\n", "incoming\n",
                                   "current\nincoming\n"};
    u32 i;

    parse(&p, ordinary, 0U);
    YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_OK);
    YEW_ASSERT_EQ_U64(p.ranges.len, 1U);
    r = &p.ranges.data[0];
    YEW_ASSERT_EQ_U64(r->line, 1U);
    YEW_ASSERT_EQ_U64(r->width, 7U);
    YEW_ASSERT(!r->has_base);
    span_is(ordinary, r->current, "current\n");
    span_is(ordinary, r->incoming, "incoming\n");
    span_is(ordinary, (Span){0U, r->envelope.lo}, "before\n");
    span_is(ordinary, (Span){r->envelope.hi, strlen(ordinary)}, "after\n");
    for (i = 0U; i < 3U; i++) {
        YEW_ASSERT(yew_git_conflict_choice(&choice, (const u8 *)ordinary,
            strlen(ordinary), r, (GitConflictChoice)i, false));
        YEW_ASSERT_EQ_U64(choice.len, strlen(expected[i]));
        YEW_ASSERT(memcmp(choice.data, expected[i], choice.len) == 0);
    }
    bytebuf_free(&choice);
    yew_git_conflict_parse_drop(&p);
}

void test_git_conflict_parser_diff3_crlf_bom_and_raw_bytes(void)
{
    static const char text[] = "\xef\xbb\xbf<<<<<<< HEAD\r\n"
        "\xff ours \t\r\n||||||| base\r\nold\r\n=======\r\n"
        "\xe7\x8c\xab\r\n>>>>>>> incoming";
    GitConflictParse p = {0};
    Bytebuf choice = {0};
    const GitConflictRange *r;

    parse(&p, text, 0U);
    YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_OK);
    YEW_ASSERT_EQ_U64(p.ranges.len, 1U);
    r = &p.ranges.data[0];
    YEW_ASSERT(r->has_base);
    YEW_ASSERT_EQ_U64(r->envelope.lo, 3U);
    YEW_ASSERT_EQ_U64(r->envelope.hi, sizeof(text) - 1U);
    span_is(text, r->current, "\xff ours \t\r\n");
    span_is(text, r->base, "old\r\n");
    span_is(text, r->incoming, "\xe7\x8c\xab\r\n");
    YEW_ASSERT(yew_git_conflict_choice(&choice, (const u8 *)text,
        sizeof(text) - 1U, r, YEW_CONFLICT_CURRENT, true));
    YEW_ASSERT_EQ_U64(choice.len, 10U);
    bytebuf_free(&choice);
    yew_git_conflict_parse_drop(&p);
}

void test_git_conflict_parser_custom_width_adjacent_and_empty_arms(void)
{
    const char *text = "<<< ours\n===\n>>> theirs\n"
                       "<<< ours\nx\n===\n>>> theirs\n";
    GitConflictParse p = {0};
    Bytebuf choice = {0};

    parse(&p, text, 3U);
    YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_OK);
    YEW_ASSERT_EQ_U64(p.ranges.len, 2U);
    YEW_ASSERT_EQ_U64(p.ranges.data[0].envelope.hi,
                      p.ranges.data[1].envelope.lo);
    YEW_ASSERT(yew_git_conflict_choice(&choice, (const u8 *)text,
        strlen(text), &p.ranges.data[0], YEW_CONFLICT_BOTH, false));
    YEW_ASSERT_EQ_U64(choice.len, 0U);
    YEW_ASSERT(yew_git_conflict_choice(&choice, (const u8 *)text,
        strlen(text), &p.ranges.data[1], YEW_CONFLICT_BOTH, false));
    YEW_ASSERT_EQ_U64(choice.len, 2U);
    YEW_ASSERT(memcmp(choice.data, "x\n", 2U) == 0);
    bytebuf_free(&choice);
    parse(&p, "<<<<<<<<<< A\na\n==========\nb\n>>>>>>>>>> B\n", 0U);
    YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_OK);
    YEW_ASSERT_EQ_U64(p.ranges.len, 1U);
    YEW_ASSERT_EQ_U64(p.width, 10U);
    yew_git_conflict_parse_drop(&p);
}

void test_git_conflict_parser_refuses_malformed_and_binary(void)
{
    const char *const bad[] = {
        "<<<<<<< x\na\n", "=======\n", ">>>>>>> x\n",
        "<<<<<<< x\n<<<<<<< y\n=======\n>>>>>>> z\n",
        "<<<<<<< x\n||||||| a\n||||||| b\n=======\n>>>>>>> z\n",
        "<<<<<<< x\n========\n>>>>>>> z\n",
        "<<<<<<< x\n=======\n>>>>>>> z\n||||||| x\n"
    };
    GitConflictParse p = {0};
    size_t i;
    const u8 binary[] = {'x', 0, 'y'};

    for (i = 0U; i < YEW_ARRAY_LEN(bad); i++) {
        parse(&p, bad[i], 0U);
        YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_INVALID);
        YEW_ASSERT_NOT_NULL(p.error);
    }
    yew_git_conflict_parse_start(&p, binary, sizeof(binary), 0U);
    YEW_ASSERT_EQ_U64(yew_git_conflict_parse_step(&p, 100U),
                      YEW_CONFLICT_BINARY);
    parse(&p, "ordinary <<<<<<< inline\n<<<<<<<operator\n", 0U);
    YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_OK);
    YEW_ASSERT_EQ_U64(p.ranges.len, 0U);
    yew_git_conflict_parse_drop(&p);
}

void test_git_conflict_parser_bounds_and_resumption(void)
{
    GitConflictParse p = {0};
    Bytebuf large = {0};
    size_t i;

    yew_git_conflict_parse_start(&p, NULL, YEW_CONFLICT_BYTES_MAX + 1U, 0U);
    YEW_ASSERT_EQ_U64(p.result, YEW_CONFLICT_LIMIT);
    yew_git_conflict_parse_start(&p, NULL, 0U, 0U);
    YEW_ASSERT_EQ_U64(yew_git_conflict_parse_step(&p, 1U), YEW_CONFLICT_OK);
    for (i = 0U; i < YEW_CONFLICT_LINES_MAX + 1U; i++)
        bytebuf_push_u8(&large, '\n');
    yew_git_conflict_parse_start(&p, large.data, large.len, 0U);
    YEW_ASSERT_EQ_U64(yew_git_conflict_parse_step(&p, 4096U),
                      YEW_CONFLICT_PENDING);
    YEW_ASSERT_EQ_U64(p.at, 4096U);
    YEW_ASSERT_EQ_U64(yew_git_conflict_parse_step(&p, SIZE_MAX),
                      YEW_CONFLICT_LIMIT);
    bytebuf_free(&large);
    yew_git_conflict_parse_drop(&p);
}

void test_git_conflict_choice_eof_boundary_and_atomic_refusal(void)
{
    static const u8 text[] = "oneTWO";
    GitConflictRange r = {0};
    Bytebuf out = {0};

    r.envelope = (Span){0U, 6U};
    r.current = (Span){0U, 3U};
    r.incoming = (Span){3U, 6U};
    YEW_ASSERT(yew_git_conflict_choice(&out, text, 6U, &r,
                                       YEW_CONFLICT_BOTH, true));
    YEW_ASSERT_EQ_U64(out.len, 8U);
    YEW_ASSERT(memcmp(out.data, "one\r\nTWO", 8U) == 0);
    r.incoming.hi = 100U;
    YEW_ASSERT(!yew_git_conflict_choice(&out, text, 6U, &r,
                                        YEW_CONFLICT_BOTH, false));
    YEW_ASSERT_EQ_U64(out.len, 8U);
    YEW_ASSERT(memcmp(out.data, "one\r\nTWO", 8U) == 0);
    bytebuf_free(&out);
}

static void index_record(Bytebuf *b, u32 stage, const char *path)
{
    bytebuf_printf(b, "100644 1234567890123456789012345678901234567890 %u\t",
                    stage);
    bytebuf_append(b, path, strlen(path) + 1U);
}

void test_git_conflict_index_literal_paths_and_atomic_validation(void)
{
    static const char path[] = "-dir/with\ttab\nand newline";
    GitConflictIndex index = {0}, before, empty = {0};
    Bytebuf bytes = {0};
    u32 i;

    for (i = 1U; i <= 3U; i++)
        index_record(&bytes, i, path);
    YEW_ASSERT(yew_git_conflict_index(&index, bytes.data, bytes.len, path));
    for (i = 0U; i < 3U; i++) {
        YEW_ASSERT(index.stage[i].present);
        YEW_ASSERT_EQ_U64(index.stage[i].mode, 0100644U);
        YEW_ASSERT_EQ_U64(strlen(index.stage[i].oid), 40U);
    }
    before = index;
    YEW_ASSERT(!yew_git_conflict_index(&index, bytes.data, bytes.len - 1U, path));
    YEW_ASSERT(yew_git_conflict_index_equal(&index, &before));
    YEW_ASSERT(!yew_git_conflict_index(&index, bytes.data, bytes.len, "other"));
    index_record(&bytes, 2U, path);
    YEW_ASSERT(!yew_git_conflict_index(&index, bytes.data, bytes.len, path));
    YEW_ASSERT(yew_git_conflict_index_equal(&index, &before));
    YEW_ASSERT(yew_git_conflict_index(&index, NULL, 0U, path));
    YEW_ASSERT(yew_git_conflict_index_equal(&index, &empty));
    bytebuf_free(&bytes);
}

void test_git_conflict_index_absent_stages_and_bad_metadata(void)
{
    GitConflictIndex index = {0};
    Bytebuf b = {0};
    index_record(&b, 2U, "file");
    index_record(&b, 3U, "file");
    YEW_ASSERT(yew_git_conflict_index(&index, b.data, b.len, "file"));
    YEW_ASSERT(!index.stage[0].present);
    b.data[0] = '8';
    YEW_ASSERT(!yew_git_conflict_index(&index, b.data, b.len, "file"));
    b.data[0] = '1';
    b.data[8] = 'g';
    YEW_ASSERT(!yew_git_conflict_index(&index, b.data, b.len, "file"));
    bytebuf_free(&b);
}

void test_git_conflict_match_unique_eol_and_ambiguous(void)
{
    static const char *const sources[] = {
        "before\none\ntwo\nafter\n", "before\none\ntwo",
        "one\none\n", "different\n", "", "x\r\ny\r\n"
    };
    static const char *const arms[] = {
        "one\r\ntwo\r\n", "one\ntwo\n", "one\n", "one\n", "", "x\ny\n"
    };
    static const bool valid[] = {true, true, false, false, true, true};
    size_t i;
    for (i = 0U; i < YEW_ARRAY_LEN(sources); i++) {
        GitConflictMatch *w = yew_git_conflict_match_new(
            (const u8 *)sources[i], strlen(sources[i]),
            (const u8 *)arms[i], strlen(arms[i]));
        GitConflictParseResult result;
        Span span;
        bool missing;
        size_t steps = 0U;
        YEW_ASSERT_NOT_NULL(w);
        do {
            result = yew_git_conflict_match_step(w, 2U);
            YEW_ASSERT(++steps < 100U);
        } while (result == YEW_CONFLICT_PENDING);
        YEW_ASSERT((result == YEW_CONFLICT_OK) == valid[i]);
        YEW_ASSERT(yew_git_conflict_match_span(w, &span, &missing) == valid[i]);
        if (valid[i]) {
            YEW_ASSERT(missing == (i == 1U));
            YEW_ASSERT(span.lo <= span.hi && span.hi <= strlen(sources[i]));
        }
        yew_git_conflict_match_free(w);
    }
}

void test_git_conflict_long_line_work_yields(void)
{
    Bytebuf text = {0};
    GitConflictParse p = {0};
    GitConflictMatch *w;
    size_t i, steps = 0U;
    Span span;
    bool missing;

    for (i = 0U; i < 32768U; i++)
        bytebuf_push_u8(&text, '<');
    yew_git_conflict_parse_start(&p, text.data, text.len, 7U);
    YEW_ASSERT_EQ_U64(yew_git_conflict_parse_step(&p, text.len - 1U),
                      YEW_CONFLICT_PENDING);
    YEW_ASSERT_EQ_U64(p.marker_run, text.len - 1U);
    YEW_ASSERT_EQ_U64(yew_git_conflict_parse_step(&p, 1U),
                      YEW_CONFLICT_INVALID);
    YEW_ASSERT_EQ_U64(p.marker_run, text.len);
    yew_git_conflict_parse_drop(&p);

    w = yew_git_conflict_match_new(text.data, text.len, text.data, text.len);
    YEW_ASSERT_NOT_NULL(w);
    /* Two scan passes, then equality itself must yield, even on one line. */
    YEW_ASSERT_EQ_U64(yew_git_conflict_match_step(w, text.len),
                      YEW_CONFLICT_PENDING);
    YEW_ASSERT_EQ_U64(yew_git_conflict_match_step(w, text.len),
                      YEW_CONFLICT_PENDING);
    YEW_ASSERT_EQ_U64(yew_git_conflict_match_step(w, 1U),
                      YEW_CONFLICT_PENDING);
    while (yew_git_conflict_match_step(w, 17U) == YEW_CONFLICT_PENDING)
        YEW_ASSERT(++steps < text.len);
    YEW_ASSERT(steps > 1000U);
    YEW_ASSERT(yew_git_conflict_match_span(w, &span, &missing));
    YEW_ASSERT_EQ_U64(span.lo, 0U);
    YEW_ASSERT_EQ_U64(span.hi, text.len);
    YEW_ASSERT(missing);
    yew_git_conflict_match_free(w);
    bytebuf_free(&text);
}
