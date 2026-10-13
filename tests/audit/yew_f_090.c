/*
 * YEW-F-090 — the DFA cache-pressure test can pass without one flush.
 *
 * Correct behavior: a lowered-cap seam deterministically observes two cache
 * flushes, then give-up, while the public dispatcher returns the Pike answer.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_090(char *why, size_t why_cap)
{
    FILE *file = fopen("tests/unit/test_re_engines.c", "rb");
    char line[1024];
    bool named = false;
    bool observes_flushes = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "test_re_dfa_two_flush_fallback") != NULL)
            named = true;
        if (strstr(line, "yew_re_dfa_test_limits") != NULL)
            observes_flushes = true;
    }
    (void)fclose(file);
    if (named && observes_flushes)
        return true;
    (void)snprintf(why, why_cap,
                   "DFA test neither lowers the cap nor observes two flushes");
    return false;
}
