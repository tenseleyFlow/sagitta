#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <stdbool.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void die(const char *what)
{
    (void)fprintf(stderr, "fakehttp: %s: %s\n", what, strerror(errno));
    exit(2);
}

static void pause_ms(long ms)
{
    struct timespec delay;

    delay.tv_sec = ms / 1000;
    delay.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static void write_all(int fd, const char *bytes, size_t len, bool bytewise)
{
    size_t sent = 0U;

    while (sent < len) {
        size_t want = bytewise ? 1U : len - sent;
        ssize_t n = send(fd, bytes + sent, want, 0);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        die("send");
    }
}

static size_t read_request(int fd, char *bytes, size_t cap)
{
    size_t len = 0U;

    if (bytes == NULL || cap == 0U)
        return 0U;
    while (len + 1U < cap) {
        ssize_t n = recv(fd, bytes + len, cap - len - 1U, 0);

        if (n > 0) {
            len += (size_t)n;
            bytes[len] = '\0';
            if (strstr(bytes, "\r\n\r\n") != NULL)
                return len;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return len;
    }
    return len;
}

static int accept_one(int listener)
{
    int fd;

    do {
        fd = accept(listener, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        die("accept");
    return fd;
}

int main(int argc, char **argv)
{
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    const char *mode = argc > 1 ? argv[1] : "giant";
    char request[4096];
    int listener;
    int fd;

    (void)signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        die("socket");
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0)
        die("bind");
    if (getsockname(listener, (struct sockaddr *)&address, &address_len) != 0)
        die("getsockname");
    if (listen(listener, 4) != 0)
        die("listen");
    (void)printf("%u\n", (unsigned)ntohs(address.sin_port));
    (void)fflush(stdout);

    fd = accept_one(listener);
    (void)read_request(fd, request, sizeof(request));
    if ((strcmp(mode, "models") == 0 ||
         strcmp(mode, "models-close") == 0 ||
         strcmp(mode, "malformed") == 0 || strcmp(mode, "rate") == 0) &&
        strncmp(request, "GET /api/tags HTTP/1.1\r\n",
                strlen("GET /api/tags HTTP/1.1\r\n")) != 0) {
        (void)fprintf(stderr, "fakehttp: unexpected model-list request\n");
        (void)close(fd);
        (void)close(listener);
        return 3;
    }
    if (strcmp(mode, "delay") == 0)
        pause_ms(200);
    if (strcmp(mode, "chunk") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n5\r\nhello\r\n0\r\nX-End: yes\r\n\r\n";
        write_all(fd, response, sizeof(response) - 1U, false);
    } else if (strcmp(mode, "models") == 0) {
        static const char body[] =
            "{\"models\":[{\"name\":\"qwen2.5\"},{\"name\":\"coder\"}]}";
        char response[512];
        int n = snprintf(response, sizeof(response),
                         "HTTP/1.1 200 OK\r\nContent-Length: %llu\r\n"
                         "Connection: close\r\n\r\n%s",
                         (unsigned long long)(sizeof(body) - 1U), body);

        if (n < 0 || (size_t)n >= sizeof(response))
            die("format response");
        write_all(fd, response, (size_t)n, false);
    } else if (strcmp(mode, "models-close") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
            "{\"models\":[{\"name\":\"qwen2.5\"},{\"name\":\"coder\"}]}";

        write_all(fd, response, sizeof(response) - 1U, true);
    } else if (strcmp(mode, "malformed") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
            "Connection: close\r\n\r\n{bad";

        write_all(fd, response, sizeof(response) - 1U, false);
    } else if (strcmp(mode, "rate") == 0) {
        static const char response[] =
            "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 21\r\n"
            "Retry-After: 3\r\nConnection: close\r\n\r\n"
            "{\"error\":\"slow down\"}";

        write_all(fd, response, sizeof(response) - 1U, false);
    } else if (strcmp(mode, "close") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello";
        write_all(fd, response, sizeof(response) - 1U, false);
    } else if (strcmp(mode, "midclose") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhel";
        write_all(fd, response, sizeof(response) - 1U, false);
    } else if (strcmp(mode, "idle") == 0) {
        static const char prefix[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nh";
        write_all(fd, prefix, sizeof(prefix) - 1U, false);
        pause_ms(200);
    } else if (strcmp(mode, "stale") == 0) {
        static const char first[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
            "Connection: keep-alive\r\n\r\none";
        static const char second[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
            "Connection: close\r\n\r\ntwo";
        write_all(fd, first, sizeof(first) - 1U, false);
        pause_ms(60);
        (void)close(fd);
        fd = accept_one(listener);
        (void)read_request(fd, request, sizeof(request));
        write_all(fd, second, sizeof(second) - 1U, false);
    } else {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
            "Connection: close\r\n\r\nhello";
        write_all(fd, response, sizeof(response) - 1U,
                  strcmp(mode, "byte") == 0);
    }
    (void)close(fd);
    (void)close(listener);
    return 0;
}
