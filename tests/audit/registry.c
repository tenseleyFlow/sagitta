#include "audit.h"

#define A(id, expect, fn) {id, expect, fn}

const YewAuditTest yew_audit_tests[] = {
    A("YEW-F-001", YEW_AUDIT_XFAIL, test_yew_f_001)
};

const size_t yew_audit_tests_len =
    sizeof(yew_audit_tests) / sizeof(yew_audit_tests[0]);
