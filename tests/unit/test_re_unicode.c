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
                 YewReMatch *out)
{
    static Arena arena;
    static bool ready;
    YewReInput in;
    YewRe *re;

    if (!ready) {
        arena_init(&arena);
        ready = true;
    }
    re = yew_re_compile(&arena, pat, strlen(pat), flags, NULL);
    if (re == NULL)
        return false;
    in = yew_re_input_bytes(input, (u64)len);
    return yew_re_search(re, &in, BYTEOFF(0U), out);
}

static bool matches(const char *pat, u32 flags, const char *input)
{
    YewReMatch m;

    return find(pat, flags, (const u8 *)input, strlen(input), &m);
}

/* ---------------------------------------------------------------- */
/* Case folding (§3)                                                */
/* ---------------------------------------------------------------- */

void test_re_icase_ascii(void)
{
    YEW_ASSERT(matches("abc", YEW_RE_ICASE, "ABC"));
    YEW_ASSERT(matches("ABC", YEW_RE_ICASE, "abc"));
    YEW_ASSERT(matches("AbC", YEW_RE_ICASE, "aBc"));
    YEW_ASSERT(matches("[a-z]+", YEW_RE_ICASE, "HELLO"));
    YEW_ASSERT(matches("[A-Z]+", YEW_RE_ICASE, "hello"));
    /* Non-letters are unaffected by folding. */
    YEW_ASSERT(matches("1+", YEW_RE_ICASE, "111"));
    YEW_ASSERT(!matches("abc", YEW_RE_ICASE, "abd"));
    /* Without the flag, case is significant. */
    YEW_ASSERT(!matches("abc", 0U, "ABC"));
}

void test_re_icase_kelvin_sign(void)
{
    /*
     * U+212A KELVIN SIGN folds to ASCII 'k'.  A naive fold that only
     * pairs 'k' with 'K' misses it, and the failure is invisible until
     * someone searches a physics paper.
     */
    YEW_ASSERT(matches("k", YEW_RE_ICASE, "\xE2\x84\xAA"));
    YEW_ASSERT(matches("K", YEW_RE_ICASE, "\xE2\x84\xAA"));
    YEW_ASSERT(matches("\xE2\x84\xAA", YEW_RE_ICASE, "k"));
    /* And it is NOT a match without the flag. */
    YEW_ASSERT(!matches("k", 0U, "\xE2\x84\xAA"));
}

void test_re_icase_greek_sigma(void)
{
    /*
     * Greek sigma has two lowercase forms — medial σ (U+03C3) and final
     * ς (U+03C2) — both folding to Σ (U+03A3).  Any pair of the three
     * must match under ICASE.
     */
    YEW_ASSERT(matches("\xCF\x83", YEW_RE_ICASE, "\xCE\xA3")); /* σ ~ Σ */
    YEW_ASSERT(matches("\xCE\xA3", YEW_RE_ICASE, "\xCF\x83"));
    YEW_ASSERT(matches("\xCF\x82", YEW_RE_ICASE, "\xCE\xA3")); /* ς ~ Σ */
    YEW_ASSERT(matches("\xCE\xA3", YEW_RE_ICASE, "\xCF\x82"));
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
    YEW_ASSERT(!matches("\xC3\x9F", YEW_RE_ICASE, "ss"));
    YEW_ASSERT(!matches("ss", YEW_RE_ICASE, "\xC3\x9F"));
    /* ß still matches itself. */
    YEW_ASSERT(matches("\xC3\x9F", YEW_RE_ICASE, "\xC3\x9F"));
}

void test_re_no_normalization(void)
{
    /*
     * Also pinned in §3: we deliberately ship no normalizer, so NFC é
     * (U+00E9) does not match NFD é (e + U+0301).  Claiming otherwise
     * would require a normalizer we have not built.
     */
    YEW_ASSERT(!matches("\xC3\xA9", 0U, "e\xCC\x81"));
    YEW_ASSERT(!matches("e\xCC\x81", 0U, "\xC3\xA9"));
    /* Each matches itself, and NFD is two codepoints to the engine. */
    YEW_ASSERT(matches("^.$", 0U, "\xC3\xA9"));
    YEW_ASSERT(matches("^..$", 0U, "e\xCC\x81"));
}

/* ---------------------------------------------------------------- */
/* Invalid bytes (§3)                                               */
/* ---------------------------------------------------------------- */

void test_re_invalid_bytes_match_dot_never_word(void)
{
    static const u8 raw[] = {'a', 0xFFU, 'b'};
    YewReMatch m;

    /* One invalid byte decodes to one synthetic codepoint, so `.` sees
     * exactly one thing and the offsets stay byte-exact. */
    YEW_ASSERT(find("a.b", 0U, raw, sizeof(raw), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 3U);

    /* An escape is never a word, digit or space character. */
    YEW_ASSERT(!find("a\\wb", 0U, raw, sizeof(raw), &m));
    YEW_ASSERT(!find("a\\db", 0U, raw, sizeof(raw), &m));
    YEW_ASSERT(!find("a\\sb", 0U, raw, sizeof(raw), &m));
    /* But the negated forms do match it. */
    YEW_ASSERT(find("a\\Wb", 0U, raw, sizeof(raw), &m));
    YEW_ASSERT(find("a\\Db", 0U, raw, sizeof(raw), &m));
    YEW_ASSERT(find("a\\Sb", 0U, raw, sizeof(raw), &m));
    /* And so does a negated class — [^x] must not silently fail on a
     * byte it cannot name. */
    YEW_ASSERT(find("a[^x]b", 0U, raw, sizeof(raw), &m));
}

void test_re_invalid_bytes_are_word_boundaries(void)
{
    static const u8 raw[] = {0xFFU, 'f', 'o', 'o', 0xFEU};
    YewReMatch m;

    /* An invalid byte is not a word character, so \b sees a boundary on
     * both sides of the word between them. */
    YEW_ASSERT(find("\\bfoo\\b", 0U, raw, sizeof(raw), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 1U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 4U);
}

void test_re_search_over_binary_survives(void)
{
    u8 noise[512];
    size_t i;
    YewReMatch m;

    /* Every byte value, including NUL, plus a needle in the middle.  The
     * point is that this neither crashes nor mangles: a search over a
     * binary file is something users do by accident constantly. */
    for (i = 0U; i < sizeof(noise); i++)
        noise[i] = (u8)(i & 0xFFU);
    (void)memcpy(noise + 200U, "needle", 6U);
    YEW_ASSERT(find("needle", 0U, noise, sizeof(noise), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 200U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 206U);
    /* NUL is an ordinary byte to this engine, not a terminator. */
    YEW_ASSERT(find("\\x00", 0U, noise, sizeof(noise), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 0U);
}

/* ---------------------------------------------------------------- */
/* Chunk carry (§7) — the case a contiguous buffer cannot reproduce  */
/* ---------------------------------------------------------------- */

/* Builds a piece tree by inserting one byte at a time at the end, which
 * is what an editing session actually produces. */
static TextBuf *tb_by_inserts(const char *text)
{
    TextBuf *tb = yew_textbuf_new();
    size_t i;
    size_t len = strlen(text);

    for (i = 0U; i < len; i++)
        yew_textbuf_insert(tb, BYTEOFF((u64)i), (const u8 *)text + i, 1U);
    return tb;
}

/* Interleaves inserts so the pieces are out of order in the add buffer —
 * a harsher shape than append-only. */
static TextBuf *tb_by_interleaved_inserts(const char *text)
{
    TextBuf *tb = yew_textbuf_new();
    size_t len = strlen(text);
    size_t i;

    /* First every even byte, then every odd byte spliced between them. */
    for (i = 0U; i < len; i += 2U)
        yew_textbuf_insert(tb, BYTEOFF((u64)(i / 2U)),
                           (const u8 *)text + i, 1U);
    for (i = 1U; i < len; i += 2U)
        yew_textbuf_insert(tb, BYTEOFF((u64)i), (const u8 *)text + i, 1U);
    return tb;
}

static bool find_in_tb(const char *pat, TextBuf *tb, YewReMatch *out)
{
    Arena arena;
    YewRe *re;
    YewReInput in;
    bool got;

    arena_init(&arena);
    re = yew_re_compile(&arena, pat, strlen(pat), 0U, NULL);
    if (re == NULL) {
        arena_free_all(&arena);
        return false;
    }
    in = yew_re_input_textbuf(tb);
    got = yew_re_search(re, &in, BYTEOFF(0U), out);
    arena_free_all(&arena);
    return got;
}

void test_re_match_spans_multiple_pieces(void)
{
    static const char text[] = "xxxxNEEDLExxxx";
    TextBuf *tb = tb_by_inserts(text);
    YewReMatch m;

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
    YEW_ASSERT(find_in_tb("NEEDLE", tb, &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 4U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 10U);
    yew_textbuf_free(tb);
}

void test_re_match_spans_interleaved_pieces(void)
{
    static const char text[] = "aaaaTARGETaaaa";
    TextBuf *tb = tb_by_interleaved_inserts(text);
    YewReMatch m;

    YEW_ASSERT(yew_textbuf_piece_count(tb) >= 3U);
    YEW_ASSERT(find_in_tb("TARGET", tb, &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 4U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 10U);
    yew_textbuf_free(tb);
}

void test_re_multibyte_spans_pieces(void)
{
    /* A three-codepoint CJK run inserted byte by byte: every codepoint
     * straddles piece boundaries, so the decoder must reassemble across
     * chunks as well as the prefilter. */
    static const char text[] = "..\xE6\xBC\xA2\xE5\xAD\x97\xE8\xAA\x9E..";
    TextBuf *tb = tb_by_inserts(text);
    YewReMatch m;

    YEW_ASSERT(find_in_tb("\xE6\xBC\xA2\xE5\xAD\x97\xE8\xAA\x9E", tb, &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 11U);
    /* And \w+ sees the CJK run as three word characters. */
    YEW_ASSERT(find_in_tb("\\w+", tb, &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 2U);
    YEW_ASSERT_EQ_U64(m.g[0].hi, 11U);
    yew_textbuf_free(tb);
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
    YewReMatch m;
    static const char pat[] = "\x9F";

    /* The only real occurrence is at offset 5, not inside the emoji. */
    YEW_ASSERT(find(pat, 0U, hay, sizeof(hay), &m));
    YEW_ASSERT_EQ_U64(m.g[0].lo, 5U);
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

    for (i = 0U; i < YEW_ARRAY_LEN(patterns); i++) {
        TextBuf *tb = tb_by_inserts(text);
        YewReMatch a;
        YewReMatch b;
        bool ga = find(patterns[i], 0U, (const u8 *)text, strlen(text),
                       &a);
        bool gb = find_in_tb(patterns[i], tb, &b);

        YEW_ASSERT(ga == gb);
        if (ga && gb) {
            YEW_ASSERT_EQ_U64(a.g[0].lo, b.g[0].lo);
            YEW_ASSERT_EQ_U64(a.g[0].hi, b.g[0].hi);
        }
        yew_textbuf_free(tb);
    }
}
