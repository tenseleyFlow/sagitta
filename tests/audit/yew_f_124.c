/*
 * YEW-F-124 — the macOS GCC package lane rebuilds the DYLD shim with GCC.
 *
 * Correct behavior: the product remains built by the requested compiler,
 * while the package runner receives the existing FAULTSHIM_CC host seam for
 * the Apple arm64/arm64e interposer.
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

bool test_yew_f_124(char *why, size_t why_cap)
{
    bool runner_seam = file_has(
        "tests/pkg/run.sh", "FAULTSHIM_CC:-${CC:-cc}");
    bool runner_uses = file_has(
        "tests/pkg/run.sh", "$faultshim_cc -std=c11");
    bool make_passes = file_has(
        "Makefile", "FAULTSHIM_CC='$(FAULTSHIM_CC)' tests/pkg/run.sh");

    if (runner_seam && runner_uses && make_passes)
        return true;
    (void)snprintf(why, why_cap,
                   "package fault shim still inherits the product compiler");
    return false;
}
