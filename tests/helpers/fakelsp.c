/* Sprint 45 integration helper: one request in, one deliberately awkward
 * JSON-RPC response out.  This is a protocol fixture, not an LSP server. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;

    while (len != 0U) {
        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_exact(int fd, unsigned char *out, size_t len)
{
    while (len != 0U) {
        ssize_t n = read(fd, out, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        out += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_request(unsigned char **body, size_t *body_len)
{
    char line[4097];
    size_t used = 0U;
    size_t want = 0U;
    bool have_len = false;

    for (;;) {
        unsigned char ch;

        if (!read_exact(STDIN_FILENO, &ch, 1U))
            return false;
        if (ch == (unsigned char)'\n') {
            if (used != 0U && line[used - 1U] == '\r')
                used--;
            line[used] = '\0';
            if (used == 0U)
                break;
            if (strncmp(line, "Content-Length:", 15U) == 0) {
                char *end = NULL;
                unsigned long n = strtoul(line + 15U, &end, 10);

                if (end == line + 15U || *end != '\0')
                    return false;
                want = (size_t)n;
                have_len = true;
            }
            used = 0U;
            continue;
        }
        if (used + 1U >= sizeof(line))
            return false;
        line[used++] = (char)ch;
    }
    if (!have_len)
        return false;
    *body = malloc(want == 0U ? 1U : want);
    if (*body == NULL)
        return false;
    *body_len = want;
    return read_exact(STDIN_FILENO, *body, want);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "giant";
    unsigned char *request = NULL;
    size_t request_len = 0U;
    char header[80];
    int hlen;

    if (!read_request(&request, &request_len))
        return 2;
    if (!write_all(STDERR_FILENO, "fake-lsp stderr\n", 16U)) {
        free(request);
        return 3;
    }
    if (strcmp(mode, "mid") == 0) {
        static const char partial[] = "Content-Length: 10\r\n\r\n{}";

        free(request);
        return write_all(STDOUT_FILENO, partial, sizeof(partial) - 1U) ?
                   0 : 3;
    }
    hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n",
                    request_len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header)) {
        free(request);
        return 3;
    }
    if (strcmp(mode, "onebyte") == 0) {
        size_t i;

        for (i = 0U; i < (size_t)hlen; i++) {
            if (!write_all(STDOUT_FILENO, header + i, 1U))
                goto fail;
        }
        for (i = 0U; i < request_len; i++) {
            if (!write_all(STDOUT_FILENO, request + i, 1U))
                goto fail;
        }
    } else if (!write_all(STDOUT_FILENO, header, (size_t)hlen) ||
               !write_all(STDOUT_FILENO, request, request_len)) {
        goto fail;
    }
    free(request);
    if (strcmp(mode, "live") == 0)
        (void)poll(NULL, 0U, 3000);
    return 0;
fail:
    free(request);
    return 3;
}
