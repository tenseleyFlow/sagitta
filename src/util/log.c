#define _POSIX_C_SOURCE 200809L

#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static YewLogSink active_sink;
static bool has_custom_sink;
static YewLogSink mirror_sink;
static bool has_mirror_sink;
static void (*bug_prehook)(void);

static const char *level_name(YewLogLevel level)
{
    static const char *const names[] = {"debug", "info", "warn", "error"};

    if (level < YEW_LOG_DEBUG || level > YEW_LOG_ERROR) {
        return "error";
    }
    return names[level];
}

static YewLogLevel configured_level(void)
{
    const char *value = getenv("YEW_LOG_LEVEL");

    if (value == NULL || strcmp(value, "info") == 0) {
        return YEW_LOG_INFO;
    }
    if (strcmp(value, "debug") == 0) {
        return YEW_LOG_DEBUG;
    }
    if (strcmp(value, "warn") == 0) {
        return YEW_LOG_WARN;
    }
    if (strcmp(value, "error") == 0) {
        return YEW_LOG_ERROR;
    }
    return YEW_LOG_INFO;
}

static bool make_dir(const char *path)
{
    if (mkdir(path, 0700) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }
}

static bool make_parent_dirs(char *path)
{
    char *p;

    for (p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!make_dir(path)) {
                *p = '/';
                return false;
            }
            *p = '/';
        }
    }
    return true;
}

static char *default_log_path(void)
{
    const char *override = getenv("YEW_LOG");
    const char *state = getenv("XDG_STATE_HOME");
    const char *home;
    const char *suffix;
    size_t size;
    char *path;

    if (override != NULL && override[0] != '\0') {
        size = strlen(override) + 1U;
        path = yew_xmalloc(size);
        (void)memcpy(path, override, size);
        return path;
    }
    if (state != NULL && state[0] != '\0') {
        suffix = "/yew/log";
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            return NULL;
        }
        state = home;
        suffix = "/.local/state/yew/log";
    }
    size = strlen(state) + strlen(suffix) + 1U;
    path = yew_xmalloc(size);
    (void)snprintf(path, size, "%s%s", state, suffix);
    return path;
}

static void write_all(int fd, const char *data, size_t len)
{
    while (len != 0U) {
        ssize_t n = write(fd, data, len);
        if (n > 0) {
            data += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void default_write(YewLogLevel level, const char *msg)
{
    char timestamp[32];
    char *path = default_log_path();
    struct tm utc;
    time_t now;
    int fd;

    if (path == NULL || !make_parent_dirs(path)) {
        free(path);
        return;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    free(path);
    if (fd < 0) {
        return;
    }
    now = time(NULL);
    if (gmtime_r(&now, &utc) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0U) {
        (void)close(fd);
        return;
    }
    write_all(fd, timestamp, strlen(timestamp));
    write_all(fd, " ", 1U);
    write_all(fd, level_name(level), strlen(level_name(level)));
    write_all(fd, ": ", 2U);
    write_all(fd, msg, strlen(msg));
    write_all(fd, "\n", 1U);
    (void)close(fd);
}

static char *format_message(const char *fmt, va_list args)
{
    va_list copy;
    int needed;
    char *message;

    if (fmt == NULL)
        return NULL;
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0U, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        return NULL;
    }
    message = yew_xmalloc((size_t)needed + 1U);
    (void)vsnprintf(message, (size_t)needed + 1U, fmt, args);
    return message;
}

void yew_log_set_sink(const YewLogSink *sink)
{
    if (sink == NULL) {
        active_sink = (YewLogSink){0};
        has_custom_sink = false;
    } else {
        active_sink = *sink;
        has_custom_sink = true;
    }
}

void yew_log_set_mirror(const YewLogSink *sink)
{
    if (sink == NULL) {
        mirror_sink = (YewLogSink){0};
        has_mirror_sink = false;
    } else {
        mirror_sink = *sink;
        has_mirror_sink = true;
    }
}

void yew_bug_set_prehook(void (*fn)(void))
{
    bug_prehook = fn;
}

void yew_log(YewLogLevel level, const char *fmt, ...)
{
    va_list args;
    char *message;

    if (level < configured_level()) {
        return;
    }
    va_start(args, fmt);
    message = format_message(fmt, args);
    va_end(args);
    if (message == NULL) {
        return;
    }
    if (has_custom_sink) {
        if (active_sink.write != NULL) {
            active_sink.write(active_sink.user, level, message);
        }
    } else {
        default_write(level, message);
    }
    if (has_mirror_sink && mirror_sink.write != NULL)
        mirror_sink.write(mirror_sink.user, level, message);
    free(message);
}

_Noreturn void yew_bug(const char *file, int line, const char *fmt, ...)
{
    static const char report[] = "yew: please report this internal error";
    va_list args;
    char *detail;
    int needed;
    char *message;

    if (bug_prehook != NULL)
        bug_prehook();
    va_start(args, fmt);
    detail = format_message(fmt, args);
    va_end(args);
    if (detail == NULL) {
        detail = yew_xmalloc(20U);
        (void)strcpy(detail, "formatting failure");
    }
    needed = snprintf(NULL, 0U, "yew: internal error at %s:%d: %s",
        file, line, detail);
    if (needed < 0) {
        message = detail;
        detail = NULL;
    } else {
        message = yew_xmalloc((size_t)needed + 1U);
        (void)snprintf(message, (size_t)needed + 1U,
            "yew: internal error at %s:%d: %s", file, line, detail);
    }
    yew_log(YEW_LOG_ERROR, "%s", message);
    yew_log(YEW_LOG_ERROR, "%s", report);
    (void)fprintf(stderr, "%s\n%s\n", message, report);
    free(detail);
    free(message);
    exit(YEW_EXIT_BUG);
}
