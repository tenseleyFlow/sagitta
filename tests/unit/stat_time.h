#ifndef YEW_TEST_STAT_TIME_H
#define YEW_TEST_STAT_TIME_H

#include <sys/stat.h>
#include <time.h>

static inline struct timespec yew_test_stat_atime(const struct stat *st)
{
#if defined(__APPLE__)
    return st->st_atimespec;
#else
    return st->st_atim;
#endif
}

static inline struct timespec yew_test_stat_mtime(const struct stat *st)
{
#if defined(__APPLE__)
    return st->st_mtimespec;
#else
    return st->st_mtim;
#endif
}

#endif
