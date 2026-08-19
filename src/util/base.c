/* _GNU_SOURCE exposes pipe2() on glibc; the macOS path below does not
 * need it, and the guard keeps the declaration from leaking elsewhere. */
#define _GNU_SOURCE

#include "util/base.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "util/log.h"

bool yew_pipe_cloexec(int fds[2])
{
#if defined(__linux__) || defined(__FreeBSD__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    /* See base.h: safe without pipe2() because the core is single-threaded
     * and never forks from a signal handler. */
    if (pipe(fds) != 0)
        return false;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
    }
    return true;
#endif
}

void *yew_xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);

    if (!ptr)
        YEW_BUG("out of memory allocating %zu bytes", size);
    return ptr;
}

void *yew_xrealloc(void *ptr, size_t size)
{
    void *resized = realloc(ptr, size ? size : 1);

    if (!resized)
        YEW_BUG("out of memory reallocating to %zu bytes", size);
    return resized;
}

void *yew_xcalloc(size_t count, size_t size)
{
    void *ptr;

    if (count == 0 || size == 0) {
        ptr = calloc(1, 1);
        if (!ptr)
            YEW_BUG("out of memory allocating zero bytes");
        return ptr;
    }
    if (size != 0 && count > SIZE_MAX / size)
        YEW_BUG("allocation size overflow: %zu * %zu", count, size);
    ptr = calloc(count, size);
    if (!ptr)
        YEW_BUG("out of memory allocating %zu elements of %zu bytes", count,
                size);
    return ptr;
}

void *yew_xreallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
        YEW_BUG("allocation size overflow: %zu * %zu", count, size);
    return yew_xrealloc(ptr, count * size);
}

void yew_memzero(void *bytes, size_t len)
{
    volatile u8 *p = bytes;

    while (len != 0U) {
        *p++ = 0U;
        len--;
    }
}
