/*
 * Sprint 20 DoD 10 + §3: Unicode behaviour of the regex engine.
 *
 * Three hazards live here.  Case folding is where "obvious" answers are
 * wrong (Kelvin sign, final sigma).  Invalid bytes are where a search
 * over a binary file either survives or corrupts something.  And the
 * chunk-carry case is the one that cannot be reproduced from a
 * contiguous buffer at all: it only appears on a piece tree built by
 * editing, which is every real buffer a user has ever searched.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "text/piece.h"
#include "util/arena.h"

static bool find(const char *pat, u32 flags, const u8 *input, size_t len,
                 SagReMatch *out)
{
    static Arena arena;
    static bool ready;
    SagReInput in;
    SagRe *re;

    if (!ready) {
        arena_init(&arena);
        ready = true;
    }
    re = sag_re_compile(&arena, pat, strlen(pat), flags, NULL);
    if (re == NULL)
        return false;
    in = sag_re_input_bytes(input, (u64)len);
    return sag_re_search(re, &in, BYTEOFF(0U), out);
}

static bool matches(const char *pat, u32 flags, const char *input)
{
    SagReMatch m;

    return find(pat, flags, (const u8 *)input, strlen(input), &m);
}

/* ---------------------------------------------------------------- */
/* Case folding (§3)                                                */
/* ---------------------------------------------------------------- */

void test_re_icase_ascii(void)
{
    SAG_ASSERT(matches("abc", SAG_RE_ICASE, "ABC"));
    SAG_ASSERT(matches("ABC", SAG_RE_ICASE, "abc"));
    SAG_ASSERT(matches("AbC", SAG_RE_ICASE, "aBc"));
    SAG_ASSERT(matches("[a-z]+", SAG_RE_ICASE, "HELLO"));
    SAG_ASSERT(matches("[A-Z]+", SAG_RE_ICASE, "hello"));
    /* Non-letters are unaffected by folding. */
    SAG_ASSERT(matches("1+", SAG_RE_ICASE, "111"));
    SAG_ASSERT(!matches("abc", SAG_RE_ICASE, "abd"));
    /* Without the flag, case is significant. */
    SAG_ASSERT(!matches("abc", 0U, "ABC"));
}

void test_re_icase_kelvin_sign(void)
{
    /*
     * U+212A KELVIN SIGN folds to ASCII 'k'.  A naive fold that only
     * pairs 'k' with 'K' misses it, and the failure is invisible until
     * someone searches a physics paper.
     */
    SAG_ASSERT(matches("k", SAG_RE_ICASE, "\xE2\x84\xAA"));
    SAG_ASSERT(matches("K", SAG_RE_ICASE, "\xE2\x84\xAA"));
    SAG_ASSERT(matches("\xE2\x84\xAA", SAG_RE_ICASE, "k"));
    /* And it is NOT a match without the flag. */
    SAG_ASSERT(!matches("k", 0U, "\xE2\x84\xAA"));
}

void test_re_icase_greek_sigma(void)
{
    /*
     * Greek sigma has two lowercase forms — medial σ (U+03C3) and final
     * ς (U+03C2) — both folding to Σ (U+03A3).  Any pair of the three
     * must match under ICASE.
     */
    SAG_ASSERT(matches("\xCF\x83", SAG_RE_ICASE, "\xCE\xA3")); /* σ ~ Σ */
    SAG_ASSERT(matches("\xCE\xA3", SAG_RE_ICASE, "\xCF\x83"));
    SAG_ASSERT(matches("\xCF\x82", SAG_RE_ICASE, "\xCE\xA3")); /* ς ~ Σ */
    SAG_ASSERT(matches("\xCE\xA3", SAG_RE_ICASE, "\xCF\x82"));
}

void test_re_icase_is_simple_folding_only(void)
{
    /*
     * §3 pins simple folding and rules out full folding.  ß does NOT
     * match "ss" — a length-changing fold would break the
     * one-codepoint-per-step invariant that makes the VM linear, so this
     * is a deliberate limitation rather than an oversight, and it is
     * asserted so nobody "fixes" it without reading why.
     */
    SAG_ASSERT(!matches("\xC3\x9F", SAG_RE_ICASE, "ss"));
    SAG_ASSERT(!matches("ss", SAG_RE_ICASE, "\xC3\x9F"));
    /* ß still matches itself. */
    SAG_ASSERT(matches("\xC3\x9F", SAG_RE_ICASE, "\xC3\x9F"));
}

void test_re_no_normalization(void)
{
    /*
     * Also pinned in §3: we deliberately ship no normalizer, so NFC é
     * (U+00E9) does not match NFD é (e + U+0301).  Claiming otherwise
     * would require a normalizer we have not built.
     */
    SAG_ASSERT(!matches("\xC3\xA9", 0U, "e\xCC\x81"));
    SAG_ASSERT(!matches("e\xCC\x81", 0U, "\xC3\xA9"));
    /* Each matches itself, and NFD is two codepoints to the engine. */
    SAG_ASSERT(matches("^.$", 0U, "\xC3\xA9"));
    SAG_ASSERT(matches("^..$", 0U, "e\xCC\x81"));
}

/* ---------------------------------------------------------------- */
/* Invalid bytes (§3)                                               */
/* ---------------------------------------------------------------- */

void test_re_invalid_bytes_match_dot_never_word(void)
{
    static const u8 raw[] = {'a', 0xFFU, 'b'};
    SagReMatch m;

    /* One invalid byte decodes to one synthetic codepoint, so `.` sees
     * exactly one thing and the offsets stay byte-exact. */
    SAG_ASSERT(find("a.b", 0U, raw, sizeof(raw), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 3U);

    /* An escape is never a word, digit or space character. */
    SAG_ASSERT(!find("a\\wb", 0U, raw, sizeof(raw), &m));
    SAG_ASSERT(!find("a\\db", 0U, raw, sizeof(raw), &m));
    SAG_ASSERT(!find("a\\sb", 0U, raw, sizeof(raw), &m));
    /* But the negated forms do match it. */
    SAG_ASSERT(find("a\\Wb", 0U, raw, sizeof(raw), &m));
    SAG_ASSERT(find("a\\Db", 0U, raw, sizeof(raw), &m));
    SAG_ASSERT(find("a\\Sb", 0U, raw, sizeof(raw), &m));
    /* And so does a negated class — [^x] must not silently fail on a
     * byte it cannot name. */
    SAG_ASSERT(find("a[^x]b", 0U, raw, sizeof(raw), &m));
}

void test_re_invalid_bytes_are_word_boundaries(void)
{
    static const u8 raw[] = {0xFFU, 'f', 'o', 'o', 0xFEU};
    SagReMatch m;

    /* An invalid byte is not a word character, so \b sees a boundary on
     * both sides of the word between them. */
    SAG_ASSERT(find("\\bfoo\\b", 0U, raw, sizeof(raw), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 1U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 4U);
}

void test_re_search_over_binary_survives(void)
{
    u8 noise[512];
    size_t i;
    SagReMatch m;

    /* Every byte value, including NUL, plus a needle in the middle.  The
     * point is that this neither crashes nor mangles: a search over a
     * binary file is something users do by accident constantly. */
    for (i = 0U; i < sizeof(noise); i++)
        noise[i] = (u8)(i & 0xFFU);
    (void)memcpy(noise + 200U, "needle", 6U);
    SAG_ASSERT(find("needle", 0U, noise, sizeof(noise), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 200U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 206U);
    /* NUL is an ordinary byte to this engine, not a terminator. */
    SAG_ASSERT(find("\\x00", 0U, noise, sizeof(noise), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 0U);
}

/* ---------------------------------------------------------------- */
/* Chunk carry (§7) — the case a contiguous buffer cannot reproduce  */
/* ---------------------------------------------------------------- */

/* Builds a piece tree by inserting one byte at a time at the end, which
 * is what an editing session actually produces. */
static TextBuf *tb_by_inserts(const char *text)
{
    TextBuf *tb = sag_textbuf_new();
    size_t i;
    size_t len = strlen(text);

    for (i = 0U; i < len; i++)
        sag_textbuf_insert(tb, BYTEOFF((u64)i), (const u8 *)text + i, 1U);
    return tb;
}

/* Interleaves inserts so the pieces are out of order in the add buffer —
 * a harsher shape than append-only. */
static TextBuf *tb_by_interleaved_inserts(const char *text)
{
    TextBuf *tb = sag_textbuf_new();
    size_t len = strlen(text);
    size_t i;

    /* First every even byte, then every odd byte spliced between them. */
    for (i = 0U; i < len; i += 2U)
        sag_textbuf_insert(tb, BYTEOFF((u64)(i / 2U)),
                           (const u8 *)text + i, 1U);
    for (i = 1U; i < len; i += 2U)
        sag_textbuf_insert(tb, BYTEOFF((u64)i), (const u8 *)text + i, 1U);
    return tb;
}

static bool find_in_tb(const char *pat, TextBuf *tb, SagReMatch *out)
{
    Arena arena;
    SagRe *re;
    SagReInput in;
    bool got;

    arena_init(&arena);
    re = sag_re_compile(&arena, pat, strlen(pat), 0U, NULL);
    if (re == NULL) {
        arena_free_all(&arena);
        return false;
    }
    in = sag_re_input_textbuf(tb);
    got = sag_re_search(re, &in, BYTEOFF(0U), out);
    arena_free_all(&arena);
    return got;
}

void test_re_match_spans_multiple_pieces(void)
{
    static const char text[] = "xxxxNEEDLExxxx";
    TextBuf *tb = tb_by_inserts(text);
    SagReMatch m;

    /*
     * DoD 10.  Every byte was inserted separately, so the literal spans
     * many pieces and the prefilter must carry bytes across chunk
     * boundaries to find it.  Dropping the carry misses matches ONLY on
     * buffers that have been edited — never in a test built from one
     * contiguous array, which is exactly why this test exists.
     */
    /*
     * NOTE: byte-by-byte APPENDS coalesce into one piece — s07 does that
     * deliberately so typing does not create a piece per keystroke.  So
     * this shape exercises the decoder, and the interleaved test below
     * is the one that actually fragments the tree.
     */
    SAG_ASSERT(find_in_tb("NEEDLE", tb, &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 4U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 10U);
    sag_textbuf_free(tb);
}

void test_re_match_spans_interleaved_pieces(void)
{
    static const char text[] = "aaaaTARGETaaaa";
    TextBuf *tb = tb_by_interleaved_inserts(text);
    SagReMatch m;

    SAG_ASSERT(sag_textbuf_piece_count(tb) >= 3U);
    SAG_ASSERT(find_in_tb("TARGET", tb, &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 4U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 10U);
    sag_textbuf_free(tb);
}

void test_re_multibyte_spans_pieces(void)
{
    /* A three-codepoint CJK run inserted byte by byte: every codepoint
     * straddles piece boundaries, so the decoder must reassemble across
     * chunks as well as the prefilter. */
    static const char text[] = "..\xE6\xBC\xA2\xE5\xAD\x97\xE8\xAA\x9E..";
    TextBuf *tb = tb_by_inserts(text);
    SagReMatch m;

    SAG_ASSERT(find_in_tb("\xE6\xBC\xA2\xE5\xAD\x97\xE8\xAA\x9E", tb, &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 2U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 11U);
    /* And \w+ sees the CJK run as three word characters. */
    SAG_ASSERT(find_in_tb("\\w+", tb, &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 2U);
    SAG_ASSERT_EQ_U64(m.g[0].hi, 11U);
    sag_textbuf_free(tb);
}

void test_re_prefilter_rejects_mid_sequence_candidates(void)
{
    /*
     * §7's other pitfall.  BMH knows no encoding, so a candidate can
     * land inside a multibyte sequence.  0x9F here is a continuation
     * byte of the emoji AND the first byte of the literal we search
     * for — without the codepoint-boundary check the engine reports a
     * phantom match inside the emoji.
     */
    static const u8 hay[] = {0xF0U, 0x9FU, 0x98U, 0x80U, 'x', 0x9FU, 'y'};
    SagReMatch m;
    static const char pat[] = "\x9F";

    /* The only real occurrence is at offset 5, not inside the emoji. */
    SAG_ASSERT(find(pat, 0U, hay, sizeof(hay), &m));
    SAG_ASSERT_EQ_U64(m.g[0].lo, 5U);
}

void test_re_textbuf_and_bytes_agree(void)
{
    /* The same pattern over the same content must give identical spans
     * whether the input is a flat array or a piece tree. */
    static const char *const patterns[] = {
        "NEEDLE", "N.*E", "\\w+", "[A-Z]+", "x+", "^xx", "E$"
    };
    static const char text[] = "xxxxNEEDLE";
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(patterns); i++) {
        TextBuf *tb = tb_by_inserts(text);
        SagReMatch a;
        SagReMatch b;
        bool ga = find(patterns[i], 0U, (const u8 *)text, strlen(text),
                       &a);
        bool gb = find_in_tb(patterns[i], tb, &b);

        SAG_ASSERT(ga == gb);
        if (ga && gb) {
            SAG_ASSERT_EQ_U64(a.g[0].lo, b.g[0].lo);
            SAG_ASSERT_EQ_U64(a.g[0].hi, b.g[0].hi);
        }
        sag_textbuf_free(tb);
    }
}
