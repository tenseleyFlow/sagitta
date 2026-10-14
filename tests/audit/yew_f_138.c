/*
 * YEW-F-138 — clipboard polling makes assertion totals scheduler-dependent.
 * Correct behavior: clock acquisition can fail the test, but polling it must
 * not increment the semantic assertion count once per asynchronous turn.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_has(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char line[2048];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            (void)fclose(file);
            return true;
        }
    }
    (void)fclose(file);
    return false;
}

bool test_yew_f_138(char *why, size_t why_cap)
{
    bool uncounted_failure = file_has(
        "tests/unit/test_prompt_keys.c",
        "if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)");
    bool counted_poll = file_has(
        "tests/unit/test_prompt_keys.c",
        "YEW_ASSERT_EQ_I64(clock_gettime(CLOCK_MONOTONIC, &ts), 0)");

    if (uncounted_failure && !counted_poll)
        return true;
    (void)snprintf(why, why_cap,
                   "clipboard polling changes the assertion total");
    return false;
}
