#include "oracle.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

static size_t checked_size(u64 value)
{
    if (value > (u64)SIZE_MAX)
        SAG_BUG("oracle buffer exceeds address space");
    return (size_t)value;
}

static void lines_reserve(OracleLines *lines, size_t need)
{
    size_t cap;

    if (lines->cap >= need)
        return;
    cap = lines->cap ? lines->cap : 8U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    lines->data = sag_xreallocarray(lines->data, cap, sizeof(*lines->data));
    lines->cap = cap;
}

static void lines_clear(Oracle *o)
{
    size_t i;

    for (i = 0U; i < o->lines.len; i++)
        bytebuf_free(&o->lines.data[i]);
    o->lines.len = 0U;
}

static void lines_push(Oracle *o, const u8 *bytes, size_t len)
{
    Bytebuf line;

    lines_reserve(&o->lines, o->lines.len + 1U);
    bytebuf_init(&line);
    bytebuf_append(&line, bytes, len);
    o->lines.data[o->lines.len++] = line;
}

static void oracle_split(Oracle *o, const u8 *bytes, size_t len)
{
    size_t start = 0U;
    size_t i;

    lines_clear(o);
    for (i = 0U; i < len; i++) {
        if (bytes[i] == (u8)'\n') {
            lines_push(o, bytes + start, i - start + 1U);
            start = i + 1U;
        }
    }
    /* A zero-byte file has one empty line.  A final LF likewise starts an
       empty last line, matching TextBuf's newline-count-plus-one contract. */
    lines_push(o, len == 0U ? NULL : bytes + start, len - start);
}

void oracle_init(Oracle *o, const u8 *bytes, u64 len)
{
    size_t n = checked_size(len);

    if (o == NULL || (n != 0U && bytes == NULL))
        SAG_BUG("oracle_init: invalid argument");
    o->lines.data = NULL;
    o->lines.len = 0U;
    o->lines.cap = 0U;
    oracle_split(o, bytes, n);
}

void oracle_free(Oracle *o)
{
    if (o == NULL)
        return;
    lines_clear(o);
    free(o->lines.data);
    o->lines.data = NULL;
    o->lines.cap = 0U;
}

u64 oracle_len(const Oracle *o)
{
    size_t i;
    u64 total = 0U;

    if (o == NULL)
        SAG_BUG("oracle_len: NULL oracle");
    for (i = 0U; i < o->lines.len; i++) {
        if ((u64)o->lines.data[i].len > UINT64_MAX - total)
            SAG_BUG("oracle length overflow");
        total += (u64)o->lines.data[i].len;
    }
    return total;
}

void oracle_materialize(const Oracle *o, Bytebuf *out)
{
    size_t i;

    if (o == NULL || out == NULL)
        SAG_BUG("oracle_materialize: invalid argument");
    for (i = 0U; i < o->lines.len; i++)
        bytebuf_append(out, o->lines.data[i].data, o->lines.data[i].len);
}

void oracle_insert(Oracle *o, u64 at, const u8 *bytes, u64 len)
{
    Bytebuf flat;
    size_t pos;
    size_t add;

    if (o == NULL)
        SAG_BUG("oracle_insert: NULL oracle");
    if (at > oracle_len(o))
        SAG_BUG("oracle_insert: offset outside buffer");
    add = checked_size(len);
    if (add != 0U && bytes == NULL)
        SAG_BUG("oracle_insert: NULL bytes");
    if (add == 0U)
        return;
    bytebuf_init(&flat);
    oracle_materialize(o, &flat);
    pos = checked_size(at);
    if (add > SIZE_MAX - flat.len)
        SAG_BUG("oracle insert size overflow");
    bytebuf_reserve(&flat, flat.len + add);
    memmove(flat.data + pos + add, flat.data + pos, flat.len - pos);
    memcpy(flat.data + pos, bytes, add);
    flat.len += add;
    oracle_split(o, flat.data, flat.len);
    bytebuf_free(&flat);
}

void oracle_delete(Oracle *o, u64 lo, u64 hi)
{
    Bytebuf flat;
    size_t begin;
    size_t end;

    if (o == NULL)
        SAG_BUG("oracle_delete: NULL oracle");
    if (lo > hi || hi > oracle_len(o))
        SAG_BUG("oracle_delete: range outside buffer");
    if (lo == hi)
        return;
    bytebuf_init(&flat);
    oracle_materialize(o, &flat);
    begin = checked_size(lo);
    end = checked_size(hi);
    memmove(flat.data + begin, flat.data + end, flat.len - end);
    flat.len -= end - begin;
    oracle_split(o, flat.data, flat.len);
    bytebuf_free(&flat);
}

u64 oracle_line_count(const Oracle *o)
{
    if (o == NULL)
        SAG_BUG("oracle_line_count: NULL oracle");
    return (u64)o->lines.len;
}

u64 oracle_line_start(const Oracle *o, u64 line)
{
    size_t i;
    u64 start = 0U;

    if (o == NULL || line >= (u64)o->lines.len)
        SAG_BUG("oracle_line_start: line outside buffer");
    for (i = 0U; i < (size_t)line; i++)
        start += (u64)o->lines.data[i].len;
    return start;
}

u64 oracle_line_of(const Oracle *o, u64 off)
{
    size_t i;
    u64 start = 0U;

    if (o == NULL || off > oracle_len(o))
        SAG_BUG("oracle_line_of: offset outside buffer");
    for (i = 0U; i + 1U < o->lines.len; i++) {
        start += (u64)o->lines.data[i].len;
        if (off < start)
            return (u64)i;
    }
    return (u64)(o->lines.len - 1U);
}
