/*
 * YEW-F-088 — native macOS GCC cannot build the unit-test prerequisite graph.
 *
 * Correct behavior: GCC builds product and test objects while an Apple
 * compiler builds the dual arm64/arm64e DYLD interposer required by macOS.
 *
 * Baseline failure: the fault-shim recipe sends both -arch flags to gcc-16,
 * which rejects arm64e and stops the unit_tests target before it can link.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

bool test_yew_f_088(char *why, size_t why_cap)
{
    FILE *file = fopen("Makefile", "rb");
    char line[1024];
    bool declares_tool = false;
    bool uses_tool = false;

    if (file == NULL) {
        (void)snprintf(why, why_cap, "cannot read Makefile");
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "FAULTSHIM_CC ?= clang") != NULL)
            declares_tool = true;
        if (strstr(line, "$(FAULTSHIM_CC) $(CFLAGS)") != NULL)
            uses_tool = true;
    }
    (void)fclose(file);
    if (declares_tool && uses_tool)
        return true;
    (void)snprintf(why, why_cap,
                   "Darwin fault shim still inherits unsupported product CC");
    return false;
}
