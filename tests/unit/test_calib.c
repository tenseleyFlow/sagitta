#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/calib.h"

void test_calib_scale_reference_is_1000(void)
{
    const CalibVec reference = {184200000U, 41800000U, 96300000U};

    YEW_ASSERT_EQ_U64(yew_calib_scale_permille(&reference, &reference),
                      1000U);
}

void test_calib_scale_uniform_double_is_2000(void)
{
    const CalibVec reference = {3U, 5U, 7U};
    const CalibVec measured = {6U, 10U, 14U};

    YEW_ASSERT_EQ_U64(yew_calib_scale_permille(&measured, &reference),
                      2000U);
}

void test_calib_scale_uses_equal_integer_component_weights(void)
{
    static const struct {
        CalibVec measured;
        CalibVec reference;
        u32 expected;
    } cases[] = {
        {{1U, 1U, 1U}, {1U, 1U, 1U}, 1000U},
        {{1U, 2U, 3U}, {1U, 2U, 3U}, 1000U},
        {{2U, 2U, 2U}, {1U, 1U, 1U}, 2000U},
        {{1U, 2U, 3U}, {2U, 2U, 2U}, 1000U},
        {{3U, 2U, 1U}, {2U, 2U, 2U}, 1000U},
        {{1U, 1U, 4U}, {2U, 2U, 2U}, 1000U},
        {{4U, 1U, 1U}, {2U, 2U, 2U}, 1000U},
        {{1U, 4U, 1U}, {2U, 2U, 2U}, 1000U},
        {{5U, 7U, 11U}, {3U, 3U, 3U}, 2555U},
        {{10U, 10U, 10U}, {3U, 4U, 5U}, 2611U},
        {{999U, 1000U, 1001U}, {1000U, 1000U, 1000U}, 1000U},
        {{500U, 3000U, 1000U}, {1000U, 1000U, 1000U}, 1500U}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        YEW_ASSERT_EQ_U64(
            yew_calib_scale_permille(&cases[i].measured,
                                      &cases[i].reference),
            cases[i].expected);
    }
}

void test_calib_scale_rejects_zero_and_overflow(void)
{
    const CalibVec valid = {1U, 1U, 1U};
    const CalibVec zero = {1U, 0U, 1U};
    const CalibVec huge = {UINT64_MAX, 1U, 1U};

    YEW_ASSERT_EQ_U64(yew_calib_scale_permille(&valid, &zero), 0U);
    YEW_ASSERT_EQ_U64(yew_calib_scale_permille(&huge, &valid), 0U);
    YEW_ASSERT_EQ_U64(yew_calib_scale_permille(NULL, &valid), 0U);
    YEW_ASSERT_EQ_U64(yew_calib_scale_permille(&valid, NULL), 0U);
}

void test_calib_limit_scales_and_rejects_overflow(void)
{
    u64 limit = 0U;

    YEW_ASSERT(yew_calib_limit_ns(5000000U, 1042U, &limit));
    YEW_ASSERT_EQ_U64(limit, 5210000U);
    YEW_ASSERT(!yew_calib_limit_ns(UINT64_MAX, 3000U, &limit));
    YEW_ASSERT(!yew_calib_limit_ns(1U, 0U, &limit));
    YEW_ASSERT(!yew_calib_limit_ns(1U, 1000U, NULL));
}

void test_calib_refusal_band_includes_only_500_through_3000(void)
{
    YEW_ASSERT(!yew_calib_scale_is_gateable(499U));
    YEW_ASSERT(yew_calib_scale_is_gateable(500U));
    YEW_ASSERT(yew_calib_scale_is_gateable(3000U));
    YEW_ASSERT(!yew_calib_scale_is_gateable(3001U));
}

void test_calib_drift_aborts_only_above_15_percent(void)
{
    YEW_ASSERT(!yew_calib_drift_exceeds(1000U, 1140U));
    YEW_ASSERT(!yew_calib_drift_exceeds(1000U, 1150U));
    YEW_ASSERT(yew_calib_drift_exceeds(1000U, 1160U));
    YEW_ASSERT(!yew_calib_drift_exceeds(1000U, 850U));
    YEW_ASSERT(yew_calib_drift_exceeds(1000U, 849U));
    YEW_ASSERT(yew_calib_drift_exceeds(0U, 1000U));
}

void test_calib_reference_parser_requires_complete_metadata_and_vector(void)
{
    static const char valid[] =
        "# yew calibration reference v1\n"
        "runner_id perf-x86_64-linux-gnu\n"
        "arch x86_64\n"
        "designated 1\n"
        "c1_chase_ns 184200000\n"
        "c2_scalar_ns 41800000\n"
        "c3_bandwidth_ns 96300000\n";
    static const char incomplete[] =
        "# yew calibration reference v1\n"
        "runner_id perf-x86_64-linux-gnu\n"
        "arch x86_64\n"
        "designated 1\n"
        "c1_chase_ns 184200000\n"
        "c2_scalar_ns 41800000\n";
    char path[] = "/tmp/yew-calib-unit-XXXXXX";
    CalibReference reference;
    char error[160];
    int fd = mkstemp(path);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, valid, sizeof(valid) - 1U),
                      (i64)(sizeof(valid) - 1U));
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT(yew_calib_reference_read(path, &reference,
                                        error, sizeof(error)));
    YEW_ASSERT_EQ_STR(reference.runner_id, "perf-x86_64-linux-gnu");
    YEW_ASSERT_EQ_STR(reference.arch, "x86_64");
    YEW_ASSERT(reference.designated);
    YEW_ASSERT_EQ_U64(reference.vec.c1, 184200000U);
    YEW_ASSERT_EQ_U64(reference.vec.c2, 41800000U);
    YEW_ASSERT_EQ_U64(reference.vec.c3, 96300000U);

    fd = open(path, O_WRONLY | O_TRUNC);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, incomplete, sizeof(incomplete) - 1U),
                      (i64)(sizeof(incomplete) - 1U));
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT(!yew_calib_reference_read(path, &reference,
                                         error, sizeof(error)));
    YEW_ASSERT(strstr(error, "incomplete") != NULL);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
}
