#ifndef YEW_TEST_FUZZ_COV_H
#define YEW_TEST_FUZZ_COV_H

#include <stdio.h>

#include "util/base.h"

/* Sprint 58 audit instrumentation.  This header is consumed only by the
 * build-cov tree; ordinary and shipping objects never link these callbacks. */
#define YEW_COV_BITS 22U
#define YEW_COV_SIZE (1U << YEW_COV_BITS)

extern u8 yew_cov_map[YEW_COV_SIZE];

void __sanitizer_cov_trace_pc_guard_init(u32 *start, u32 *stop);
void __sanitizer_cov_trace_pc_guard(u32 *guard);

void yew_cov_reset(void);
u32 yew_cov_new_edges(void);
void yew_cov_merge(void);
void yew_cov_report(FILE *out);
u64 yew_cov_hash(void);
u32 yew_cov_total_edges(void);

/* Kept out of product headers: the standalone cov self-test needs to replay
 * deterministic guard-init order without starting a second process. */
void yew_cov_test_clear_all(void);

#endif
