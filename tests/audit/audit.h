#ifndef YEW_TEST_AUDIT_H
#define YEW_TEST_AUDIT_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*YewAuditFn)(char *why, size_t why_cap);

typedef enum YewAuditExpect {
    YEW_AUDIT_PASS = 0,
    YEW_AUDIT_XFAIL
} YewAuditExpect;

typedef struct YewAuditTest {
    const char *id;
    YewAuditExpect expect;
    YewAuditFn fn;
} YewAuditTest;

extern const YewAuditTest yew_audit_tests[];
extern const size_t yew_audit_tests_len;

bool test_yew_f_001(char *why, size_t why_cap);

#endif
