#ifndef YEW_UTIL_BASE_H
#define YEW_UTIL_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define YEW_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

enum {
    YEW_EXIT_OK = 0,
    YEW_EXIT_ERR,
    YEW_EXIT_BATCH,
    YEW_EXIT_IO,
    YEW_EXIT_BUG
};

#define YEW_VERSION "1.0.0-dev"

/*
 * The ONLY place in the tree allowed to call pipe(): every other site uses
 * this, so no descriptor is ever created without close-on-exec.
 *
 * pipe2() is Linux/FreeBSD; macOS (a locked 1.0 target) has no such call,
 * so there it degrades to pipe() + FD_CLOEXEC.  That fallback is race-free
 * *here* because invariant 8 makes the core single-threaded and nothing
 * forks from a signal handler — the window pipe2() closes needs a
 * concurrent spawn to matter, and yew has none.
 */
bool yew_pipe_cloexec(int fds[2]);

/* Allocation failure is an internal error, never a recoverable NULL. */
void *yew_xmalloc(size_t size);
void *yew_xrealloc(void *ptr, size_t size);
void *yew_xcalloc(size_t count, size_t size);
void *yew_xreallocarray(void *ptr, size_t count, size_t size);

/*
 * Wipe bytes through a volatile-qualified pointer so an optimizing compiler
 * cannot discard the stores merely because the object is never read again.
 * The pointed-to object must be writable even when a borrowing API exposes
 * it as const while it is in flight.
 */
void yew_memzero(void *bytes, size_t len);

#endif
