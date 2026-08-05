/*
 * Sprint 21 §6: the one implementation of "escape this literal".
 *
 * `*` and `#` build a pattern out of the word under the cursor, `:s//`
 * reuses register `/`, and Sprint 26's finder will want the same thing.
 * Each of those is a chance to write a slightly different escaper, and
 * the ones that differ are the ones that are wrong — a missed
 * metacharacter turns a search for `a.b` into a search for `axb`, and a
 * missed backslash turns it into a compile error in the message line.
 * So there is exactly one, it lives beside the engine whose grammar it
 * has to agree with, and DoD 13 fuzzes it against that grammar.
 */
#include "search/regex.h"

#include "util/buf.h"

/*
 * The parser accepts `\` before any ASCII punctuation and folds it to
 * that character (parse.c's "metacharacters and punctuation escape to
 * themselves").  Escaping all of it rather than just the current
 * metacharacter set is deliberate: `{`, `}` and `-` are not special in
 * every position today, and a future grammar that gives one of them a
 * meaning would silently change what already-quoted patterns mean.
 */
static bool is_ascii_punct(u8 c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

void sag_re_quote(Bytebuf *out, const u8 *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (out == NULL || (s == NULL && n != 0U))
        return;
    /*
     * Quoting the empty literal must still yield a usable buffer.  An
     * untouched Bytebuf has data == NULL, and sag_re_compile rejects a
     * NULL pattern as "no pattern" — so every caller that quoted a word
     * that happened to be empty would get a compile error instead of an
     * empty pattern, and would have to special-case it.  Reserve one
     * byte so `out->data` is real even when nothing is appended.
     */
    bytebuf_reserve(out, 1U);
    for (i = 0U; i < n; i++) {
        u8 c = s[i];

        if (c >= 0x80U) {
            /*
             * Pass bytes ≥ 0x80 through untouched, valid sequence or
             * not.  A well-formed sequence is one literal codepoint to
             * the parser; a stray byte becomes the same U+DCxx escape
             * on both sides, since pattern and subject are decoded by
             * the same rule.  Escaping it as \x{DCxx} would also work
             * and would make `/` and the message line unreadable for
             * every non-ASCII search.
             */
            bytebuf_push_u8(out, c);
        } else if (is_ascii_punct(c)) {
            bytebuf_push_u8(out, (u8)'\\');
            bytebuf_push_u8(out, c);
        } else if (c < 0x20U || c == 0x7FU) {
            /* Controls have no escapable spelling, and emitting a raw
             * newline into a pattern would end the line it is shown on. */
            bytebuf_push_u8(out, (u8)'\\');
            bytebuf_push_u8(out, (u8)'x');
            bytebuf_push_u8(out, (u8)hex[(c >> 4) & 0x0FU]);
            bytebuf_push_u8(out, (u8)hex[c & 0x0FU]);
        } else {
            /* Letters, digits and space: literal, and left legible. */
            bytebuf_push_u8(out, c);
        }
    }
}
