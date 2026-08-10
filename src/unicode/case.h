#ifndef YEW_UNICODE_CASE_H
#define YEW_UNICODE_CASE_H

#include <stddef.h>

#include "util/base.h"

enum {
    YEW_CASE_MAX_CODEPOINTS = 3,
    YEW_CASE_MAX_UTF8 = YEW_CASE_MAX_CODEPOINTS * 4
};

typedef enum {
    YEW_CASE_LOWER = 0,
    YEW_CASE_UPPER,
    YEW_CASE_TOGGLE
} YewCaseKind;

/* Locale-neutral full default mappings. Identity mappings, including raw
 * invalid-byte escapes, return the input as a one-codepoint result. */
u8 yew_case_map(u32 cp, YewCaseKind kind,
                u32 out[YEW_CASE_MAX_CODEPOINTS]);
size_t yew_case_map_utf8(u32 cp, YewCaseKind kind,
                         u8 out[YEW_CASE_MAX_UTF8]);

#endif
