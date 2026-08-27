#ifndef YEW_UTIL_CALIB_H
#define YEW_UTIL_CALIB_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

typedef struct CalibVec {
    u64 c1;
    u64 c2;
    u64 c3;
} CalibVec;

typedef struct CalibReference {
    CalibVec vec;
    char runner_id[64];
    char arch[32];
    bool designated;
} CalibReference;

/* Returns zero when either vector cannot produce a checked u32 scale. */
u32 yew_calib_scale_permille(const CalibVec *measured,
                             const CalibVec *reference);
bool yew_calib_limit_ns(u64 budget_ns, u32 scale_permille, u64 *limit_ns);
bool yew_calib_scale_is_gateable(u32 scale_permille);
bool yew_calib_drift_exceeds(u32 before_permille, u32 after_permille);

/* Strict v1 parser: all metadata and all three nonzero components required. */
bool yew_calib_reference_read(const char *path, CalibReference *out,
                              char *error, size_t error_cap);

#endif
