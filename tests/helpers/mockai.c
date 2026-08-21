#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    MOCK_MAX_LINE = 1024 * 1024,
    MOCK_MAX_REQUEST = 1024 * 1024,
    MOCK_MAX_HEADERS = 64,
    MOCK_MAX_OPS = 65536
};

typedef enum MockFraming {
    MOCK_FRAME_CLOSE,
    MOCK_FRAME_LENGTH,
    MOCK_FRAME_CHUNKED,
    MOCK_FRAME_SSE,
    MOCK_FRAME_NDJSON
} MockFraming;

typedef enum MockOpKind {
    MOCK_OP_DELAY,
    MOCK_OP_EMIT,
    MOCK_OP_CHUNK,
    MOCK_OP_SPLIT,
    MOCK_OP_CLOSE
} MockOpKind;

typedef struct MockOp {
    MockOpKind kind;
    unsigned char *bytes;
    size_t len;
    unsigned long number;
} MockOp;

typedef struct MockHeader {
    char *line;
} MockHeader;

typedef struct MockScript {
    unsigned status;
    int exit_code;
    MockFraming framing;
    MockHeader *headers;
    size_t nheaders;
    MockOp *ops;
    size_t nops;
} MockScript;

#ifndef YEW_MOCKAI_NO_MAIN
static volatile sig_atomic_t mock_stop;

static void mock_stop_handler(int signo)
{
    (void)signo;
    mock_stop = 1;
}
#define MOCK_STOP_REQUESTED() (mock_stop != 0)
#else
#define MOCK_STOP_REQUESTED() false
#endif

static void mock_error(const char *message)
{
    (void)fprintf(stderr, "mockai: %s\n", message);
}

static void mock_system_error(const char *what)
{
    (void)fprintf(stderr, "mockai: %s: %s\n", what, strerror(errno));
}

static bool mock_write_all(int fd, const void *data, size_t len, bool split)
{
    const unsigned char *bytes = data;
    size_t sent = 0U;

    while (sent < len) {
        size_t want = split ? 1U : len - sent;
        ssize_t n = write(fd, bytes + sent, want);

        if (n < 0 && errno == EINTR && !MOCK_STOP_REQUESTED())
            continue;
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool mock_pause_ms(unsigned long ms)
{
    struct timespec delay;

    if (ms > 86400000UL) {
        mock_error("delay exceeds 24 hours");
        return false;
    }
    delay.tv_sec = (time_t)(ms / 1000UL);
    delay.tv_nsec = (long)(ms % 1000UL) * 1000000L;
    while (nanosleep(&delay, &delay) != 0) {
        if (errno != EINTR || MOCK_STOP_REQUESTED()) {
            if (MOCK_STOP_REQUESTED())
                return false;
            mock_system_error("nanosleep");
            return false;
        }
    }
    return true;
}

static bool mock_number(const char *text, unsigned long limit,
                        unsigned long *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0' || *text == '-')
        return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > limit)
        return false;
    *out = value;
    return true;
}

static int mock_hex(unsigned char c)
{
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9')
        return (int)(c - (unsigned char)'0');
    if (c >= (unsigned char)'a' && c <= (unsigned char)'f')
        return (int)(c - (unsigned char)'a') + 10;
    if (c >= (unsigned char)'A' && c <= (unsigned char)'F')
        return (int)(c - (unsigned char)'A') + 10;
    return -1;
}

static bool mock_literal(const char *text, unsigned char **out, size_t *outlen)
{
    size_t input_len = strlen(text);
    unsigned char *bytes = malloc(input_len + 1U);
    size_t at;
    size_t len = 0U;

    if (bytes == NULL) {
        mock_system_error("malloc");
        return false;
    }
    for (at = 0U; at < input_len; at++) {
        unsigned char c = (unsigned char)text[at];

        if (c != (unsigned char)'\\') {
            bytes[len++] = c;
            continue;
        }
        at++;
        if (at == input_len) {
            mock_error("literal ends with an escape");
            free(bytes);
            return false;
        }
        c = (unsigned char)text[at];
        if (c == (unsigned char)'n')
            bytes[len++] = (unsigned char)'\n';
        else if (c == (unsigned char)'r')
            bytes[len++] = (unsigned char)'\r';
        else if (c == (unsigned char)'t')
            bytes[len++] = (unsigned char)'\t';
        else if (c == (unsigned char)'\\')
            bytes[len++] = (unsigned char)'\\';
        else if (c == (unsigned char)'x') {
            int hi;
            int lo;

            if (at + 2U >= input_len ||
                (hi = mock_hex((unsigned char)text[at + 1U])) < 0 ||
                (lo = mock_hex((unsigned char)text[at + 2U])) < 0) {
                mock_error("\\x escape requires two hexadecimal digits");
                free(bytes);
                return false;
            }
            bytes[len++] = (unsigned char)((hi << 4) | lo);
            at += 2U;
        } else {
            mock_error("unknown literal escape");
            free(bytes);
            return false;
        }
    }
    *out = bytes;
    *outlen = len;
    return true;
}

static bool mock_push_header(MockScript *script, const char *line)
{
    MockHeader *headers;
    char *copy;

    if (script->nheaders >= MOCK_MAX_HEADERS) {
        mock_error("too many response headers");
        return false;
    }
    if (*line == '\0' || strchr(line, '\r') != NULL ||
        strchr(line, '\n') != NULL || strchr(line, ':') == NULL) {
        mock_error("invalid header directive");
        return false;
    }
    copy = strdup(line);
    if (copy == NULL) {
        mock_system_error("strdup");
        return false;
    }
    headers = realloc(script->headers,
                      (script->nheaders + 1U) * sizeof(*headers));
    if (headers == NULL) {
        mock_system_error("realloc");
        free(copy);
        return false;
    }
    script->headers = headers;
    script->headers[script->nheaders].line = copy;
    script->nheaders++;
    return true;
}

static bool mock_push_op(MockScript *script, MockOpKind kind,
                         unsigned char *bytes, size_t len,
                         unsigned long number)
{
    MockOp *ops;

    if (script->nops >= MOCK_MAX_OPS) {
        mock_error("too many script operations");
        free(bytes);
        return false;
    }
    ops = realloc(script->ops, (script->nops + 1U) * sizeof(*ops));
    if (ops == NULL) {
        mock_system_error("realloc");
        free(bytes);
        return false;
    }
    script->ops = ops;
    script->ops[script->nops].kind = kind;
    script->ops[script->nops].bytes = bytes;
    script->ops[script->nops].len = len;
    script->ops[script->nops].number = number;
    script->nops++;
    return true;
}

static bool mock_set_framing(MockScript *script, const char *value)
{
    if (strcmp(value, "sse") == 0)
        script->framing = MOCK_FRAME_SSE;
    else if (strcmp(value, "ndjson") == 0)
        script->framing = MOCK_FRAME_NDJSON;
    else if (strcmp(value, "length") == 0)
        script->framing = MOCK_FRAME_LENGTH;
    else if (strcmp(value, "close") == 0)
        script->framing = MOCK_FRAME_CLOSE;
    else if (strcmp(value, "chunked") == 0)
        script->framing = MOCK_FRAME_CHUNKED;
    else {
        mock_error("unknown framing directive");
        return false;
    }
    return true;
}

static void mock_script_free(MockScript *script)
{
    size_t i;

    for (i = 0U; i < script->nheaders; i++)
        free(script->headers[i].line);
    for (i = 0U; i < script->nops; i++)
        free(script->ops[i].bytes);
    free(script->headers);
    free(script->ops);
    (void)memset(script, 0, sizeof(*script));
}

static bool mock_script_load(const char *path, MockScript *script)
{
    FILE *file;
    char *line = NULL;
    size_t cap = 0U;
    ssize_t got;
    unsigned long lineno = 0UL;
    bool ok = true;

    (void)memset(script, 0, sizeof(*script));
    script->status = 200U;
    script->framing = MOCK_FRAME_CLOSE;
    file = fopen(path, "r");
    if (file == NULL) {
        mock_system_error(path);
        return false;
    }
    while (ok && (got = getline(&line, &cap, file)) >= 0) {
        char *arg;
        char *space;
        unsigned long number;

        lineno++;
        if ((size_t)got > MOCK_MAX_LINE) {
            mock_error("script line exceeds 1 MiB");
            ok = false;
            break;
        }
        while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r'))
            line[--got] = '\0';
        if (line[0] == '\0' || line[0] == '#')
            continue;
        space = strchr(line, ' ');
        if (space == NULL) {
            arg = line + strlen(line);
        } else {
            *space = '\0';
            arg = space + 1;
        }
        if (strcmp(line, "status") == 0) {
            ok = mock_number(arg, 999UL, &number) && number >= 100UL;
            if (ok)
                script->status = (unsigned)number;
        } else if (strcmp(line, "header") == 0) {
            ok = mock_push_header(script, arg);
        } else if (strcmp(line, "framing") == 0) {
            ok = mock_set_framing(script, arg);
        } else if (strcmp(line, "delay") == 0) {
            ok = mock_number(arg, 86400000UL, &number) &&
                 mock_push_op(script, MOCK_OP_DELAY, NULL, 0U, number);
        } else if (strcmp(line, "emit") == 0 || strcmp(line, "chunk") == 0) {
            unsigned char *bytes = NULL;
            size_t len = 0U;

            ok = mock_literal(arg, &bytes, &len) &&
                 mock_push_op(script, strcmp(line, "emit") == 0 ?
                              MOCK_OP_EMIT : MOCK_OP_CHUNK,
                              bytes, len, 0UL);
        } else if (strcmp(line, "split") == 0) {
            ok = mock_number(arg, 1UL, &number) &&
                 mock_push_op(script, MOCK_OP_SPLIT, NULL, 0U, number);
        } else if (strcmp(line, "close") == 0) {
            ok = *arg == '\0' &&
                 mock_push_op(script, MOCK_OP_CLOSE, NULL, 0U, 0UL);
        } else if (strcmp(line, "exit") == 0) {
            ok = mock_number(arg, 255UL, &number);
            if (ok)
                script->exit_code = (int)number;
        } else {
            ok = false;
        }
        if (!ok)
            (void)fprintf(stderr, "mockai: invalid directive at %s:%lu\n",
                          path, lineno);
    }
    if (ferror(file)) {
        mock_system_error("read script");
        ok = false;
    }
    free(line);
    if (fclose(file) != 0) {
        mock_system_error("close script");
        ok = false;
    }
    if (!ok)
        mock_script_free(script);
    return ok;
}

static const char *mock_reason(unsigned status)
{
    switch (status) {
    case 200U: return "OK";
    case 201U: return "Created";
    case 204U: return "No Content";
    case 400U: return "Bad Request";
    case 401U: return "Unauthorized";
    case 403U: return "Forbidden";
    case 404U: return "Not Found";
    case 408U: return "Request Timeout";
    case 413U: return "Content Too Large";
    case 429U: return "Too Many Requests";
    case 500U: return "Internal Server Error";
    case 503U: return "Service Unavailable";
    default: return "Scripted";
    }
}

static bool mock_header_named(const MockScript *script, const char *name)
{
    size_t want = strlen(name);
    size_t i;

    for (i = 0U; i < script->nheaders; i++) {
        const char *line = script->headers[i].line;
        size_t line_len = strlen(line);
        size_t at;

        if (line_len <= want)
            continue;
        for (at = 0U; at < want; at++) {
            unsigned char a = (unsigned char)line[at];
            unsigned char b = (unsigned char)name[at];

            if (a >= (unsigned char)'A' && a <= (unsigned char)'Z')
                a = (unsigned char)(a - (unsigned char)'A' + 'a');
            if (b >= (unsigned char)'A' && b <= (unsigned char)'Z')
                b = (unsigned char)(b - (unsigned char)'A' + 'a');
            if (a != b)
                break;
        }
        if (at == want && line[at] == ':')
            return true;
    }
    return false;
}

static bool mock_size_add(size_t *total, size_t add)
{
    if (add > (size_t)-1 - *total) {
        mock_error("scripted response is too large");
        return false;
    }
    *total += add;
    return true;
}

static bool mock_http_body_size(const MockScript *script, size_t *out)
{
    size_t total = 0U;
    size_t i;

    for (i = 0U; i < script->nops; i++) {
        const MockOp *op = &script->ops[i];

        if (op->kind == MOCK_OP_EMIT) {
            if (!mock_size_add(&total, op->len))
                return false;
        } else if (op->kind == MOCK_OP_CHUNK) {
            char size_line[32];
            int n = snprintf(size_line, sizeof(size_line), "%zx\r\n", op->len);

            if (n <= 0 || (size_t)n >= sizeof(size_line) ||
                !mock_size_add(&total, (size_t)n) ||
                !mock_size_add(&total, op->len) ||
                !mock_size_add(&total, 2U))
                return false;
        }
    }
    *out = total;
    return true;
}

static bool mock_http_headers(int fd, const MockScript *script)
{
    char line[128];
    int n;
    size_t i;

    n = snprintf(line, sizeof(line), "HTTP/1.1 %u %s\r\n",
                 script->status, mock_reason(script->status));
    if (n < 0 || (size_t)n >= sizeof(line) ||
        !mock_write_all(fd, line, (size_t)n, false))
        return false;
    for (i = 0U; i < script->nheaders; i++) {
        const char *header = script->headers[i].line;

        if (!mock_write_all(fd, header, strlen(header), false) ||
            !mock_write_all(fd, "\r\n", 2U, false))
            return false;
    }
    if ((script->framing == MOCK_FRAME_SSE ||
         script->framing == MOCK_FRAME_NDJSON) &&
        !mock_header_named(script, "content-type")) {
        const char *ctype = script->framing == MOCK_FRAME_SSE ?
                            "Content-Type: text/event-stream\r\n" :
                            "Content-Type: application/x-ndjson\r\n";
        if (!mock_write_all(fd, ctype, strlen(ctype), false))
            return false;
    }
    if (script->framing == MOCK_FRAME_LENGTH &&
        !mock_header_named(script, "content-length")) {
        size_t body_size;

        if (!mock_http_body_size(script, &body_size))
            return false;
        n = snprintf(line, sizeof(line), "Content-Length: %zu\r\n",
                     body_size);
        if (n < 0 || (size_t)n >= sizeof(line) ||
            !mock_write_all(fd, line, (size_t)n, false))
            return false;
    } else if (script->framing == MOCK_FRAME_CHUNKED &&
               !mock_header_named(script, "transfer-encoding")) {
        if (!mock_write_all(fd, "Transfer-Encoding: chunked\r\n", 28U,
                            false))
            return false;
    }
    if (!mock_header_named(script, "connection") &&
        !mock_write_all(fd, "Connection: close\r\n", 19U, false))
        return false;
    return mock_write_all(fd, "\r\n", 2U, false);
}

static bool mock_chunk_write(int fd, const unsigned char *bytes, size_t len,
                             bool split)
{
    char line[32];
    int n = snprintf(line, sizeof(line), "%zx\r\n", len);

    return n > 0 && (size_t)n < sizeof(line) &&
           mock_write_all(fd, line, (size_t)n, split) &&
           mock_write_all(fd, bytes, len, split) &&
           mock_write_all(fd, "\r\n", 2U, split);
}

static bool mock_script_replay(const MockScript *script, int fd,
                               bool http_transport)
{
    bool split = false;
    bool closed = false;
    size_t i;

    if (http_transport && !mock_http_headers(fd, script))
        return false;
    for (i = 0U; i < script->nops; i++) {
        const MockOp *op = &script->ops[i];
        bool ok = true;

        if (op->kind == MOCK_OP_DELAY)
            ok = mock_pause_ms(op->number);
        else if (op->kind == MOCK_OP_SPLIT)
            split = op->number != 0UL;
        else if (op->kind == MOCK_OP_EMIT)
            ok = mock_write_all(fd, op->bytes, op->len, split);
        else if (op->kind == MOCK_OP_CHUNK) {
            ok = http_transport ? mock_chunk_write(fd, op->bytes, op->len,
                                                    split) :
                                  mock_write_all(fd, op->bytes, op->len,
                                                 split);
        } else if (op->kind == MOCK_OP_CLOSE) {
            closed = true;
            break;
        }
        if (!ok)
            return false;
    }
    if (http_transport && script->framing == MOCK_FRAME_CHUNKED && !closed)
        return mock_write_all(fd, "0\r\n\r\n", 5U, split);
    return true;
}

#ifndef YEW_MOCKAI_NO_MAIN
static bool mock_read_request(int fd)
{
    unsigned char *request = malloc(MOCK_MAX_REQUEST + 1U);
    size_t len = 0U;
    size_t header_end = 0U;
    size_t body_len = 0U;

    if (request == NULL) {
        mock_system_error("malloc");
        return false;
    }
    while (len < MOCK_MAX_REQUEST) {
        ssize_t n = read(fd, request + len, MOCK_MAX_REQUEST - len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += (size_t)n;
        request[len] = '\0';
        if (header_end == 0U) {
            unsigned char *marker = (unsigned char *)strstr((char *)request,
                                                            "\r\n\r\n");
            if (marker != NULL) {
                char *length = strstr((char *)request, "Content-Length:");

                header_end = (size_t)(marker - request) + 4U;
                if (length != NULL && (size_t)(length - (char *)request) <
                                      header_end) {
                    unsigned long parsed;
                    char *value = length + strlen("Content-Length:");
                    char *end = NULL;

                    while (*value == ' ' || *value == '\t')
                        value++;
                    errno = 0;
                    parsed = strtoul(value, &end, 10);
                    if (errno != 0 || end == value || parsed >
                        MOCK_MAX_REQUEST - header_end) {
                        free(request);
                        return false;
                    }
                    body_len = (size_t)parsed;
                }
            }
        }
        if (header_end != 0U && len >= header_end + body_len) {
            free(request);
            return true;
        }
    }
    free(request);
    return header_end != 0U && len >= header_end + body_len;
}

static int mock_listener(unsigned port, unsigned *bound_port)
{
    struct sockaddr_in address;
    socklen_t address_len = (socklen_t)sizeof(address);
    int listener;
    int reuse = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        mock_system_error("socket");
        return -1;
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     (socklen_t)sizeof(reuse));
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    if (bind(listener, (const struct sockaddr *)&address,
             (socklen_t)sizeof(address)) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_len) != 0 ||
        listen(listener, 16) != 0) {
        mock_system_error("bind/listen");
        (void)close(listener);
        return -1;
    }
    *bound_port = (unsigned)ntohs(address.sin_port);
    return listener;
}

int main(int argc, char **argv)
{
    const char *script_path = NULL;
    unsigned long port = 0UL;
    unsigned long seed = 0UL;
    unsigned bound_port;
    MockScript script;
    struct sigaction stop_action;
    int listener;
    int i;

    for (i = 1; i < argc; i++) {
        unsigned long value;

        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc &&
            mock_number(argv[++i], 65535UL, &value))
            port = value;
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc)
            script_path = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc &&
                 mock_number(argv[++i], 0xffffffffUL, &value))
            seed = value;
        else {
            mock_error("usage: mockai --port N --script PATH [--seed N]");
            return 2;
        }
    }
    (void)seed;
    if (script_path == NULL) {
        mock_error("--script is required");
        return 2;
    }
    if (!mock_script_load(script_path, &script))
        return 2;
    (void)memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = mock_stop_handler;
    if (sigemptyset(&stop_action.sa_mask) != 0 ||
        sigaction(SIGTERM, &stop_action, NULL) != 0) {
        mock_system_error("install SIGTERM handler");
        mock_script_free(&script);
        return 2;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    listener = mock_listener((unsigned)port, &bound_port);
    if (listener < 0) {
        mock_script_free(&script);
        return 2;
    }
    (void)printf("port %u\n", bound_port);
    (void)fflush(stdout);
    while (!MOCK_STOP_REQUESTED()) {
        int fd;

        do {
            fd = accept(listener, NULL, NULL);
        } while (fd < 0 && errno == EINTR && !MOCK_STOP_REQUESTED());
        if (fd < 0) {
            if (MOCK_STOP_REQUESTED())
                break;
            mock_system_error("accept");
            break;
        }
        if (mock_read_request(fd))
            (void)mock_script_replay(&script, fd, true);
        (void)close(fd);
    }
    (void)close(listener);
    mock_script_free(&script);
    return MOCK_STOP_REQUESTED() ? 0 : 2;
}
#endif
