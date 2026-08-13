#include "harness.h"

#include <string.h>

#include "mod/lsp/json.h"
#include "util/arena.h"

void test_json_number_policy(void)
{
    static const struct {
        const char *text;
        JsonKind kind;
        bool lossy;
        i64 integer;
        double real;
    } cases[] = {
        {"0", YEW_JS_INT, false, 0, 0},
        {"-0", YEW_JS_INT, false, 0, 0},
        {"1", YEW_JS_INT, false, 1, 0},
        {"-1", YEW_JS_INT, false, -1, 0},
        {"42", YEW_JS_INT, false, 42, 0},
        {"9223372036854775807", YEW_JS_INT, false, INT64_MAX, 0},
        {"-9223372036854775808", YEW_JS_INT, false, INT64_MIN, 0},
        {"9223372036854775808", YEW_JS_REAL, true, 0,
         9223372036854775808.0},
        {"-9223372036854775809", YEW_JS_REAL, true, 0,
         -9223372036854775808.0},
        {"18446744073709551615", YEW_JS_REAL, true, 0,
         18446744073709551616.0},
        {"3.14", YEW_JS_REAL, false, 0, 3.14},
        {"1.0", YEW_JS_REAL, false, 0, 1.0},
        {"1E+2", YEW_JS_REAL, false, 0, 100.0},
        {"1e-2", YEW_JS_REAL, false, 0, 0.01},
        {"1e308", YEW_JS_REAL, false, 0, 1e308},
        {"5e-324", YEW_JS_REAL, false, 0, 5e-324},
        {"1e-400", YEW_JS_REAL, true, 0, 0.0},
        {"2.2250738585072014e-308", YEW_JS_REAL, false, 0,
         2.2250738585072014e-308},
        {"10e0", YEW_JS_REAL, false, 0, 10.0},
        {"6.022e23", YEW_JS_REAL, false, 0, 6.022e23},
        {"-2.5e3", YEW_JS_REAL, false, 0, -2500.0},
        {"1000000000000000000", YEW_JS_INT, false,
         INT64_C(1000000000000000000), 0},
        {"-1000000000000000000", YEW_JS_INT, false,
         -INT64_C(1000000000000000000), 0},
        {"2147483647", YEW_JS_INT, false, 2147483647, 0},
        {"-2147483648", YEW_JS_INT, false, -INT64_C(2147483648), 0},
        {"0.000001", YEW_JS_REAL, false, 0, 0.000001},
        {"1e20", YEW_JS_REAL, false, 0, 1e20},
        {"-1e20", YEW_JS_REAL, false, 0, -1e20},
        {"7.5e-10", YEW_JS_REAL, false, 0, 7.5e-10},
        {"9007199254740991", YEW_JS_INT, false, INT64_C(9007199254740991), 0},
        {"9007199254740992", YEW_JS_INT, false, INT64_C(9007199254740992), 0},
        {"123456789", YEW_JS_INT, false, 123456789, 0},
        {"-123456789", YEW_JS_INT, false, -123456789, 0},
        {"0e0", YEW_JS_REAL, false, 0, 0.0},
        {"-0.0", YEW_JS_REAL, false, 0, -0.0},
        {"1.2345678901234567", YEW_JS_REAL, false, 0,
         1.2345678901234567},
        {"4e-200", YEW_JS_REAL, false, 0, 4e-200},
        {"8e200", YEW_JS_REAL, false, 0, 8e200},
        {"99.99", YEW_JS_REAL, false, 0, 99.99},
        {"-17.125", YEW_JS_REAL, false, 0, -17.125},
        {"314159265358979323846", YEW_JS_REAL, true, 0,
         3.1415926535897933e20},
    };
    size_t i;

    for (i = 0u; i < YEW_ARRAY_LEN(cases); i++) {
        Arena a;
        JsonErr err;
        JsonValue *v;

        arena_init(&a);
        v = yew_json_parse(&a, (const u8 *)cases[i].text,
                           strlen(cases[i].text), &err);
        YEW_ASSERT_NOT_NULL(v);
        YEW_ASSERT_EQ_U64(v->kind, cases[i].kind);
        YEW_ASSERT(((v->flags & YEW_JSF_LOSSY_NUM) != 0u) == cases[i].lossy);
        if (v->kind == YEW_JS_INT)
            YEW_ASSERT_EQ_I64(v->i, cases[i].integer);
        else
            YEW_ASSERT(v->d == cases[i].real);
        arena_free_all(&a);
    }
}

void test_json_number_rejections(void)
{
    static const char *cases[] = {
        "01", "+1", ".5", "1.", "1.e3", "-", "--1", "00",
        "1e", "1e+", "1e-", "-01", "2..0", "1e309", "-1e309"
    };
    size_t i;

    for (i = 0u; i < YEW_ARRAY_LEN(cases); i++) {
        Arena a;
        JsonErr err;

        arena_init(&a);
        YEW_ASSERT_NULL(yew_json_parse(&a, (const u8 *)cases[i],
                                       strlen(cases[i]), &err));
        if (strstr(cases[i], "309") != NULL)
            YEW_ASSERT_EQ_STR(err.msg, "number out of range");
        else
            YEW_ASSERT(strstr(err.msg, "number") != NULL ||
                       strstr(err.msg, "JSON value") != NULL ||
                       strstr(err.msg, "trailing bytes") != NULL);
        arena_free_all(&a);
    }
}
