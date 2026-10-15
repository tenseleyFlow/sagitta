/*
 * YEW-F-143 — text-buffer fuzzing installs its fake clock after root time.
 * Correct behavior: the deterministic root timestamp is rebased with the
 * injected clock before generated edits begin.
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

bool test_yew_f_143(char *why, size_t why_cap)
{
    bool wall = file_has("tests/fuzz/fuzz_textbuf.c",
                         "nodes.data[0].t_wall = run->wall_sec");
    bool mono = file_has("tests/fuzz/fuzz_textbuf.c",
                         "nodes.data[0].t_last_ms = run->clock_ms");

    if (wall && mono)
        return true;
    (void)snprintf(why, why_cap,
                   "fuzz_textbuf root predates its deterministic clock");
    return false;
}
