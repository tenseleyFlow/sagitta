/*
 * Sprint 31 deliverable 2: the `str` module.
 *
 * GRAPHEME CLUSTERS BY DEFAULT.  Indices are cluster indices unless the
 * name ends in _bytes, because a user who writes str.at(s, 0) means the
 * first thing they can see, not the first byte of a four-byte sequence.
 *
 * THE BYTE APIS ARE WHERE INVARIANT 2 DIES IF YOU ARE CARELESS.  A
 * byte-indexed slice that splits a cluster produces a string that still
 * "works" and renders as a broken accent three screens later, with
 * nothing in between to blame.  Every byte offset that crosses this
 * boundary is validated against sag_gb_prev_bytes and refused with kind
 * "index" naming cluster splitting, so the message teaches the fix.
 * The honest escape hatch for byte-oriented work is str.bytes and
 * str.from_bytes, which do not pretend to be about text.
 *
 * INVALID BYTES SURVIVE EVERYTHING.  s02 decodes them to U+DC80..DCFF
 * and every function here round-trips them byte for byte -- upper,
 * lower and trim included.  A string builtin that "cleans" its input is
 * a string builtin that loses data.
 */
#include "fl/std.h"

#include <stdlib.h>
#include <string.h>

#include "fl/gc.h"
#include "unicode/case.h"
#include "unicode/category.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "unicode/width.h"

/* Result cap shared by the functions that can multiply their input. */
enum { STR_MAX = 64U * 1024U * 1024U };

static bool too_big(FlVm *vm, size_t n, const char *who)
{
    if (n <= (size_t)STR_MAX)
        return false;
    (void)fl_raise(vm, "limit", "%s: result exceeds 64 MiB", who);
    return true;
}

static FlValue take(FlVm *vm, Bytebuf *bb)
{
    FlValue v = FL_OBJ_V(FL_STR, fl_str_take(vm, bb));

    bytebuf_free(bb);
    return v;
}

static FlValue slice_val(FlVm *vm, const FlStr *s, size_t lo, size_t hi)
{
    Bytebuf bb;

    bytebuf_init(&bb);
    if (hi > lo)
        bytebuf_append(&bb, s->b + lo, hi - lo);
    return take(vm, &bb);
}

/* ---------------------------------------------------------------- */
/* Cluster arithmetic                                               */
/* ---------------------------------------------------------------- */

/* Byte offset of cluster `i`, or len when i == count.  Walking is O(i);
 * strings in a config are short and a cluster index cache would be a
 * second source of truth about where clusters are. */
static size_t cluster_off(const FlStr *s, i64 i, size_t count, bool *ok)
{
    i64 want = i < 0 ? (i64)count + i : i;
    size_t at = 0U;
    i64 k;

    *ok = false;
    if (want < 0 || want > (i64)count)
        return 0U;
    for (k = 0; k < want; k++)
        at = sag_gb_next_bytes((const u8 *)s->b, s->len, at);
    *ok = true;
    return at;
}

/*
 * True when `off` sits ON a cluster boundary.
 *
 * sag_gb_prev_bytes returns the boundary STRICTLY BEFORE `off`, not the
 * one at it -- so comparing its result against `off` reports every
 * valid interior boundary as a split, which is the opposite of useful.
 * Stepping forward from that boundary lands exactly on `off` when `off`
 * is one, and past it when it is not.  s02 bounds a cluster at 64
 * codepoints, so the pair is amortized O(1).
 */
static bool on_boundary(const FlStr *s, size_t off)
{
    size_t prev;

    if (off == 0U || off == (size_t)s->len)
        return true;
    if (off > (size_t)s->len)
        return false;
    /*
     * A CODEPOINT boundary FIRST.
     *
     * sag_gb_prev_bytes assumes its argument starts a character.  Handed
     * an offset in the middle of a UTF-8 sequence it walks back from the
     * wrong place and answers with an offset that is not a boundary at
     * all -- for a ZWJ family, four of the twenty-four interior offsets
     * came back as boundaries and str.slice_bytes cut the cluster in
     * half at each of them.  A truncated sequence renders as a broken
     * glyph three screens later with nothing in between to blame, which
     * is the exact failure invariant 2 and DoD 4 exist to prevent.
     */
    if (!sag_utf8_is_boundary((const u8 *)s->b, s->len, off))
        return false;
    prev = sag_gb_prev_bytes((const u8 *)s->b, s->len, off);
    return sag_gb_next_bytes((const u8 *)s->b, s->len, prev) == off;
}

static bool byte_off(FlVm *vm, const FlStr *s, i64 i, size_t *out,
                     const char *who)
{
    i64 r = i < 0 ? (i64)s->len + i : i;

    if (r < 0 || r > (i64)s->len)
        return fl_raise(vm, "index", "%s: byte offset %lld out of range",
                        who, (long long)i);
    if (!on_boundary(s, (size_t)r))
        return fl_raise(vm, "index",
                        "%s: offset %lld splits a grapheme cluster",
                        who, (long long)r);
    *out = (size_t)r;
    return true;
}

static bool is_ws(u32 cp)
{
    /* White_Space: the ASCII controls, NEL, and the separators.  The
     * category trie carries Zs; the rest are named because they are not
     * separators by general category. */
    if (cp == 0x09U || cp == 0x0AU || cp == 0x0BU || cp == 0x0CU ||
        cp == 0x0DU || cp == 0x20U || cp == 0x85U ||
        cp == 0x2028U || cp == 0x2029U)
        return true;
    return (sag_cat_rec(cp) & (u16)SAG_CAT_ZS) != 0U;
}

/* ---------------------------------------------------------------- */
/* Length and access                                                */
/* ---------------------------------------------------------------- */

static bool s_len(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    *out = FL_INT_V((i64)sag_gb_count_bytes((const u8 *)s->b, s->len));
    return true;
}

static bool s_len_bytes(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    *out = FL_INT_V((i64)s->len);
    return true;
}

static bool s_is_empty(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    *out = FL_BOOL_V(s->len == 0U);
    return true;
}

static bool s_at(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    i64 i;
    size_t count;
    size_t lo;
    size_t hi;
    bool ok;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_int(vm, a, 1U, &i))
        return false;
    count = sag_gb_count_bytes((const u8 *)s->b, s->len);
    lo = cluster_off(s, i, count, &ok);
    if (!ok || lo == (size_t)s->len)
        return fl_raise(vm, "index", "str.at: index %lld out of range",
                        (long long)i);
    hi = sag_gb_next_bytes((const u8 *)s->b, s->len, lo);
    *out = slice_val(vm, s, lo, hi);
    return true;
}

static bool s_slice(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    i64 lo;
    i64 hi;
    size_t count;
    size_t a0;
    size_t a1;
    bool ok;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_int(vm, a, 1U, &lo))
        return false;
    count = sag_gb_count_bytes((const u8 *)s->b, s->len);
    hi = (i64)count;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &hi))
        return false;
    a0 = cluster_off(s, lo, count, &ok);
    if (!ok)
        return fl_raise(vm, "index", "str.slice: index %lld out of range",
                        (long long)lo);
    a1 = cluster_off(s, hi, count, &ok);
    if (!ok)
        return fl_raise(vm, "index", "str.slice: index %lld out of range",
                        (long long)hi);
    *out = slice_val(vm, s, a0, a1 < a0 ? a0 : a1);
    return true;
}

static bool s_slice_bytes(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    i64 lo;
    i64 hi;
    size_t a0;
    size_t a1;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_int(vm, a, 1U, &lo))
        return false;
    hi = (i64)s->len;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &hi))
        return false;
    /* BOTH offsets validated: this is the invariant-2 gate. */
    if (!byte_off(vm, s, lo, &a0, "str.slice_bytes") ||
        !byte_off(vm, s, hi, &a1, "str.slice_bytes"))
        return false;
    *out = slice_val(vm, s, a0, a1 < a0 ? a0 : a1);
    return true;
}

/* ---------------------------------------------------------------- */
/* Searching                                                        */
/* ---------------------------------------------------------------- */

/* Byte offset of the first occurrence at or after `from`, or -1. */
static i64 find_bytes_at(const FlStr *h, const FlStr *nd, size_t from)
{
    size_t i;

    if (nd->len == 0U)
        return (i64)from;
    if (nd->len > h->len)
        return -1;
    for (i = from; i + nd->len <= (size_t)h->len; i++) {
        if (memcmp(h->b + i, nd->b, nd->len) == 0)
            return (i64)i;
    }
    return -1;
}

static bool s_find(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *nd;
    i64 from = 0;
    size_t count;
    size_t start;
    bool ok;
    i64 at;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &nd))
        return false;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &from))
        return false;
    count = sag_gb_count_bytes((const u8 *)s->b, s->len);
    start = cluster_off(s, from, count, &ok);
    if (!ok)
        return fl_raise(vm, "index", "str.find: index %lld out of range",
                        (long long)from);
    at = find_bytes_at(s, nd, start);
    if (at < 0) {
        *out = FL_INT_V(-1);
        return true;
    }
    /* Reported as a CLUSTER index, so it can be fed back to slice/at.
     * A match landing mid-cluster counts the cluster it starts in. */
    {
        size_t off = 0U;
        i64 ci = 0;

        while (off < (size_t)at) {
            off = sag_gb_next_bytes((const u8 *)s->b, s->len, off);
            ci++;
        }
        *out = FL_INT_V(ci);
    }
    return true;
}

static bool s_find_bytes(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *nd;
    i64 from = 0;
    size_t start;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &nd))
        return false;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &from))
        return false;
    if (!byte_off(vm, s, from, &start, "str.find_bytes"))
        return false;
    *out = FL_INT_V(find_bytes_at(s, nd, start));
    return true;
}

static bool s_rfind(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *nd;
    i64 best = -1;
    i64 at = 0;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &nd))
        return false;
    for (;;) {
        i64 hit = find_bytes_at(s, nd, (size_t)at);

        if (hit < 0)
            break;
        best = hit;
        at = hit + 1;
        if ((size_t)at > (size_t)s->len)
            break;
    }
    if (best < 0) {
        *out = FL_INT_V(-1);
        return true;
    }
    {
        size_t off = 0U;
        i64 ci = 0;

        while (off < (size_t)best) {
            off = sag_gb_next_bytes((const u8 *)s->b, s->len, off);
            ci++;
        }
        *out = FL_INT_V(ci);
    }
    return true;
}

static bool s_contains(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *nd;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &nd))
        return false;
    *out = FL_BOOL_V(find_bytes_at(s, nd, 0U) >= 0);
    return true;
}

static bool s_starts_with(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *p;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &p))
        return false;
    /* A byte prefix, with no cluster question: a prefix that matches
     * bytes matches text, because it starts where the string starts. */
    *out = FL_BOOL_V(p->len <= s->len && memcmp(s->b, p->b, p->len) == 0);
    return true;
}

static bool s_ends_with(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *p;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &p))
        return false;
    *out = FL_BOOL_V(p->len <= s->len &&
                     memcmp(s->b + (s->len - p->len), p->b, p->len) == 0);
    return true;
}

/* ---------------------------------------------------------------- */
/* Case and trimming                                                */
/* ---------------------------------------------------------------- */

static bool case_map(FlVm *vm, FlValue *a, FlValue *out, SagCaseKind kind)
{
    const FlStr *s;
    Bytebuf bb;
    size_t at = 0U;

    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    bytebuf_init(&bb);
    while (at < (size_t)s->len) {
        u32 cp;
        size_t adv = sag_utf8_decode((const u8 *)s->b + at, s->len - at, &cp);
        u8 buf[SAG_CASE_MAX_UTF8];
        size_t got;

        if (adv == 0U)
            adv = 1U;
        /* sag_case_map_utf8 returns the input unchanged for identity
         * mappings, invalid-byte escapes included -- which is what
         * makes DoD 4's round-trip hold without a special case here. */
        got = sag_case_map_utf8(cp, kind, buf);
        bytebuf_append(&bb, (const char *)buf, got);
        at += adv;
    }
    *out = take(vm, &bb);
    return true;
}

static bool s_upper(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return case_map(vm, a, out, SAG_CASE_UPPER);
}

static bool s_lower(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return case_map(vm, a, out, SAG_CASE_LOWER);
}

static bool trim_impl(FlVm *vm, FlValue *a, FlValue *out, bool start,
                      bool end)
{
    const FlStr *s;
    size_t lo = 0U;
    size_t hi;

    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    hi = (size_t)s->len;
    if (start) {
        while (lo < hi) {
            u32 cp;
            size_t adv = sag_utf8_decode((const u8 *)s->b + lo, hi - lo, &cp);

            if (adv == 0U || !is_ws(cp))
                break;
            lo += adv;
        }
    }
    if (end) {
        while (hi > lo) {
            size_t prev = sag_gb_prev_bytes((const u8 *)s->b, s->len, hi);
            u32 cp;

            if (prev >= hi)
                break;
            (void)sag_utf8_decode((const u8 *)s->b + prev, hi - prev, &cp);
            if (!is_ws(cp))
                break;
            hi = prev;
        }
    }
    *out = slice_val(vm, s, lo, hi);
    return true;
}

static bool s_trim(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return trim_impl(vm, a, out, true, true);
}

static bool s_trim_start(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return trim_impl(vm, a, out, true, false);
}

static bool s_trim_end(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return trim_impl(vm, a, out, false, true);
}

/* ---------------------------------------------------------------- */
/* Splitting and joining                                            */
/* ---------------------------------------------------------------- */

static bool s_split(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *sep;
    i64 limit = 0;
    FlList *r;
    size_t at = 0U;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &sep))
        return false;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &limit))
        return false;
    if (limit < 0)
        return fl_raise(vm, "limit", "str.split: limit must not be negative");
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    if (sep->len == 0U) {
        /* An empty separator splits into CLUSTERS, not bytes -- the
         * whole module is cluster-first and splitting a string into
         * broken halves of an emoji would be the one place it was not. */
        while (at < (size_t)s->len) {
            size_t nxt = sag_gb_next_bytes((const u8 *)s->b, s->len, at);

            if (limit > 0 && (i64)r->n == limit - 1) {
                (void)fl_list_push(vm, r, slice_val(vm, s, at, s->len));
                at = (size_t)s->len;
                break;
            }
            (void)fl_list_push(vm, r, slice_val(vm, s, at, nxt));
            at = nxt;
        }
    } else {
        for (;;) {
            i64 hit;

            if (limit > 0 && (i64)r->n == limit - 1)
                break;
            hit = find_bytes_at(s, sep, at);
            if (hit < 0)
                break;
            (void)fl_list_push(vm, r, slice_val(vm, s, at, (size_t)hit));
            at = (size_t)hit + sep->len;
        }
        (void)fl_list_push(vm, r, slice_val(vm, s, at, s->len));
    }
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

/*
 * Shared with io.read_lines, which the sprint defines AS this function
 * applied to a file's bytes.  One implementation is the only way that
 * definition stays true.
 *
 * A trailing terminator produces a final empty line: "a\r\nb\r\n" is
 * three lines, the last of them empty.  That is what the sprint's
 * wording says and it is the reading that round-trips -- join(split(s))
 * is s only if the terminator is represented.
 */
FlValue fl_split_lines(FlVm *vm, const char *b, u32 n)
{
    FlList *r = fl_list_new(vm);
    size_t at = 0U;
    Bytebuf piece;

    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    while (at <= (size_t)n) {
        size_t nl = at;
        size_t end;

        while (nl < (size_t)n && b[nl] != '\n')
            nl++;
        end = nl;
        /* ONE trailing \r per line, so a CRLF file reads the same as a
         * LF one -- and a line that genuinely ends "\r\r" keeps the
         * first, because only the line terminator is ours to remove. */
        if (end > at && b[end - 1U] == '\r')
            end--;
        bytebuf_init(&piece);
        if (end > at)
            bytebuf_append(&piece, b + at, end - at);
        (void)fl_list_push(vm, r, take(vm, &piece));
        if (nl >= (size_t)n)
            break;
        at = nl + 1U;
    }
    fl_gc_release(vm, 1U);
    return FL_OBJ_V(FL_LIST, r);
}

static bool s_split_lines(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    *out = fl_split_lines(vm, s->b, s->len);
    return true;
}

static bool s_join(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    const FlStr *sep;
    Bytebuf bb;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l) || !fl_arg_str(vm, a, 1U, &sep))
        return false;
    bytebuf_init(&bb);
    for (i = 0U; i < l->n; i++) {
        const FlStr *it;

        if (l->v[i].t != (u8)FL_STR) {
            bytebuf_free(&bb);
            return fl_raise(vm, "type",
                            "str.join: item %u must be str, found %s",
                            (unsigned)i, fl_type_name((FlType)l->v[i].t));
        }
        it = (const FlStr *)l->v[i].as.o;
        if (i != 0U)
            bytebuf_append(&bb, sep->b, sep->len);
        bytebuf_append(&bb, it->b, it->len);
        if (too_big(vm, bb.len, "str.join")) {
            bytebuf_free(&bb);
            return false;
        }
    }
    *out = take(vm, &bb);
    return true;
}

static bool s_replace(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *from;
    const FlStr *to;
    i64 limit = 0;
    Bytebuf bb;
    size_t at = 0U;
    i64 done = 0;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &from) ||
        !fl_arg_str(vm, a, 2U, &to))
        return false;
    if (n >= 4U && !fl_arg_int(vm, a, 3U, &limit))
        return false;
    if (from->len == 0U) {
        *out = a[0];              /* nothing to find; no infinite loop */
        return true;
    }
    bytebuf_init(&bb);
    while (at < (size_t)s->len) {
        i64 hit = (limit > 0 && done >= limit)
                      ? -1 : find_bytes_at(s, from, at);

        if (hit < 0)
            break;
        bytebuf_append(&bb, s->b + at, (size_t)hit - at);
        bytebuf_append(&bb, to->b, to->len);
        /* Non-overlapping and left to right: resume PAST the match, so
         * replacing "aa" in "aaaa" yields two replacements. */
        at = (size_t)hit + from->len;
        done++;
        if (too_big(vm, bb.len, "str.replace")) {
            bytebuf_free(&bb);
            return false;
        }
    }
    bytebuf_append(&bb, s->b + at, (size_t)s->len - at);
    *out = take(vm, &bb);
    return true;
}

static bool s_repeat(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    i64 times;
    Bytebuf bb;
    i64 k;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_int(vm, a, 1U, &times))
        return false;
    if (times < 0)
        return fl_raise(vm, "type", "str.repeat: n must not be negative");
    /* Checked BEFORE building, not after: the point of the cap is to
     * refuse the allocation, not to notice it succeeded. */
    if (too_big(vm, (size_t)s->len * (size_t)times, "str.repeat"))
        return false;
    bytebuf_init(&bb);
    for (k = 0; k < times; k++)
        bytebuf_append(&bb, s->b, s->len);
    *out = take(vm, &bb);
    return true;
}

/* ---------------------------------------------------------------- */
/* Width and padding                                                */
/* ---------------------------------------------------------------- */

static bool s_width(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    i64 tabw = 8;

    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    if (n >= 2U && !fl_arg_int(vm, a, 1U, &tabw))
        return false;
    if (tabw < 0 || tabw > 256)
        return fl_raise(vm, "type", "str.width: tabw out of range");
    *out = FL_INT_V((i64)sag_str_width((const u8 *)s->b, s->len, (u32)tabw));
    return true;
}

static bool pad(FlVm *vm, FlValue *a, u32 n, FlValue *out, bool at_start)
{
    const FlStr *s;
    i64 want;
    const FlStr *fill = NULL;
    int have;
    int fw;
    Bytebuf bb;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_int(vm, a, 1U, &want))
        return false;
    if (n >= 3U && !fl_arg_str(vm, a, 2U, &fill))
        return false;
    /* CELLS, not clusters and not bytes, so padding lines up in a
     * terminal where a CJK cluster occupies two columns. */
    have = sag_str_width((const u8 *)s->b, s->len, 8U);
    fw = fill == NULL ? 1 : sag_str_width((const u8 *)fill->b, fill->len, 8U);
    if (fw <= 0 || want <= (i64)have) {
        *out = a[0];
        return true;
    }
    if (too_big(vm, (size_t)want, "str.pad"))
        return false;
    bytebuf_init(&bb);
    {
        i64 need = ((i64)want - (i64)have) / fw;
        i64 k;

        if (!at_start)
            bytebuf_append(&bb, s->b, s->len);
        for (k = 0; k < need; k++) {
            if (fill == NULL)
                bytebuf_push_u8(&bb, (u8)' ');
            else
                bytebuf_append(&bb, fill->b, fill->len);
        }
        if (at_start)
            bytebuf_append(&bb, s->b, s->len);
    }
    *out = take(vm, &bb);
    return true;
}

static bool s_pad_start(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    return pad(vm, a, n, out, true);
}

static bool s_pad_end(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    return pad(vm, a, n, out, false);
}

/* ---------------------------------------------------------------- */
/* The byte escape hatch                                            */
/* ---------------------------------------------------------------- */

static bool s_bytes(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    FlList *r;
    u32 i;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    if (too_big(vm, s->len, "str.bytes"))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    for (i = 0U; i < s->len; i++)
        (void)fl_list_push(vm, r, FL_INT_V((i64)(u8)s->b[i]));
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool s_from_bytes(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlList *l;
    Bytebuf bb;
    u32 i;

    (void)n;
    if (!fl_arg_list(vm, a, 0U, &l))
        return false;
    bytebuf_init(&bb);
    for (i = 0U; i < l->n; i++) {
        if (l->v[i].t != (u8)FL_INT || l->v[i].as.i < 0 ||
            l->v[i].as.i > 255) {
            bytebuf_free(&bb);
            return fl_raise(vm, "index",
                            "str.from_bytes: item %u is not a byte",
                            (unsigned)i);
        }
        bytebuf_push_u8(&bb, (u8)l->v[i].as.i);
    }
    /* NO UTF-8 VALIDATION, deliberately: this is the honest byte hatch,
     * and s02's decoder escapes whatever is not valid rather than
     * losing it.  Refusing here would make round-tripping a binary file
     * impossible. */
    *out = take(vm, &bb);
    return true;
}

/* ---------------------------------------------------------------- */
/* Conversion                                                       */
/* ---------------------------------------------------------------- */

static bool s_to_int(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    i64 base = 10;
    size_t i = 0U;
    bool neg = false;
    u64 acc = 0U;
    bool any = false;

    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    if (n >= 2U && !fl_arg_int(vm, a, 1U, &base))
        return false;
    if (base < 2 || base > 36)
        return fl_raise(vm, "type", "str.to_int: base must be 2..36");
    while (i < (size_t)s->len && (s->b[i] == ' ' || s->b[i] == '\t'))
        i++;
    if (i < (size_t)s->len && (s->b[i] == '+' || s->b[i] == '-')) {
        neg = s->b[i] == '-';
        i++;
    }
    for (; i < (size_t)s->len; i++) {
        char c = s->b[i];
        int d;

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= (int)base)
            break;
        acc = acc * (u64)base + (u64)d;
        any = true;
    }
    /* nil when NOT FULLY CONSUMED: "12abc" is not a number, and
     * returning 12 is how a typo becomes a silently wrong setting. */
    *out = (!any || i != (size_t)s->len)
               ? FL_NIL_V
               : FL_INT_V(neg ? -(i64)acc : (i64)acc);
    return true;
}

static bool s_to_float(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    char buf[64];
    char *end = NULL;
    double d;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    if (s->len == 0U || s->len >= sizeof(buf)) {
        *out = FL_NIL_V;
        return true;
    }
    (void)memcpy(buf, s->b, s->len);
    buf[s->len] = '\0';
    /*
     * strtod, as the Sprint 29 lexer already uses for float literals --
     * one rounding rule for the language, not two.
     *
     * Its decimal point is locale-dependent in general, which would be
     * a determinism hazard.  It is safe here because the C locale is in
     * force for the whole process: the library call that would change
     * it is itself refused by scripts/bans.sh, and that ban is what
     * this line depends on.
     */
    d = strtod(buf, &end);
    *out = (end == NULL || *end != '\0') ? FL_NIL_V : FL_FLOAT_V(d);
    return true;
}

static bool s_cmp(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *x;
    const FlStr *y;
    u32 m;
    int c;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &x) || !fl_arg_str(vm, a, 1U, &y))
        return false;
    m = x->len < y->len ? x->len : y->len;
    c = m == 0U ? 0 : memcmp(x->b, y->b, m);
    if (c == 0)
        c = x->len == y->len ? 0 : (x->len < y->len ? -1 : 1);
    /* BYTEWISE.  Spec §4 has no collation: a locale-aware order would
     * make a sorted config file reorder itself on another machine. */
    *out = FL_INT_V(c < 0 ? -1 : (c > 0 ? 1 : 0));
    return true;
}

/* ---------------------------------------------------------------- */

static const FlNativeDef STR_DEFS[] = {
    {"len",         s_len,         1U, 1U, 0U, "(s) -> int"},
    {"len_bytes",   s_len_bytes,   1U, 1U, 0U, "(s) -> int"},
    {"is_empty",    s_is_empty,    1U, 1U, 0U, "(s) -> bool"},
    {"at",          s_at,          2U, 2U, 0U, "(s, i) -> s"},
    {"slice",       s_slice,       2U, 3U, 0U, "(s, lo, [hi]) -> s"},
    {"slice_bytes", s_slice_bytes, 2U, 3U, 0U, "(s, lo, [hi]) -> s"},
    {"find",        s_find,        2U, 3U, 0U, "(s, needle, [from]) -> int"},
    {"find_bytes",  s_find_bytes,  2U, 3U, 0U, "(s, needle, [from]) -> int"},
    {"rfind",       s_rfind,       2U, 2U, 0U, "(s, needle) -> int"},
    {"contains",    s_contains,    2U, 2U, 0U, "(s, needle) -> bool"},
    {"starts_with", s_starts_with, 2U, 2U, 0U, "(s, p) -> bool"},
    {"ends_with",   s_ends_with,   2U, 2U, 0U, "(s, p) -> bool"},
    {"upper",       s_upper,       1U, 1U, 0U, "(s) -> s"},
    {"lower",       s_lower,       1U, 1U, 0U, "(s) -> s"},
    {"trim",        s_trim,        1U, 1U, 0U, "(s) -> s"},
    {"trim_start",  s_trim_start,  1U, 1U, 0U, "(s) -> s"},
    {"trim_end",    s_trim_end,    1U, 1U, 0U, "(s) -> s"},
    {"split",       s_split,       2U, 3U, 0U, "(s, sep, [limit]) -> list"},
    {"split_lines", s_split_lines, 1U, 1U, 0U, "(s) -> list"},
    {"join",        s_join,        2U, 2U, 0U, "(list, sep) -> s"},
    {"replace",     s_replace,     3U, 4U, 0U,
     "(s, from, to, [limit]) -> s"},
    {"repeat",      s_repeat,      2U, 2U, 0U, "(s, n) -> s"},
    {"pad_start",   s_pad_start,   2U, 3U, 0U, "(s, w, [fill]) -> s"},
    {"pad_end",     s_pad_end,     2U, 3U, 0U, "(s, w, [fill]) -> s"},
    {"width",       s_width,       1U, 2U, 0U, "(s, [tabw]) -> int"},
    {"bytes",       s_bytes,       1U, 1U, 0U, "(s) -> list"},
    {"from_bytes",  s_from_bytes,  1U, 1U, 0U, "(list) -> s"},
    {"to_int",      s_to_int,      1U, 2U, 0U, "(s, [base]) -> int|nil"},
    {"to_float",    s_to_float,    1U, 1U, 0U, "(s) -> float|nil"},
    {"cmp",         s_cmp,         2U, 2U, 0U, "(a, b) -> int"}
};

const FlModuleDef fl_mod_str = {
    "str", STR_DEFS, (u32)SAG_ARRAY_LEN(STR_DEFS), NULL, 0U
};
