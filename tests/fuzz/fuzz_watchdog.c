/* fuzzlib-selftest fixture, never a campaign target: hangs on exactly one
 * builtin seed so the per-input watchdog must fire, and must leave that
 * input behind as a replayable crash file. */
#include "fuzzlib.h"

#include <string.h>

static volatile unsigned long watchdog_spin;

static bool check_watchdog(const u8 *data, size_t len, char *why,
                           size_t why_cap)
{
    (void)why;
    (void)why_cap;
    if (len == 4U && memcmp(data, "yew\n", 4U) == 0) {
        for (;;)
            watchdog_spin++;
    }
    return true;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_watchdog", NULL, check_watchdog);
}
