#include "unicode/grapheme.h"

#include <assert.h>

#include "unicode/tables.h"
#include "unicode/utf8.h"
#include "unicode/width.h"

_Static_assert(sizeof(SagGbState) == 2,
               "grapheme restart state must remain two bytes");

enum { SAG_GBF_SEEN = 1u << 7 };

static SagGcb cp_gcb(u32 cp, u16 rec)
{
    if (sag_utf8_is_escape(cp))
        return SAG_GCB_CONTROL;
    return (SagGcb)(rec & SAG_U_GCB_MASK);
}

static SagIncb cp_incb(u16 rec)
{
    return (SagIncb)((rec & SAG_U_INCB_MASK) >> SAG_U_INCB_SHIFT);
}

static bool is_control(SagGcb gcb)
{
    return gcb == SAG_GCB_CONTROL || gcb == SAG_GCB_CR ||
           gcb == SAG_GCB_LF;
}

void sag_gb_init(SagGbState *st)
{
    assert(st != NULL);
    st->prev_gcb = SAG_GCB_OTHER;
    st->flags = 0;
}

static bool gb_break(const SagGbState *st, SagGcb curr, SagIncb incb,
                     bool ext_pict)
{
    SagGcb prev = (SagGcb)st->prev_gcb;

    if ((st->flags & SAG_GBF_SEEN) == 0)
        return true; /* GB1 */
    if (prev == SAG_GCB_CR && curr == SAG_GCB_LF)
        return false; /* GB3 */
    if (is_control(prev) || is_control(curr))
        return true; /* GB4, GB5 */
    if (prev == SAG_GCB_L &&
        (curr == SAG_GCB_L || curr == SAG_GCB_V ||
         curr == SAG_GCB_LV || curr == SAG_GCB_LVT))
        return false; /* GB6 */
    if ((prev == SAG_GCB_LV || prev == SAG_GCB_V) &&
        (curr == SAG_GCB_V || curr == SAG_GCB_T))
        return false; /* GB7 */
    if ((prev == SAG_GCB_LVT || prev == SAG_GCB_T) &&
        curr == SAG_GCB_T)
        return false; /* GB8 */
    if (curr == SAG_GCB_EXTEND || curr == SAG_GCB_ZWJ)
        return false; /* GB9 */
    if (curr == SAG_GCB_SPACINGMARK)
        return false; /* GB9a */
    if (prev == SAG_GCB_PREPEND)
        return false; /* GB9b */
    if (incb == SAG_INCB_CONSONANT &&
        (st->flags & (SAG_GBF_INCB_C | SAG_GBF_INCB_L)) ==
            (SAG_GBF_INCB_C | SAG_GBF_INCB_L))
        return false; /* GB9c */
    if (prev == SAG_GCB_ZWJ && (st->flags & SAG_GBF_PICT) != 0 &&
        ext_pict)
        return false; /* GB11 */
    if (curr == SAG_GCB_RI && (st->flags & SAG_GBF_RI_ODD) != 0)
        return false; /* GB12, GB13 */
    return true; /* GB999 (also GB1 for the zeroed state) */
}

static void gb_absorb(SagGbState *st, SagGcb curr, SagIncb incb,
                      bool ext_pict)
{
    SagGcb prev = (SagGcb)st->prev_gcb;
    u8 flags = st->flags;

    if (curr == SAG_GCB_RI)
        flags ^= SAG_GBF_RI_ODD;
    else
        flags &= (u8)~SAG_GBF_RI_ODD;

    if (ext_pict) {
        flags |= SAG_GBF_PICT;
    } else if (curr == SAG_GCB_EXTEND && prev != SAG_GCB_ZWJ) {
        /* Preserve ExtPict Extend*. Extend after the ZWJ is not GB11. */
    } else if (curr == SAG_GCB_ZWJ && prev != SAG_GCB_ZWJ &&
               (flags & SAG_GBF_PICT) != 0) {
        /* Preserve the chain for the single following pictograph. */
    } else {
        flags &= (u8)~SAG_GBF_PICT;
    }

    if (incb == SAG_INCB_CONSONANT) {
        flags |= SAG_GBF_INCB_C;
        flags &= (u8)~SAG_GBF_INCB_L;
    } else if (incb == SAG_INCB_LINKER &&
               (flags & SAG_GBF_INCB_C) != 0) {
        flags |= SAG_GBF_INCB_L;
    } else if (incb != SAG_INCB_EXTEND) {
        flags &= (u8)~(SAG_GBF_INCB_C | SAG_GBF_INCB_L);
    }

    st->prev_gcb = (u8)curr;
    st->flags = flags | SAG_GBF_SEEN;
}

bool sag_gb_boundary(SagGbState *st, u32 cp)
{
    SagGcb curr;
    SagIncb incb;
    bool ext_pict;
    bool boundary;
    u16 rec = 0;

    assert(st != NULL);

    if (!sag_utf8_is_escape(cp) && cp < 0x80u) {
        if (cp == '\r')
            curr = SAG_GCB_CR;
        else if (cp == '\n')
            curr = SAG_GCB_LF;
        else if (cp < 0x20u || cp == 0x7Fu)
            curr = SAG_GCB_CONTROL;
        else
            curr = SAG_GCB_OTHER;
        incb = SAG_INCB_NONE;
        ext_pict = false;
    } else {
        if (!sag_utf8_is_escape(cp))
            rec = sag_u_rec(cp);
        curr = cp_gcb(cp, rec);
        incb = cp_incb(rec);
        ext_pict = (rec & SAG_U_EXT_PICT) != 0;
    }

    /* Hot ASCII text reaches GB999 without a table lookup. */
    if (st->prev_gcb == SAG_GCB_OTHER && cp >= 0x20u && cp < 0x7Fu)
        boundary = true;
    else
        boundary = gb_break(st, curr, incb, ext_pict);

    gb_absorb(st, curr, incb, ext_pict);
    return boundary;
}

size_t sag_gb_next_bytes(const u8 *s, size_t len, size_t pos)
{
    SagGbState st;
    size_t off;
    u32 cp;

    if (pos >= len)
        return len;
    assert(s != NULL);

    sag_gb_init(&st);
    off = pos;
    off += sag_utf8_decode(s + off, len - off, &cp);
    (void)sag_gb_boundary(&st, cp);
    while (off < len) {
        size_t n = sag_utf8_decode(s + off, len - off, &cp);
        if (sag_gb_boundary(&st, cp))
            break;
        off += n;
    }
    return off;
}

static bool safe_restart_cp(u32 cp)
{
    u16 rec;
    SagGcb gcb;
    SagIncb incb;

    if (sag_utf8_is_escape(cp))
        return true;
    rec = sag_u_rec(cp);
    gcb = cp_gcb(cp, rec);
    incb = cp_incb(rec);
    return (gcb == SAG_GCB_OTHER || is_control(gcb)) &&
           gcb != SAG_GCB_RI && (rec & SAG_U_EXT_PICT) == 0 &&
           incb != SAG_INCB_CONSONANT;
}

static bool cp_is_prepend(u32 cp)
{
    return !sag_utf8_is_escape(cp) &&
           cp_gcb(cp, sag_u_rec(cp)) == SAG_GCB_PREPEND;
}

size_t sag_gb_prev_bytes(const u8 *s, size_t len, size_t pos)
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
        size_t n = sag_utf8_decode_prev(s, 0, cursor, &cp);
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
        size_t next = sag_gb_next_bytes(s, len, cursor);
        if (next >= pos)
            return cursor;
        if (next <= cursor)
            break;
        cursor = next;
    }
    return restart;
}

size_t sag_gb_count_bytes(const u8 *s, size_t len)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t next = sag_gb_next_bytes(s, len, pos);
        assert(next > pos);
        pos = next;
        count++;
    }
    return count;
}

bool sag_cluster_next(const u8 *s, size_t len, size_t *pos,
                      SagCluster *out)
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
    end = sag_gb_next_bytes(s, len, off);
    out->off = off;
    out->len = end - off;
    out->base_cp = 0;

    scan = off;
    while (scan < end) {
        u16 rec;
        SagGcb gcb;
        scan += sag_utf8_decode(s + scan, end - scan, &cp);
        rec = sag_utf8_is_escape(cp) ? 0 : sag_u_rec(cp);
        gcb = cp_gcb(cp, rec);
        if (out->base_cp == 0 && gcb != SAG_GCB_PREPEND)
            out->base_cp = cp;
    }
    if (out->base_cp == 0)
        (void)sag_utf8_decode(s + off, end - off, &out->base_cp);

    if (out->len == 1 && s[off] == '\t')
        out->cells = SAG_CLUSTER_TAB;
    else
        out->cells = (u8)sag_cluster_width(s + off, out->len);
    *pos = end;
    return true;
}
