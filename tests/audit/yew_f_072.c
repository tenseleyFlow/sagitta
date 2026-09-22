/*
 * YEW-F-072 — designated performance evidence remains placeholder-only.
 *
 * Correct behavior: both designated performance lanes have committed,
 * non-template calibration references and baselines plus a passing 30-run
 * noise-floor report, as required by Sprint 58 F15 q3.
 *
 * Baseline failure: both calibration references and the arm64 baseline are
 * absent, and the x86_64 baseline retains an all-zero template vector.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool calibration_reference_valid(const char *path,
                                        const char *runner_id,
                                        const char *arch)
{
    char line[4096];
    char value[128];
    FILE *file = fopen(path, "rb");
    unsigned long c1 = 0UL;
    unsigned long c2 = 0UL;
    unsigned long c3 = 0UL;
    bool runner_valid = false;
    bool arch_valid = false;
    bool designated = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "runner_id %127s", value) == 1)
            runner_valid = strcmp(value, runner_id) == 0;
        else if (sscanf(line, "arch %127s", value) == 1)
            arch_valid = strcmp(value, arch) == 0;
        else if (strcmp(line, "designated 1\n") == 0)
            designated = true;
        else
            (void)sscanf(line, "c1_chase_ns %lu", &c1);
        if (sscanf(line, "c2_scalar_ns %lu", &c2) == 1)
            continue;
        (void)sscanf(line, "c3_bandwidth_ns %lu", &c3);
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return runner_valid && arch_valid && designated && c1 != 0UL &&
           c2 != 0UL && c3 != 0UL;
}

static bool baseline_calibrated(const char *path)
{
    char line[4096];
    FILE *file = fopen(path, "rb");
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

static bool evidence_valid(const char *path, const char *runner_id)
{
    char line[4096];
    char value[128];
    FILE *file = fopen(path, "rb");
    unsigned metrics = 0U;
    unsigned runs = 0U;
    unsigned failures = 1U;
    bool runner_valid = false;
    bool declared_runs = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned count;

        if (sscanf(line, "runner_id %127s", value) == 1)
            runner_valid = strcmp(value, runner_id) == 0;
        if (sscanf(line, "runs %u", &count) == 1)
            declared_runs = count == 30U;
        (void)sscanf(line,
                     "noise-floor: metrics=%u runs=%u failures=%u",
                     &metrics, &runs, &failures);
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return runner_valid && declared_runs && metrics != 0U && runs == 30U &&
           failures == 0U;
}

bool test_yew_f_072(char *why, size_t why_cap)
{
    bool x86_ref = calibration_reference_valid(
        "tests/perf/calib-reference.txt", "perf-x86_64-linux-gnu", "x86_64");
    bool arm_ref = calibration_reference_valid(
        "tests/perf/calib-reference-arm64.txt", "perf-arm64-linux", "aarch64");
    bool arm_base =
        baseline_calibrated("tests/perf/baselines/perf-arm64-linux.txt");
    bool x86_base = baseline_calibrated(
        "tests/perf/baselines/perf-x86_64-linux-gnu.txt");
    bool x86_evidence = evidence_valid(
        ".docs/audits/evidence/F072-perf-x86_64-linux-gnu.txt",
        "perf-x86_64-linux-gnu");
    bool arm_evidence = evidence_valid(
        ".docs/audits/evidence/F072-perf-arm64-linux.txt",
        "perf-arm64-linux");

    if (!x86_ref || !arm_ref || !arm_base || !x86_base || !x86_evidence ||
        !arm_evidence)
        (void)snprintf(why, why_cap,
                       "designated inputs x86_ref=%u x86_base=%u "
                       "x86_evidence=%u arm_ref=%u arm_base=%u "
                       "arm_evidence=%u",
                       x86_ref ? 1U : 0U, x86_base ? 1U : 0U,
                       x86_evidence ? 1U : 0U, arm_ref ? 1U : 0U,
                       arm_base ? 1U : 0U, arm_evidence ? 1U : 0U);
    return x86_ref && arm_ref && arm_base && x86_base && x86_evidence &&
           arm_evidence;
}
