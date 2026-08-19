#ifndef YEW_MOD_AI_HTTP_H
#define YEW_MOD_AI_HTTP_H

#include <stddef.h>

#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

#define YEW_HTTP_MAX_HDRS 64U
#define YEW_HTTP_MAX_BODY (64U * 1024U * 1024U)

typedef enum {
    YEW_HTTP_SCHEME_HTTP = 0
} HttpScheme;

typedef struct HttpUrl {
    const char *host;
    u16 port;
    const char *path;
    bool loopback;
} HttpUrl;

bool yew_http_url_parse(Arena *a, const char *url, HttpUrl *out,
                        char *err, size_t errsz);

typedef struct HttpHdr {
    const char *name;
    const char *value;
} HttpHdr;

typedef struct HttpReq {
    const char *method;
    const char *path;
    const HttpHdr *hdrs;
    u32 nhdr;
    const u8 *body;
    u64 blen;
    bool keepalive;
} HttpReq;

bool yew_http_req_build(Bytebuf *out, const HttpUrl *u, const HttpReq *r,
                        char *err, size_t errsz);

typedef enum {
    YEW_HX_STATUS = 0,
    YEW_HX_HDR,
    YEW_HX_BODY_LEN,
    YEW_HX_CHUNK_SIZE,
    YEW_HX_CHUNK_DATA,
    YEW_HX_CHUNK_CRLF,
    YEW_HX_TRAILER,
    YEW_HX_BODY_EOF,
    YEW_HX_DONE,
    YEW_HX_DEAD
} HttpRxState;

typedef struct HttpRx {
    u8 state;
    bool pending_cr;
    Bytebuf line;
    u16 nhdr;
    u16 status;
    u8 ver_minor;
    bool have_len;
    bool chunked;
    bool close_delimited;
    bool no_body;
    u64 want;
    u64 got;
    u64 body_total;
    char ctype[64];
    i64 retry_after_ms;
    char err[96];
} HttpRx;

typedef void (*HttpBodyFn)(void *ctx, const u8 *p, u64 n);

void yew_http_rx_init(HttpRx *rx);
void yew_http_rx_feed(HttpRx *rx, const u8 *b, u64 n, bool at_eof,
                      HttpBodyFn on_body, void *ctx);

#endif
