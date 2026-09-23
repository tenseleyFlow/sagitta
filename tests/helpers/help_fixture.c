/*
 * Sprint 57.25: a NATIVE executable whose `--help` a test controls.
 *
 * The help layer runs only ELF / Mach-O programs by default, so a shell
 * script cannot stand in for "a tool on PATH".  Tests copy this binary
 * under whatever name they need (`tool`, `rm`, `mkfs.fake`) and put its
 * behaviour in files beside the copy, keyed by the path it was run as:
 *
 *   <argv0>.log          every invocation is appended: its argv, space-
 *                        joined, then ` stdin=eof` (or `=data`) -- how a
 *                        test proves WHAT ran, and that stdin was closed;
 *   <argv0>.hold         while it exists, wait (a pending answer that a
 *                        test or PTY case releases by deleting it);
 *   <argv0>[.a.b].help   printed to stdout for `argv0 a b --help`;
 *   <argv0>[.a.b].help2  printed to stderr for the same;
 *   <argv0>.ran          created when run WITHOUT a trailing --help.
 *
 * It never reads the terminal and gives up holding after 60 s, so a
 * broken test cannot leave it running.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void emit(const char *path, int fd)
{
    char buf[4096];
    int in = open(path, O_RDONLY);
    ssize_t n;

    if (in < 0)
        return;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;

        while (off < n) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));

            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                break;
            off += w;
        }
    }
    (void)close(in);
}

int main(int argc, char **argv)
{
    char path[4096];
    char chunk[16];
    const char *me = argc > 0 ? argv[0] : "help_fixture";
    int help = argc > 1 && strcmp(argv[argc - 1], "--help") == 0;
    ssize_t got;
    FILE *log;
    int i;
    int waited = 0;

    do {
        got = read(0, chunk, sizeof(chunk));
    } while (got < 0 && errno == EINTR);
    (void)snprintf(path, sizeof(path), "%s.log", me);
    log = fopen(path, "a");
    if (log != NULL) {
        for (i = 0; i < argc; i++)
            (void)fprintf(log, "%s%s", i == 0 ? "" : " ", argv[i]);
        (void)fprintf(log, " stdin=%s\n",
                      got == 0 ? "eof" : got > 0 ? "data" : "err");
        (void)fclose(log);
    }
    (void)snprintf(path, sizeof(path), "%s.hold", me);
    while (access(path, F_OK) == 0 && waited < 3000) {
        struct timespec ts = {0, 20L * 1000L * 1000L};

        (void)nanosleep(&ts, NULL);
        waited++;
    }
    if (!help) {
        (void)snprintf(path, sizeof(path), "%s.ran", me);
        log = fopen(path, "w");
        if (log != NULL)
            (void)fclose(log);
        return 0;
    }
    {
        size_t n = (size_t)snprintf(path, sizeof(path), "%s", me);

        for (i = 1; i < argc - 1 && n < sizeof(path) - 64U; i++)
            n += (size_t)snprintf(path + n, sizeof(path) - n, ".%s", argv[i]);
        (void)snprintf(path + n, sizeof(path) - n, ".help");
        emit(path, 1);
        (void)snprintf(path + n, sizeof(path) - n, ".help2");
        emit(path, 2);
    }
    /* Many real tools exit non-zero after printing help. */
    return 2;
}
