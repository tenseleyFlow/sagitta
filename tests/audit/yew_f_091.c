/*
 * YEW-F-091 — required zero-width replacements lack PTY goldens.
 *
 * Correct behavior: committed PTY cases drive line-prefix replacement with
 * and without a final newline and terminating global x-star replacement.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool test_yew_f_091(char *why, size_t why_cap)
{
    static const char *const names[] = {
        "s21_zero_width_prefix_final_newline",
        "s21_zero_width_prefix_no_final_newline",
        "s21_zero_width_global_terminates"
    };
    FILE *file = fopen("tests/pty/registry.c", "rb");
    char line[2048];
    bool found[3] = {false, false, false};
    size_t i;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        for (i = 0U; i < 3U; i++) {
            if (strstr(line, names[i]) != NULL)
                found[i] = true;
        }
    }
    (void)fclose(file);
    for (i = 0U; i < 3U; i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "tests/pty/goldens/%s.golden",
                       names[i]);
        if (!found[i] || access(path, R_OK) != 0) {
            (void)snprintf(why, why_cap,
                           "missing PTY case or golden: %s", names[i]);
            return false;
        }
    }
    return true;
}
