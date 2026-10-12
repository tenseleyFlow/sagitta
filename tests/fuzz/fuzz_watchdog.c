/* fuzzlib-selftest fixture, never a campaign target: hangs on exactly one
 * builtin seed so the per-input watchdog must fire, and must leave that
 * input behind as a replayable crash file. */
#include "fuzzlib.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile unsigned long watchdog_spin;
static bool watchdog_suspended;

static bool check_watchdog(const u8 *data, size_t len, char *why,
                           size_t why_cap)
{
    (void)why;
    (void)why_cap;
    if (len == 4U && memcmp(data, "yew\n", 4U) == 0) {
        if (getenv("YEW_FUZZ_WATCHDOG_SUSPEND") != NULL &&
            !watchdog_suspended) {
            int fd = open("suspend.ready", O_WRONLY | O_CREAT | O_TRUNC,
                          0666);

            if (fd < 0 || close(fd) != 0)
                return false;
            watchdog_suspended = true;
            (void)raise(SIGSTOP);
            return true;
        }
        for (;;)
            watchdog_spin++;
    }
    return true;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_watchdog", NULL, check_watchdog);
}
