#include "harness.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "unicode/grapheme.h"

static bool take_marker(const unsigned char **cursor, bool *boundary)
{
    const unsigned char *p = *cursor;

    while (*p == ' ' || *p == '\t')
        p++;
    if (p[0] == 0xC3u && p[1] == 0xB7u) {
        *boundary = true;
        *cursor = p + 2;
        return true;
    }
    if (p[0] == 0xC3u && p[1] == 0x97u) {
        *boundary = false;
        *cursor = p + 2;
        return true;
    }
    return false;
}

void test_graphemebreaktest_conformance(void)
{
    FILE *fp = fopen(
        "tests/unit/fixtures/unicode/GraphemeBreakTest.txt", "r");
    char line[8192];
    size_t cases = 0;
    size_t passed = 0;

    SAG_ASSERT_NOT_NULL(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        const unsigned char *p = (const unsigned char *)line;
        SagGbState state;
        bool expected;
        bool case_ok = true;
        size_t cps = 0;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0')
            continue;

        sag_gb_init(&state);
        SAG_ASSERT(take_marker(&p, &expected));
        while (true) {
            char *end;
            unsigned long value;
            bool actual;

            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '#')
                break;
            if (*p == '\r' || *p == '\n' || *p == '\0')
                break;
            value = strtoul((const char *)p, &end, 16);
            SAG_ASSERT(end != (const char *)p && value <= 0x10FFFFu);
            p = (const unsigned char *)end;
            actual = sag_gb_boundary(&state, (u32)value);
            sag_test_count_assertion();
            if (actual != expected)
                case_ok = false;
            cps++;
            SAG_ASSERT(take_marker(&p, &expected));
        }
        SAG_ASSERT(cps != 0);
        SAG_ASSERT(expected); /* Every UCD row ends with eot division. */
        cases++;
        if (case_ok)
            passed++;
    }
    SAG_ASSERT(!ferror(fp));
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    SAG_ASSERT_EQ_U64(cases, 1093);
    SAG_ASSERT_EQ_U64(passed, cases);
    printf("graphemebreaktest: %zu/%zu cases pass\n", passed, cases);
}
