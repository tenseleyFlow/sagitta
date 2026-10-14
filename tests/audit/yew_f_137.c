/*
 * YEW-F-137 — documentation targets ignore the selected BUILD tree.
 * Correct behavior: every checker and its negative selftest receives the
 * selected binaries/helpers, and manual shell examples resolve that binary.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static size_t file_count(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char line[2048];
    size_t count = 0U;

    if (file == NULL)
        return 0U;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL)
            count++;
    }
    (void)fclose(file);
    return count;
}

bool test_yew_f_137(char *why, size_t why_cap)
{
    bool make_targets =
        file_count("Makefile",
                   "YEW_MAN_BIN=$(abspath $(BUILD)/yew)") >= 2U &&
        file_count("Makefile",
                   "YEW_MANUAL_BIN=$(abspath $(BUILD)/yew)") >= 2U &&
        file_count("Makefile", "YEW_MANUAL_PTY_RUNNER=") >= 2U &&
        file_count("Makefile", "YEW_MANUAL_DEMO_BIN=") >= 2U &&
        file_count("Makefile",
                   "YEW_DOCS_BIN=$(abspath $(BUILD)/yew)") >= 2U &&
        file_count("Makefile", "YEW_DOCS_OPTION_DUMP=") >= 2U;
    bool selftests =
        file_count("scripts/tests/check-man.test.sh",
                   "yew_bin=${YEW_MAN_BIN:-$root/build/yew}") == 1U &&
        file_count("scripts/tests/check-manual.test.sh",
                   "yew_bin=${YEW_MANUAL_BIN:-$root/build/yew}") == 1U &&
        file_count("scripts/tests/check-manual.test.sh",
                   "pty_runner=${YEW_MANUAL_PTY_RUNNER:-") == 1U &&
        file_count("scripts/tests/check-manual.test.sh",
                   "demo_bin=${YEW_MANUAL_DEMO_BIN:-") == 1U &&
        file_count("scripts/tests/check-docs-sync.test.sh",
                   "yew_bin=${YEW_DOCS_BIN:-$root/build/yew}") == 1U &&
        file_count("scripts/tests/check-docs-sync.test.sh",
                   "option_dump=${YEW_DOCS_OPTION_DUMP:-") == 1U;
    bool manual_path =
        file_count("scripts/check-manual.sh",
                   "yew_dir=$(dirname -- \"$yew_bin\")") == 1U &&
        file_count("scripts/check-manual.sh", "PATH=\"$yew_dir:") == 2U;

    if (make_targets && selftests && manual_path)
        return true;
    (void)snprintf(why, why_cap,
                   "documentation targets ignore selected BUILD binaries");
    return false;
}
