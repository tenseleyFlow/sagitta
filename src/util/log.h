#ifndef YEW_UTIL_LOG_H
#define YEW_UTIL_LOG_H

#include <stdbool.h>

#include "util/base.h"

typedef enum {
    YEW_LOG_DEBUG,
    YEW_LOG_INFO,
    YEW_LOG_WARN,
    YEW_LOG_ERROR
} YewLogLevel;

typedef struct YewLogSink {
    void (*write)(void *user, YewLogLevel level, const char *msg);
    void *user;
} YewLogSink;

void yew_log(YewLogLevel level, const char *fmt, ...);
void yew_log_set_sink(const YewLogSink *sink);
/* An observer in addition to the file/custom sink.  Batch uses this to
 * mirror WARN+ without changing the durable logging contract. */
void yew_log_set_mirror(const YewLogSink *sink);

/* Runs before an internal-error report so terminal owners can restore it. */
void yew_bug_set_prehook(void (*fn)(void));
_Noreturn void yew_bug(const char *file, int line, const char *fmt, ...);
#define YEW_BUG(...) yew_bug(__FILE__, __LINE__, __VA_ARGS__)

#endif
