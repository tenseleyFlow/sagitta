#include "audit.h"

#ifndef YEW_WITH_PLUGINS
#define YEW_WITH_PLUGINS 0
#endif

#define A(id, expect, fn) {id, expect, fn}

const YewAuditTest yew_audit_tests[] = {
    A("YEW-F-001", YEW_AUDIT_XFAIL, test_yew_f_001),
    A("YEW-F-002", YEW_AUDIT_XFAIL, test_yew_f_002),
    A("YEW-F-003", YEW_AUDIT_XFAIL, test_yew_f_003),
    A("YEW-F-004", YEW_AUDIT_XFAIL, test_yew_f_004),
    A("YEW-F-005", YEW_AUDIT_XFAIL, test_yew_f_005),
    A("YEW-F-006", YEW_AUDIT_XFAIL, test_yew_f_006),
    A("YEW-F-007", YEW_AUDIT_XFAIL, test_yew_f_007),
#if YEW_WITH_PLUGINS
    A("YEW-F-008", YEW_AUDIT_XFAIL, test_yew_f_008),
#endif
    A("YEW-F-009", YEW_AUDIT_XFAIL, test_yew_f_009),
    A("YEW-F-010", YEW_AUDIT_XFAIL, test_yew_f_010),
    A("YEW-F-011", YEW_AUDIT_XFAIL, test_yew_f_011),
    A("YEW-F-012", YEW_AUDIT_XFAIL, test_yew_f_012),
    A("YEW-F-013", YEW_AUDIT_XFAIL, test_yew_f_013),
    A("YEW-F-014", YEW_AUDIT_XFAIL, test_yew_f_014),
    A("YEW-F-015", YEW_AUDIT_XFAIL, test_yew_f_015),
    A("YEW-F-016", YEW_AUDIT_XFAIL, test_yew_f_016),
    A("YEW-F-017", YEW_AUDIT_XFAIL, test_yew_f_017),
    A("YEW-F-018", YEW_AUDIT_XFAIL, test_yew_f_018),
    A("YEW-F-019", YEW_AUDIT_XFAIL, test_yew_f_019),
    A("YEW-F-020", YEW_AUDIT_XFAIL, test_yew_f_020)
#if YEW_WITH_PLUGINS
    , A("YEW-F-021", YEW_AUDIT_XFAIL, test_yew_f_021)
#endif
};

const size_t yew_audit_tests_len =
    sizeof(yew_audit_tests) / sizeof(yew_audit_tests[0]);
