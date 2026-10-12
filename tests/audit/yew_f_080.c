/*
 * YEW-F-080 — host sleep fabricates a fuzz-watchdog finding.
 *
 * Correct behavior: a per-input watchdog measures time consumed by the fuzz
 * process, so suspending the host or stopping the process does not spend the
 * hang budget.  A real CPU spin must still expire and preserve its input.
 *
 * Baseline failure: fuzzlib arms alarm(2), whose wall-clock deadline expires
 * while macOS is asleep.  Resume delivers SIGALRM against the harmless input
 * that happened to be active before sleep and records it as a hang.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_080(char *why, size_t why_cap)
{
    char line[4096];
    FILE *file = fopen("tests/fuzz/fuzzlib.c", "rb");
    bool cpu_timer = false;
    bool cpu_signal = false;
    bool wall_alarm = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "setitimer(ITIMER_PROF") != NULL)
            cpu_timer = true;
        if (strstr(line, "sigaction(SIGPROF") != NULL)
            cpu_signal = true;
        if (strstr(line, "alarm(run->watchdog_seconds)") != NULL)
            wall_alarm = true;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    if (!cpu_timer || !cpu_signal || wall_alarm)
        (void)snprintf(why, why_cap,
                       "cpu_timer=%u cpu_signal=%u wall_alarm=%u",
                       cpu_timer ? 1U : 0U, cpu_signal ? 1U : 0U,
                       wall_alarm ? 1U : 0U);
    return cpu_timer && cpu_signal && !wall_alarm;
}
