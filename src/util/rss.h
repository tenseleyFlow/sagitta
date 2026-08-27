#ifndef YEW_UTIL_RSS_H
#define YEW_UTIL_RSS_H

#include "util/base.h"

/* Current resident bytes, or zero when the platform cannot report them. */
u64 yew_rss_bytes(void);

/* Peak resident bytes, normalized across supported operating systems. */
u64 yew_rss_peak_bytes(void);

/* Emit one machine-readable INFO row only when YEW_PROF is exactly "1". */
void yew_rss_checkpoint(const char *name);

#endif
