#include "audit.h"

#define A(id, expect, fn) {id, expect, fn}

const YewAuditTest yew_audit_tests[] = {
    A("YEW-F-001", YEW_AUDIT_XFAIL, test_yew_f_001),
    A("YEW-F-002", YEW_AUDIT_XFAIL, test_yew_f_002),
    A("YEW-F-003", YEW_AUDIT_XFAIL, test_yew_f_003),
    A("YEW-F-004", YEW_AUDIT_XFAIL, test_yew_f_004)
};

const size_t yew_audit_tests_len =
    sizeof(yew_audit_tests) / sizeof(yew_audit_tests[0]);
