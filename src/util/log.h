#ifndef SAG_UTIL_LOG_H
#define SAG_UTIL_LOG_H

#include <stdbool.h>

#include "util/base.h"

typedef enum {
    SAG_LOG_DEBUG,
    SAG_LOG_INFO,
    SAG_LOG_WARN,
    SAG_LOG_ERROR
} SagLogLevel;

typedef struct SagLogSink {
    void (*write)(void *user, SagLogLevel level, const char *msg);
    void *user;
} SagLogSink;

void sag_log(SagLogLevel level, const char *fmt, ...);
void sag_log_set_sink(const SagLogSink *sink);

/* Runs before an internal-error report so terminal owners can restore it. */
void sag_bug_set_prehook(void (*fn)(void));
_Noreturn void sag_bug(const char *file, int line, const char *fmt, ...);
#define SAG_BUG(...) sag_bug(__FILE__, __LINE__, __VA_ARGS__)

#endif
