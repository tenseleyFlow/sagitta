#ifndef SAG_UNICODE_CASE_H
#define SAG_UNICODE_CASE_H

#include <stddef.h>

#include "util/base.h"

enum {
    SAG_CASE_MAX_CODEPOINTS = 3,
    SAG_CASE_MAX_UTF8 = SAG_CASE_MAX_CODEPOINTS * 4
};

typedef enum {
    SAG_CASE_LOWER = 0,
    SAG_CASE_UPPER,
    SAG_CASE_TOGGLE
} SagCaseKind;

/* Locale-neutral full default mappings. Identity mappings, including raw
 * invalid-byte escapes, return the input as a one-codepoint result. */
u8 sag_case_map(u32 cp, SagCaseKind kind,
                u32 out[SAG_CASE_MAX_CODEPOINTS]);
size_t sag_case_map_utf8(u32 cp, SagCaseKind kind,
                         u8 out[SAG_CASE_MAX_UTF8]);

#endif
