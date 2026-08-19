#define _POSIX_C_SOURCE 200112L

#include "mod/ai/http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static const char http_tls_error[] =
    "yew has no TLS: https backends run through curl.\n"
    "set transport: \"curl\" on this backend, or use a http:// endpoint.";

static bool http_error(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL && errsz != 0U) {
        va_start(ap, fmt);
        (void)vsnprintf(err, errsz, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool ascii_prefix_ci(const char *s, const char *prefix)
{
    size_t i;

    if (s == NULL)
        return false;
    for (i = 0U; prefix[i] != '\0'; i++) {
        if (s[i] == '\0' ||
            (u8)tolower((unsigned char)s[i]) !=
                (u8)tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

static bool bytes_safe_url(const char *s, size_t n)
{
    size_t i;

    for (i = 0U; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];

        if (c <= 0x20U || c == 0x7fU)
            return false;
    }
    return true;
}

static bool sockaddr_loopback(const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        const u32 addr = ntohl(in->sin_addr.s_addr);

        return (addr & 0xff000000U) == 0x7f000000U;
    }
    if (sa->sa_family == AF_INET6) {
        static const u8 loopback[16] = {
            0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
            0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U
        };
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;

        return memcmp(&in6->sin6_addr, loopback, sizeof(loopback)) == 0;
    }
    return false;
}

static bool host_loopback(const char *host, u16 port)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *it;
    char service[6];
    bool saw_address = false;
    bool loopback = true;

    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0)
        return false;
    for (it = addresses; it != NULL; it = it->ai_next) {
        saw_address = true;
        if (!sockaddr_loopback(it->ai_addr))
            loopback = false;
    }
    freeaddrinfo(addresses);
    return saw_address && loopback;
}

static bool parse_port(const char *s, size_t n, u16 *out)
{
    u32 value = 0U;
    size_t i;

    if (n == 0U)
        return false;
    for (i = 0U; i < n; i++) {
        const u8 c = (u8)s[i];

        if (c < (u8)'0' || c > (u8)'9')
            return false;
        value = value * 10U + (u32)(c - (u8)'0');
        if (value > 65535U)
            return false;
    }
    if (value == 0U)
        return false;
    *out = (u16)value;
    return true;
}

static char *path_copy(Arena *a, const char *start)
{
    size_t len = strlen(start);
    char *path;

    if (start[0] != '?')
        return arena_strdup(a, start);
    path = arena_alloc(a, len + 2U, 1U);
    path[0] = '/';
    (void)memcpy(path + 1U, start, len + 1U);
    return path;
}

bool yew_http_url_parse(Arena *a, const char *url, HttpUrl *out,
                        char *err, size_t errsz)
{
    const char *authority;
    const char *tail;
    const char *host_start;
    const char *host_end;
    const char *port_start = NULL;
    char *host;
    char *path;
    u16 port = 80U;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (a == NULL || url == NULL || out == NULL)
        return http_error(err, errsz, "bad url");
    if (ascii_prefix_ci(url, "https://"))
        return http_error(err, errsz, "%s", http_tls_error);
    if (ascii_prefix_ci(url, "unix:"))
        return http_error(err, errsz,
                          "unix socket endpoints are deferred to Sprint 49 "
                          "and will not ship in yew 1.0");
    if (!ascii_prefix_ci(url, "http://"))
        return http_error(err, errsz, "bad url");

    authority = url + 7U;
    tail = authority + strcspn(authority, "/?#");
    if (tail == authority ||
        !bytes_safe_url(authority, (size_t)(tail - authority)))
        return http_error(err, errsz, "bad url");
    if (memchr(authority, '@', (size_t)(tail - authority)) != NULL)
        return http_error(err, errsz, "bad url: userinfo is not allowed");
    if (*tail == '#')
        return http_error(err, errsz, "bad url");
    if (strchr(tail, '#') != NULL)
        return http_error(err, errsz, "bad url");

    host_start = authority;
    if (*authority == '[') {
        const char *close = memchr(authority + 1U, ']',
                                   (size_t)(tail - authority - 1U));

        if (close == NULL || close == authority + 1U)
            return http_error(err, errsz, "bad url");
        host_start = authority + 1U;
        host_end = close;
        if ((size_t)(host_end - host_start) >= 256U)
            return http_error(err, errsz, "bad url");
        {
            struct in6_addr parsed;
            char candidate[256];
            const size_t host_len = (size_t)(host_end - host_start);

            (void)memcpy(candidate, host_start, host_len);
            candidate[host_len] = '\0';
            if (inet_pton(AF_INET6, candidate, &parsed) != 1)
                return http_error(err, errsz, "bad url");
        }
        if (close + 1U != tail) {
            if (close[1] != ':')
                return http_error(err, errsz, "bad url");
            port_start = close + 2U;
        }
    } else {
        const char *colon = memchr(authority, ':',
                                   (size_t)(tail - authority));

        host_end = colon == NULL ? tail : colon;
        if (colon != NULL) {
            if (memchr(colon + 1U, ':', (size_t)(tail - colon - 1U)) != NULL)
                return http_error(err, errsz, "bad url");
            port_start = colon + 1U;
        }
    }
    if (host_end == host_start)
        return http_error(err, errsz, "bad url");
    if ((size_t)(host_end - host_start) >= 256U)
        return http_error(err, errsz, "bad url");
    if (port_start != NULL &&
        !parse_port(port_start, (size_t)(tail - port_start), &port))
        return http_error(err, errsz, "bad url");
    if (*tail != '\0' && !bytes_safe_url(tail, strlen(tail)))
        return http_error(err, errsz, "bad url");

    host = arena_strndup(a, host_start, (size_t)(host_end - host_start));
    path = *tail == '\0' ? arena_strdup(a, "/") : path_copy(a, tail);
    out->host = host;
    out->port = port;
    out->path = path;
    out->loopback = host_loopback(host, port);
    return true;
}

static bool http_token(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p == NULL || *p == '\0')
        return false;
    for (; *p != '\0'; p++) {
        if (isalnum(*p))
            continue;
        if (strchr("!#$%&'*+-.^_`|~", (int)*p) == NULL)
            return false;
    }
    return true;
}

static bool http_value(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p == NULL)
        return false;
    for (; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n' || *p == 0x7fU ||
            (*p < 0x20U && *p != '\t'))
            return false;
    }
    return true;
}

static bool request_target(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p == NULL || *p != (unsigned char)'/')
        return false;
    for (; *p != '\0'; p++) {
        if (*p <= 0x20U || *p == 0x7fU)
            return false;
    }
    return true;
}

bool yew_http_req_build(Bytebuf *out, const HttpUrl *u, const HttpReq *r,
                        char *err, size_t errsz)
{
    Bytebuf built;
    const char *path;
    u32 i;
    bool body_bearing;
    bool ipv6;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (out == NULL || u == NULL || r == NULL || u->host == NULL ||
        u->host[0] == '\0' || u->port == 0U || !http_token(r->method))
        return http_error(err, errsz, "bad http request");
    path = r->path == NULL ? u->path : r->path;
    if (!request_target(path) || !bytes_safe_url(u->host, strlen(u->host)))
        return http_error(err, errsz, "bad http request");
    if (r->nhdr != 0U && r->hdrs == NULL)
        return http_error(err, errsz, "bad http request headers");
    if (r->blen != 0U && r->body == NULL)
        return http_error(err, errsz, "bad http request body");
    if (r->blen > (u64)SIZE_MAX)
        return http_error(err, errsz, "http request body is too large");
    for (i = 0U; i < r->nhdr; i++) {
        if (!http_token(r->hdrs[i].name) || !http_value(r->hdrs[i].value))
            return http_error(err, errsz, "bad http header");
    }

    bytebuf_init(&built);
    bytebuf_printf(&built, "%s %s HTTP/1.1\r\n", r->method, path);
    ipv6 = strchr(u->host, ':') != NULL;
    if (u->port == 80U) {
        if (ipv6)
            bytebuf_printf(&built, "Host: [%s]\r\n", u->host);
        else
            bytebuf_printf(&built, "Host: %s\r\n", u->host);
    } else if (ipv6) {
        bytebuf_printf(&built, "Host: [%s]:%u\r\n", u->host,
                       (unsigned)u->port);
    } else {
        bytebuf_printf(&built, "Host: %s:%u\r\n", u->host,
                       (unsigned)u->port);
    }
    bytebuf_append(&built, "User-Agent: yew/1.0.0\r\n", 23U);
    bytebuf_append(&built, "Accept: application/json\r\n", 26U);
    bytebuf_append(&built, "Content-Type: application/json\r\n", 32U);
    body_bearing = r->body != NULL || r->blen != 0U;
    if (body_bearing)
        bytebuf_printf(&built, "Content-Length: %llu\r\n",
                       (unsigned long long)r->blen);
    bytebuf_printf(&built, "Connection: %s\r\n",
                   r->keepalive ? "keep-alive" : "close");
    for (i = 0U; i < r->nhdr; i++)
        bytebuf_printf(&built, "%s: %s\r\n", r->hdrs[i].name,
                       r->hdrs[i].value);
    bytebuf_append(&built, "\r\n", 2U);
    if (r->blen != 0U)
        bytebuf_append(&built, r->body, (size_t)r->blen);
    bytebuf_append(out, built.data, built.len);
    bytebuf_free(&built);
    return true;
}
