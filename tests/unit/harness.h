#ifndef SAG_TEST_HARNESS_H
#define SAG_TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "util/base.h"
#include "util/log.h"

typedef struct {
    const char *name;
    void (*fn)(void);
} SagTest;

extern const SagTest sag_tests[];
extern const size_t sag_tests_len;

void sag_test_count_assertion(void);
_Noreturn void sag_test_fail(const char *file, int line, const char *detail);
_Noreturn void sag_test_fail_i64(const char *file, int line,
                                 i64 left, i64 right);
_Noreturn void sag_test_fail_u64(const char *file, int line,
                                 u64 left, u64 right);
_Noreturn void sag_test_fail_str(const char *file, int line,
                                 const char *left, const char *right);
_Noreturn void sag_test_fail_mem(const char *file, int line,
                                 size_t offset, u8 left, u8 right);
_Noreturn void sag_test_fail_pointer(const char *file, int line,
                                     const char *macro, bool is_null);

void sag_test_capture_log(void);
size_t sag_test_log_count(void);
bool sag_test_log_contains(SagLogLevel level, const char *substr);
void sag_test_teardown(void);

bool sag_test_name_matches(const char *name, const char *filter);
const char *sag_test_program_path(void);

#define SAG_ASSERT(cond)                                                      \
    do {                                                                      \
        bool sag_assert_value_ = (cond);                                      \
        sag_test_count_assertion();                                           \
        if (!sag_assert_value_)                                               \
            sag_test_fail(__FILE__, __LINE__, "SAG_ASSERT " #cond);           \
    } while (0)

#define SAG_ASSERT_EQ_I64(a, b)                                               \
    do {                                                                      \
        i64 sag_assert_left_ = (i64)(a);                                      \
        i64 sag_assert_right_ = (i64)(b);                                    \
        sag_test_count_assertion();                                           \
        if (sag_assert_left_ != sag_assert_right_)                            \
            sag_test_fail_i64(__FILE__, __LINE__, sag_assert_left_,           \
                               sag_assert_right_);                            \
    } while (0)

#define SAG_ASSERT_EQ_U64(a, b)                                               \
    do {                                                                      \
        u64 sag_assert_left_ = (u64)(a);                                      \
        u64 sag_assert_right_ = (u64)(b);                                    \
        sag_test_count_assertion();                                           \
        if (sag_assert_left_ != sag_assert_right_)                            \
            sag_test_fail_u64(__FILE__, __LINE__, sag_assert_left_,           \
                               sag_assert_right_);                            \
    } while (0)

#define SAG_ASSERT_EQ_STR(a, b)                                               \
    do {                                                                      \
        const char *sag_assert_left_ = (a);                                   \
        const char *sag_assert_right_ = (b);                                  \
        bool sag_assert_equal_ =                                              \
            sag_assert_left_ == sag_assert_right_ ||                          \
            (sag_assert_left_ != NULL && sag_assert_right_ != NULL &&         \
             strcmp(sag_assert_left_, sag_assert_right_) == 0);               \
        sag_test_count_assertion();                                           \
        if (!sag_assert_equal_)                                               \
            sag_test_fail_str(__FILE__, __LINE__, sag_assert_left_,           \
                               sag_assert_right_);                            \
    } while (0)

#define SAG_ASSERT_EQ_MEM(a, b, n)                                            \
    do {                                                                      \
        const void *sag_assert_left_ = (a);                                   \
        const void *sag_assert_right_ = (b);                                  \
        size_t sag_assert_size_ = (size_t)(n);                                \
        const u8 *sag_assert_left_bytes_ = sag_assert_left_;                  \
        const u8 *sag_assert_right_bytes_ = sag_assert_right_;                \
        size_t sag_assert_offset_;                                            \
        sag_test_count_assertion();                                           \
        for (sag_assert_offset_ = 0; sag_assert_offset_ < sag_assert_size_;   \
             sag_assert_offset_++) {                                          \
            if (sag_assert_left_bytes_[sag_assert_offset_] !=                 \
                sag_assert_right_bytes_[sag_assert_offset_])                  \
                sag_test_fail_mem(                                            \
                    __FILE__, __LINE__, sag_assert_offset_,                   \
                    sag_assert_left_bytes_[sag_assert_offset_],               \
                    sag_assert_right_bytes_[sag_assert_offset_]);             \
        }                                                                     \
    } while (0)

#define SAG_ASSERT_NULL(p)                                                    \
    do {                                                                      \
        const void *sag_assert_pointer_ = (p);                                \
        sag_test_count_assertion();                                           \
        if (sag_assert_pointer_ != NULL)                                      \
            sag_test_fail_pointer(__FILE__, __LINE__, "SAG_ASSERT_NULL",     \
                                  false);                                     \
    } while (0)

#define SAG_ASSERT_NOT_NULL(p)                                                \
    do {                                                                      \
        const void *sag_assert_pointer_ = (p);                                \
        sag_test_count_assertion();                                           \
        if (sag_assert_pointer_ == NULL)                                      \
            sag_test_fail_pointer(__FILE__, __LINE__,                         \
                                  "SAG_ASSERT_NOT_NULL", true);              \
    } while (0)

#endif
