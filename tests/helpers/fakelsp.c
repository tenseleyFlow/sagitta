/* Sprint 45 integration helper: one request in, one deliberately awkward
 * JSON-RPC response out.  This is a protocol fixture, not an LSP server. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

static bool body_has(const unsigned char *body, size_t len, const char *text)
{
    size_t n = strlen(text);
    size_t i;

    if (n > len)
        return false;
    for (i = 0U; i + n <= len; i++)
        if (memcmp(body + i, text, n) == 0)
            return true;
    return false;
}

static unsigned long long body_id(const unsigned char *body, size_t len)
{
    static const char key[] = "\"id\":";
    size_t i;

    for (i = 0U; i + sizeof(key) - 1U < len; i++) {
        unsigned long long id = 0U;
        size_t at;

        if (memcmp(body + i, key, sizeof(key) - 1U) != 0)
            continue;
        at = i + sizeof(key) - 1U;
        while (at < len && body[at] >= '0' && body[at] <= '9') {
            id = id * 10U + (unsigned long long)(body[at] - '0');
            at++;
        }
        return id;
    }
    return 0U;
}

static bool write_frame(const char *body)
{
    char header[80];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "Content-Length: %zu\r\n\r\n", len);

    return n > 0 && (size_t)n < sizeof(header) &&
           write_all(STDOUT_FILENO, header, (size_t)n) &&
           write_all(STDOUT_FILENO, body, len);
}

static bool read_method(const char *method, unsigned long long *id)
{
    unsigned char *body = NULL;
    size_t len = 0U;
    bool ok;

    if (!read_request(&body, &len))
        return false;
    ok = body_has(body, len, method);
    if (id != NULL)
        *id = body_id(body, len);
    free(body);
    return ok;
}

static int run_session(const char *mode, const char *marker)
{
    unsigned long long id = 0U;
    const char *encoding = NULL;
    char response[320];
    bool crash = false;
    int n;

    if (strcmp(mode, "session-resistant") == 0)
        (void)signal(SIGTERM, SIG_IGN);
    if (strcmp(mode, "session-crash-restart") == 0) {
        int fd;

        if (marker == NULL)
            return 9;
        fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            crash = true;
            (void)close(fd);
        } else if (errno != EEXIST) {
            return 9;
        }
    }

    if (!read_method("\"method\":\"initialize\"", &id) || id == 0U)
        return 10;
    if (!write_all(STDERR_FILENO, "seq:initialize\n", 15U))
        return 11;
    if (strcmp(mode, "session-timeout") == 0) {
        (void)poll(NULL, 0U, 3000);
        return 0;
    }
    if (strcmp(mode, "session-utf8") == 0 ||
        strcmp(mode, "session-sync") == 0 ||
        strcmp(mode, "session-close") == 0 ||
        strcmp(mode, "session-unversioned") == 0 ||
        strcmp(mode, "session-stale") == 0 ||
        strcmp(mode, "session-resistant") == 0 ||
        strcmp(mode, "session-crash-restart") == 0)
        encoding = ",\"positionEncoding\":\"utf-8\"";
    else if (strcmp(mode, "session-utf16") == 0)
        encoding = ",\"positionEncoding\":\"utf-16\"";
    else if (strcmp(mode, "session-garbage") == 0)
        encoding = ",\"positionEncoding\":\"utf-32\"";
    else if (strcmp(mode, "session-absent") == 0 ||
             strcmp(mode, "session-nosync") == 0)
        encoding = "";
    else
        return 12;
    if (strcmp(mode, "session-nosync") == 0)
        n = snprintf(response, sizeof(response),
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{"
            "\"capabilities\":{\"hoverProvider\":true}}}", id);
    else
        n = snprintf(response, sizeof(response),
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{"
            "\"capabilities\":{\"textDocumentSync\":{"
            "\"openClose\":true,\"change\":2,\"save\":true}%s,"
            "\"hoverProvider\":true}}}", id, encoding);
    if (n < 0 || (size_t)n >= sizeof(response) || !write_frame(response))
        return 13;
    if (!read_method("\"method\":\"initialized\"", NULL) ||
        !write_all(STDERR_FILENO, "seq:initialized\n", 16U))
        return 14;
    if (!read_method("\"method\":\"textDocument/didOpen\"", NULL) ||
        !write_all(STDERR_FILENO, "seq:didOpen\n", 12U))
        return 15;
    if (crash) {
        (void)write_all(STDERR_FILENO, "seq:crash\n", 10U);
        (void)poll(NULL, 0U, 1000);
        return 25;
    }
    if (strcmp(mode, "session-sync") == 0) {
        if (!read_method("\"method\":\"textDocument/didChange\"", NULL) ||
            !write_all(STDERR_FILENO, "seq:didChange\n", 14U))
            return 19;
        if (!read_method("\"method\":\"textDocument/didSave\"", NULL) ||
            !write_all(STDERR_FILENO, "seq:didSave\n", 12U))
            return 20;
        if (!read_method("\"method\":\"textDocument/didClose\"", NULL) ||
            !write_all(STDERR_FILENO, "seq:didClose\n", 13U))
            return 21;
    } else if (strcmp(mode, "session-close") == 0) {
        if (!read_method("\"method\":\"textDocument/didClose\"", NULL) ||
            !write_all(STDERR_FILENO, "seq:didClose\n", 13U))
            return 21;
    } else if (strcmp(mode, "session-unversioned") == 0) {
        if (!read_method("\"method\":\"textDocument/didChange\"", NULL) ||
            !write_all(STDERR_FILENO, "seq:didChange\n", 14U))
            return 19;
    } else if (strcmp(mode, "session-stale") == 0) {
        if (!read_method("\"method\":\"textDocument/hover\"", &id) ||
            id == 0U ||
            !write_all(STDERR_FILENO, "seq:hover\n", 10U))
            return 22;
        (void)poll(NULL, 0U, 200);
        n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%llu,"
                     "\"result\":{\"contents\":\"old\"}}", id);
        if (n < 0 || (size_t)n >= sizeof(response) || !write_frame(response))
            return 23;
        if (!read_method("\"method\":\"textDocument/didChange\"", NULL) ||
            !write_all(STDERR_FILENO, "seq:didChange\n", 14U))
            return 24;
    }
    if (!read_method("\"method\":\"shutdown\"", &id) || id == 0U ||
        !write_all(STDERR_FILENO, "seq:shutdown\n", 13U))
        return 16;
    n = snprintf(response, sizeof(response),
                 "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":null}",
                 id);
    if (n < 0 || (size_t)n >= sizeof(response) || !write_frame(response))
        return 17;
    if (!read_method("\"method\":\"exit\"", NULL) ||
        !write_all(STDERR_FILENO, "seq:exit\n", 9U))
        return 18;
    if (strcmp(mode, "session-resistant") == 0)
        for (;;)
            (void)poll(NULL, 0U, 1000);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "giant";
    unsigned char *request = NULL;
    size_t request_len = 0U;
    char header[80];
    int hlen;

    if (strncmp(mode, "session-", 8U) == 0)
        return run_session(mode, argc > 2 ? argv[2] : NULL);

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
