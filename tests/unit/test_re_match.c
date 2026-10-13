/*
 * Sprint 20 DoD 4: the golden match table.
 *
 * Every row is (pattern, input, flags, expected span, expected captures).
 * Captures are compared element-wise, not just counted — a run that gets
 * the whole match right and group 2 wrong is a failure, and that is
 * exactly the class of bug a "did it match?" test misses.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "search/regex_internal.h"
#include "text/piece.h"
#include "util/arena.h"

#include "re_golden.h"

static void re_test_put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8U);
    p[2] = (u8)(v >> 16U);
    p[3] = (u8)(v >> 24U);
}

static bool re_test_unpack_rejected(const Bytebuf *packed)
{
    Arena arena;
    bool rejected;

    arena_init(&arena);
    rejected = yew_re_unpack(&arena, packed->data, packed->len, NULL) == NULL;
    arena_free_all(&arena);
    return rejected;
}


static void check_row(const ReRow *row, bool use_textbuf)
{
    Arena arena;
    YewReErr err = {0, NULL};
    YewRe *re;
    YewReMatch m;
    YewReInput in;
    TextBuf *tb = NULL;
    size_t ilen = strlen(row->input);
    bool got;

    arena_init(&arena);
    re = yew_re_compile(&arena, row->pat, strlen(row->pat), row->flags,
                        &err);
    if (re == NULL) {
        (void)fprintf(stderr, "compile failed for /%s/: %s\n", row->pat,
                      err.msg == NULL ? "?" : err.msg);
        YEW_ASSERT_NOT_NULL(re);
        arena_free_all(&arena);
        return;
    }
    if (use_textbuf) {
        tb = yew_textbuf_from_bytes((const u8 *)row->input, (u64)ilen);
        in = yew_re_input_textbuf(tb);
    } else {
        in = yew_re_input_bytes((const u8 *)row->input, (u64)ilen);
    }
    (void)memset(&m, 0, sizeof(m));
    got = yew_re_search(re, &in, BYTEOFF(0U), &m);
    if (got != row->match) {
        (void)fprintf(stderr,
                      "/%s/ on \"%s\"%s: expected %s, got %s\n", row->pat,
                      row->input, use_textbuf ? " [textbuf]" : "",
                      row->match ? "match" : "no match",
                      got ? "match" : "no match");
    }
    YEW_ASSERT(got == row->match);
    if (!row->match) {
        if (tb != NULL)
            yew_textbuf_free(tb);
        arena_free_all(&arena);
        return;
    }
    if (m.g[0].lo != (u64)row->lo || m.g[0].hi != (u64)row->hi)
        (void)fprintf(stderr, "/%s/ on \"%s\": span %llu..%llu, want %lld..%lld\n",
                      row->pat, row->input,
                      (unsigned long long)m.g[0].lo,
                      (unsigned long long)m.g[0].hi,
                      (long long)row->lo, (long long)row->hi);
    YEW_ASSERT_EQ_U64(m.g[0].lo, (u64)row->lo);
    YEW_ASSERT_EQ_U64(m.g[0].hi, (u64)row->hi);
    if (row->g1lo != NO) {
        YEW_ASSERT_EQ_U64(m.g[1].lo, (u64)row->g1lo);
        YEW_ASSERT_EQ_U64(m.g[1].hi, (u64)row->g1hi);
    }
    if (row->g2lo != NO) {
        YEW_ASSERT_EQ_U64(m.g[2].lo, (u64)row->g2lo);
        YEW_ASSERT_EQ_U64(m.g[2].hi, (u64)row->g2hi);
    }
    if (tb != NULL)
        yew_textbuf_free(tb);
    arena_free_all(&arena);
}

void test_re_golden_table_bytes(void)
{
    size_t i;

    /* The table is the contract; keep it above the DoD floor. */
    YEW_ASSERT(YEW_ARRAY_LEN(rows) >= 130U);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++)
        check_row(&rows[i], false);
}

void test_re_golden_table_textbuf(void)
{
    size_t i;

    /* Same rows through the piece tree: the engine must read identically
     * through TextIter as it does over a flat array. */
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++)
        check_row(&rows[i], true);
}

void test_re_leftmost_first_is_pinned(void)
{
    Arena arena;
    YewRe *re;
    YewReMatch m;
    YewReInput in = yew_re_input_bytes((const u8 *)"ab", 2U);

    arena_init(&arena);
    /* Leftmost-FIRST, not leftmost-longest: earlier branch wins ties.
     * A POSIX engine would answer 0..2 here. */
    re = yew_re_compile(&arena, "a|ab", 4U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].hi, 1U);

    re = yew_re_compile(&arena, "ab|a", 4U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].hi, 2U);
    arena_free_all(&arena);
}

void test_re_empty_width_loop_terminates(void)
{
    Arena arena;
    YewRe *re;
    YewReMatch m;
    YewReInput in = yew_re_input_bytes((const u8 *)"aaaa", 4U);

    /*
     * DoD 9.  `(a*)*` chains SPLIT/JMP without consuming input.  This
     * completes only because addthread's sparse_has check lives INSIDE
     * its loop. The deep-subject control below protects that placement
     * against an accidentally shallow-only regression.
     */
    arena_init(&arena);
    re = yew_re_compile(&arena, "(a*)*", 5U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 4U);

    re = yew_re_compile(&arena, "(|a)*", 5U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));

    re = yew_re_compile(&arena, "(a?)*b", 6U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(!yew_re_search(re, &in, BYTEOFF(0U), &m));
    arena_free_all(&arena);
}

void test_re_empty_width_deep_subject(void)
{
    enum { DEEP_SUBJECT_BYTES = 100000U };
    Arena arena;
    YewRe *re;
    YewReMatch m;
    YewReInput in;
    TextBuf *tb;
    u8 *subject;

    /* Sprint 58 F06 Q2: run the empty-width closure through the TextIter
     * path over the stated 100,000-byte subject, not only a toy literal. */
    subject = yew_xmalloc(DEEP_SUBJECT_BYTES);
    (void)memset(subject, 'a', DEEP_SUBJECT_BYTES);
    tb = yew_textbuf_from_bytes(subject, DEEP_SUBJECT_BYTES);
    yew_xfree(subject);
    YEW_ASSERT_NOT_NULL(tb);

    arena_init(&arena);
    re = yew_re_compile(&arena, "(a*)*", 5U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    in = yew_re_input_textbuf(tb);
    (void)memset(&m, 0, sizeof(m));
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, DEEP_SUBJECT_BYTES);

    arena_free_all(&arena);
    yew_textbuf_free(tb);
}

void test_re_match_at_is_anchored(void)
{
    Arena arena;
    YewRe *re;
    YewReMatch m;
    YewReInput in = yew_re_input_bytes((const u8 *)"xxabc", 5U);

    arena_init(&arena);
    re = yew_re_compile(&arena, "abc", 3U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    /* search finds it; match_at only succeeds at the exact offset. */
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT(!yew_re_match_at(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT(!yew_re_match_at(re, &in, BYTEOFF(1U), &m));
    YEW_ASSERT(yew_re_match_at(re, &in, BYTEOFF(2U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].hi, 5U);
    arena_free_all(&arena);
}

void test_re_long_literal_bypasses_vm_limit(void)
{
    enum { LITERAL_LEN = 4240U, PREFIX_LEN = 3U, SUFFIX_LEN = 3U };
    u8 literal[LITERAL_LEN];
    u8 subject[PREFIX_LEN + LITERAL_LEN + SUFFIX_LEN];
    Arena arena;
    Arena restored_arena;
    Arena truncated_arena;
    Bytebuf pattern;
    Bytebuf packed;
    YewReErr err = {0};
    YewReWorkspace workspace;
    YewReMatch match;
    YewReInput input;
    YewRe *re;
    YewRe *literal_re;
    YewRe *limit_re;
    YewRe *restored;
    TextBuf *tb;
    u8 *limit_literal;
    size_t used = 0U;
    size_t i;
    u64 at;
    u64 end = 0U;

    for (i = 0U; i < sizeof(literal); i++)
        literal[i] = (u8)('a' + (i * 17U) % 26U);
    literal[0] = 'Q';
    literal[17] = '*';
    literal[100] = '\n';
    literal[101] = 0U;
    literal[102] = 0x80U;
    literal[103] = 0xFFU;
    literal[104] = '\\';
    literal[105] = '_';
    (void)memcpy(subject, "xx ", PREFIX_LEN);
    (void)memcpy(subject + PREFIX_LEN, literal, sizeof(literal));
    (void)memcpy(subject + PREFIX_LEN + LITERAL_LEN, " yy", SUFFIX_LEN);

    bytebuf_init(&pattern);
    bytebuf_init(&packed);
    yew_re_quote(&pattern, literal, sizeof(literal));
    arena_init(&arena);
    re = yew_re_compile(&arena, (const char *)pattern.data, pattern.len,
                        0U, &err);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT_EQ_U64(yew_re_whole_literal_bytes(re), LITERAL_LEN);
    YEW_ASSERT_EQ_U64(yew_re_min_len(re), LITERAL_LEN);
    YEW_ASSERT(yew_re_has_upper_literal(re));

    input = yew_re_input_bytes(subject, sizeof(subject));
    YEW_ASSERT(yew_re_search(re, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);
    YEW_ASSERT_EQ_U64(match.g[0].hi, PREFIX_LEN + LITERAL_LEN);
    YEW_ASSERT(yew_re_pike_run(re, &input, PREFIX_LEN, &match));
    YEW_ASSERT_EQ_U64(match.g[0].hi, PREFIX_LEN + LITERAL_LEN);
    YEW_ASSERT(yew_re_pike_run_ex(re, &input, 0U, false, &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);
    YEW_ASSERT_EQ_I64(yew_re_dfa_test(re, &input, 0U), YEW_DFA_YES);
    YEW_ASSERT_EQ_I64(yew_re_dfa_find_end(re, &input, 0U, &end),
                      YEW_DFA_YES);
    YEW_ASSERT_EQ_U64(end, PREFIX_LEN + LITERAL_LEN);
    YEW_ASSERT(yew_re_match_at(re, &input, BYTEOFF(PREFIX_LEN), &match));
    YEW_ASSERT(!yew_re_match_at(re, &input, BYTEOFF(PREFIX_LEN - 1U),
                                &match));
    yew_re_workspace_init(&workspace);
    YEW_ASSERT(yew_re_match_at_ws(&workspace, re, &input,
                                  BYTEOFF(PREFIX_LEN), &match));
    yew_re_workspace_free(&workspace);
    YEW_ASSERT(yew_re_search_back(re, &input, BYTEOFF(sizeof(subject)),
                                  &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);
    YEW_ASSERT(!yew_re_search(re, &input, BYTEOFF(PREFIX_LEN + 1U),
                              &match));

    /* Force the literal across hundreds of piece boundaries.  The long
     * path streams KMP state between chunks; it must never materialize the
     * buffer or depend on a fixed-size carry window. */
    tb = yew_textbuf_new();
    at = sizeof(subject);
    while (at != 0U) {
        u64 take = at < 13U ? at : 13U;

        at -= take;
        yew_textbuf_insert(tb, BYTEOFF(0U), subject + at, take);
    }
    /* YEW-F-087: end-appends coalesce in the add store.  Reverse prepends
     * retain the same subject bytes while forcing the promised seams. */
    YEW_ASSERT(yew_textbuf_piece_count(tb) >= 100U);
    input = yew_re_input_textbuf(tb);
    YEW_ASSERT(yew_re_search(re, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);
    YEW_ASSERT(yew_re_match_at(re, &input, BYTEOFF(PREFIX_LEN), &match));
    YEW_ASSERT(yew_re_search_back(re, &input, BYTEOFF(sizeof(subject)),
                                  &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);
    yew_textbuf_free(tb);

    /* Long whole literals are cacheable.  REP2 adds only the variable
     * literal payload; REP1 remains byte-identical for ordinary patterns. */
    YEW_ASSERT(yew_re_pack(re, &packed));
    YEW_ASSERT(packed.len > LITERAL_LEN);
    YEW_ASSERT(memcmp(packed.data, "YEWREP2", 7U) == 0);
    arena_init(&truncated_arena);
    YEW_ASSERT_NULL(yew_re_unpack(&truncated_arena, packed.data,
                                  packed.len - 1U, NULL));
    arena_free_all(&truncated_arena);
    arena_init(&restored_arena);
    restored = yew_re_unpack(&restored_arena, packed.data, packed.len, &used);
    YEW_ASSERT_NOT_NULL(restored);
    YEW_ASSERT_EQ_U64(used, packed.len);
    input = yew_re_input_bytes(subject, sizeof(subject));
    YEW_ASSERT(yew_re_search(restored, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);
    arena_free_all(&restored_arena);

    literal_re = yew_re_compile(&arena, (const char *)literal,
                                sizeof(literal), YEW_RE_LITERAL, &err);
    YEW_ASSERT_NOT_NULL(literal_re);
    YEW_ASSERT(yew_re_search(literal_re, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, PREFIX_LEN);

    subject[PREFIX_LEN + LITERAL_LEN / 2U] ^= 1U;
    input = yew_re_input_bytes(subject, sizeof(subject));
    YEW_ASSERT(!yew_re_search(re, &input, BYTEOFF(0U), &match));

    /* Quoting expands controls fourfold.  The semantic 64 KiB literal
     * limit still round-trips even though its safe textual spelling is
     * 256 KiB; non-literal patterns retain the ordinary syntax cap. */
    limit_literal = yew_xmalloc(YEW_RE_MAX_PATTERN);
    (void)memset(limit_literal, '\n', YEW_RE_MAX_PATTERN);
    pattern.len = 0U;
    yew_re_quote(&pattern, limit_literal, YEW_RE_MAX_PATTERN);
    YEW_ASSERT_EQ_U64(pattern.len, (u64)YEW_RE_MAX_PATTERN * 4U);
    limit_re = yew_re_compile(&arena, (const char *)pattern.data,
                              pattern.len, 0U, &err);
    YEW_ASSERT_NOT_NULL(limit_re);
    YEW_ASSERT_EQ_U64(yew_re_whole_literal_bytes(limit_re),
                      YEW_RE_MAX_PATTERN);
    input = yew_re_input_bytes(limit_literal, YEW_RE_MAX_PATTERN);
    YEW_ASSERT(yew_re_search(limit_re, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].hi, YEW_RE_MAX_PATTERN);
    yew_xfree(limit_literal);

    arena_free_all(&arena);
    bytebuf_free(&packed);
    bytebuf_free(&pattern);
}

void test_re_whole_literals_follow_utf8_decoder_boundaries(void)
{
    enum { LONG_PREFIX = 4094U, LONG_LEN = LONG_PREFIX + 1U };
    static const u8 valid_cent[] = {0xC2U, 0xA2U};
    static const u8 malformed_prefix[] = {0xE0U, 0x80U, 'x'};
    static const u8 raw_c2[] = {0xC2U};
    static const u8 raw_cont[] = {0x80U};
    u8 long_literal[LONG_LEN];
    u8 long_subject[LONG_LEN + 1U];
    Arena arena;
    Bytebuf pattern;
    YewReMatch match;
    YewReInput input;
    YewRe *short_lead;
    YewRe *short_cont;
    YewRe *long_lead;
    TextBuf *tb;
    u64 at;

    arena_init(&arena);
    bytebuf_init(&pattern);

    yew_re_quote(&pattern, raw_c2, sizeof(raw_c2));
    short_lead = yew_re_compile(&arena, (const char *)pattern.data,
                                pattern.len, 0U, NULL);
    YEW_ASSERT_NOT_NULL(short_lead);
    input = yew_re_input_bytes(valid_cent, sizeof(valid_cent));
    YEW_ASSERT(!yew_re_search(short_lead, &input, BYTEOFF(0U), &match));
    YEW_ASSERT(!yew_re_match_at(short_lead, &input, BYTEOFF(0U), &match));
    YEW_ASSERT(!yew_re_pike_run(short_lead, &input, 0U, &match));
    YEW_ASSERT_EQ_I64(yew_re_dfa_test(short_lead, &input, 0U), YEW_DFA_NO);
    input = yew_re_input_bytes(raw_c2, sizeof(raw_c2));
    YEW_ASSERT(yew_re_search(short_lead, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].hi, 1U);

    pattern.len = 0U;
    yew_re_quote(&pattern, raw_cont, sizeof(raw_cont));
    short_cont = yew_re_compile(&arena, (const char *)pattern.data,
                                pattern.len, 0U, NULL);
    YEW_ASSERT_NOT_NULL(short_cont);
    input = yew_re_input_bytes(malformed_prefix, sizeof(malformed_prefix));
    YEW_ASSERT(yew_re_search(short_cont, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].lo, 1U);
    YEW_ASSERT_EQ_U64(match.g[0].hi, 2U);
    YEW_ASSERT(yew_re_match_at(short_cont, &input, BYTEOFF(1U), &match));

    (void)memset(long_literal, 'a', LONG_PREFIX);
    long_literal[LONG_PREFIX] = 0xC2U;
    (void)memset(long_subject, 'a', LONG_PREFIX);
    long_subject[LONG_PREFIX] = 0xC2U;
    long_subject[LONG_PREFIX + 1U] = 0xA2U;
    pattern.len = 0U;
    yew_re_quote(&pattern, long_literal, sizeof(long_literal));
    long_lead = yew_re_compile(&arena, (const char *)pattern.data,
                               pattern.len, 0U, NULL);
    YEW_ASSERT_NOT_NULL(long_lead);
    input = yew_re_input_bytes(long_subject, sizeof(long_subject));
    YEW_ASSERT(!yew_re_search(long_lead, &input, BYTEOFF(0U), &match));
    YEW_ASSERT(!yew_re_match_at(long_lead, &input, BYTEOFF(0U), &match));

    /* Reverse-prepending chunks prevents append coalescing, so the same
     * endpoint decision is exercised across real TextBuf piece seams. */
    tb = yew_textbuf_new();
    at = sizeof(long_subject);
    while (at != 0U) {
        u64 take = at < 13U ? at : 13U;

        at -= take;
        yew_textbuf_insert(tb, BYTEOFF(0U), long_subject + at, take);
    }
    input = yew_re_input_textbuf(tb);
    YEW_ASSERT(!yew_re_search(long_lead, &input, BYTEOFF(0U), &match));
    yew_textbuf_free(tb);

    input = yew_re_input_bytes(long_literal, sizeof(long_literal));
    YEW_ASSERT(yew_re_search(long_lead, &input, BYTEOFF(0U), &match));
    YEW_ASSERT_EQ_U64(match.g[0].hi, sizeof(long_literal));

    bytebuf_free(&pattern);
    arena_free_all(&arena);
}

void test_re_rep2_rejects_impossible_direct_metadata(void)
{
    enum { LITERAL_LEN = YEW_RE_MAX_PROG - 2U, HEADER = 96U, INST = 16U };
    u8 literal[LITERAL_LEN];
    Arena arena;
    Arena control_arena;
    Bytebuf packed;
    YewRe *re;

    (void)memset(literal, 'a', sizeof(literal));
    arena_init(&arena);
    bytebuf_init(&packed);
    re = yew_re_compile(&arena, (const char *)literal, sizeof(literal),
                        YEW_RE_LITERAL, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(yew_re_pack(re, &packed));
    YEW_ASSERT(memcmp(packed.data, "YEWREP2", 7U) == 0);
    arena_init(&control_arena);
    YEW_ASSERT_NOT_NULL(yew_re_unpack(&control_arena, packed.data,
                                      packed.len, NULL));
    arena_free_all(&control_arena);

    re_test_put32(packed.data + 24U, YEW_RE_LITERAL | YEW_RE_ICASE);
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    re_test_put32(packed.data + 24U, YEW_RE_LITERAL);
    re_test_put32(packed.data + 20U, 2U);
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    re_test_put32(packed.data + 20U, 1U);
    re_test_put32(packed.data + 16U, 1U);
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    re_test_put32(packed.data + 16U, 0U);
    re_test_put32(packed.data + 36U, 2U);
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    re_test_put32(packed.data + 36U, 0U);
    re_test_put32(packed.data + 48U, 1U);
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    re_test_put32(packed.data + 48U, 0U);
    re_test_put32(packed.data + 28U, LITERAL_LEN - 1U);
    re_test_put32(packed.data + 32U, LITERAL_LEN - 1U);
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    re_test_put32(packed.data + 28U, LITERAL_LEN);
    re_test_put32(packed.data + 32U, LITERAL_LEN);
    packed.data[HEADER + INST + 12U] = (u8)RE_MATCH;
    YEW_ASSERT(re_test_unpack_rejected(&packed));
    packed.data[HEADER + INST + 12U] = (u8)RE_SAVE;
    packed.data[HEADER + 3U * INST + 12U] = (u8)RE_MATCH;
    YEW_ASSERT(re_test_unpack_rejected(&packed));

    bytebuf_free(&packed);
    arena_free_all(&arena);
}

void test_re_workspace_reuses_mixed_programs_and_captures(void)
{
    static const char wide_pat[] = "(a)(b)?(a)(c)?(a)(d)?(a)(b)(r)(a)";
    Arena arena;
    YewReWorkspace workspace;
    YewRe *short_re;
    YewRe *wide_re;
    YewRe *boundary_re;
    YewReMatch m;
    YewReMatch standard;
    YewReInput in = yew_re_input_bytes((const u8 *)"abacadabra", 10U);
    YewReInput boundary_in =
        yew_re_input_bytes((const u8 *)"ab outside", 10U);
    u32 i;

    arena_init(&arena);
    yew_re_workspace_init(&workspace);
    short_re = yew_re_compile(&arena, "a|ab", 4U, 0U, NULL);
    wide_re = yew_re_compile(&arena, wide_pat, sizeof(wide_pat) - 1U,
                             0U, NULL);
    boundary_re = yew_re_compile(&arena, "\\b(a|ab)\\b", 10U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(short_re);
    YEW_ASSERT_NOT_NULL(wide_re);
    YEW_ASSERT_NOT_NULL(boundary_re);

    /* Alternate smaller and larger programs so capacity growth, capture
     * slot growth, and stale sparse-set generations are all exercised.
     * The short pattern also pins leftmost-first priority under reuse. */
    for (i = 0U; i < 1000U; i++) {
        YEW_ASSERT(yew_re_match_at_ws(&workspace, short_re, &in,
                                      BYTEOFF(0U), &m));
        YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
        YEW_ASSERT_EQ_U64(m.g[0].hi, 1U);
        YEW_ASSERT(yew_re_match_at_ws(&workspace, wide_re, &in,
                                      BYTEOFF(0U), &m));
        YEW_ASSERT_EQ_U64(m.ngroups, 11U);
        YEW_ASSERT_EQ_U64(m.g[0].hi, 10U);
        YEW_ASSERT_EQ_U64(m.g[2].lo, 1U);
        YEW_ASSERT_EQ_U64(m.g[2].hi, 2U);
        YEW_ASSERT_EQ_U64(m.g[4].lo, 3U);
        YEW_ASSERT_EQ_U64(m.g[4].hi, 4U);
        YEW_ASSERT(!yew_re_match_at_ws(&workspace, wide_re, &in,
                                       BYTEOFF(1U), &m));
        YEW_ASSERT(yew_re_match_at(boundary_re, &boundary_in, BYTEOFF(0U),
                                   &standard));
        YEW_ASSERT(yew_re_match_at_ws(&workspace, boundary_re, &boundary_in,
                                      BYTEOFF(0U), &m));
        YEW_ASSERT_EQ_U64(m.ngroups, standard.ngroups);
        YEW_ASSERT_EQ_U64(m.g[0].lo, standard.g[0].lo);
        YEW_ASSERT_EQ_U64(m.g[0].hi, standard.g[0].hi);
        YEW_ASSERT_EQ_U64(m.g[1].lo, standard.g[1].lo);
        YEW_ASSERT_EQ_U64(m.g[1].hi, standard.g[1].hi);
    }
    yew_re_workspace_free(&workspace);
    YEW_ASSERT(workspace.impl == NULL);
    /* Empty and repeated teardown are valid lifecycle edges. */
    yew_re_workspace_free(&workspace);
    arena_free_all(&arena);
}

void test_re_search_from_offset_and_back(void)
{
    static const u8 overlap_subject[] = "ababa";
    static const u8 cents[] = {
        0xC2U, 0xA2U, 0xC2U, 0xA2U, 0xC2U, 0xA2U
    };
    Arena arena;
    YewRe *re;
    YewRe *overlap_re;
    YewRe *capture_re;
    YewRe *zero_re;
    YewRe *unicode_re;
    YewReMatch m;
    YewReInput in = yew_re_input_bytes((const u8 *)"a b a b a", 9U);
    Bytebuf pattern;
    TextBuf *tb;
    u64 at;

    arena_init(&arena);
    bytebuf_init(&pattern);
    re = yew_re_compile(&arena, "a", 1U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(0U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(1U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 4U);
    YEW_ASSERT(yew_re_search(re, &in, BYTEOFF(5U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 8U);
    YEW_ASSERT(!yew_re_search(re, &in, BYTEOFF(9U), &m));
    /* Backward search returns the last match strictly before the point. */
    YEW_ASSERT(yew_re_search_back(re, &in, BYTEOFF(9U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 8U);
    YEW_ASSERT(yew_re_search_back(re, &in, BYTEOFF(8U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 4U);
    YEW_ASSERT(yew_re_search_back(re, &in, BYTEOFF(1U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
    YEW_ASSERT(!yew_re_search_back(re, &in, BYTEOFF(0U), &m));

    overlap_re = yew_re_compile(&arena, "aba", 3U, 0U, NULL);
    capture_re = yew_re_compile(&arena, "(a.)a", 5U, 0U, NULL);
    zero_re = yew_re_compile(&arena, "a*", 2U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(overlap_re);
    YEW_ASSERT_NOT_NULL(capture_re);
    YEW_ASSERT_NOT_NULL(zero_re);
    in = yew_re_input_bytes(overlap_subject, sizeof(overlap_subject) - 1U);
    YEW_ASSERT(yew_re_search_back(overlap_re, &in,
                                  BYTEOFF(sizeof(overlap_subject) - 1U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 5U);
    YEW_ASSERT(yew_re_search_back(capture_re, &in,
                                  BYTEOFF(sizeof(overlap_subject) - 1U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 5U);
    YEW_ASSERT_EQ_U64(m.g[1].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[1].hi, 4U);
    YEW_ASSERT(yew_re_search_back(zero_re, &in, BYTEOFF(3U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 3U);

    /* Fragment every byte so overlap enumeration also crosses piece seams. */
    tb = yew_textbuf_new();
    at = sizeof(overlap_subject) - 1U;
    while (at != 0U) {
        at--;
        yew_textbuf_insert(tb, BYTEOFF(0U), overlap_subject + at, 1U);
    }
    in = yew_re_input_textbuf(tb);
    YEW_ASSERT(yew_re_search_back(capture_re, &in,
                                  BYTEOFF(sizeof(overlap_subject) - 1U), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 5U);
    yew_textbuf_free(tb);

    /* Advancing from a prior match start follows decoded codepoints, not
     * continuation bytes, while still admitting a Unicode overlap. */
    yew_re_quote(&pattern, cents, 4U);
    unicode_re = yew_re_compile(&arena, (const char *)pattern.data,
                                pattern.len, 0U, NULL);
    YEW_ASSERT_NOT_NULL(unicode_re);
    in = yew_re_input_bytes(cents, sizeof(cents));
    YEW_ASSERT(yew_re_search_back(unicode_re, &in, BYTEOFF(sizeof(cents)),
                                  &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 6U);

    bytebuf_free(&pattern);
    arena_free_all(&arena);
}

void test_re_group_count_and_min_len(void)
{
    Arena arena;
    YewRe *re;

    arena_init(&arena);
    re = yew_re_compile(&arena, "a(b)(c)", 7U, 0U, NULL);
    YEW_ASSERT_NOT_NULL(re);
    YEW_ASSERT_EQ_U64(yew_re_group_count(re), 3U);   /* incl. group 0 */
    YEW_ASSERT_EQ_U64(yew_re_min_len(re), 3U);

    re = yew_re_compile(&arena, "a*", 2U, 0U, NULL);
    YEW_ASSERT_EQ_U64(yew_re_min_len(re), 0U);
    re = yew_re_compile(&arena, "ab|c", 4U, 0U, NULL);
    YEW_ASSERT_EQ_U64(yew_re_min_len(re), 1U);
    re = yew_re_compile(&arena, "a{3,5}", 6U, 0U, NULL);
    YEW_ASSERT_EQ_U64(yew_re_min_len(re), 3U);
    arena_free_all(&arena);
}
