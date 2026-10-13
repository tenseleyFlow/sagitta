#include "audit.h"

#ifndef YEW_WITH_PLUGINS
#define YEW_WITH_PLUGINS 0
#endif

#define A(id, expect, fn) {id, expect, fn}

const YewAuditTest yew_audit_tests[] = {
    A("YEW-F-001", YEW_AUDIT_PASS, test_yew_f_001),
    A("YEW-F-002", YEW_AUDIT_PASS, test_yew_f_002),
    A("YEW-F-003", YEW_AUDIT_PASS, test_yew_f_003),
    A("YEW-F-004", YEW_AUDIT_PASS, test_yew_f_004),
    A("YEW-F-005", YEW_AUDIT_PASS, test_yew_f_005),
    A("YEW-F-006", YEW_AUDIT_PASS, test_yew_f_006),
    A("YEW-F-007", YEW_AUDIT_PASS, test_yew_f_007),
#if YEW_WITH_PLUGINS
    A("YEW-F-008", YEW_AUDIT_PASS, test_yew_f_008),
#endif
    A("YEW-F-009", YEW_AUDIT_PASS, test_yew_f_009),
    A("YEW-F-010", YEW_AUDIT_PASS, test_yew_f_010),
    A("YEW-F-011", YEW_AUDIT_PASS, test_yew_f_011),
    A("YEW-F-012", YEW_AUDIT_PASS, test_yew_f_012),
    A("YEW-F-013", YEW_AUDIT_PASS, test_yew_f_013),
    A("YEW-F-014", YEW_AUDIT_PASS, test_yew_f_014),
    A("YEW-F-015", YEW_AUDIT_PASS, test_yew_f_015),
    A("YEW-F-016", YEW_AUDIT_PASS, test_yew_f_016),
    A("YEW-F-017", YEW_AUDIT_PASS, test_yew_f_017),
    A("YEW-F-018", YEW_AUDIT_PASS, test_yew_f_018),
    A("YEW-F-019", YEW_AUDIT_PASS, test_yew_f_019),
    A("YEW-F-020", YEW_AUDIT_PASS, test_yew_f_020)
#if YEW_WITH_PLUGINS
    , A("YEW-F-021", YEW_AUDIT_PASS, test_yew_f_021)
    , A("YEW-F-022", YEW_AUDIT_PASS, test_yew_f_022)
    , A("YEW-F-023", YEW_AUDIT_PASS, test_yew_f_023)
#endif
    , A("YEW-F-024", YEW_AUDIT_PASS, test_yew_f_024)
    , A("YEW-F-025", YEW_AUDIT_PASS, test_yew_f_025)
    , A("YEW-F-026", YEW_AUDIT_PASS, test_yew_f_026)
    , A("YEW-F-027", YEW_AUDIT_PASS, test_yew_f_027)
    , A("YEW-F-028", YEW_AUDIT_PASS, test_yew_f_028)
    , A("YEW-F-029", YEW_AUDIT_PASS, test_yew_f_029)
    , A("YEW-F-030", YEW_AUDIT_PASS, test_yew_f_030)
    , A("YEW-F-031", YEW_AUDIT_PASS, test_yew_f_031)
    , A("YEW-F-032", YEW_AUDIT_PASS, test_yew_f_032)
    , A("YEW-F-033", YEW_AUDIT_PASS, test_yew_f_033)
    , A("YEW-F-034", YEW_AUDIT_PASS, test_yew_f_034)
    , A("YEW-F-035", YEW_AUDIT_PASS, test_yew_f_035)
    , A("YEW-F-036", YEW_AUDIT_PASS, test_yew_f_036)
    , A("YEW-F-037", YEW_AUDIT_PASS, test_yew_f_037)
    , A("YEW-F-038", YEW_AUDIT_PASS, test_yew_f_038)
    , A("YEW-F-039", YEW_AUDIT_PASS, test_yew_f_039)
    , A("YEW-F-040", YEW_AUDIT_PASS, test_yew_f_040)
    , A("YEW-F-041", YEW_AUDIT_PASS, test_yew_f_041)
    , A("YEW-F-042", YEW_AUDIT_PASS, test_yew_f_042)
    , A("YEW-F-043", YEW_AUDIT_PASS, test_yew_f_043)
    , A("YEW-F-044", YEW_AUDIT_PASS, test_yew_f_044)
    , A("YEW-F-045", YEW_AUDIT_PASS, test_yew_f_045)
    , A("YEW-F-046", YEW_AUDIT_PASS, test_yew_f_046)
    , A("YEW-F-047", YEW_AUDIT_PASS, test_yew_f_047)
    , A("YEW-F-048", YEW_AUDIT_PASS, test_yew_f_048)
    , A("YEW-F-049", YEW_AUDIT_PASS, test_yew_f_049)
    , A("YEW-F-050", YEW_AUDIT_PASS, test_yew_f_050)
    , A("YEW-F-051", YEW_AUDIT_PASS, test_yew_f_051)
    , A("YEW-F-052", YEW_AUDIT_PASS, test_yew_f_052)
    , A("YEW-F-053", YEW_AUDIT_PASS, test_yew_f_053)
    , A("YEW-F-054", YEW_AUDIT_PASS, test_yew_f_054)
    , A("YEW-F-055", YEW_AUDIT_PASS, test_yew_f_055)
    , A("YEW-F-056", YEW_AUDIT_PASS, test_yew_f_056)
    , A("YEW-F-057", YEW_AUDIT_PASS, test_yew_f_057)
    , A("YEW-F-058", YEW_AUDIT_PASS, test_yew_f_058)
    , A("YEW-F-059", YEW_AUDIT_PASS, test_yew_f_059)
    , A("YEW-F-060", YEW_AUDIT_PASS, test_yew_f_060)
    , A("YEW-F-061", YEW_AUDIT_PASS, test_yew_f_061)
    , A("YEW-F-062", YEW_AUDIT_PASS, test_yew_f_062)
    , A("YEW-F-063", YEW_AUDIT_PASS, test_yew_f_063)
    , A("YEW-F-064", YEW_AUDIT_PASS, test_yew_f_064)
    , A("YEW-F-065", YEW_AUDIT_PASS, test_yew_f_065)
    , A("YEW-F-066", YEW_AUDIT_PASS, test_yew_f_066)
    , A("YEW-F-067", YEW_AUDIT_PASS, test_yew_f_067)
    , A("YEW-F-068", YEW_AUDIT_PASS, test_yew_f_068)
    , A("YEW-F-069", YEW_AUDIT_PASS, test_yew_f_069)
    , A("YEW-F-070", YEW_AUDIT_PASS, test_yew_f_070)
    , A("YEW-F-071", YEW_AUDIT_PASS, test_yew_f_071)
    , A("YEW-F-072", YEW_AUDIT_PASS, test_yew_f_072)
    , A("YEW-F-073", YEW_AUDIT_PASS, test_yew_f_073)
    , A("YEW-F-074", YEW_AUDIT_PASS, test_yew_f_074)
    , A("YEW-F-075", YEW_AUDIT_PASS, test_yew_f_075)
    , A("YEW-F-076", YEW_AUDIT_PASS, test_yew_f_076)
    , A("YEW-F-077", YEW_AUDIT_PASS, test_yew_f_077)
    , A("YEW-F-078", YEW_AUDIT_PASS, test_yew_f_078)
    , A("YEW-F-079", YEW_AUDIT_PASS, test_yew_f_079)
    , A("YEW-F-080", YEW_AUDIT_PASS, test_yew_f_080)
    , A("YEW-F-081", YEW_AUDIT_PASS, test_yew_f_081)
    , A("YEW-F-082", YEW_AUDIT_PASS, test_yew_f_082)
    , A("YEW-F-083", YEW_AUDIT_PASS, test_yew_f_083)
    , A("YEW-F-084", YEW_AUDIT_PASS, test_yew_f_084)
    , A("YEW-F-085", YEW_AUDIT_PASS, test_yew_f_085)
    , A("YEW-F-086", YEW_AUDIT_PASS, test_yew_f_086)
    , A("YEW-F-087", YEW_AUDIT_PASS, test_yew_f_087)
    , A("YEW-F-088", YEW_AUDIT_PASS, test_yew_f_088)
    , A("YEW-F-089", YEW_AUDIT_PASS, test_yew_f_089)
    , A("YEW-F-090", YEW_AUDIT_PASS, test_yew_f_090)
    , A("YEW-F-091", YEW_AUDIT_PASS, test_yew_f_091)
    , A("YEW-F-092", YEW_AUDIT_PASS, test_yew_f_092)
    , A("YEW-F-093", YEW_AUDIT_PASS, test_yew_f_093)
    , A("YEW-F-094", YEW_AUDIT_PASS, test_yew_f_094)
    , A("YEW-F-095", YEW_AUDIT_PASS, test_yew_f_095)
    , A("YEW-F-096", YEW_AUDIT_PASS, test_yew_f_096)
    , A("YEW-F-097", YEW_AUDIT_PASS, test_yew_f_097)
    , A("YEW-F-098", YEW_AUDIT_PASS, test_yew_f_098)
    , A("YEW-F-099", YEW_AUDIT_PASS, test_yew_f_099)
    , A("YEW-F-100", YEW_AUDIT_PASS, test_yew_f_100)
    , A("YEW-F-101", YEW_AUDIT_PASS, test_yew_f_101)
    , A("YEW-F-102", YEW_AUDIT_PASS, test_yew_f_102)
    , A("YEW-F-103", YEW_AUDIT_PASS, test_yew_f_103)
    , A("YEW-F-104", YEW_AUDIT_PASS, test_yew_f_104)
    , A("YEW-F-105", YEW_AUDIT_PASS, test_yew_f_105)
    , A("YEW-F-106", YEW_AUDIT_PASS, test_yew_f_106)
    , A("YEW-F-107", YEW_AUDIT_PASS, test_yew_f_107)
    , A("YEW-F-108", YEW_AUDIT_PASS, test_yew_f_108)
    , A("YEW-F-109", YEW_AUDIT_PASS, test_yew_f_109)
    , A("YEW-F-110", YEW_AUDIT_PASS, test_yew_f_110)
    , A("YEW-F-111", YEW_AUDIT_PASS, test_yew_f_111)
    , A("YEW-F-112", YEW_AUDIT_PASS, test_yew_f_112)
    , A("YEW-F-113", YEW_AUDIT_PASS, test_yew_f_113)
    , A("YEW-F-114", YEW_AUDIT_PASS, test_yew_f_114)
    , A("YEW-F-115", YEW_AUDIT_PASS, test_yew_f_115)
    , A("YEW-F-116", YEW_AUDIT_PASS, test_yew_f_116)
    , A("YEW-F-117", YEW_AUDIT_PASS, test_yew_f_117)
    , A("YEW-F-118", YEW_AUDIT_PASS, test_yew_f_118)
    , A("YEW-F-119", YEW_AUDIT_PASS, test_yew_f_119)
    , A("YEW-F-120", YEW_AUDIT_PASS, test_yew_f_120)
    , A("YEW-F-121", YEW_AUDIT_PASS, test_yew_f_121)
    , A("YEW-F-122", YEW_AUDIT_PASS, test_yew_f_122)
    , A("YEW-F-123", YEW_AUDIT_PASS, test_yew_f_123)
    , A("YEW-F-124", YEW_AUDIT_PASS, test_yew_f_124)
    , A("YEW-F-125", YEW_AUDIT_PASS, test_yew_f_125)
    , A("YEW-F-126", YEW_AUDIT_PASS, test_yew_f_126)
    , A("YEW-F-127", YEW_AUDIT_XFAIL, test_yew_f_127)
    , A("YEW-F-128", YEW_AUDIT_XFAIL, test_yew_f_128)
    , A("YEW-F-129", YEW_AUDIT_XFAIL, test_yew_f_129)
    , A("YEW-F-130", YEW_AUDIT_XFAIL, test_yew_f_130)
};

const size_t yew_audit_tests_len =
    sizeof(yew_audit_tests) / sizeof(yew_audit_tests[0]);
