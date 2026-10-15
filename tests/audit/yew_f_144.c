/*
 * YEW-F-144 — the GCC sanitizer package harness preloads its fault shim
 * before libasan.
 * Correct behavior: test-pkg passes the discovered GCC ASan runtime into the
 * package harness, which places that runtime before its Linux fault shim.
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

bool test_yew_f_144(char *why, size_t why_cap)
{
    bool passed = file_has("Makefile", "YEW_TEST_ASAN_RUNTIME");
    bool consumed = file_has("tests/pkg/run.sh", "YEW_TEST_ASAN_RUNTIME");
    bool ordered = file_has("tests/pkg/run.sh", "${asan_runtime}:$shim");

    if (passed && consumed && ordered)
        return true;
    (void)snprintf(why, why_cap,
                   "GCC package fault shim is not ordered after libasan");
    return false;
}
