#ifndef YEW_TEST_FUZZLIB_H
#define YEW_TEST_FUZZLIB_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

typedef bool (*YewFuzzCheck)(const u8 *data, size_t len,
                             char *why, size_t why_cap);

/* Run a deterministic corpus/mutation campaign.  `corpus_dir` may be NULL;
 * the built-in seed corpus is always present. */
int yew_fuzz_main(int argc, char **argv, const char *target,
                  const char *corpus_dir, YewFuzzCheck check);

/* Shared invariants 2--4: total decode, byte-identical round trip, valid
 * scalar domain/canonical length, plus incremental/one-shot agreement. */
bool yew_fuzz_check_utf8(const u8 *data, size_t len,
                         char *why, size_t why_cap);

#endif
