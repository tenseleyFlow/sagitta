#ifndef SAG_UTIL_BASE_H
#define SAG_UTIL_BASE_H

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

#define SAG_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

enum {
    SAG_EXIT_OK = 0,
    SAG_EXIT_ERR,
    SAG_EXIT_BATCH,
    SAG_EXIT_IO,
    SAG_EXIT_BUG
};

#define SAG_VERSION "1.0.0-dev"

/* Allocation failure is an internal error, never a recoverable NULL. */
void *sag_xmalloc(size_t size);
void *sag_xrealloc(void *ptr, size_t size);
void *sag_xcalloc(size_t count, size_t size);
void *sag_xreallocarray(void *ptr, size_t count, size_t size);

#endif
