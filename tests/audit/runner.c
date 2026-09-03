#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool id_valid(const char *id)
{
    size_t i;

    if (id == NULL || strlen(id) != 9U || memcmp(id, "YEW-F-", 6U) != 0)
        return false;
    for (i = 6U; i < 9U; i++)
        if (id[i] < '0' || id[i] > '9')
            return false;
    return true;
}

static int run_one(const YewAuditTest *test)
{
    char why[512] = "reproducer returned false without an explanation";
    bool passed = test->fn(why, sizeof(why));

    if (test->expect == YEW_AUDIT_XFAIL) {
        if (passed) {
            (void)printf("XPASS %s\n", test->id);
            return 1;
        }
        (void)printf("XFAIL %s: %s\n", test->id, why);
        return 0;
    }
    if (!passed) {
        (void)printf("FAIL %s: %s\n", test->id, why);
        return 1;
    }
    (void)printf("PASS %s\n", test->id);
    return 0;
}

static bool xpass_probe(char *why, size_t why_cap)
{
    (void)why;
    (void)why_cap;
    return true;
}

int main(int argc, char **argv)
{
    size_t i;
    size_t failures = 0U;

    if (argc == 2 && strcmp(argv[1], "--xpass-probe") == 0) {
        const YewAuditTest probe = {
            "YEW-F-" "000", YEW_AUDIT_XFAIL, xpass_probe
        };

        return run_one(&probe);
    }
    if (argc != 1) {
        (void)fprintf(stderr, "audit: usage: %s [--xpass-probe]\n", argv[0]);
        return 2;
    }
    for (i = 0U; i < yew_audit_tests_len; i++) {
        size_t j;

        if (!id_valid(yew_audit_tests[i].id)) {
            (void)printf("CONFIG invalid audit id '%s'\n",
                         yew_audit_tests[i].id);
            failures++;
            continue;
        }
        for (j = 0U; j < i; j++) {
            if (strcmp(yew_audit_tests[j].id, yew_audit_tests[i].id) == 0) {
                (void)printf("CONFIG duplicate audit id '%s'\n",
                             yew_audit_tests[i].id);
                failures++;
                break;
            }
        }
        if (j == i && run_one(&yew_audit_tests[i]) != 0)
            failures++;
    }
    (void)printf("audit: %zu tests, %zu failures\n",
                 yew_audit_tests_len, failures);
    return failures == 0U ? 0 : 1;
}
