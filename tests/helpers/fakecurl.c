#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { CONFIG_MAX = 1024 * 1024 };

static int env_code(const char *name, int fallback)
{
    const char *s = getenv(name);
    char *end = NULL;
    long n;

    if (s == NULL || *s == '\0')
        return fallback;
    errno = 0;
    n = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || n < 0 || n > 255)
        return fallback;
    return (int)n;
}

static int append_counter(const char *path)
{
    int fd;
    ssize_t n;

    if (path == NULL || *path == '\0')
        return 0;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return -1;
    do {
        n = write(fd, "x", 1U);
    } while (n < 0 && errno == EINTR);
    if (close(fd) != 0 || n != 1)
        return -1;
    return 0;
}

static int write_all(int fd, const void *bytes, size_t len)
{
    const unsigned char *p = bytes;

    while (len != 0U) {
        ssize_t n = write(fd, p, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int capture_config(const char *path, const unsigned char *buf,
                          size_t len)
{
    int fd;
    int rc;

    if (path == NULL || *path == '\0')
        return 0;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    rc = write_all(fd, buf, len);
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

static int cmdline_has(const char *needle)
{
    unsigned char buf[4096];
    int fd;
    ssize_t n;
    size_t i;
    size_t want;

    if (needle == NULL || *needle == '\0')
        return 0;
    fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0)
        return 0; /* Non-Linux: argv is checked below as a fallback. */
    do {
        n = read(fd, buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);
    (void)close(fd);
    if (n <= 0)
        return 0;
    want = strlen(needle);
    for (i = 0U; i + want <= (size_t)n; i++) {
        if (memcmp(buf + i, needle, want) == 0)
            return 1;
    }
    return 0;
}

static int request_mode(int argc, char **argv)
{
    unsigned char *config;
    size_t len = 0U;
    const char *secret = getenv("YEW_FAKECURL_SECRET");
    const char *fixture = getenv("YEW_FAKECURL_STDOUT");
    const char *status = getenv("YEW_FAKECURL_STATUS");
    int i;

    if (argc != 5 || strcmp(argv[1], "-sS") != 0 ||
        strcmp(argv[2], "--no-buffer") != 0 ||
        strcmp(argv[3], "--config") != 0 || strcmp(argv[4], "-") != 0)
        return 2;
    for (i = 0; i < argc; i++) {
        if (secret != NULL && *secret != '\0' && strstr(argv[i], secret) != NULL)
            return 2;
    }
    if (cmdline_has(secret))
        return 2;

    config = malloc(CONFIG_MAX + 1U);
    if (config == NULL)
        return 2;
    while (len < CONFIG_MAX) {
        ssize_t n = read(STDIN_FILENO, config + len, CONFIG_MAX - len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            free(config);
            return 2;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    config[len] = '\0';
    if (len == CONFIG_MAX || strstr((char *)config, "url = \"") == NULL ||
        strstr((char *)config, "request = \"") == NULL ||
        strstr((char *)config, "data-binary = \"") == NULL ||
        strstr((char *)config, "write-out = \"") == NULL ||
        (secret != NULL && *secret != '\0' &&
         strstr((char *)config, secret) == NULL) ||
        capture_config(getenv("YEW_FAKECURL_CAPTURE"), config, len) != 0) {
        free(config);
        return 2;
    }
    free(config);

    if (fixture != NULL && write_all(STDOUT_FILENO, fixture, strlen(fixture)) != 0)
        return 2;
    if (status == NULL)
        status = "200";
    if (write_all(STDERR_FILENO, "yew-http-status: ", 17U) != 0 ||
        write_all(STDERR_FILENO, status, strlen(status)) != 0 ||
        write_all(STDERR_FILENO, "\n", 1U) != 0)
        return 2;
    return env_code("YEW_FAKECURL_EXIT", 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        const char *version = getenv("YEW_FAKECURL_VERSION");

        if (append_counter(getenv("YEW_FAKECURL_COUNTER")) != 0)
            return 2;
        if (version == NULL)
            version = "curl 8.5.0 fakecurl";
        if (write_all(STDOUT_FILENO, version, strlen(version)) != 0 ||
            write_all(STDOUT_FILENO, "\n", 1U) != 0)
            return 2;
        return env_code("YEW_FAKECURL_EXIT", 0);
    }
    return request_mode(argc, argv);
}
