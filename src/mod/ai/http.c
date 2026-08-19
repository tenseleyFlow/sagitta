#define _POSIX_C_SOURCE 200112L

#include "mod/ai/http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "util/log.h"

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

static void rx_die(HttpRx *rx, const char *msg)
{
    rx->state = YEW_HX_DEAD;
    rx->pending_cr = false;
    (void)snprintf(rx->err, sizeof(rx->err), "%s", msg);
}

static bool ascii_eq_ci_n(const u8 *a, u32 an, const char *b)
{
    u32 i;

    if (strlen(b) != (size_t)an)
        return false;
    for (i = 0U; i < an; i++) {
        if ((u8)tolower(a[i]) !=
            (u8)tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool http_token_n(const u8 *s, u32 n)
{
    u32 i;

    if (n == 0U)
        return false;
    for (i = 0U; i < n; i++) {
        if (isalnum(s[i]))
            continue;
        if (strchr("!#$%&'*+-.^_`|~", (int)s[i]) == NULL)
            return false;
    }
    return true;
}

static bool parse_u64_dec(const u8 *s, u32 n, u64 *out)
{
    u64 value = 0U;
    u32 i;

    if (n == 0U)
        return false;
    for (i = 0U; i < n; i++) {
        u64 digit;

        if (s[i] < (u8)'0' || s[i] > (u8)'9')
            return false;
        digit = (u64)(s[i] - (u8)'0');
        if (value > (UINT64_MAX - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

static int hex_value(u8 c)
{
    if (c >= (u8)'0' && c <= (u8)'9')
        return (int)(c - (u8)'0');
    if (c >= (u8)'a' && c <= (u8)'f')
        return 10 + (int)(c - (u8)'a');
    if (c >= (u8)'A' && c <= (u8)'F')
        return 10 + (int)(c - (u8)'A');
    return -1;
}

static bool chunk_size_parse(const u8 *line, u32 n, u64 *out)
{
    u64 value = 0U;
    u32 i = 0U;

    for (; i < n && line[i] != (u8)';'; i++) {
        const int digit = hex_value(line[i]);

        if (digit < 0 || value > (UINT64_MAX - (u64)digit) / 16U)
            return false;
        value = value * 16U + (u64)digit;
    }
    if (i == 0U || value > YEW_HTTP_MAX_CHUNK)
        return false;
    *out = value;
    return true;
}

static bool te_has_chunked(const u8 *s, u32 n)
{
    u32 pos = 0U;

    while (pos < n) {
        u32 start;
        u32 end;

        while (pos < n && (s[pos] == (u8)' ' || s[pos] == (u8)'\t' ||
                           s[pos] == (u8)','))
            pos++;
        start = pos;
        while (pos < n && s[pos] != (u8)',' && s[pos] != (u8)';')
            pos++;
        end = pos;
        while (end > start &&
               (s[end - 1U] == (u8)' ' || s[end - 1U] == (u8)'\t'))
            end--;
        if (ascii_eq_ci_n(s + start, end - start, "chunked"))
            return true;
        while (pos < n && s[pos] != (u8)',')
            pos++;
    }
    return false;
}

static int month_number(const u8 *s)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month;

    for (month = 0; month < 12; month++) {
        if (memcmp(s, names + (size_t)month * 3U, 3U) == 0)
            return month + 1;
    }
    return 0;
}

static bool leap_year(i64 year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static i64 days_from_civil(i64 year, unsigned month, unsigned day)
{
    const i64 adjusted = year - (month <= 2U ? 1 : 0);
    const i64 era = (adjusted >= 0 ? adjusted : adjusted - 399) / 400;
    const unsigned yoe = (unsigned)(adjusted - era * 400);
    const unsigned shifted = month > 2U ? month - 3U : month + 9U;
    const unsigned doy = (153U * shifted + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

    return era * 146097 + (i64)doe - 719468;
}

static bool http_date_seconds(const u8 *s, u32 n, i64 *out)
{
    static const char weekdays[] = "MonTueWedThuFriSatSun";
    static const u8 month_days[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    u64 day;
    u64 year;
    u64 hour;
    u64 minute;
    u64 second;
    int month;
    u8 max_day;
    bool weekday_ok = false;
    u32 i;

    if (n != 29U || s[3] != (u8)',' || s[4] != (u8)' ' ||
        s[7] != (u8)' ' || s[11] != (u8)' ' || s[16] != (u8)' ' ||
        s[19] != (u8)':' || s[22] != (u8)':' || s[25] != (u8)' ' ||
        memcmp(s + 26U, "GMT", 3U) != 0)
        return false;
    for (i = 0U; i < 7U; i++) {
        if (memcmp(s, weekdays + (size_t)i * 3U, 3U) == 0) {
            weekday_ok = true;
            break;
        }
    }
    if (!weekday_ok)
        return false;
    if (!parse_u64_dec(s + 5U, 2U, &day) ||
        !parse_u64_dec(s + 12U, 4U, &year) ||
        !parse_u64_dec(s + 17U, 2U, &hour) ||
        !parse_u64_dec(s + 20U, 2U, &minute) ||
        !parse_u64_dec(s + 23U, 2U, &second))
        return false;
    month = month_number(s + 8U);
    if (month == 0 || year < 1970U || year > 9999U || day == 0U ||
        hour > 23U || minute > 59U || second > 59U)
        return false;
    max_day = month_days[month - 1];
    if (month == 2 && leap_year((i64)year))
        max_day = 29U;
    if (day > max_day)
        return false;
    *out = days_from_civil((i64)year, (unsigned)month, (unsigned)day) *
               86400 +
           (i64)hour * 3600 + (i64)minute * 60 + (i64)second;
    return true;
}

static i64 retry_after_parse(const u8 *s, u32 n)
{
    u64 seconds;
    i64 date_seconds;
    i64 now;

    if (parse_u64_dec(s, n, &seconds)) {
        if (seconds > (u64)INT64_MAX / 1000U)
            return -1;
        return (i64)(seconds * 1000U);
    }
    if (!http_date_seconds(s, n, &date_seconds))
        return -1;
    now = (i64)time(NULL);
    if (date_seconds <= now)
        return 0;
    if (date_seconds - now > INT64_MAX / 1000)
        return -1;
    return (date_seconds - now) * 1000;
}

static void rx_reset_message(HttpRx *rx)
{
    rx->nhdr = 0U;
    rx->have_len = false;
    rx->bad_len = false;
    rx->conflicting_len = false;
    rx->chunked = false;
    rx->close_delimited = false;
    rx->no_body = false;
    rx->want = 0U;
    rx->got = 0U;
    rx->body_total = 0U;
    rx->ctype[0] = '\0';
    rx->retry_after_ms = -1;
}

static bool rx_status_line(HttpRx *rx, const u8 *line, u32 n)
{
    if (n < 12U || memcmp(line, "HTTP/1.", 7U) != 0 ||
        (line[7] != (u8)'0' && line[7] != (u8)'1') ||
        line[8] != (u8)' ' || line[9] < (u8)'0' ||
        line[9] > (u8)'9' || line[10] < (u8)'0' ||
        line[10] > (u8)'9' || line[11] < (u8)'0' ||
        line[11] > (u8)'9' || (n > 12U && line[12] != (u8)' ')) {
        rx_die(rx, "bad status line");
        return false;
    }
    rx_reset_message(rx);
    rx->ver_minor = (u8)(line[7] - (u8)'0');
    rx->status = (u16)((line[9] - (u8)'0') * 100U +
                       (line[10] - (u8)'0') * 10U +
                       (line[11] - (u8)'0'));
    rx->state = YEW_HX_HDR;
    return true;
}

static void ctype_store(HttpRx *rx, const u8 *s, u32 n)
{
    u32 len = 0U;

    while (len < n && s[len] != (u8)';' && len + 1U < sizeof(rx->ctype)) {
        rx->ctype[len] = (char)tolower(s[len]);
        len++;
    }
    while (len > 0U &&
           (rx->ctype[len - 1U] == ' ' || rx->ctype[len - 1U] == '\t'))
        len--;
    rx->ctype[len] = '\0';
}

static bool rx_header_line(HttpRx *rx, const u8 *line, u32 n, bool apply)
{
    const u8 *colon;
    u32 name_len;
    u32 value_start;
    u32 value_end = n;

    if (line[0] == (u8)' ' || line[0] == (u8)'\t') {
        rx_die(rx, "obsolete header folding");
        return false;
    }
    if (rx->nhdr == YEW_HTTP_MAX_HDRS) {
        rx_die(rx, "too many headers");
        return false;
    }
    rx->nhdr++;
    colon = memchr(line, ':', n);
    if (colon == NULL) {
        rx_die(rx, "bad header");
        return false;
    }
    name_len = (u32)(colon - line);
    if (!http_token_n(line, name_len)) {
        rx_die(rx, "bad header");
        return false;
    }
    value_start = name_len + 1U;
    while (value_start < n &&
           (line[value_start] == (u8)' ' || line[value_start] == (u8)'\t'))
        value_start++;
    while (value_end > value_start &&
           (line[value_end - 1U] == (u8)' ' ||
            line[value_end - 1U] == (u8)'\t'))
        value_end--;
    {
        u32 i;

        for (i = value_start; i < value_end; i++) {
            if ((line[i] < 0x20U && line[i] != (u8)'\t') ||
                line[i] == 0x7fU) {
                rx_die(rx, "bad header");
                return false;
            }
        }
    }
    if (!apply)
        return true;
    if (ascii_eq_ci_n(line, name_len, "content-length")) {
        u64 length;

        if (!parse_u64_dec(line + value_start, value_end - value_start,
                           &length)) {
            rx->bad_len = true;
        } else if (rx->have_len && rx->want != length) {
            rx->conflicting_len = true;
        } else {
            rx->have_len = true;
            rx->want = length;
        }
    } else if (ascii_eq_ci_n(line, name_len, "transfer-encoding")) {
        if (te_has_chunked(line + value_start, value_end - value_start))
            rx->chunked = true;
    } else if (ascii_eq_ci_n(line, name_len, "content-type")) {
        ctype_store(rx, line + value_start, value_end - value_start);
    } else if (ascii_eq_ci_n(line, name_len, "retry-after")) {
        rx->retry_after_ms = retry_after_parse(
            line + value_start, value_end - value_start);
    }
    return true;
}

static void rx_headers_done(HttpRx *rx)
{
    if (rx->status >= 100U && rx->status < 200U) {
        rx->no_body = true;
        rx->state = YEW_HX_STATUS;
    } else if (rx->status == 204U || rx->status == 304U) {
        rx->no_body = true;
        rx->state = YEW_HX_DONE;
    } else if (rx->chunked) {
        if (rx->have_len || rx->bad_len || rx->conflicting_len)
            yew_log(YEW_LOG_WARN,
                    "HTTP response has chunked encoding and Content-Length; "
                    "ignoring Content-Length");
        rx->want = 0U;
        rx->got = 0U;
        rx->state = YEW_HX_CHUNK_SIZE;
    } else if (rx->bad_len) {
        rx_die(rx, "bad Content-Length");
    } else if (rx->conflicting_len) {
        rx_die(rx, "conflicting Content-Length");
    } else if (rx->have_len) {
        if (rx->want > YEW_HTTP_MAX_BODY) {
            rx_die(rx, "response too large");
            return;
        }
        rx->got = 0U;
        rx->state = rx->want == 0U ? YEW_HX_DONE : YEW_HX_BODY_LEN;
    } else {
        rx->close_delimited = true;
        rx->state = YEW_HX_BODY_EOF;
    }
}

static void rx_line_done(HttpRx *rx)
{
    const u8 *line = rx->line.data;
    const u32 n = (u32)rx->line.len;

    if (rx->state == YEW_HX_STATUS) {
        (void)rx_status_line(rx, line, n);
    } else if (rx->state == YEW_HX_HDR) {
        if (n == 0U)
            rx_headers_done(rx);
        else
            (void)rx_header_line(rx, line, n, true);
    } else if (rx->state == YEW_HX_CHUNK_SIZE) {
        u64 size;

        if (!chunk_size_parse(line, n, &size)) {
            rx_die(rx, "bad chunk size");
        } else if (size > YEW_HTTP_MAX_BODY - rx->body_total) {
            rx_die(rx, "response too large");
        } else if (size == 0U) {
            rx->state = YEW_HX_TRAILER;
        } else {
            rx->want = size;
            rx->got = 0U;
            rx->state = YEW_HX_CHUNK_DATA;
        }
    } else if (rx->state == YEW_HX_TRAILER) {
        if (n == 0U)
            rx->state = YEW_HX_DONE;
        else
            (void)rx_header_line(rx, line, n, false);
    }
    rx->line.len = 0U;
}

static bool rx_line_byte(HttpRx *rx, u8 byte)
{
    if (rx->pending_cr) {
        if (byte != (u8)'\n') {
            rx_die(rx, "bad line ending");
            return false;
        }
        rx->pending_cr = false;
        rx_line_done(rx);
        return rx->state != YEW_HX_DEAD;
    }
    if (byte == (u8)'\n') {
        rx_die(rx, "bad line ending");
        return false;
    }
    if (byte == (u8)'\r') {
        rx->pending_cr = true;
        return true;
    }
    if (rx->line.len == YEW_HTTP_MAX_HDRLINE) {
        if (rx->state == YEW_HX_STATUS)
            rx_die(rx, "bad status line");
        else if (rx->state == YEW_HX_CHUNK_SIZE)
            rx_die(rx, "bad chunk size");
        else
            rx_die(rx, "header line too long");
        return false;
    }
    bytebuf_push_u8(&rx->line, byte);
    return true;
}

static void rx_body(HttpRx *rx, const u8 *bytes, u64 n,
                    HttpBodyFn on_body, void *ctx)
{
    if (n == 0U)
        return;
    if (n > YEW_HTTP_MAX_BODY - rx->body_total) {
        rx_die(rx, "response too large");
        return;
    }
    if (on_body != NULL)
        on_body(ctx, bytes, n);
    rx->body_total += n;
}

void yew_http_rx_init(HttpRx *rx)
{
    (void)memset(rx, 0, sizeof(*rx));
    bytebuf_init(&rx->line);
    rx->state = YEW_HX_STATUS;
    rx->retry_after_ms = -1;
}

u64 yew_http_rx_feed(HttpRx *rx, const u8 *bytes, u64 n, bool at_eof,
                     HttpBodyFn on_body, void *ctx)
{
    u64 pos = 0U;

    if (rx == NULL)
        return 0U;
    if (rx->state == YEW_HX_DONE || rx->state == YEW_HX_DEAD)
        return 0U;
    if (n != 0U && bytes == NULL) {
        rx_die(rx, "invalid response bytes");
        return 0U;
    }
    while (pos < n && rx->state != YEW_HX_DONE &&
           rx->state != YEW_HX_DEAD) {
        if (rx->state == YEW_HX_STATUS || rx->state == YEW_HX_HDR ||
            rx->state == YEW_HX_CHUNK_SIZE ||
            rx->state == YEW_HX_TRAILER) {
            (void)rx_line_byte(rx, bytes[pos]);
            pos++;
        } else if (rx->state == YEW_HX_BODY_LEN ||
                   rx->state == YEW_HX_CHUNK_DATA) {
            const u64 remain = rx->want - rx->got;
            const u64 available = n - pos;
            const u64 take = remain < available ? remain : available;

            rx_body(rx, bytes + pos, take, on_body, ctx);
            if (rx->state == YEW_HX_DEAD)
                break;
            rx->got += take;
            pos += take;
            if (rx->got == rx->want) {
                rx->got = 0U;
                if (rx->state == YEW_HX_BODY_LEN)
                    rx->state = YEW_HX_DONE;
                else
                    rx->state = YEW_HX_CHUNK_CRLF;
            }
        } else if (rx->state == YEW_HX_CHUNK_CRLF) {
            if ((rx->got == 0U && bytes[pos] != (u8)'\r') ||
                (rx->got == 1U && bytes[pos] != (u8)'\n')) {
                rx_die(rx, "bad chunk terminator");
                break;
            }
            rx->got++;
            pos++;
            if (rx->got == 2U) {
                rx->got = 0U;
                rx->state = YEW_HX_CHUNK_SIZE;
            }
        } else if (rx->state == YEW_HX_BODY_EOF) {
            rx_body(rx, bytes + pos, n - pos, on_body, ctx);
            pos = n;
        }
    }
    if (!at_eof || rx->state == YEW_HX_DONE || rx->state == YEW_HX_DEAD)
        return pos;
    if (rx->state == YEW_HX_BODY_EOF && !rx->pending_cr) {
        rx->state = YEW_HX_DONE;
        return pos;
    }
    rx_die(rx, "response ended early");
    return pos;
}
