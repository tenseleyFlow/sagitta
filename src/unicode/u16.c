#include "unicode/u16.h"

#include "unicode/utf8.h"
#include "util/log.h"

typedef struct {
    TextIter it;
    const TextBuf *tb;
    const u8 *bytes;
    u64 len;
    u64 at;
    u64 off;
    u64 end;
    bool live;
} U16Stream;

static void stream_init(U16Stream *s, const TextBuf *tb, u64 lo, u64 hi)
{
    s->tb = tb;
    s->bytes = NULL;
    s->len = 0U;
    s->at = 0U;
    s->off = lo;
    s->end = hi;
    s->live = lo < hi && yew_textiter_begin(&s->it, tb, BYTEOFF(lo));
    if (s->live &&
        !yew_textiter_chunk(&s->it, tb, &s->bytes, &s->len))
        YEW_BUG("u16 stream has no first piece");
}

static bool stream_take(U16Stream *s, u8 *out)
{
    if (!s->live || s->off >= s->end)
        return false;
    while (s->at == s->len) {
        if (!yew_textiter_advance(&s->it, s->tb)) {
            s->live = false;
            return false;
        }
        if (!yew_textiter_chunk(&s->it, s->tb, &s->bytes, &s->len))
            YEW_BUG("u16 stream lost its piece");
        s->at = 0U;
    }
    *out = s->bytes[(size_t)s->at++];
    s->off++;
    return true;
}

static u8 byte_at(const TextBuf *tb, u64 off)
{
    U16Stream s;
    u8 byte = 0U;

    stream_init(&s, tb, off, off + 1U);
    if (!stream_take(&s, &byte))
        YEW_BUG("u16 byte lookup ended early");
    return byte;
}

static u64 content_end(const TextBuf *tb, Span line)
{
    u64 end = line.hi;

    if (end > line.lo && byte_at(tb, end - 1U) == (u8)'\n') {
        end--;
        if (end > line.lo && byte_at(tb, end - 1U) == (u8)'\r')
            end--;
    }
    return end;
}

static size_t stream_decode(U16Stream *s, u32 *cp)
{
    u64 avail = s->len - s->at;
    u64 remain = s->end - s->off;
    U16Stream look;
    u8 encoded[YEW_UTF8_MAX];
    size_t have = 0U;

    if (avail >= YEW_UTF8_MAX || avail >= remain) {
        size_t n = avail < remain ? (size_t)avail : (size_t)remain;

        return yew_utf8_decode(s->bytes + (size_t)s->at, n, cp);
    }
    /* Only a piece seam needs iterator lookahead and the four-byte copy. */
    look = *s;
    while (have < YEW_UTF8_MAX && stream_take(&look, &encoded[have]))
        have++;
    if (have == 0U)
        return 0U;
    return yew_utf8_decode(encoded, have, cp);
}

static void stream_skip(U16Stream *s, size_t n)
{
    size_t i;

    for (i = 0U; i < n; i++) {
        u8 ignored;

        if (!stream_take(s, &ignored))
            YEW_BUG("u16 decode crossed the line end");
    }
}

static u64 scalar_units(u32 cp)
{
    return cp > 0xFFFFU ? 2U : 1U;
}

static u64 checked_content_end(const TextBuf *tb, Span line)
{
    if (tb == NULL)
        YEW_BUG("u16 conversion called with NULL buffer");
    if (line.lo > line.hi || line.hi > yew_textbuf_len(tb))
        YEW_BUG("u16 conversion received an invalid line span");
    return content_end(tb, line);
}

U16Col yew_off_to_u16col(const TextBuf *tb, Span line, ByteOff pos)
{
    u64 end = checked_content_end(tb, line);
    u64 target = pos.v;
    u64 units = 0U;
    U16Stream s;

    if (target < line.lo)
        target = line.lo;
    if (target > end)
        target = end;
    stream_init(&s, tb, line.lo, end);
    while (s.off < target) {
        u32 cp;
        size_t n = stream_decode(&s, &cp);

        if (n == 0U)
            YEW_BUG("u16 decode ended before its target");
        if (s.off + (u64)n > target)
            break;
        stream_skip(&s, n);
        units += scalar_units(cp);
    }
    return U16COL(units);
}

ByteOff yew_u16col_to_off(const TextBuf *tb, Span line, U16Col col)
{
    u64 end = checked_content_end(tb, line);
    u64 units = 0U;
    U16Stream s;

    stream_init(&s, tb, line.lo, end);
    while (s.off < end) {
        u32 cp;
        size_t n;
        u64 next;

        if (col.v == units)
            return BYTEOFF(s.off);
        n = stream_decode(&s, &cp);
        if (n == 0U)
            YEW_BUG("u16 decode ended before the line end");
        next = units + scalar_units(cp);
        if (col.v < next)
            return BYTEOFF(s.off);
        stream_skip(&s, n);
        units = next;
    }
    return BYTEOFF(end);
}
