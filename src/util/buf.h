#ifndef YEW_UTIL_BUF_H
#define YEW_UTIL_BUF_H

#include <stddef.h>

#include "util/base.h"

struct Bytebuf {
    u8 *data;
    size_t len;
    size_t cap;
};

void bytebuf_init(Bytebuf *buf);
void bytebuf_free(Bytebuf *buf);
void bytebuf_reserve(Bytebuf *buf, size_t need);
void bytebuf_append(Bytebuf *buf, const void *data, size_t len);
void bytebuf_push_u8(Bytebuf *buf, u8 byte);
void bytebuf_printf(Bytebuf *buf, const char *fmt, ...);

#endif
