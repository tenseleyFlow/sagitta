/*
 * YEW-F-072 — designated performance evidence remains placeholder-only.
 *
 * Correct behavior: both designated performance lanes have committed,
 * non-template calibration references and baselines, permitting the 30-run
 * noise-floor recomputation required by Sprint 58 F15 q3.
 *
 * Baseline failure: both calibration references and the arm64 baseline are
 * absent, and the x86_64 baseline retains an all-zero template vector.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return false;
    return fclose(file) == 0;
}

static bool x86_baseline_calibrated(void)
{
    char line[4096];
    FILE *file = fopen("tests/perf/baselines/perf-x86_64-linux-gnu.txt",
                       "rb");
    bool valid = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned scale;
        unsigned c1;
        unsigned c2;
        unsigned c3;

        if (sscanf(line, "# calib scale_permille=%u c1=%u c2=%u c3=%u",
                   &scale, &c1, &c2, &c3) == 4) {
            valid = scale != 0U && c1 != 0U && c2 != 0U && c3 != 0U;
            break;
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return valid;
}

bool test_yew_f_072(char *why, size_t why_cap)
{
    bool x86_ref = file_exists("tests/perf/calib-reference.txt");
    bool arm_ref = file_exists("tests/perf/calib-reference-arm64.txt");
    bool arm_base =
        file_exists("tests/perf/baselines/perf-arm64-linux.txt");
    bool x86_base = x86_baseline_calibrated();

    if (!x86_ref || !arm_ref || !arm_base || !x86_base)
        (void)snprintf(why, why_cap,
                       "designated inputs x86_ref=%u x86_base=%u "
                       "arm_ref=%u arm_base=%u",
                       x86_ref ? 1U : 0U, x86_base ? 1U : 0U,
                       arm_ref ? 1U : 0U, arm_base ? 1U : 0U);
    return x86_ref && arm_ref && arm_base && x86_base;
}
