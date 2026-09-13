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
bool test_yew_f_002(char *why, size_t why_cap);
bool test_yew_f_003(char *why, size_t why_cap);
bool test_yew_f_004(char *why, size_t why_cap);
bool test_yew_f_005(char *why, size_t why_cap);
bool test_yew_f_006(char *why, size_t why_cap);
bool test_yew_f_007(char *why, size_t why_cap);
bool test_yew_f_008(char *why, size_t why_cap);
bool test_yew_f_009(char *why, size_t why_cap);
bool test_yew_f_010(char *why, size_t why_cap);
bool test_yew_f_011(char *why, size_t why_cap);
bool test_yew_f_012(char *why, size_t why_cap);
bool test_yew_f_013(char *why, size_t why_cap);
bool test_yew_f_014(char *why, size_t why_cap);
bool test_yew_f_015(char *why, size_t why_cap);
bool test_yew_f_016(char *why, size_t why_cap);
bool test_yew_f_017(char *why, size_t why_cap);
bool test_yew_f_018(char *why, size_t why_cap);
bool test_yew_f_019(char *why, size_t why_cap);
bool test_yew_f_020(char *why, size_t why_cap);
bool test_yew_f_021(char *why, size_t why_cap);
bool test_yew_f_022(char *why, size_t why_cap);
bool test_yew_f_023(char *why, size_t why_cap);
bool test_yew_f_024(char *why, size_t why_cap);
bool test_yew_f_025(char *why, size_t why_cap);
bool test_yew_f_026(char *why, size_t why_cap);
bool test_yew_f_027(char *why, size_t why_cap);
bool test_yew_f_028(char *why, size_t why_cap);
bool test_yew_f_029(char *why, size_t why_cap);
bool test_yew_f_030(char *why, size_t why_cap);
bool test_yew_f_031(char *why, size_t why_cap);
bool test_yew_f_032(char *why, size_t why_cap);
bool test_yew_f_033(char *why, size_t why_cap);
bool test_yew_f_034(char *why, size_t why_cap);
bool test_yew_f_035(char *why, size_t why_cap);
bool test_yew_f_036(char *why, size_t why_cap);
bool test_yew_f_037(char *why, size_t why_cap);
bool test_yew_f_038(char *why, size_t why_cap);
bool test_yew_f_039(char *why, size_t why_cap);
bool test_yew_f_040(char *why, size_t why_cap);
bool test_yew_f_041(char *why, size_t why_cap);
bool test_yew_f_042(char *why, size_t why_cap);
bool test_yew_f_043(char *why, size_t why_cap);
bool test_yew_f_044(char *why, size_t why_cap);
bool test_yew_f_045(char *why, size_t why_cap);
bool test_yew_f_046(char *why, size_t why_cap);
bool test_yew_f_047(char *why, size_t why_cap);
bool test_yew_f_048(char *why, size_t why_cap);
bool test_yew_f_049(char *why, size_t why_cap);
bool test_yew_f_050(char *why, size_t why_cap);
bool test_yew_f_051(char *why, size_t why_cap);
bool test_yew_f_052(char *why, size_t why_cap);
bool test_yew_f_053(char *why, size_t why_cap);
bool test_yew_f_054(char *why, size_t why_cap);
bool test_yew_f_055(char *why, size_t why_cap);
bool test_yew_f_056(char *why, size_t why_cap);
bool test_yew_f_057(char *why, size_t why_cap);
bool test_yew_f_058(char *why, size_t why_cap);
bool test_yew_f_059(char *why, size_t why_cap);
bool test_yew_f_060(char *why, size_t why_cap);
bool test_yew_f_061(char *why, size_t why_cap);
bool test_yew_f_062(char *why, size_t why_cap);
bool test_yew_f_063(char *why, size_t why_cap);
bool test_yew_f_064(char *why, size_t why_cap);
bool test_yew_f_065(char *why, size_t why_cap);
bool test_yew_f_066(char *why, size_t why_cap);
bool test_yew_f_067(char *why, size_t why_cap);
bool test_yew_f_068(char *why, size_t why_cap);
bool test_yew_f_069(char *why, size_t why_cap);
bool test_yew_f_070(char *why, size_t why_cap);
bool test_yew_f_071(char *why, size_t why_cap);
bool test_yew_f_072(char *why, size_t why_cap);
bool test_yew_f_073(char *why, size_t why_cap);
bool test_yew_f_074(char *why, size_t why_cap);
bool test_yew_f_075(char *why, size_t why_cap);
bool test_yew_f_076(char *why, size_t why_cap);
bool test_yew_f_077(char *why, size_t why_cap);
bool test_yew_f_078(char *why, size_t why_cap);

#endif
