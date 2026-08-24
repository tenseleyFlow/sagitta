#ifndef YEW_UNICODE_UTF8_H
#define YEW_UNICODE_UTF8_H

#include "util/base.h"

/*
 * Invalid input bytes exist only as lossless decoded-stream escapes. Raw text
 * storage remains byte-verbatim. yew deliberately performs no Unicode
 * normalization: normalization during load or save would rewrite user data.
 */
#define YEW_UTF8_MAX 4
#define YEW_U8_MAX_FLUSH 4
#define YEW_CP_ESC_LO 0xDC80u
#define YEW_CP_ESC_HI 0xDCFFu

typedef struct {
    u8 state;
    u8 n;
    u8 pend[YEW_UTF8_MAX];
    u32 cp;
    u32 out[YEW_U8_MAX_FLUSH];
} YewU8Dec;

void yew_utf8_dec_init(YewU8Dec *d);
/* Results are written to d->out; zero means the sequence is incomplete. */
u8 yew_utf8_push(YewU8Dec *d, u8 b);
/* Flushes each byte of an incomplete sequence as its own escape. */
u8 yew_utf8_finish(YewU8Dec *d);

/* Total for len > 0: writes out and consumes at least one byte. */
size_t yew_utf8_decode(const u8 *s, size_t len, u32 *out);
/* Decodes the codepoint ending at pos and returns its byte length. */
size_t yew_utf8_decode_prev(const u8 *s, size_t start, size_t pos,
                            u32 *out);

/* Escapes emit their original byte; invalid synthesized scalars return zero. */
size_t yew_utf8_encode(u32 cp, u8 out[YEW_UTF8_MAX]);
size_t yew_utf8_len(u32 cp);

bool yew_utf8_is_escape(u32 cp);
u8 yew_utf8_escape_byte(u32 cp);
u32 yew_utf8_escape_of(u8 b);

/* Returns the first invalid byte offset, or len when the input is valid. */
size_t yew_utf8_validate(const u8 *s, size_t len);
/* Also reports whether every byte is printable ASCII or a line feed. */
size_t yew_utf8_validate_simple(const u8 *s, size_t len,
                                bool *simple_ascii);
bool yew_utf8_is_boundary(const u8 *s, size_t len, size_t pos);

#endif
