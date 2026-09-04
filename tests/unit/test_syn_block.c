#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/block.h"
#include "edit/buf.h"
#include "unit/syn_toy.h"

typedef struct SynBlockFixture {
    SynToy toy;
    Buffer buf;
    UnitCtx unit;
} SynBlockFixture;

static u64 find_off(const char *text, const char *needle)
{
    const char *at = strstr(text, needle);

    YEW_ASSERT_NOT_NULL(at);
    return (u64)(at - text);
}

static void toy_replace_rule(SynToy *toy, u32 rule, const char *pattern,
                             u8 op, u16 target, u8 consume)
{
    YewReErr err = {0U, NULL};
    SynRule *r = &toy->rules[rule];

    r->re = yew_re_compile(&toy->arena, pattern, strlen(pattern), 0U, &err);
    YEW_ASSERT_NOT_NULL(r->re);
    r->op = op;
    r->target = target;
    r->consume = consume;
    (void)memset(r->first, 0, sizeof(r->first));
    r->first[(u8)'f' >> 3U] |=
        (u8)(1U << ((u8)'f' & 7U));
    yew_syn_engine_set_def(toy->engine, &toy->def);
}

static void fixture_init(SynBlockFixture *f, const char *text)
{
    SynSettleReport report;
    u64 lines;

    (void)memset(f, 0, sizeof(*f));
    syn_toy_init(&f->toy);
    f->toy.ctxs[SYN_TOY_MAIN].flags = YEW_SYN_CTX_UNIT_SPAN;
    f->toy.ctxs[SYN_TOY_STRING].flags = YEW_SYN_CTX_UNIT_ATOM;
    f->toy.ctxs[SYN_TOY_COMMENT_BLOCK].flags = YEW_SYN_CTX_UNIT_ATOM;
    f->toy.ctxs[SYN_TOY_COMMENT_LINE].flags = YEW_SYN_CTX_UNIT_ATOM;
    f->buf.tb = yew_textbuf_from_bytes((const u8 *)text, strlen(text));
    f->buf.lang = "toy";
    f->buf.tabwidth = 4U;
    yew_syn_buf_init(&f->buf.syn);
    yew_syn_buf_bind(&f->buf.syn, f->toy.engine);
    yew_syn_attach(&f->buf.syn, 1U, f->buf.tb);
    lines = yew_textbuf_line_count(f->buf.tb);
    yew_syn_settle(&f->buf.syn, f->buf.tb, LINENO(0U), LINENO(lines),
                   INT64_MAX, &report);
    YEW_ASSERT(report.fixpoint);
    f->unit.tb = f->buf.tb;
    f->unit.buf = &f->buf;
    yew_block_provider_syntax_install(true);
}

static void fixture_free(SynBlockFixture *f)
{
    yew_block_provider_syntax_install(false);
    yew_syn_detach(&f->buf.syn);
    yew_textbuf_free(f->buf.tb);
    syn_toy_free(&f->toy);
}

void test_syn_block_atom_beats_delimiters_inside_string(void)
{
    static const char text[] = "head\nx = \"first\" + \"a[b\"\ntail\n";
    SynBlockFixture f;
    u64 at = find_off(text, "[b") + 1U;
    Span atom;
    ByteOff match;

    fixture_init(&f, text);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(at), 0U, &atom));
    YEW_ASSERT_EQ_U64(atom.lo, find_off(text, "a[b"));
    YEW_ASSERT_EQ_U64(atom.hi, find_off(text, "\"\ntail") + 1U);
    YEW_ASSERT(!yew_block_match(&f.unit, BYTEOFF(at), false, &match));
    YEW_ASSERT(yew_syn_in_string_or_comment(&f.buf, BYTEOFF(at)));
    YEW_ASSERT(!yew_syn_in_string_or_comment(
        &f.buf, BYTEOFF(find_off(text, "tail"))));
    fixture_free(&f);
}

void test_syn_block_comment_predicate_makes_scope_exact(void)
{
    static const char text[] =
        "before\n"
        "/* ) [ hidden */ [live]\n"
        "after\n";
    SynBlockFixture f;
    u64 hidden = find_off(text, "hidden");
    u64 live = find_off(text, "live");
    Span span;
    ByteOff match;

    fixture_init(&f, text);
    YEW_ASSERT(yew_syn_in_string_or_comment(&f.buf, BYTEOFF(hidden)));
    YEW_ASSERT(!yew_block_match(&f.unit, BYTEOFF(hidden), false, &match));
    YEW_ASSERT(yew_block_match(&f.unit, BYTEOFF(live), false, &match));
    YEW_ASSERT_EQ_U64(match.v, live - 1U);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(hidden), 0U, &span));
    YEW_ASSERT_EQ_U64(span.lo, find_off(text, " ) [ hidden"));
    YEW_ASSERT(span.hi >= find_off(text, "*/") + 2U);
    fixture_free(&f);
}

void test_syn_block_unsettled_and_unbound_use_plain_fallback(void)
{
    static const char text[] = "one\n\n  two\n    three\n";
    SynBlockFixture f;
    Span exact;
    Span fallback;
    u64 at = find_off(text, "three");

    fixture_init(&f, text);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(at), 0U, &exact));
    f.buf.syn.settled_to = LINENO(1U);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(at), 0U, &fallback));
    YEW_ASSERT(fallback.lo <= at && fallback.hi >= at);
    f.buf.syn.settled_to = LINENO(f.buf.syn.entry.len);
    f.buf.syn.lang = YEW_LANG_NONE;
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(at), 0U, &fallback));
    YEW_ASSERT(fallback.lo <= at && fallback.hi >= at);
    f.buf.syn.lang = 1U;
    yew_block_provider_syntax_install(false);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(at), 0U, &fallback));
    YEW_ASSERT(fallback.lo <= at && fallback.hi >= at);
    YEW_ASSERT(exact.lo <= at && exact.hi >= at);
    fixture_free(&f);
}

void test_syn_block_unit_motion_has_no_fixed_points(void)
{
    static const char text[] =
        "head\n"
        "call(\"a[b]\", { nested: true })\n"
        "/* multi\nline */ tail\n";
    SynBlockFixture f;
    ByteOff p = BYTEOFF(0U);
    u64 len;

    fixture_init(&f, text);
    len = yew_textbuf_len(f.buf.tb);
    for (;;) {
        for (u8 alt = 0U; alt < 2U; alt++) {
            ByteOff next = yew_unit_block.next(&f.unit, p, alt != 0U);
            ByteOff prev = yew_unit_block.prev(&f.unit, p, alt != 0U);

            if (p.v < len) {
                YEW_ASSERT(next.v > p.v);
                YEW_ASSERT(next.v <= len);
            } else {
                YEW_ASSERT_EQ_U64(next.v, len);
            }
            if (p.v != 0U) {
                YEW_ASSERT(prev.v < p.v);
            } else {
                YEW_ASSERT_EQ_U64(prev.v, 0U);
            }
        }
        if (p.v == len)
            break;
        p = yew_grapheme_next_boundary(f.buf.tb, p);
    }
    fixture_free(&f);
}

void test_syn_stack_at_reports_prefix_without_eol_transition(void)
{
    SynToy toy;
    SynState state;
    static const char line[] = "x = \"abc\"";

    syn_toy_init(&toy);
    YEW_ASSERT(yew_syn_stack_at(toy.engine, YEW_SYN_STATE_ROOT,
                                (const u8 *)line, strlen(line), 6U,
                                &state));
    YEW_ASSERT_EQ_U64(state.depth, 2U);
    YEW_ASSERT_EQ_U64(state.f[1].ctx, SYN_TOY_STRING);
    YEW_ASSERT(yew_syn_stack_at(toy.engine, YEW_SYN_STATE_ROOT,
                                (const u8 *)line, strlen(line),
                                strlen(line), &state));
    YEW_ASSERT_EQ_U64(state.depth, 1U);
    YEW_ASSERT(!yew_syn_stack_at(toy.engine, YEW_SYN_STATE_ROOT,
                                 (const u8 *)line,
                                 YEW_SYN_LINE_BYTE_CAP + 1U, 0U, &state));
    syn_toy_free(&toy);
}

void test_syn_stack_at_preserves_full_line_end_anchor_semantics(void)
{
    SynToy toy;
    SynState state;
    static const char line[] = "fooX";

    syn_toy_init(&toy);
    toy_replace_rule(&toy, 3U, "foo$", SYN_OP_PUSH, SYN_TOY_STRING, 0U);
    YEW_ASSERT(yew_syn_stack_at(toy.engine, YEW_SYN_STATE_ROOT,
                                (const u8 *)line, strlen(line), 3U,
                                &state));
    YEW_ASSERT_EQ_U64(state.depth, 1U);
    YEW_ASSERT_EQ_U64(state.f[0].ctx, SYN_TOY_MAIN);
    syn_toy_free(&toy);
}

void test_syn_stack_at_consume_capture_can_use_lookahead_bytes(void)
{
    SynToy toy;
    SynState state;
    static const char line[] = "fooX";

    syn_toy_init(&toy);
    toy_replace_rule(&toy, 3U, "(foo)X", SYN_OP_PUSH, SYN_TOY_STRING, 1U);
    YEW_ASSERT(yew_syn_stack_at(toy.engine, YEW_SYN_STATE_ROOT,
                                (const u8 *)line, strlen(line), 3U,
                                &state));
    YEW_ASSERT_EQ_U64(state.depth, 2U);
    YEW_ASSERT_EQ_U64(state.f[1].ctx, SYN_TOY_STRING);
    syn_toy_free(&toy);
}

void test_syn_block_long_line_replay_is_bounded_to_constant_passes(void)
{
    enum { N = 32768 };
    char *text = malloc((size_t)N + 3U);
    SynBlockFixture f;
    Span span;

    YEW_ASSERT_NOT_NULL(text);
    text[0] = '"';
    (void)memset(text + 1U, 'a', N);
    text[N + 1U] = '"';
    text[N + 2U] = '\0';
    fixture_init(&f, text);
    yew_syn_engine_reset_counters(f.toy.engine);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(N / 2U), 0U, &span));
    YEW_ASSERT_EQ_U64(span.lo, 1U);
    YEW_ASSERT_EQ_U64(span.hi, N + 2U);
    YEW_ASSERT(yew_syn_engine_line_calls(f.toy.engine) <= 3U);
    fixture_free(&f);
    free(text);
}

void test_syn_block_multiline_scan_replays_only_boundaries(void)
{
    enum { LINES = 100000 };
    size_t len = 3U + ((size_t)LINES - 2U) * 2U + 2U;
    char *text = malloc(len + 1U);
    SynBlockFixture f;
    Span span;
    u64 at;

    YEW_ASSERT_NOT_NULL(text);
    (void)memcpy(text, "/*\n", 3U);
    for (u32 line = 1U; line + 1U < LINES; line++) {
        text[3U + ((size_t)line - 1U) * 2U] = 'x';
        text[4U + ((size_t)line - 1U) * 2U] = '\n';
    }
    (void)memcpy(text + len - 2U, "*/", 2U);
    text[len] = '\0';
    at = 3U + ((u64)LINES / 2U - 1U) * 2U;

    fixture_init(&f, text);
    yew_syn_engine_reset_counters(f.toy.engine);
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(at), 0U, &span));
    YEW_ASSERT_EQ_U64(span.lo, 2U);
    YEW_ASSERT_EQ_U64(span.hi, len);
    YEW_ASSERT(yew_syn_engine_line_calls(f.toy.engine) <= 3U);
    fixture_free(&f);
    free(text);
}

void test_syn_block_local_settlement_ignores_unrelated_distant_tail(void)
{
    static const char text[] = "\"abc\"\nplain\n/* distant tail\n";
    SynBlockFixture f;
    Span span;

    fixture_init(&f, text);
    f.buf.syn.settled_to = LINENO(1U);
    for (size_t i = 1U; i < f.buf.syn.entry.len; i++)
        f.buf.syn.entry.data[i] = YEW_SYN_STATE_UNKNOWN;
    YEW_ASSERT(yew_block_level(&f.unit, BYTEOFF(2U), 0U, &span));
    YEW_ASSERT_EQ_U64(span.lo, 1U);
    YEW_ASSERT_EQ_U64(span.hi, 5U);
    fixture_free(&f);
}

void test_syn_block_scan_cap_falls_through_to_paragraph(void)
{
    enum { LINES = 100002 };
    char *text = malloc((size_t)LINES * 2U + 1U);
    SynBlockFixture f;
    Span span;

    YEW_ASSERT_NOT_NULL(text);
    for (u32 i = 0U; i < LINES; i++) {
        text[i * 2U] = 'x';
        text[i * 2U + 1U] = '\n';
    }
    text[(size_t)LINES * 2U] = '\0';
    fixture_init(&f, text);
    YEW_ASSERT(yew_block_level(&f.unit,
                               BYTEOFF((u64)(LINES - 1U) * 2U),
                               0U, &span));
    YEW_ASSERT_EQ_U64(span.lo, 0U);
    YEW_ASSERT_EQ_U64(span.hi, (u64)LINES * 2U);
    fixture_free(&f);
    free(text);
}
