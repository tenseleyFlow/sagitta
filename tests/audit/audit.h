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
bool test_yew_f_079(char *why, size_t why_cap);
bool test_yew_f_080(char *why, size_t why_cap);
bool test_yew_f_081(char *why, size_t why_cap);
bool test_yew_f_082(char *why, size_t why_cap);
bool test_yew_f_083(char *why, size_t why_cap);
bool test_yew_f_084(char *why, size_t why_cap);
bool test_yew_f_085(char *why, size_t why_cap);
bool test_yew_f_086(char *why, size_t why_cap);
bool test_yew_f_087(char *why, size_t why_cap);
bool test_yew_f_088(char *why, size_t why_cap);
bool test_yew_f_089(char *why, size_t why_cap);
bool test_yew_f_090(char *why, size_t why_cap);
bool test_yew_f_091(char *why, size_t why_cap);
bool test_yew_f_092(char *why, size_t why_cap);
bool test_yew_f_093(char *why, size_t why_cap);
bool test_yew_f_094(char *why, size_t why_cap);
bool test_yew_f_095(char *why, size_t why_cap);
bool test_yew_f_096(char *why, size_t why_cap);
bool test_yew_f_097(char *why, size_t why_cap);
bool test_yew_f_098(char *why, size_t why_cap);
bool test_yew_f_099(char *why, size_t why_cap);
bool test_yew_f_100(char *why, size_t why_cap);
bool test_yew_f_101(char *why, size_t why_cap);
bool test_yew_f_102(char *why, size_t why_cap);
bool test_yew_f_103(char *why, size_t why_cap);
bool test_yew_f_104(char *why, size_t why_cap);
bool test_yew_f_105(char *why, size_t why_cap);
bool test_yew_f_106(char *why, size_t why_cap);
bool test_yew_f_107(char *why, size_t why_cap);
bool test_yew_f_108(char *why, size_t why_cap);
bool test_yew_f_109(char *why, size_t why_cap);
bool test_yew_f_110(char *why, size_t why_cap);
bool test_yew_f_111(char *why, size_t why_cap);
bool test_yew_f_112(char *why, size_t why_cap);
bool test_yew_f_113(char *why, size_t why_cap);
bool test_yew_f_114(char *why, size_t why_cap);
bool test_yew_f_115(char *why, size_t why_cap);
bool test_yew_f_116(char *why, size_t why_cap);
bool test_yew_f_117(char *why, size_t why_cap);
bool test_yew_f_118(char *why, size_t why_cap);
bool test_yew_f_119(char *why, size_t why_cap);
bool test_yew_f_120(char *why, size_t why_cap);
bool test_yew_f_121(char *why, size_t why_cap);
bool test_yew_f_122(char *why, size_t why_cap);
bool test_yew_f_123(char *why, size_t why_cap);
bool test_yew_f_124(char *why, size_t why_cap);
bool test_yew_f_125(char *why, size_t why_cap);
bool test_yew_f_126(char *why, size_t why_cap);
bool test_yew_f_127(char *why, size_t why_cap);
bool test_yew_f_128(char *why, size_t why_cap);
bool test_yew_f_129(char *why, size_t why_cap);
bool test_yew_f_130(char *why, size_t why_cap);
bool test_yew_f_131(char *why, size_t why_cap);
bool test_yew_f_132(char *why, size_t why_cap);
bool test_yew_f_133(char *why, size_t why_cap);
bool test_yew_f_134(char *why, size_t why_cap);

#endif
