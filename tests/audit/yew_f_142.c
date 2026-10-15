/*
 * YEW-F-142 — timed text-buffer fuzzing retains an unbounded trace.
 * Correct behavior: the custom stateful target rolls reproducible epochs at
 * a fixed trace-storage high-water mark instead of retaining every payload
 * for the full multi-hour stream.
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

bool test_yew_f_142(char *why, size_t why_cap)
{
    bool cap = file_has("tests/fuzz/fuzz_textbuf.c",
                        "YEW_TEXT_FUZZ_TRACE_BYTES_MAX");
    bool accounting = file_has("tests/fuzz/fuzz_textbuf.c",
                               "trace_storage_bytes");
    bool rollover = file_has("tests/fuzz/fuzz_textbuf.c",
                             "rollover_epoch");

    if (cap && accounting && rollover)
        return true;
    (void)snprintf(why, why_cap,
                   "fuzz_textbuf retains one unbounded campaign trace");
    return false;
}
