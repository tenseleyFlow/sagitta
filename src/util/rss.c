#define _POSIX_C_SOURCE 200809L

#include "util/rss.h"

#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static u64 checked_scale(unsigned long long value, u64 scale)
{
    if (value > UINT64_MAX / scale)
        return 0U;
    return (u64)value * scale;
}

u64 yew_rss_peak_bytes(void)
{
    struct rusage usage;
    unsigned long long peak;

    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0)
        return 0U;
    peak = (unsigned long long)usage.ru_maxrss;
#if defined(__APPLE__)
    {
        u64 bytes = (u64)peak;

        return (unsigned long long)bytes == peak ? bytes : 0U;
    }
#else
    return checked_scale(peak, 1024U);
#endif
}

u64 yew_rss_bytes(void)
{
#if defined(__linux__)
    unsigned long long total_pages;
    unsigned long long resident_pages;
    long page_size;
    FILE *statm = fopen("/proc/self/statm", "r");

    if (statm == NULL)
        return 0U;
    if (fscanf(statm, "%llu %llu", &total_pages, &resident_pages) != 2) {
        (void)fclose(statm);
        return 0U;
    }
    (void)total_pages;
    if (fclose(statm) != 0)
        return 0U;
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 0U;
    return checked_scale(resident_pages, (u64)page_size);
#else
    return yew_rss_peak_bytes();
#endif
}

void yew_rss_checkpoint(const char *name)
{
    const char *prof = getenv("YEW_PROF");
    u64 current;
    u64 peak;

    if (name == NULL || name[0] == '\0' || prof == NULL ||
        strcmp(prof, "1") != 0)
        return;
    current = yew_rss_bytes();
    peak = yew_rss_peak_bytes();
    yew_log(YEW_LOG_INFO,
            "rss checkpoint=%s current_bytes=%llu peak_bytes=%llu", name,
            (unsigned long long)current, (unsigned long long)peak);
}
