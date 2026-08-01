#include "util/buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

void bytebuf_init(Bytebuf *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void bytebuf_free(Bytebuf *buf)
{
    free(buf->data);
    bytebuf_init(buf);
}

void bytebuf_reserve(Bytebuf *buf, size_t need)
{
    size_t cap;

    if (buf->cap >= need)
        return;
    cap = buf->cap ? buf->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    buf->data = sag_xrealloc(buf->data, cap);
    buf->cap = cap;
}

void bytebuf_append(Bytebuf *buf, const void *data, size_t len)
{
    if (len == 0)
        return;
    if (len > SIZE_MAX - buf->len)
        SAG_BUG("byte buffer size overflow");
    bytebuf_reserve(buf, buf->len + len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void bytebuf_push_u8(Bytebuf *buf, u8 byte)
{
    if (buf->len == SIZE_MAX)
        SAG_BUG("byte buffer size overflow");
    bytebuf_reserve(buf, buf->len + 1);
    buf->data[buf->len++] = byte;
}

void bytebuf_printf(Bytebuf *buf, const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    int written;
    size_t required;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(copy);
        SAG_BUG("bytebuf_printf: formatting failed");
    }
    if (buf->len == SIZE_MAX ||
        (size_t)needed > SIZE_MAX - buf->len - 1) {
        va_end(copy);
        SAG_BUG("byte buffer size overflow");
    }
    required = buf->len + (size_t)needed + 1;
    bytebuf_reserve(buf, required);
    written = vsnprintf((char *)buf->data + buf->len, (size_t)needed + 1,
                        fmt, copy);
    va_end(copy);
    if (written != needed)
        SAG_BUG("bytebuf_printf: inconsistent formatting result");
    buf->len += (size_t)needed;
}
