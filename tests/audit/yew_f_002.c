/*
 * YEW-F-002 — long RI output delays a completed flag cluster.
 *
 * Correct behavior: a non-EOF job read ending in 65 regional indicators
 * emits the first 64 and holds only the final four-byte RI, which the next
 * read could extend into a flag pair.
 *
 * Baseline failure: the bounded backward restart mis-parities the odd run,
 * so yew_job_safe_prefix holds the final two RIs (eight bytes) and delays a
 * completed flag until more output arrives or the pipe reaches EOF.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/job.h"
#include "unicode/utf8.h"

bool test_yew_f_002(char *why, size_t why_cap)
{
    u8 text[65U * 4U];
    size_t at = 0U;
    size_t i;
    u64 safe;
    u64 expected = (u64)sizeof(text) - 4U;

    for (i = 0U; i < 65U; i++) {
        u8 encoded[YEW_UTF8_MAX];
        size_t n = yew_utf8_encode(0x1F1E6U + (u32)(i % 26U), encoded);

        if (n != 4U || at + n > sizeof(text)) {
            (void)snprintf(why, why_cap, "could not construct RI stream");
            return false;
        }
        (void)memcpy(text + at, encoded, n);
        at += n;
    }
    safe = yew_job_safe_prefix(text, (u64)at, false);
    if (safe != expected) {
        (void)snprintf(why, why_cap,
                       "safe prefix is %llu, expected %llu; %llu bytes held",
                       (unsigned long long)safe,
                       (unsigned long long)expected,
                       (unsigned long long)((u64)at - safe));
        return false;
    }
    return true;
}
