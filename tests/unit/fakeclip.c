#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool write_all(int fd, const void *data, size_t len)
{
    const unsigned char *at = data;

    while (len != 0U) {
        ssize_t n = write(fd, at, len);

        if (n > 0) {
            at += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static int copy_fd(int from, int to)
{
    unsigned char bytes[4096];

    for (;;) {
        ssize_t n = read(from, bytes, sizeof(bytes));

        if (n > 0) {
            if (!write_all(to, bytes, (size_t)n))
                return 1;
        } else if (n == 0) {
            return 0;
        } else if (errno != EINTR) {
            return 1;
        }
    }
}

static void record_argv(const char *path, int argc, char **argv)
{
    size_t len = strlen(path);
    char *name = malloc(len + sizeof(".argv"));
    int fd;
    int i;

    if (name == NULL)
        return;
    (void)memcpy(name, path, len);
    (void)memcpy(name + len, ".argv", sizeof(".argv"));
    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    free(name);
    if (fd < 0)
        return;
    for (i = 0; i < argc; i++) {
        (void)write_all(fd, argv[i], strlen(argv[i]));
        (void)write_all(fd, "\n", 1U);
    }
    (void)close(fd);
}

static void record_pid(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    char text[64];
    int n;

    if (fd < 0)
        return;
    n = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
    if (n > 0)
        (void)write_all(fd, text, (size_t)n);
    (void)close(fd);
}

int main(int argc, char **argv)
{
    const char *path;
    const char *mode;
    const char *base;
    int fd;
    int result;

    base = strrchr(argv[0], '/');
    base = base != NULL ? base + 1 : argv[0];
    if (strcmp(base, "wl-copy") == 0) {
        path = getenv("SAG_FAKECLIP_OUTPUT");
        mode = "write";
        if (path == NULL)
            return 2;
    } else {
        if (argc < 3)
            return 2;
        path = argv[1];
        mode = argv[2];
    }
    if (strcmp(mode, "exit") == 0)
        return 17;
    if (strcmp(mode, "hang") == 0) {
        for (;;)
            (void)pause();
    }
    if (strcmp(mode, "read-hold") == 0) {
        pid_t holder = fork();

        if (holder < 0)
            return 4;
        if (holder == 0) {
            struct timespec pause = {0, 250000000L};

            while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
                ;
            fd = open(path, O_RDONLY);
            if (fd < 0)
                _exit(3);
            result = copy_fd(fd, STDOUT_FILENO);
            (void)close(fd);
            _exit(result);
        }
        return 0;
    }
    if (strcmp(mode, "read") == 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0)
            return 3;
        result = copy_fd(fd, STDOUT_FILENO);
        (void)close(fd);
        return result;
    }
    if (strcmp(mode, "write") != 0 && strcmp(mode, "stay") != 0)
        return 2;
    record_argv(path, argc, argv);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 3;
    result = copy_fd(STDIN_FILENO, fd);
    (void)close(fd);
    if (result != 0 || strcmp(mode, "stay") != 0)
        return result;
    if (argc < 4)
        return 2;
    record_pid(argv[3]);
    for (;;)
        (void)pause();
}
