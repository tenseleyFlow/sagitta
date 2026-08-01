#include "unicode/utf8.h"

#include <assert.h>
#include <string.h>

/*
 * UTF-8 validity is encoded entirely in this 12-class, 9-state DFA.
 * Overlong encodings, surrogate encodings, and values above Unicode's
 * ceiling cannot reach ACC; the decoder performs no post-hoc range check.
 */
enum {
    SAG_U8_ACC = 0,
    SAG_U8_REJ,
    SAG_U8_T1,
    SAG_U8_T2,
    SAG_U8_T3,
    SAG_U8_T2A,
    SAG_U8_T2B,
    SAG_U8_T3A,
    SAG_U8_T3B
};

static const u8 u8_class[256] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
     3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
     3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
     4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  7,  7,
     9, 10, 10, 10, 11,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
};

static const u8 u8_next[9][12] = {
    {SAG_U8_ACC, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_T1,  SAG_U8_T2A, SAG_U8_T2,
     SAG_U8_T2B, SAG_U8_T3A, SAG_U8_T3,  SAG_U8_T3B},
    {SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_ACC, SAG_U8_ACC, SAG_U8_ACC,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_T1, SAG_U8_T1, SAG_U8_T1,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_T2, SAG_U8_T2, SAG_U8_T2,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_T1,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_T1, SAG_U8_T1, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_REJ, SAG_U8_T2, SAG_U8_T2,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ},
    {SAG_U8_REJ, SAG_U8_T2, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ,
     SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ, SAG_U8_REJ}
};

static const u8 lead_mask[12] = {
    0x7Fu, 0, 0, 0, 0, 0x1Fu, 0x0Fu, 0x0Fu, 0x0Fu, 0x07u, 0x07u, 0x07u
};

static u8 step(SagU8Dec *d, u8 b)
{
    u8 cls = u8_class[b];
    u8 st = u8_next[d->state][cls];

    d->cp = d->state == SAG_U8_ACC
                ? (u32)(b & lead_mask[cls])
                : (d->cp << 6) | (u32)(b & 0x3Fu);
    d->state = st;
    return st;
}

static void drop_pending(SagU8Dec *d, u8 count)
{
    assert(count <= d->n);
    d->n = (u8)(d->n - count);
    if (d->n != 0)
        memmove(d->pend, d->pend + count, d->n);
}

void sag_utf8_dec_init(SagU8Dec *d)
{
    memset(d, 0, sizeof(*d));
    d->state = SAG_U8_ACC;
}

u8 sag_utf8_push(SagU8Dec *d, u8 b)
{
    u8 out_len = 0;

    assert(d->n < SAG_UTF8_MAX);
    d->pend[d->n++] = b;

    while (d->n != 0) {
        u8 i;

        d->state = SAG_U8_ACC;
        d->cp = 0;
        for (i = 0; i < d->n; i++) {
            u8 state = step(d, d->pend[i]);

            if (state == SAG_U8_REJ) {
                assert(out_len < SAG_U8_MAX_FLUSH);
                d->out[out_len++] = sag_utf8_escape_of(d->pend[0]);
                drop_pending(d, 1);
                break;
            }
            if (state == SAG_U8_ACC) {
                assert(out_len < SAG_U8_MAX_FLUSH);
                d->out[out_len++] = d->cp;
                drop_pending(d, (u8)(i + 1));
                break;
            }
        }
        if (i == d->n && d->state != SAG_U8_ACC &&
            d->state != SAG_U8_REJ)
            return out_len;
    }

    d->state = SAG_U8_ACC;
    d->cp = 0;
    return out_len;
}

u8 sag_utf8_finish(SagU8Dec *d)
{
    u8 i;
    u8 n = d->n;

    for (i = 0; i < n; i++)
        d->out[i] = sag_utf8_escape_of(d->pend[i]);
    d->n = 0;
    d->state = SAG_U8_ACC;
    d->cp = 0;
    return n;
}

size_t sag_utf8_decode(const u8 *s, size_t len, u32 *out)
{
    SagU8Dec d;
    size_t i;
    size_t limit = len < SAG_UTF8_MAX ? len : SAG_UTF8_MAX;

    assert(out != NULL);
    if (len == 0) {
        *out = 0;
        return 0;
    }
    assert(s != NULL);

    sag_utf8_dec_init(&d);
    for (i = 0; i < limit; i++) {
        u8 state = step(&d, s[i]);

        if (state == SAG_U8_REJ) {
            *out = sag_utf8_escape_of(s[0]);
            return 1;
        }
        if (state == SAG_U8_ACC) {
            *out = d.cp;
            return i + 1;
        }
    }

    *out = sag_utf8_escape_of(s[0]);
    return 1;
}

size_t sag_utf8_decode_prev(const u8 *s, size_t start, size_t pos, u32 *out)
{
    size_t cursor;
    size_t previous_len = 0;
    u32 previous_cp = 0;

    assert(out != NULL);
    if (start >= pos) {
        *out = 0;
        return 0;
    }
    assert(s != NULL);

    cursor = pos - start > SAG_UTF8_MAX ? pos - SAG_UTF8_MAX : start;
    while (cursor < pos) {
        u32 cp;
        size_t n = sag_utf8_decode(s + cursor, pos - cursor, &cp);

        previous_cp = cp;
        previous_len = n;
        cursor += n;
    }

    *out = previous_cp;
    return previous_len;
}

bool sag_utf8_is_escape(u32 cp)
{
    return cp >= SAG_CP_ESC_LO && cp <= SAG_CP_ESC_HI;
}

u8 sag_utf8_escape_byte(u32 cp)
{
    assert(sag_utf8_is_escape(cp));
    return (u8)(cp - 0xDC00u);
}

u32 sag_utf8_escape_of(u8 b)
{
    assert(b >= 0x80u);
    return 0xDC00u + (u32)b;
}

size_t sag_utf8_encode(u32 cp, u8 out[SAG_UTF8_MAX])
{
    assert(out != NULL);

    if (sag_utf8_is_escape(cp)) {
        out[0] = sag_utf8_escape_byte(cp);
        return 1;
    }
    if (cp <= 0x7Fu) {
        out[0] = (u8)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        out[0] = (u8)(0xC0u | (cp >> 6));
        out[1] = (u8)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if ((cp >> 11) == 0x1Bu)
        return 0;
    if (cp <= 0xFFFFu) {
        out[0] = (u8)(0xE0u | (cp >> 12));
        out[1] = (u8)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (u8)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if ((cp >> 16) <= 0x10u) {
        out[0] = (u8)(0xF0u | (cp >> 18));
        out[1] = (u8)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (u8)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (u8)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

size_t sag_utf8_len(u32 cp)
{
    u8 encoded[SAG_UTF8_MAX];

    return sag_utf8_encode(cp, encoded);
}

size_t sag_utf8_validate(const u8 *s, size_t len)
{
    size_t pos = 0;
    size_t high_bits = 0U;
    size_t i;

    assert(s != NULL || len == 0U);
    for (i = 0U; i < sizeof(high_bits); i++)
        high_bits = (high_bits << 8) | 0x80U;

    while (pos < len) {
        u32 cp;
        size_t n;

        while (len - pos >= sizeof(size_t)) {
            size_t word;

            memcpy(&word, s + pos, sizeof(word));
            if ((word & high_bits) != 0U)
                break;
            pos += sizeof(word);
        }
        while (pos < len && s[pos] < 0x80U)
            pos++;
        if (pos == len)
            return len;
        n = sag_utf8_decode(s + pos, len - pos, &cp);

        if (sag_utf8_is_escape(cp))
            return pos;
        pos += n;
    }
    return len;
}

bool sag_utf8_is_boundary(const u8 *s, size_t len, size_t pos)
{
    size_t cursor = 0;

    if (pos > len)
        return false;
    if (pos == 0)
        return true;

    while (cursor < pos) {
        u32 cp;
        size_t n = sag_utf8_decode(s + cursor, len - cursor, &cp);

        (void)cp;
        cursor += n;
    }
    return cursor == pos;
}
