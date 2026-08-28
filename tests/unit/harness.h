#ifndef YEW_TEST_HARNESS_H
#define YEW_TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "util/base.h"
#include "util/log.h"

typedef struct {
    const char *name;
    void (*fn)(void);
} YewTest;

typedef struct Ed Ed;

extern const YewTest yew_tests[];
extern const size_t yew_tests_len;

void yew_test_count_assertion(void);
_Noreturn void yew_test_fail(const char *file, int line, const char *detail);
_Noreturn void yew_test_fail_i64(const char *file, int line,
                                 i64 left, i64 right);
_Noreturn void yew_test_fail_u64(const char *file, int line,
                                 u64 left, u64 right);
_Noreturn void yew_test_fail_str(const char *file, int line,
                                 const char *left, const char *right);
_Noreturn void yew_test_fail_mem(const char *file, int line,
                                 size_t offset, u8 left, u8 right);
_Noreturn void yew_test_fail_pointer(const char *file, int line,
                                     const char *macro, bool is_null);

void yew_test_capture_log(void);
size_t yew_test_log_count(void);
bool yew_test_log_contains(YewLogLevel level, const char *substr);
void yew_test_teardown(void);

bool yew_test_name_matches(const char *name, const char *filter);
const char *yew_test_program_path(void);
void yew_test_load_runtime(Ed *ed);
bool yew_test_canonicalize_path(char *path, size_t cap);

#define YEW_ASSERT(cond)                                                      \
    do {                                                                      \
        bool yew_assert_value_ = (cond);                                      \
        yew_test_count_assertion();                                           \
        if (!yew_assert_value_)                                               \
            yew_test_fail(__FILE__, __LINE__, "YEW_ASSERT " #cond);           \
    } while (0)

#define YEW_ASSERT_EQ_I64(a, b)                                               \
    do {                                                                      \
        i64 yew_assert_left_ = (i64)(a);                                      \
        i64 yew_assert_right_ = (i64)(b);                                    \
        yew_test_count_assertion();                                           \
        if (yew_assert_left_ != yew_assert_right_)                            \
            yew_test_fail_i64(__FILE__, __LINE__, yew_assert_left_,           \
                               yew_assert_right_);                            \
    } while (0)

#define YEW_ASSERT_EQ_U64(a, b)                                               \
    do {                                                                      \
        u64 yew_assert_left_ = (u64)(a);                                      \
        u64 yew_assert_right_ = (u64)(b);                                    \
        yew_test_count_assertion();                                           \
        if (yew_assert_left_ != yew_assert_right_)                            \
            yew_test_fail_u64(__FILE__, __LINE__, yew_assert_left_,           \
                               yew_assert_right_);                            \
    } while (0)

#define YEW_ASSERT_EQ_STR(a, b)                                               \
    do {                                                                      \
        const char *yew_assert_left_ = (a);                                   \
        const char *yew_assert_right_ = (b);                                  \
        bool yew_assert_equal_ =                                              \
            yew_assert_left_ == yew_assert_right_ ||                          \
            (yew_assert_left_ != NULL && yew_assert_right_ != NULL &&         \
             strcmp(yew_assert_left_, yew_assert_right_) == 0);               \
        yew_test_count_assertion();                                           \
        if (!yew_assert_equal_)                                               \
            yew_test_fail_str(__FILE__, __LINE__, yew_assert_left_,           \
                               yew_assert_right_);                            \
    } while (0)

#define YEW_ASSERT_EQ_MEM(a, b, n)                                            \
    do {                                                                      \
        const void *yew_assert_left_ = (a);                                   \
        const void *yew_assert_right_ = (b);                                  \
        size_t yew_assert_size_ = (size_t)(n);                                \
        const u8 *yew_assert_left_bytes_ = yew_assert_left_;                  \
        const u8 *yew_assert_right_bytes_ = yew_assert_right_;                \
        size_t yew_assert_offset_;                                            \
        yew_test_count_assertion();                                           \
        for (yew_assert_offset_ = 0; yew_assert_offset_ < yew_assert_size_;   \
             yew_assert_offset_++) {                                          \
            if (yew_assert_left_bytes_[yew_assert_offset_] !=                 \
                yew_assert_right_bytes_[yew_assert_offset_])                  \
                yew_test_fail_mem(                                            \
                    __FILE__, __LINE__, yew_assert_offset_,                   \
                    yew_assert_left_bytes_[yew_assert_offset_],               \
                    yew_assert_right_bytes_[yew_assert_offset_]);             \
        }                                                                     \
    } while (0)

#define YEW_ASSERT_NULL(p)                                                    \
    do {                                                                      \
        const void *yew_assert_pointer_ = (p);                                \
        yew_test_count_assertion();                                           \
        if (yew_assert_pointer_ != NULL)                                      \
            yew_test_fail_pointer(__FILE__, __LINE__, "YEW_ASSERT_NULL",     \
                                  false);                                     \
    } while (0)

#define YEW_ASSERT_NOT_NULL(p)                                                \
    do {                                                                      \
        const void *yew_assert_pointer_ = (p);                                \
        yew_test_count_assertion();                                           \
        if (yew_assert_pointer_ == NULL)                                      \
            yew_test_fail_pointer(__FILE__, __LINE__,                         \
                                  "YEW_ASSERT_NOT_NULL", true);              \
    } while (0)

#endif
