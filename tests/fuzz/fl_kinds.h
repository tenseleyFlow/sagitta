#ifndef YEW_TEST_FUZZ_FL_KINDS_H
#define YEW_TEST_FUZZ_FL_KINDS_H

#include <string.h>

#include "util/base.h"

/*
 * Fletch spec §9.1, "The closed kind set": the 13 runtime error kinds,
 * closed for 1.0.  Amendment A1 (Sprint 30) added "limit"; A2 (Sprint 34)
 * added "handle" (§16).  ONE copy for every Fletch fuzzer's oracle: three
 * private copies drifted, and two of them still listed twelve after A2,
 * so fuzz_fl_vm reported a documented "handle" raise as a product bug.
 * tests/fletch/16-amendments.fl pins the count on the product side.
 */
static inline bool yew_fl_fuzz_spec_kind(const char *k)
{
    static const char *const kinds[] = {
        "type", "arity", "name", "index", "key", "div",
        "capability", "io", "import", "motion", "user", "limit",
        "handle"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(kinds); i++) {
        if (strcmp(k, kinds[i]) == 0)
            return true;
    }
    return false;
}

#endif
