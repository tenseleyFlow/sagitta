#define _POSIX_C_SOURCE 200809L

#include "util/calib.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ratio_permille(u64 measured, u64 reference, u64 *out)
{
    if (reference == 0U || measured > UINT64_MAX / 1000U)
        return false;
    *out = measured * 1000U / reference;
    return true;
}

u32 yew_calib_scale_permille(const CalibVec *measured,
                             const CalibVec *reference)
{
    u64 ratios[3];
    u64 sum = 0U;
    size_t i;

    if (measured == NULL || reference == NULL ||
        !ratio_permille(measured->c1, reference->c1, &ratios[0]) ||
        !ratio_permille(measured->c2, reference->c2, &ratios[1]) ||
        !ratio_permille(measured->c3, reference->c3, &ratios[2]))
        return 0U;
    for (i = 0U; i < YEW_ARRAY_LEN(ratios); i++) {
        if (ratios[i] > UINT64_MAX - sum)
            return 0U;
        sum += ratios[i];
    }
    sum /= YEW_ARRAY_LEN(ratios);
    return sum <= UINT32_MAX ? (u32)sum : 0U;
}

bool yew_calib_limit_ns(u64 budget_ns, u32 scale_permille, u64 *limit_ns)
{
    u64 whole;
    u64 fraction;

    if (scale_permille == 0U || limit_ns == NULL ||
        budget_ns / 1000U > UINT64_MAX / scale_permille)
        return false;
    whole = budget_ns / 1000U * scale_permille;
    fraction = budget_ns % 1000U * scale_permille / 1000U;
    if (fraction > UINT64_MAX - whole)
        return false;
    *limit_ns = whole + fraction;
    return true;
}

bool yew_calib_scale_is_gateable(u32 scale_permille)
{
    return scale_permille >= 500U && scale_permille <= 3000U;
}

bool yew_calib_drift_exceeds(u32 before_permille, u32 after_permille)
{
    u32 difference;

    if (before_permille == 0U)
        return true;
    difference = before_permille > after_permille
                     ? before_permille - after_permille
                     : after_permille - before_permille;
    return (u64)difference * 100U > (u64)before_permille * 15U;
}

static void set_error(char *error, size_t cap, const char *message)
{
    if (error != NULL && cap != 0U)
        (void)snprintf(error, cap, "%s", message);
}

static bool parse_u64(const char *text, u64 *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || *text == '\0' || *text == '-' || *text == '+')
        return false;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0')
        return false;
    *out = (u64)value;
    return (unsigned long long)*out == value;
}

static bool copy_field(char *dst, size_t cap, const char *value)
{
    size_t len = strlen(value);

    if (len == 0U || len >= cap)
        return false;
    (void)memcpy(dst, value, len + 1U);
    return true;
}

bool yew_calib_reference_read(const char *path, CalibReference *out,
                              char *error, size_t error_cap)
{
    enum {
        SEEN_HEADER = 1U << 0,
        SEEN_RUNNER = 1U << 1,
        SEEN_ARCH = 1U << 2,
        SEEN_DESIGNATED = 1U << 3,
        SEEN_C1 = 1U << 4,
        SEEN_C2 = 1U << 5,
        SEEN_C3 = 1U << 6,
        SEEN_ALL = (1U << 7) - 1U
    };
    CalibReference parsed;
    unsigned seen = 0U;
    char line[512];
    FILE *file;

    if (path == NULL || out == NULL) {
        set_error(error, error_cap, "invalid calibration reference request");
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        set_error(error, error_cap, "cannot open calibration reference");
        return false;
    }
    (void)memset(&parsed, 0, sizeof(parsed));
    while (fgets(line, sizeof(line), file) != NULL) {
        char key[64];
        char value[256];
        char extra;
        size_t len = strlen(line);
        unsigned bit = 0U;

        if (len != 0U && line[len - 1U] != '\n' && !feof(file)) {
            set_error(error, error_cap, "calibration reference line too long");
            (void)fclose(file);
            return false;
        }
        if (len != 0U && line[len - 1U] == '\n')
            line[--len] = '\0';
        if (len != 0U && line[len - 1U] == '\r')
            line[--len] = '\0';
        if (line[0] == '\0')
            continue;
        if (line[0] == '#') {
            if (strcmp(line, "# yew calibration reference v1") == 0) {
                if ((seen & SEEN_HEADER) != 0U) {
                    set_error(error, error_cap,
                              "duplicate calibration reference header");
                    (void)fclose(file);
                    return false;
                }
                seen |= SEEN_HEADER;
            }
            continue;
        }
        if (sscanf(line, "%63s %255s %c", key, value, &extra) != 2) {
            set_error(error, error_cap, "malformed calibration reference row");
            (void)fclose(file);
            return false;
        }
        if (strcmp(key, "runner_id") == 0) {
            bit = SEEN_RUNNER;
            if (!copy_field(parsed.runner_id, sizeof(parsed.runner_id), value))
                bit = 0U;
        } else if (strcmp(key, "arch") == 0) {
            bit = SEEN_ARCH;
            if (!copy_field(parsed.arch, sizeof(parsed.arch), value))
                bit = 0U;
        } else if (strcmp(key, "designated") == 0) {
            bit = SEEN_DESIGNATED;
            if (strcmp(value, "0") == 0)
                parsed.designated = false;
            else if (strcmp(value, "1") == 0)
                parsed.designated = true;
            else
                bit = 0U;
        } else if (strcmp(key, "c1_chase_ns") == 0) {
            bit = SEEN_C1;
            if (!parse_u64(value, &parsed.vec.c1) || parsed.vec.c1 == 0U)
                bit = 0U;
        } else if (strcmp(key, "c2_scalar_ns") == 0) {
            bit = SEEN_C2;
            if (!parse_u64(value, &parsed.vec.c2) || parsed.vec.c2 == 0U)
                bit = 0U;
        } else if (strcmp(key, "c3_bandwidth_ns") == 0) {
            bit = SEEN_C3;
            if (!parse_u64(value, &parsed.vec.c3) || parsed.vec.c3 == 0U)
                bit = 0U;
        }
        if (bit == 0U || (seen & bit) != 0U) {
            set_error(error, error_cap,
                      bit == 0U ? "invalid calibration reference row"
                                : "duplicate calibration reference row");
            (void)fclose(file);
            return false;
        }
        seen |= bit;
    }
    if (ferror(file) || fclose(file) != 0) {
        set_error(error, error_cap, "cannot read calibration reference");
        return false;
    }
    if (seen != SEEN_ALL) {
        set_error(error, error_cap, "incomplete calibration reference");
        return false;
    }
    *out = parsed;
    if (error != NULL && error_cap != 0U)
        error[0] = '\0';
    return true;
}
