#include "unicode/grapheme.h"

#include <assert.h>

#include "unicode/tables.h"
#include "unicode/utf8.h"
#include "unicode/width.h"

_Static_assert(sizeof(YewGbState) == 2,
               "grapheme restart state must remain two bytes");

enum { YEW_GBF_SEEN = 1u << 7 };

static YewGcb cp_gcb(u32 cp, u16 rec)
{
    if (yew_utf8_is_escape(cp))
        return YEW_GCB_CONTROL;
    return (YewGcb)(rec & YEW_U_GCB_MASK);
}

static YewIncb cp_incb(u16 rec)
{
    return (YewIncb)((rec & YEW_U_INCB_MASK) >> YEW_U_INCB_SHIFT);
}

static bool is_control(YewGcb gcb)
{
    return gcb == YEW_GCB_CONTROL || gcb == YEW_GCB_CR ||
           gcb == YEW_GCB_LF;
}

void yew_gb_init(YewGbState *st)
{
    assert(st != NULL);
    st->prev_gcb = YEW_GCB_OTHER;
    st->flags = 0;
}

static bool gb_break(const YewGbState *st, YewGcb curr, YewIncb incb,
                     bool ext_pict)
{
    YewGcb prev = (YewGcb)st->prev_gcb;

    if ((st->flags & YEW_GBF_SEEN) == 0)
        return true; /* GB1 */
    if (prev == YEW_GCB_CR && curr == YEW_GCB_LF)
        return false; /* GB3 */
    if (is_control(prev) || is_control(curr))
        return true; /* GB4, GB5 */
    if (prev == YEW_GCB_L &&
        (curr == YEW_GCB_L || curr == YEW_GCB_V ||
         curr == YEW_GCB_LV || curr == YEW_GCB_LVT))
        return false; /* GB6 */
    if ((prev == YEW_GCB_LV || prev == YEW_GCB_V) &&
        (curr == YEW_GCB_V || curr == YEW_GCB_T))
        return false; /* GB7 */
    if ((prev == YEW_GCB_LVT || prev == YEW_GCB_T) &&
        curr == YEW_GCB_T)
        return false; /* GB8 */
    if (curr == YEW_GCB_EXTEND || curr == YEW_GCB_ZWJ)
        return false; /* GB9 */
    if (curr == YEW_GCB_SPACINGMARK)
        return false; /* GB9a */
    if (prev == YEW_GCB_PREPEND)
        return false; /* GB9b */
    if (incb == YEW_INCB_CONSONANT &&
        (st->flags & (YEW_GBF_INCB_C | YEW_GBF_INCB_L)) ==
            (YEW_GBF_INCB_C | YEW_GBF_INCB_L))
        return false; /* GB9c */
    if (prev == YEW_GCB_ZWJ && (st->flags & YEW_GBF_PICT) != 0 &&
        ext_pict)
        return false; /* GB11 */
    if (curr == YEW_GCB_RI && (st->flags & YEW_GBF_RI_ODD) != 0)
        return false; /* GB12, GB13 */
    return true; /* GB999 (also GB1 for the zeroed state) */
}

static void gb_absorb(YewGbState *st, YewGcb curr, YewIncb incb,
                      bool ext_pict)
{
    YewGcb prev = (YewGcb)st->prev_gcb;
    u8 flags = st->flags;

    if (curr == YEW_GCB_RI)
        flags ^= YEW_GBF_RI_ODD;
    else
        flags &= (u8)~YEW_GBF_RI_ODD;

    if (ext_pict) {
        flags |= YEW_GBF_PICT;
    } else if (curr == YEW_GCB_EXTEND && prev != YEW_GCB_ZWJ) {
        /* Preserve ExtPict Extend*. Extend after the ZWJ is not GB11. */
    } else if (curr == YEW_GCB_ZWJ && prev != YEW_GCB_ZWJ &&
               (flags & YEW_GBF_PICT) != 0) {
        /* Preserve the chain for the single following pictograph. */
    } else {
        flags &= (u8)~YEW_GBF_PICT;
    }

    if (incb == YEW_INCB_CONSONANT) {
        flags |= YEW_GBF_INCB_C;
        flags &= (u8)~YEW_GBF_INCB_L;
    } else if (incb == YEW_INCB_LINKER &&
               (flags & YEW_GBF_INCB_C) != 0) {
        flags |= YEW_GBF_INCB_L;
    } else if (incb != YEW_INCB_EXTEND) {
        flags &= (u8)~(YEW_GBF_INCB_C | YEW_GBF_INCB_L);
    }

    st->prev_gcb = (u8)curr;
    st->flags = flags | YEW_GBF_SEEN;
}

bool yew_gb_boundary(YewGbState *st, u32 cp)
{
    YewGcb curr;
    YewIncb incb;
    bool ext_pict;
    bool boundary;
    u16 rec = 0;

    assert(st != NULL);

    if (st->prev_gcb == YEW_GCB_OTHER && cp >= 0x20u && cp < 0x7Fu) {
        st->flags = YEW_GBF_SEEN;
        return true;
    }

    if (!yew_utf8_is_escape(cp) && cp < 0x80u) {
        if (cp == '\r')
            curr = YEW_GCB_CR;
        else if (cp == '\n')
            curr = YEW_GCB_LF;
        else if (cp < 0x20u || cp == 0x7Fu)
            curr = YEW_GCB_CONTROL;
        else
            curr = YEW_GCB_OTHER;
        incb = YEW_INCB_NONE;
        ext_pict = false;
    } else {
        if (!yew_utf8_is_escape(cp))
            rec = yew_u_rec(cp);
        curr = cp_gcb(cp, rec);
        incb = cp_incb(rec);
        ext_pict = (rec & YEW_U_EXT_PICT) != 0;
    }

    boundary = gb_break(st, curr, incb, ext_pict);

    gb_absorb(st, curr, incb, ext_pict);
    return boundary;
}

size_t yew_gb_next_bytes(const u8 *s, size_t len, size_t pos)
{
    YewGbState st;
    size_t off;
    u32 cp;

    if (pos >= len)
        return len;
    assert(s != NULL);

    yew_gb_init(&st);
    off = pos;
    off += yew_utf8_decode(s + off, len - off, &cp);
    (void)yew_gb_boundary(&st, cp);
    while (off < len) {
        size_t n = yew_utf8_decode(s + off, len - off, &cp);
        if (yew_gb_boundary(&st, cp))
            break;
        off += n;
    }
    return off;
}

static bool safe_restart_cp(u32 cp)
{
    u16 rec;
    YewGcb gcb;
    YewIncb incb;

    if (yew_utf8_is_escape(cp))
        return true;
    rec = yew_u_rec(cp);
    gcb = cp_gcb(cp, rec);
    incb = cp_incb(rec);
    return (gcb == YEW_GCB_OTHER || is_control(gcb)) &&
           gcb != YEW_GCB_RI && (rec & YEW_U_EXT_PICT) == 0 &&
           incb != YEW_INCB_CONSONANT;
}

static bool cp_is_prepend(u32 cp)
{
    return !yew_utf8_is_escape(cp) &&
           cp_gcb(cp, yew_u_rec(cp)) == YEW_GCB_PREPEND;
}

size_t yew_gb_prev_bytes(const u8 *s, size_t len, size_t pos)
{
    size_t restart;
    size_t cursor;
    size_t scanned = 0;
    bool found_safe = false;
    u32 cp;

    if (pos == 0 || len == 0)
        return 0;
    assert(s != NULL);
    if (pos > len)
        pos = len;

    restart = pos;
    cursor = pos;
    while (cursor > 0 && scanned < 64) {
        size_t n = yew_utf8_decode_prev(s, 0, cursor, &cp);
        if (n == 0 || n > cursor)
            break;
        cursor -= n;
        restart = cursor;
        scanned++;
        if (found_safe && !cp_is_prepend(cp))
            break;
        if (!found_safe)
            found_safe = safe_restart_cp(cp);
    }

    cursor = restart;
    while (cursor < pos) {
        size_t next = yew_gb_next_bytes(s, len, cursor);
        if (next >= pos)
            return cursor;
        if (next <= cursor)
            break;
        cursor = next;
    }
    return restart;
}

size_t yew_gb_count_bytes(const u8 *s, size_t len)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t next = yew_gb_next_bytes(s, len, pos);
        assert(next > pos);
        pos = next;
        count++;
    }
    return count;
}

bool yew_cluster_next(const u8 *s, size_t len, size_t *pos,
                      YewCluster *out)
{
    size_t off;
    size_t end;
    size_t scan;
    u32 cp;

    assert(pos != NULL);
    assert(out != NULL);
    if (*pos >= len)
        return false;
    assert(s != NULL);

    off = *pos;
    end = yew_gb_next_bytes(s, len, off);
    out->off = off;
    out->len = end - off;
    out->base_cp = 0;

    scan = off;
    while (scan < end) {
        u16 rec;
        YewGcb gcb;
        scan += yew_utf8_decode(s + scan, end - scan, &cp);
        rec = yew_utf8_is_escape(cp) ? 0 : yew_u_rec(cp);
        gcb = cp_gcb(cp, rec);
        if (out->base_cp == 0 && gcb != YEW_GCB_PREPEND)
            out->base_cp = cp;
    }
    if (out->base_cp == 0)
        (void)yew_utf8_decode(s + off, end - off, &out->base_cp);

    if (out->len == 1 && s[off] == '\t')
        out->cells = YEW_CLUSTER_TAB;
    else
        out->cells = (u8)yew_cluster_width(s + off, out->len);
    *pos = end;
    return true;
}
