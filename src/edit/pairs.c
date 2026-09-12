#include "edit/pairs.h"

#include <string.h>

#include "edit/block.h"
#include "edit/buf.h"
#include "edit/option.h"
#include "util/log.h"

typedef struct PairSpec {
    u8 open;
    u8 close;
} PairSpec;

typedef struct PairLang {
    const char *name;
    const PairSpec *v;
    u32 n;
} PairLang;

/*
 * The default set.  A quote pair is one whose opener equals its closer;
 * those additionally refuse to close after a word byte, so `don't` does not
 * become `don''t`.
 */
static const PairSpec pairs_default[] = {
    {(u8)'(', (u8)')'}, {(u8)'[', (u8)']'}, {(u8)'{', (u8)'}'},
    {(u8)'"', (u8)'"'}, {(u8)'\'', (u8)'\''}
};

/* Languages whose backtick is a real delimiter (command substitution,
 * template literals, code spans). */
static const PairSpec pairs_backtick[] = {
    {(u8)'(', (u8)')'}, {(u8)'[', (u8)']'}, {(u8)'{', (u8)'}'},
    {(u8)'"', (u8)'"'}, {(u8)'\'', (u8)'\''}, {(u8)'`', (u8)'`'}
};

/* Languages where a lone apostrophe is a lifetime, a type variable or a
 * prime, so closing it is always wrong. */
static const PairSpec pairs_no_apostrophe[] = {
    {(u8)'(', (u8)')'}, {(u8)'[', (u8)']'}, {(u8)'{', (u8)'}'},
    {(u8)'"', (u8)'"'}
};

#define PAIR_SET(v_) (v_), (u32)(sizeof(v_) / sizeof((v_)[0]))

static const PairLang pairs_by_lang[] = {
    {"fish", PAIR_SET(pairs_backtick)},
    {"go", PAIR_SET(pairs_backtick)},
    {"haskell", PAIR_SET(pairs_no_apostrophe)},
    {"javascript", PAIR_SET(pairs_backtick)},
    {"markdown", PAIR_SET(pairs_backtick)},
    {"ocaml", PAIR_SET(pairs_no_apostrophe)},
    {"powershell", PAIR_SET(pairs_backtick)},
    {"ruby", PAIR_SET(pairs_backtick)},
    {"rust", PAIR_SET(pairs_no_apostrophe)},
    {"sh", PAIR_SET(pairs_backtick)},
    {"typescript", PAIR_SET(pairs_backtick)},
    {"zsh", PAIR_SET(pairs_backtick)}
};

bool yew_pairs_interesting(u8 byte)
{
    switch (byte) {
    case (u8)'(':
    case (u8)')':
    case (u8)'[':
    case (u8)']':
    case (u8)'{':
    case (u8)'}':
    case (u8)'"':
    case (u8)'\'':
    case (u8)'`':
        return true;
    default:
        return false;
    }
}

static const PairSpec *pairs_table(const Buffer *b, u32 *n)
{
    u32 i;

    if (b != NULL && b->lang != NULL) {
        for (i = 0U; i < (u32)(sizeof(pairs_by_lang) /
                               sizeof(pairs_by_lang[0])); i++) {
            if (strcmp(b->lang, pairs_by_lang[i].name) == 0) {
                *n = pairs_by_lang[i].n;
                return pairs_by_lang[i].v;
            }
        }
    }
    *n = (u32)(sizeof(pairs_default) / sizeof(pairs_default[0]));
    return pairs_default;
}

static bool pairs_byte_at(const TextBuf *tb, u64 off, u8 *out)
{
    TextIter it;
    const u8 *bytes;
    u64 n;

    return off < yew_textbuf_len(tb) &&
           yew_textiter_begin(&it, tb, BYTEOFF(off)) &&
           yew_textiter_chunk(&it, tb, &bytes, &n) && n != 0U &&
           (*out = bytes[0], true);
}

static bool pairs_word_byte(u8 byte)
{
    return (byte >= (u8)'a' && byte <= (u8)'z') ||
           (byte >= (u8)'A' && byte <= (u8)'Z') ||
           (byte >= (u8)'0' && byte <= (u8)'9') ||
           byte == (u8)'_' || byte >= 0x80U;
}

static bool pairs_alive(const Buffer *b, const PairMark *entry)
{
    return b->marks != NULL && yew_mark_alive(b->marks, entry->close);
}

static void pairs_drop_from(Buffer *b, u32 first)
{
    u32 i;

    for (i = first; i < (u32)b->pairs.n; i++) {
        if (b->marks != NULL && yew_mark_alive(b->marks,
                                               b->pairs.v[i].close))
            yew_mark_del(b->marks, b->pairs.v[i].close);
    }
    b->pairs.n = (u8)first;
}

/*
 * Forgets ONE entry.  Entries above it are not necessarily nested inside
 * it: with several cursors the stack interleaves sibling pairs, and
 * dropping the tail would make every other cursor's closer duplicate
 * instead of skip.  A stale inner entry is harmless — its mark simply
 * never matches again, and the stack bound retires it.
 */
static void pairs_drop_at(Buffer *b, u32 at)
{
    if (b->marks != NULL && yew_mark_alive(b->marks, b->pairs.v[at].close))
        yew_mark_del(b->marks, b->pairs.v[at].close);
    if (at + 1U < (u32)b->pairs.n)
        (void)memmove(&b->pairs.v[at], &b->pairs.v[at + 1U],
                      sizeof(b->pairs.v[0]) *
                          (size_t)((u32)b->pairs.n - at - 1U));
    b->pairs.n = (u8)(b->pairs.n - 1U);
}

void yew_pairs_clear(Buffer *b)
{
    if (b == NULL)
        return;
    pairs_drop_from(b, 0U);
    b->pairs.syn_valid = false;
}

void yew_pairs_note_edit(Buffer *b, LineNo line)
{
    if (b == NULL || b->tb == NULL || !b->pairs.syn_valid)
        return;
    if (b->pairs.syn_line == line.v ||
        b->pairs.syn_lines != yew_textbuf_line_count(b->tb))
        b->pairs.syn_valid = false;
}

static bool pairs_in_string_or_comment(Buffer *b, ByteOff at)
{
    u64 line;
    u64 lines;

    if (b->tb == NULL)
        return false;
    line = yew_textbuf_line_of(b->tb, at).v;
    lines = yew_textbuf_line_count(b->tb);
    if (b->pairs.syn_valid && b->pairs.syn_line == line &&
        b->pairs.syn_lines == lines)
        return b->pairs.syn_in;
    /*
     * Fails open (false) when the settle wave has not reached the line or
     * the buffer has no language.  Pairing in unsettled text is a cosmetic
     * annoyance; refusing to type is not.
     */
    b->pairs.syn_in = yew_syn_in_string_or_comment(b, at);
    b->pairs.syn_line = line;
    b->pairs.syn_lines = lines;
    b->pairs.syn_valid = true;
    return b->pairs.syn_in;
}

PairAction yew_pairs_decide(Buffer *b, ByteOff at, u8 byte, u8 *closer)
{
    const PairSpec *table;
    u32 n = 0U;
    u32 i;
    u8 prev = 0U;

    if (b == NULL || b->tb == NULL || closer == NULL ||
        !yew_pairs_interesting(byte) ||
        !yew_opt_buffer_bool(b, "autopair", 8U))
        return YEW_PAIR_LITERAL;

    /*
     * Type-over first, so a quote (whose opener IS its closer) skips an
     * auto-inserted partner instead of opening a second pair.
     */
    for (i = (u32)b->pairs.n; i-- > 0U;) {
        const PairMark *entry = &b->pairs.v[i];
        u8 here = 0U;

        if (!pairs_alive(b, entry))
            continue;
        if (entry->closer != byte ||
            yew_mark_pos(b->marks, entry->close).v != at.v)
            continue;
        /*
         * A mark inside a deleted range CLAMPS to the deletion point
         * rather than dying (text/mark.c adjust_delete), so a remembered
         * closer can end up pointing at a byte that is not that closer.
         * Skipping there would advance the caret over someone else's
         * byte — invariant 2.  Verify the byte, and retire the entry when
         * it lies.
         */
        if (!pairs_byte_at(b->tb, at.v, &here) || here != byte) {
            pairs_drop_at(b, i);
            continue;
        }
        pairs_drop_at(b, i);
        return YEW_PAIR_SKIP;
    }

    table = pairs_table(b, &n);
    for (i = 0U; i < n; i++) {
        if (table[i].open != byte)
            continue;
        if (table[i].open == table[i].close && at.v != 0U &&
            pairs_byte_at(b->tb, at.v - 1U, &prev) && pairs_word_byte(prev))
            return YEW_PAIR_LITERAL;
        if (pairs_in_string_or_comment(b, at))
            return YEW_PAIR_LITERAL;
        *closer = table[i].close;
        return YEW_PAIR_CLOSE;
    }
    return YEW_PAIR_LITERAL;
}

void yew_pairs_remember(Buffer *b, ByteOff closer_at, u8 closer)
{
    PairMark entry;

    if (b == NULL || b->marks == NULL)
        return;
    if ((u32)b->pairs.n >= (u32)YEW_PAIR_STACK_MAX) {
        /* Oldest first: the outermost pair is the one the caret is least
         * likely to still be inside. */
        if (yew_mark_alive(b->marks, b->pairs.v[0].close))
            yew_mark_del(b->marks, b->pairs.v[0].close);
        (void)memmove(&b->pairs.v[0], &b->pairs.v[1],
                      sizeof(b->pairs.v[0]) *
                          (size_t)(YEW_PAIR_STACK_MAX - 1));
        b->pairs.n = (u8)(YEW_PAIR_STACK_MAX - 1);
    }
    entry.close = yew_mark_add(b->marks, closer_at, YEW_BIAS_RIGHT);
    entry.closer = closer;
    b->pairs.v[b->pairs.n] = entry;
    b->pairs.n = (u8)(b->pairs.n + 1U);
}

static bool pairs_opener_of(const Buffer *b, u8 closer, u8 *out)
{
    const PairSpec *table;
    u32 n = 0U;
    u32 i;

    table = pairs_table(b, &n);
    for (i = 0U; i < n; i++) {
        if (table[i].close == closer) {
            *out = table[i].open;
            return true;
        }
    }
    return false;
}

bool yew_pairs_backspace(Buffer *b, ByteOff at, Span *both)
{
    u32 i;
    u8 here = 0U;
    u8 prev = 0U;
    u8 opener = 0U;

    if (b == NULL || b->tb == NULL || both == NULL || at.v == 0U ||
        !yew_opt_buffer_bool(b, "autopair", 8U) ||
        !pairs_byte_at(b->tb, at.v, &here) ||
        !pairs_byte_at(b->tb, at.v - 1U, &prev) ||
        !pairs_opener_of(b, here, &opener) || prev != opener)
        return false;
    /* The pair must still be EMPTY: the remembered closer sits at the
     * caret and its opener immediately before it. */
    for (i = (u32)b->pairs.n; i-- > 0U;) {
        const PairMark *entry = &b->pairs.v[i];

        if (!pairs_alive(b, entry))
            continue;
        if (entry->closer != here ||
            yew_mark_pos(b->marks, entry->close).v != at.v)
            continue;
        pairs_drop_at(b, i);
        both->lo = at.v - 1U;
        both->hi = at.v + 1U;
        return true;
    }
    return false;
}
