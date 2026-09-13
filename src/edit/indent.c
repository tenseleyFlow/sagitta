#include "edit/indent.h"

#include <string.h>

#include "edit/option.h"
#include "unicode/wordbreak.h"
#include "util/log.h"
#include "ui/viewport.h"

static bool ascii_white(u8 byte)
{
    return byte == (u8)' ' || byte == (u8)'\t' || byte == (u8)'\n' ||
           byte == (u8)'\r' || byte == (u8)'\v' || byte == (u8)'\f';
}

/* Most source and prose lines reveal their indentation entirely in ASCII.
 * Keep column calculation in unicode/coords.c, but avoid constructing a
 * grapheme reader for every unindented ASCII line in a large paragraph.
 * Returns false when a non-ASCII byte is reached before any non-blank one,
 * which is the only case that needs the cluster walk. */
static bool ascii_first_nonwhite(const TextBuf *tb, Span span,
                                 ByteOff *first, bool *blank)
{
    TextIter it;
    u64 consumed = 0U;

    *first = BYTEOFF(span.lo);
    *blank = true;
    if (span.lo == span.hi)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        YEW_BUG("indent classifier cannot start iterator");
    while (consumed < span.hi - span.lo) {
        const u8 *chunk;
        u64 chunk_len;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &chunk, &chunk_len))
            YEW_BUG("indent classifier cannot read iterator");
        take = span.hi - span.lo - consumed;
        if (take > chunk_len)
            take = chunk_len;
        for (u64 i = 0U; i < take; i++) {
            if (chunk[i] >= 0x80U)
                return false;
            if (!ascii_white(chunk[i])) {
                *first = BYTEOFF(span.lo + consumed + i);
                *blank = false;
                return true;
            }
        }
        consumed += take;
        if (consumed != span.hi - span.lo && !yew_textiter_advance(&it, tb))
            YEW_BUG("indent classifier iterator ended early");
    }
    return true;
}

static u8 line_byte(const TextBuf *tb, u64 off)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (!yew_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        YEW_BUG("indent: cannot read valid byte offset");
    return bytes[0];
}

static u64 line_content_hi(const TextBuf *tb, Span span)
{
    u64 hi = span.hi;

    if (hi != span.lo && line_byte(tb, hi - 1U) == (u8)'\n') {
        hi--;
        if (hi != span.lo && line_byte(tb, hi - 1U) == (u8)'\r')
            hi--;
    }
    return hi;
}

bool yew_indent_last_nonwhite(const TextBuf *tb, Span line, u8 *out)
{
    u64 at = line.hi;

    if (tb == NULL || out == NULL)
        return false;
    while (at != line.lo) {
        u8 byte;

        at--;
        byte = line_byte(tb, at);
        if (!ascii_white(byte)) {
            *out = byte;
            return true;
        }
    }
    return false;
}

bool yew_indent_info(const TextBuf *tb, Span line, u32 tabwidth,
                     IndentInfo *out)
{
    ByteOff first;
    bool blank = true;

    if (tb == NULL || out == NULL || line.hi < line.lo)
        return false;
    if (tabwidth == 0U)
        tabwidth = YEW_VP_TABWIDTH;
    out->blank = true;
    out->first = BYTEOFF(line_content_hi(tb, line));
    out->tabs = line.hi != line.lo && line_byte(tb, line.lo) == (u8)'\t';
    if (!ascii_first_nonwhite(tb, line, &first, &blank)) {
        ByteOff at = BYTEOFF(line.lo);

        while (at.v < line.hi) {
            YewTextCluster cluster;

            if (!yew_text_cluster_next(tb, line, at, &cluster))
                break;
            if (!yew_unicode_is_white_space(cluster.base_cp)) {
                blank = false;
                first = at;
                break;
            }
            at = BYTEOFF(cluster.bytes.hi);
        }
    }
    if (!blank) {
        out->blank = false;
        out->first = first;
    }
    out->width = yew_off_to_ccol(tb, line, out->first, tabwidth);
    return true;
}

u32 yew_indent_unit(const Buffer *b, u8 *out, u32 cap)
{
    u32 tabwidth = b != NULL && b->tabwidth != 0U ? b->tabwidth
                                                  : YEW_VP_TABWIDTH;

    if (out == NULL)
        return 0U;
    if (tabwidth > (u32)YEW_INDENT_UNIT_MAX)
        tabwidth = (u32)YEW_INDENT_UNIT_MAX;
    if (!yew_opt_buffer_bool(b, "expandtab", 9U)) {
        if (cap < 1U)
            return 0U;
        out[0] = (u8)'\t';
        return 1U;
    }
    if (cap < tabwidth)
        return 0U;
    (void)memset(out, ' ', (size_t)tabwidth);
    return tabwidth;
}

void yew_indent_unit_append(const Buffer *b, Bytebuf *out)
{
    u8 unit[YEW_INDENT_UNIT_MAX];
    u32 n = yew_indent_unit(b, unit, (u32)sizeof(unit));

    if (out != NULL && n != 0U)
        bytebuf_append(out, unit, (size_t)n);
}

void yew_indent_lead_append(const TextBuf *tb, Span lead, Bytebuf *out)
{
    TextIter it;
    u64 want;
    u64 done = 0U;

    if (tb == NULL || out == NULL || lead.hi <= lead.lo)
        return;
    want = lead.hi - lead.lo;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(lead.lo)))
        YEW_BUG("indent: cannot start leading-whitespace iterator");
    while (done < want) {
        const u8 *chunk;
        u64 chunk_len;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &chunk, &chunk_len))
            YEW_BUG("indent: cannot read leading whitespace");
        take = want - done;
        if (take > chunk_len)
            take = chunk_len;
        bytebuf_append(out, chunk, (size_t)take);
        done += take;
        if (done != want && !yew_textiter_advance(&it, tb))
            YEW_BUG("indent: leading whitespace ended early");
    }
}

ByteOff yew_indent_back(const TextBuf *tb, Span line, u32 tabwidth,
                        ByteOff from)
{
    IndentInfo info;
    CCol col;
    u64 target;
    ByteOff at;

    if (tb == NULL)
        return from;
    if (tabwidth == 0U)
        tabwidth = YEW_VP_TABWIDTH;
    if (from.v <= line.lo || from.v > line.hi ||
        !yew_indent_info(tb, line, tabwidth, &info) ||
        from.v > info.first.v)
        return yew_grapheme_prev_boundary(tb, from);
    col = yew_off_to_ccol(tb, line, from, tabwidth);
    if (col.v == 0U)
        return BYTEOFF(line.lo);
    target = ((col.v - 1U) / (u64)tabwidth) * (u64)tabwidth;
    at = from;
    while (at.v > line.lo &&
           yew_off_to_ccol(tb, line, at, tabwidth).v > target)
        at = yew_grapheme_prev_boundary(tb, at);
    return at;
}
