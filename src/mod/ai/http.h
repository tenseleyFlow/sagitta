#ifndef YEW_MOD_AI_HTTP_H
#define YEW_MOD_AI_HTTP_H

#include <stddef.h>
#include <poll.h>
#include <sys/socket.h>

#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

#define YEW_HTTP_MAX_HDRS 64U
#define YEW_HTTP_MAX_HDRLINE (8U * 1024U)
#define YEW_HTTP_MAX_BODY (64U * 1024U * 1024U)
#define YEW_HTTP_MAX_CHUNK (16U * 1024U * 1024U)
#define YEW_HTTP_POOL_MAX 4U

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
    bool bad_len;
    bool conflicting_len;
    bool chunked;
    bool close_delimited;
    bool no_body;
    bool peer_close;
    u64 want;
    u64 got;
    u64 body_total;
    char ctype[64];
    i64 retry_after_ms;
    char err[96];
} HttpRx;

typedef void (*HttpBodyFn)(void *ctx, const u8 *p, u64 n);

void yew_http_rx_init(HttpRx *rx);
/* Returns the exact prefix of b consumed.  A terminal response may consume
 * less than n, leaving the caller to retain bytes for the next protocol
 * message or reject them at its own boundary. */
u64 yew_http_rx_feed(HttpRx *rx, const u8 *b, u64 n, bool at_eof,
                     HttpBodyFn on_body, void *ctx);

typedef struct Ed Ed;
typedef struct AiErr AiErr;

typedef enum {
    YEW_HC_IDLE = 0,
    YEW_HC_CONNECTING,
    YEW_HC_SENDING,
    YEW_HC_RECVING,
    YEW_HC_DONE,
    YEW_HC_DEAD
} HttpConnState;

typedef struct HttpConn HttpConn;
typedef void (*HttpDoneFn)(void *ctx, const HttpConn *conn);

struct HttpConn {
    int fd;
    u8 state;
    char host[256];
    u16 port;
    Bytebuf out;
    u64 sent;
    HttpRx *rx;
    i64 t_start;
    i64 t_connected;
    i64 t_first_byte;
    i64 t_deadline;
    bool reusable;
    void *owner;

    /* Callbacks are invoked synchronously from yew_http_pump().  Body
     * spans borrow the read buffer and are valid only for the callback.
     * The done callback runs once, after state becomes DONE or DEAD. */
    HttpBodyFn on_body;
    HttpDoneFn on_done;
    void *callback_ctx;
    AiErr *error;

    /* Private transport bookkeeping; public only so HttpConn stays a
     * caller-owned observable handle without another allocation layer. */
    struct HttpConn *next;
    struct sockaddr_storage address;
    socklen_t address_len;
    i64 t_last_byte;
    i64 t_request_sent;
    i64 connect_timeout_ms;
    i64 first_byte_timeout_ms;
    i64 idle_timeout_ms;
    i64 total_timeout_ms;
    bool request_keepalive;
    bool from_pool;
    bool retried;
    bool response_seen;
    bool notified;
    bool stream_complete;
    bool contains_secret;
};

typedef struct HttpState HttpState;

HttpState *yew_http_state_new(void);
void yew_http_state_free(HttpState *state);

/* Process-local diagnostic used by the hermetic test lane to prove that
 * an opt-out profile never reaches socket(2). */
u64 yew_http_socket_call_count(void);
/* Process-local diagnostic proving numeric literals bypass name service. */
u64 yew_http_resolver_call_count(void);

/* Add one caller-supplied numeric address under a logical endpoint name.
 * Repeated calls preserve insertion order and allow resolver-free hosts. */
bool yew_http_register_address(Ed *ed, HttpUrl *u, const char *numeric,
                               AiErr *e);

/* Named hosts must be registered before use.  Registration performs the
 * sole blocking lookup and caches every eligible sockaddr in resolver
 * order.  Numeric hosts are converted directly without name service. */
bool yew_http_register_endpoint(Ed *ed, HttpUrl *u, AiErr *e);
HttpConn *yew_http_begin(Ed *ed, const HttpUrl *u, const HttpReq *r,
                         AiErr *e);
void yew_http_conn_callbacks(HttpConn *c, HttpBodyFn body,
                             HttpDoneFn done, void *ctx);
/* Mark a request whose serialized headers contain credential bytes.  The
 * full allocation is wiped before release, including unused capacity. */
void yew_http_conn_mark_secret(HttpConn *c);
/* A close-delimited streaming response is successful only after its
 * adapter has observed the protocol terminator. */
void yew_http_conn_mark_stream_done(HttpConn *c);
void yew_http_conn_release(Ed *ed, HttpConn *c);
void yew_http_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n);
void yew_http_pump(Ed *ed, const struct pollfd *pfd, u32 n);
i64 yew_http_deadline(const Ed *ed, i64 now_ms);
void yew_http_abort(Ed *ed, HttpConn *c);

#endif
