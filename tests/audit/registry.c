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
    A("YEW-F-006", YEW_AUDIT_PASS, test_yew_f_006),
    A("YEW-F-007", YEW_AUDIT_PASS, test_yew_f_007),
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
    , A("YEW-F-022", YEW_AUDIT_XFAIL, test_yew_f_022)
    , A("YEW-F-023", YEW_AUDIT_XFAIL, test_yew_f_023)
#endif
    , A("YEW-F-024", YEW_AUDIT_XFAIL, test_yew_f_024)
    , A("YEW-F-025", YEW_AUDIT_XFAIL, test_yew_f_025)
    , A("YEW-F-026", YEW_AUDIT_XFAIL, test_yew_f_026)
    , A("YEW-F-027", YEW_AUDIT_XFAIL, test_yew_f_027)
    , A("YEW-F-028", YEW_AUDIT_XFAIL, test_yew_f_028)
    , A("YEW-F-029", YEW_AUDIT_XFAIL, test_yew_f_029)
    , A("YEW-F-030", YEW_AUDIT_XFAIL, test_yew_f_030)
    , A("YEW-F-031", YEW_AUDIT_XFAIL, test_yew_f_031)
    , A("YEW-F-032", YEW_AUDIT_XFAIL, test_yew_f_032)
    , A("YEW-F-033", YEW_AUDIT_XFAIL, test_yew_f_033)
    , A("YEW-F-034", YEW_AUDIT_XFAIL, test_yew_f_034)
    , A("YEW-F-035", YEW_AUDIT_XFAIL, test_yew_f_035)
    , A("YEW-F-036", YEW_AUDIT_XFAIL, test_yew_f_036)
    , A("YEW-F-037", YEW_AUDIT_XFAIL, test_yew_f_037)
    , A("YEW-F-038", YEW_AUDIT_XFAIL, test_yew_f_038)
    , A("YEW-F-039", YEW_AUDIT_XFAIL, test_yew_f_039)
    , A("YEW-F-040", YEW_AUDIT_XFAIL, test_yew_f_040)
    , A("YEW-F-041", YEW_AUDIT_XFAIL, test_yew_f_041)
    , A("YEW-F-042", YEW_AUDIT_XFAIL, test_yew_f_042)
    , A("YEW-F-043", YEW_AUDIT_XFAIL, test_yew_f_043)
    , A("YEW-F-044", YEW_AUDIT_XFAIL, test_yew_f_044)
    , A("YEW-F-045", YEW_AUDIT_XFAIL, test_yew_f_045)
    , A("YEW-F-046", YEW_AUDIT_XFAIL, test_yew_f_046)
    , A("YEW-F-047", YEW_AUDIT_XFAIL, test_yew_f_047)
    , A("YEW-F-048", YEW_AUDIT_XFAIL, test_yew_f_048)
    , A("YEW-F-049", YEW_AUDIT_XFAIL, test_yew_f_049)
    , A("YEW-F-050", YEW_AUDIT_XFAIL, test_yew_f_050)
    , A("YEW-F-051", YEW_AUDIT_XFAIL, test_yew_f_051)
    , A("YEW-F-052", YEW_AUDIT_XFAIL, test_yew_f_052)
    , A("YEW-F-053", YEW_AUDIT_XFAIL, test_yew_f_053)
    , A("YEW-F-054", YEW_AUDIT_XFAIL, test_yew_f_054)
    , A("YEW-F-055", YEW_AUDIT_XFAIL, test_yew_f_055)
    , A("YEW-F-056", YEW_AUDIT_XFAIL, test_yew_f_056)
    , A("YEW-F-057", YEW_AUDIT_XFAIL, test_yew_f_057)
    , A("YEW-F-058", YEW_AUDIT_XFAIL, test_yew_f_058)
    , A("YEW-F-059", YEW_AUDIT_XFAIL, test_yew_f_059)
    , A("YEW-F-060", YEW_AUDIT_XFAIL, test_yew_f_060)
    , A("YEW-F-061", YEW_AUDIT_XFAIL, test_yew_f_061)
    , A("YEW-F-062", YEW_AUDIT_XFAIL, test_yew_f_062)
    , A("YEW-F-063", YEW_AUDIT_XFAIL, test_yew_f_063)
    , A("YEW-F-064", YEW_AUDIT_XFAIL, test_yew_f_064)
    , A("YEW-F-065", YEW_AUDIT_XFAIL, test_yew_f_065)
    , A("YEW-F-066", YEW_AUDIT_XFAIL, test_yew_f_066)
    , A("YEW-F-067", YEW_AUDIT_XFAIL, test_yew_f_067)
    , A("YEW-F-068", YEW_AUDIT_XFAIL, test_yew_f_068)
    , A("YEW-F-069", YEW_AUDIT_XFAIL, test_yew_f_069)
    , A("YEW-F-070", YEW_AUDIT_XFAIL, test_yew_f_070)
    , A("YEW-F-071", YEW_AUDIT_XFAIL, test_yew_f_071)
    , A("YEW-F-072", YEW_AUDIT_XFAIL, test_yew_f_072)
    , A("YEW-F-073", YEW_AUDIT_XFAIL, test_yew_f_073)
    , A("YEW-F-074", YEW_AUDIT_XFAIL, test_yew_f_074)
    , A("YEW-F-075", YEW_AUDIT_PASS, test_yew_f_075)
    , A("YEW-F-076", YEW_AUDIT_XFAIL, test_yew_f_076)
    , A("YEW-F-077", YEW_AUDIT_XFAIL, test_yew_f_077)
    , A("YEW-F-078", YEW_AUDIT_XFAIL, test_yew_f_078)
    , A("YEW-F-079", YEW_AUDIT_PASS, test_yew_f_079)
};

const size_t yew_audit_tests_len =
    sizeof(yew_audit_tests) / sizeof(yew_audit_tests[0]);
