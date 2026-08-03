#include "edit/word.h"

#include <stddef.h>

#include "unicode/coords.h"
#include "unicode/utf8.h"
#include "unicode/wordbreak.h"

typedef struct {
    u64 lo;
    u64 hi;
    u32 cp;
    SagWb prop;
} WordCp;

typedef enum {
    SUB_OTHER = 0,
    SUB_LOWER,
    SUB_UPPER,
    SUB_DIGIT
} SubClass;

static size_t copy_bytes(const TextBuf *tb, u64 at, u8 *out, size_t cap)
{
    TextIter it;
    size_t copied = 0U;
    u64 len = sag_textbuf_len(tb);

    if (at >= len || cap == 0U || !sag_textiter_begin(&it, tb, BYTEOFF(at)))
        return 0U;
    while (copied < cap) {
        const u8 *chunk;
        u64 chunk_len;
        size_t take;

        if (!sag_textiter_chunk(&it, tb, &chunk, &chunk_len))
            break;
        take = (size_t)(chunk_len < (u64)(cap - copied)
                            ? chunk_len
                            : (u64)(cap - copied));
        for (size_t i = 0U; i < take; i++)
            out[copied + i] = chunk[i];
        copied += take;
        if (copied == cap || !sag_textiter_advance(&it, tb))
            break;
    }
    return copied;
}

static bool cp_next(const TextBuf *tb, u64 at, WordCp *out)
{
    u8 bytes[SAG_UTF8_MAX];
    size_t have;
    size_t used;

    if (at >= sag_textbuf_len(tb))
        return false;
    have = copy_bytes(tb, at, bytes, sizeof(bytes));
    used = sag_utf8_decode(bytes, have, &out->cp);
    if (used == 0U)
        return false;
    out->lo = at;
    out->hi = at + (u64)used;
    out->prop = sag_wb_prop(out->cp);
    return true;
}

static bool cp_prev(const TextBuf *tb, u64 at, WordCp *out)
{
    u8 bytes[SAG_UTF8_MAX];
    size_t back;
    size_t best = 0U;
    u32 best_cp = 0U;

    if (at == 0U)
        return false;
    for (back = 1U; back <= SAG_UTF8_MAX && (u64)back <= at; back++) {
        size_t have = copy_bytes(tb, at - (u64)back, bytes, back);
        u32 cp;
        size_t used;

        if (have != back)
            continue;
        used = sag_utf8_decode(bytes, have, &cp);
        if (used == back) {
            best = back;
            best_cp = cp;
        }
    }
    if (best == 0U)
        return false;
    out->lo = at - (u64)best;
    out->hi = at;
    out->cp = best_cp;
    out->prop = sag_wb_prop(best_cp);
    return true;
}

static bool prev_significant(const TextBuf *tb, u64 at, WordCp *out)
{
    while (cp_prev(tb, at, out)) {
        if (!sag_wb_is_ignored(out->prop))
            return true;
        at = out->lo;
    }
    return false;
}

static bool next_significant(const TextBuf *tb, u64 at, WordCp *out)
{
    while (cp_next(tb, at, out)) {
        if (!sag_wb_is_ignored(out->prop))
            return true;
        at = out->hi;
    }
    return false;
}

static bool is_newline(SagWb p)
{
    return p == SAG_WB_CR || p == SAG_WB_LF || p == SAG_WB_NEWLINE;
}

static bool is_ah(SagWb p)
{
    return p == SAG_WB_ALETTER || p == SAG_WB_HEBREW_LETTER;
}

static bool is_mid_letter(SagWb p)
{
    return p == SAG_WB_MIDLETTER || p == SAG_WB_MIDNUMLET ||
           p == SAG_WB_SINGLE_QUOTE;
}

static bool is_mid_num(SagWb p)
{
    return p == SAG_WB_MIDNUM || p == SAG_WB_MIDNUMLET ||
           p == SAG_WB_SINGLE_QUOTE;
}

static bool is_ext_glue_left(SagWb p)
{
    return is_ah(p) || p == SAG_WB_NUMERIC || p == SAG_WB_KATAKANA ||
           p == SAG_WB_EXTENDNUMLET;
}

static bool is_ext_glue_right(SagWb p)
{
    return is_ah(p) || p == SAG_WB_NUMERIC || p == SAG_WB_KATAKANA;
}

bool sag_word_boundary(const TextBuf *tb, ByteOff pos)
{
    WordCp immediate_left;
    WordCp immediate_right;
    WordCp left;
    WordCp right;
    WordCp left2;
    WordCp right2;
    bool have_left;
    bool have_right;
    bool have_left2;
    bool have_right2;
    u64 len;

    if (tb == NULL)
        return true;
    len = sag_textbuf_len(tb);
    if (pos.v == 0U || pos.v >= len)
        return true;
    if (!cp_prev(tb, pos.v, &immediate_left) ||
        !cp_next(tb, pos.v, &immediate_right))
        return true;

    /* WB3, WB3a, WB3b, WB3c and WB3d precede ignored-character
     * normalization and therefore use the adjacent codepoints. */
    if (immediate_left.prop == SAG_WB_CR &&
        immediate_right.prop == SAG_WB_LF)
        return false;
    if (is_newline(immediate_left.prop) ||
        is_newline(immediate_right.prop))
        return true;
    if (immediate_left.prop == SAG_WB_ZWJ &&
        sag_unicode_is_extended_pictographic(immediate_right.cp))
        return false;
    if (immediate_left.prop == SAG_WB_WSEGSPACE &&
        immediate_right.prop == SAG_WB_WSEGSPACE)
        return false;

    /* WB4: ignored codepoints attach to the codepoint before them. */
    if (sag_wb_is_ignored(immediate_right.prop))
        return false;

    have_left = prev_significant(tb, pos.v, &left);
    have_right = next_significant(tb, pos.v, &right);
    if (!have_left || !have_right)
        return true;
    have_left2 = prev_significant(tb, left.lo, &left2);
    have_right2 = next_significant(tb, right.hi, &right2);

    /* WB5-WB7: alphabetic and Hebrew punctuation sequences. */
    if (is_ah(left.prop) && is_ah(right.prop))
        return false;
    if (is_ah(left.prop) && is_mid_letter(right.prop) && have_right2 &&
        is_ah(right2.prop))
        return false;
    if (have_left2 && is_ah(left2.prop) && is_mid_letter(left.prop) &&
        is_ah(right.prop))
        return false;
    if (left.prop == SAG_WB_HEBREW_LETTER &&
        right.prop == SAG_WB_SINGLE_QUOTE)
        return false;
    if (left.prop == SAG_WB_HEBREW_LETTER &&
        right.prop == SAG_WB_DOUBLE_QUOTE && have_right2 &&
        right2.prop == SAG_WB_HEBREW_LETTER)
        return false;
    if (have_left2 && left2.prop == SAG_WB_HEBREW_LETTER &&
        left.prop == SAG_WB_DOUBLE_QUOTE &&
        right.prop == SAG_WB_HEBREW_LETTER)
        return false;

    /* WB8-WB13: numbers, letters, and Katakana. */
    if (left.prop == SAG_WB_NUMERIC && right.prop == SAG_WB_NUMERIC)
        return false;
    if (is_ah(left.prop) && right.prop == SAG_WB_NUMERIC)
        return false;
    if (left.prop == SAG_WB_NUMERIC && is_ah(right.prop))
        return false;
    if (have_left2 && left2.prop == SAG_WB_NUMERIC &&
        is_mid_num(left.prop) && right.prop == SAG_WB_NUMERIC)
        return false;
    if (left.prop == SAG_WB_NUMERIC && is_mid_num(right.prop) &&
        have_right2 && right2.prop == SAG_WB_NUMERIC)
        return false;
    if (left.prop == SAG_WB_KATAKANA && right.prop == SAG_WB_KATAKANA)
        return false;
    if (is_ext_glue_left(left.prop) &&
        right.prop == SAG_WB_EXTENDNUMLET)
        return false;
    if (left.prop == SAG_WB_EXTENDNUMLET && is_ext_glue_right(right.prop))
        return false;

    /* WB15/WB16: pair regional indicators from the start of their run. */
    if (left.prop == SAG_WB_REGIONAL_INDICATOR &&
        right.prop == SAG_WB_REGIONAL_INDICATOR) {
        u64 scan = left.lo;
        u64 count = 1U;
        WordCp prior;

        while (prev_significant(tb, scan, &prior) &&
               prior.prop == SAG_WB_REGIONAL_INDICATOR) {
            count++;
            scan = prior.lo;
        }
        if ((count & 1U) != 0U)
            return false;
    }
    return true; /* WB999 */
}

static ByteOff clamp_grapheme(const TextBuf *tb, ByteOff p)
{
    u64 len = sag_textbuf_len(tb);

    if (p.v > len)
        p.v = len;
    if (!sag_is_grapheme_boundary(tb, p))
        p = sag_grapheme_prev(tb, p);
    return p;
}

static bool cluster_cp(const TextBuf *tb, ByteOff p, WordCp *cp)
{
    return cp_next(tb, p.v, cp);
}

static bool cluster_white(const TextBuf *tb, ByteOff p)
{
    WordCp cp;

    return cluster_cp(tb, p, &cp) && sag_unicode_is_white_space(cp.cp);
}

static Span uax_span(const TextBuf *tb, ByteOff p)
{
    u64 len = sag_textbuf_len(tb);
    ByteOff lo;
    ByteOff hi;

    p = clamp_grapheme(tb, p);
    if (len == 0U)
        return (Span){0U, 0U};
    if (p.v == len)
        p = sag_grapheme_prev_boundary(tb, p);
    lo = p;
    while (lo.v > 0U && !sag_word_boundary(tb, lo))
        lo = sag_grapheme_prev_boundary(tb, lo);
    hi = sag_grapheme_next_boundary(tb, p);
    while (hi.v < len && !sag_word_boundary(tb, hi))
        hi = sag_grapheme_next_boundary(tb, hi);
    return (Span){lo.v, hi.v};
}

static Span word_span(const TextBuf *tb, ByteOff p, bool alt)
{
    u64 len;
    ByteOff lo;
    ByteOff hi;
    bool white;

    if (!alt)
        return uax_span(tb, p);
    len = sag_textbuf_len(tb);
    p = clamp_grapheme(tb, p);
    if (len == 0U)
        return (Span){0U, 0U};
    if (p.v == len)
        p = sag_grapheme_prev_boundary(tb, p);
    white = cluster_white(tb, p);
    lo = p;
    while (lo.v > 0U) {
        ByteOff prior = sag_grapheme_prev_boundary(tb, lo);

        if (cluster_white(tb, prior) != white)
            break;
        lo = prior;
    }
    hi = sag_grapheme_next_boundary(tb, p);
    while (hi.v < len && cluster_white(tb, hi) == white)
        hi = sag_grapheme_next_boundary(tb, hi);
    return (Span){lo.v, hi.v};
}

static u64 linebreaks_in(const TextBuf *tb, Span span)
{
    u64 at = span.lo;
    u64 count = 0U;
    bool after_cr = false;
    WordCp cp;

    while (at < span.hi && cp_next(tb, at, &cp)) {
        if (cp.prop == SAG_WB_LF) {
            if (!after_cr)
                count++;
        } else if (cp.prop == SAG_WB_CR || cp.prop == SAG_WB_NEWLINE) {
            count++;
        }
        after_cr = cp.prop == SAG_WB_CR;
        at = cp.hi;
    }
    return count;
}

static ByteOff word_next(UnitCtx *u, ByteOff p, bool alt)
{
    u64 len = sag_textbuf_len(u->tb);
    Span current;
    u64 breaks;
    ByteOff at;

    p = clamp_grapheme(u->tb, p);
    if (p.v >= len)
        return BYTEOFF(len);
    current = word_span(u->tb, p, alt);
    breaks = cluster_white(u->tb, BYTEOFF(current.lo))
                 ? linebreaks_in(u->tb, current)
                 : 0U;
    at = BYTEOFF(current.hi);
    while (at.v < len) {
        Span candidate = word_span(u->tb, at, alt);

        if (!cluster_white(u->tb, BYTEOFF(candidate.lo)))
            return BYTEOFF(candidate.lo);
        breaks += linebreaks_in(u->tb, candidate);
        if (breaks >= 2U)
            return BYTEOFF(candidate.lo);
        at = BYTEOFF(candidate.hi);
    }
    return BYTEOFF(len);
}

static ByteOff word_prev(UnitCtx *u, ByteOff p, bool alt)
{
    u64 len = sag_textbuf_len(u->tb);
    Span current;
    u64 breaks = 0U;
    ByteOff at;

    p = clamp_grapheme(u->tb, p);
    if (p.v == 0U || len == 0U)
        return BYTEOFF(0U);
    if (p.v < len && !sag_word_boundary(u->tb, p)) {
        current = word_span(u->tb, p, alt);
        return BYTEOFF(current.lo);
    }
    at = p;
    while (at.v > 0U) {
        ByteOff prior = sag_grapheme_prev_boundary(u->tb, at);
        Span candidate = word_span(u->tb, prior, alt);

        if (!cluster_white(u->tb, BYTEOFF(candidate.lo)))
            return BYTEOFF(candidate.lo);
        breaks += linebreaks_in(u->tb, candidate);
        if (breaks >= 2U)
            return BYTEOFF(candidate.lo);
        at = BYTEOFF(candidate.lo);
    }
    return BYTEOFF(0U);
}

static ByteOff word_home(UnitCtx *u, ByteOff p, bool alt)
{
    return BYTEOFF(word_span(u->tb, p, alt).lo);
}

static ByteOff word_end(UnitCtx *u, ByteOff p, bool alt)
{
    return BYTEOFF(word_span(u->tb, p, alt).hi);
}

static Span word_unit_span(UnitCtx *u, ByteOff p, bool alt)
{
    return word_span(u->tb, p, alt);
}

static SubClass sub_class(u32 cp)
{
    if (cp >= (u32)'a' && cp <= (u32)'z')
        return SUB_LOWER;
    if (cp >= (u32)'A' && cp <= (u32)'Z')
        return SUB_UPPER;
    if (cp >= (u32)'0' && cp <= (u32)'9')
        return SUB_DIGIT;
    return SUB_OTHER;
}

static bool sub_separator(u32 cp)
{
    return cp == (u32)'_' || cp == (u32)'-' ||
           sag_unicode_is_white_space(cp);
}

static bool sub_split(SubClass before, SubClass after, SubClass after2)
{
    if (before == SUB_LOWER && after == SUB_UPPER)
        return true;
    if (before == SUB_DIGIT &&
        (after == SUB_LOWER || after == SUB_UPPER))
        return true;
    return before == SUB_UPPER && after == SUB_UPPER &&
           after2 == SUB_LOWER;
}

ByteOff sag_word_sub_next(UnitCtx *u, ByteOff p)
{
    const TextBuf *tb = u->tb;
    u64 len = sag_textbuf_len(tb);
    ByteOff at;
    WordCp cp;
    SubClass before;

    p = clamp_grapheme(tb, p);
    if (p.v >= len)
        return BYTEOFF(len);
    at = p;
    if (!cluster_cp(tb, at, &cp))
        return BYTEOFF(len);
    while (sub_separator(cp.cp)) {
        at = sag_grapheme_next_boundary(tb, at);
        if (at.v >= len || !cluster_cp(tb, at, &cp))
            return BYTEOFF(len);
    }
    if (at.v > p.v)
        return at;
    before = sub_class(cp.cp);
    for (;;) {
        ByteOff next = sag_grapheme_next_boundary(tb, at);
        WordCp after_cp;
        SubClass after;
        SubClass after2 = SUB_OTHER;

        if (next.v >= len)
            return BYTEOFF(len);
        if (!cluster_cp(tb, next, &after_cp))
            return BYTEOFF(len);
        if (sub_separator(after_cp.cp)) {
            at = next;
            do {
                at = sag_grapheme_next_boundary(tb, at);
                if (at.v >= len || !cluster_cp(tb, at, &after_cp))
                    return BYTEOFF(len);
            } while (sub_separator(after_cp.cp));
            return at;
        }
        if (sag_word_boundary(tb, next))
            return next;
        after = sub_class(after_cp.cp);
        {
            ByteOff after_at = sag_grapheme_next_boundary(tb, next);
            WordCp after2_cp;

            if (after_at.v < len && cluster_cp(tb, after_at, &after2_cp) &&
                !sub_separator(after2_cp.cp) &&
                !sag_word_boundary(tb, after_at))
                after2 = sub_class(after2_cp.cp);
        }
        if (sub_split(before, after, after2))
            return next;
        before = after;
        at = next;
    }
}

ByteOff sag_word_sub_prev(UnitCtx *u, ByteOff p)
{
    const TextBuf *tb = u->tb;
    u64 len = sag_textbuf_len(tb);
    ByteOff target;
    ByteOff at;
    ByteOff last;
    WordCp cp;

    p = clamp_grapheme(tb, p);
    if (p.v == 0U || len == 0U)
        return BYTEOFF(0U);
    target = p;
    at = sag_grapheme_prev_boundary(tb, target);
    while (cluster_cp(tb, at, &cp) && sub_separator(cp.cp)) {
        if (at.v == 0U)
            return BYTEOFF(0U);
        at = sag_grapheme_prev_boundary(tb, at);
    }

    /* Replaying next-boundary decisions from the containing UAX unit keeps
     * acronym splitting identical in both directions. */
    at = BYTEOFF(uax_span(tb, at).lo);
    last = at;
    while (at.v < target.v) {
        ByteOff next = sag_word_sub_next(u, at);

        if (next.v >= target.v || next.v <= at.v)
            break;
        last = next;
        at = next;
    }
    return last;
}

const UnitOps sag_unit_word = {
    "word", word_next, word_prev, word_home, word_end, word_unit_span,
};
