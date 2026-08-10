/* Sprint 38: sharing headers are metadata, never an execution policy. */

#include "flfix.h"

#include <string.h>

#include "fl/macrolib.h"

static void assert_text(SagMacroText got, const char *want)
{
    size_t len = strlen(want);

    SAG_ASSERT(got.present);
    SAG_ASSERT_NOT_NULL(got.s);
    SAG_ASSERT_EQ_U64(got.len, len);
    SAG_ASSERT_EQ_MEM(got.s, want, len);
}

static void assert_missing(SagMacroText got)
{
    SAG_ASSERT(!got.present);
    SAG_ASSERT_NULL(got.s);
    SAG_ASSERT_EQ_U64(got.len, 0U);
}

void test_macro_header_parses_all_five_fields(void)
{
    static const char source[] =
        "# sagitta-macro: 1\n"
        "# recorded-with:   sagitta 1.2.3  \n"
        "# keymap: custom-nav\n"
        "# recorded: 2026-07-31T14:22:07Z\n"
        "# describe: wrap selection in parens and reindent\n"
        "fn wrap() { return 7 }\n";
    SagMacroHeader h;

    SAG_ASSERT_EQ_I64(sag_macro_header_parse(source, sizeof(source) - 1U,
                                               &h),
                      SAG_MACRO_HEADER_OK);
    SAG_ASSERT(h.has_schema);
    SAG_ASSERT_EQ_U64(h.schema, 1U);
    assert_text(h.recorded_with, "sagitta 1.2.3");
    assert_text(h.keymap, "custom-nav");
    assert_text(h.recorded, "2026-07-31T14:22:07Z");
    assert_text(h.describe, "wrap selection in parens and reindent");
}

void test_macro_header_accepts_missing_optional_fields(void)
{
    static const char source[] =
        "# sagitta-macro: 1\n"
        "# describe: only one optional field\n"
        "fn sparse() { return 1 }\n";
    SagMacroHeader h;

    SAG_ASSERT_EQ_I64(sag_macro_header_parse(source, sizeof(source) - 1U,
                                               &h),
                      SAG_MACRO_HEADER_OK);
    SAG_ASSERT(h.has_schema);
    SAG_ASSERT_EQ_U64(h.schema, 1U);
    assert_missing(h.recorded_with);
    assert_missing(h.keymap);
    assert_missing(h.recorded);
    assert_text(h.describe, "only one optional field");
}

void test_macro_header_unknown_schema_is_skipped(void)
{
    static const char *const sources[] = {
        "# sagitta-macro: 0\nfn x() {}\n",
        "# sagitta-macro: 2\nfn x() {}\n",
        "# sagitta-macro: 9\nfn x() {}\n",
        "# sagitta-macro: 10\nfn x() {}\n",
        "# sagitta-macro: 42\nfn x() {}\n",
        "# sagitta-macro: 100\nfn x() {}\n"
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(sources); i++) {
        SagMacroHeader h;

        SAG_ASSERT_EQ_I64(sag_macro_header_parse(sources[i],
                                                  strlen(sources[i]), &h),
                          SAG_MACRO_HEADER_UNSUPPORTED);
        SAG_ASSERT(h.has_schema);
        SAG_ASSERT(h.schema != 1U);
        assert_missing(h.recorded_with);
        assert_missing(h.keymap);
        assert_missing(h.recorded);
        assert_missing(h.describe);
    }
}

void test_macro_header_absent_header_still_loads(void)
{
    static const char source[] = "fn plain() { return 17 }\nreturn plain()\n";
    SagMacroHeader h;
    FlFix f;

    SAG_ASSERT_EQ_I64(sag_macro_header_parse(source, sizeof(source) - 1U,
                                               &h),
                      SAG_MACRO_HEADER_OK);
    SAG_ASSERT(!h.has_schema);
    SAG_ASSERT_EQ_U64(h.schema, 0U);
    assert_missing(h.recorded_with);
    assert_missing(h.keymap);
    assert_missing(h.recorded);
    assert_missing(h.describe);
    flfix_open(&f);
    FL_EQ(&f, source, "17");
    flfix_close(&f);
}

void test_macro_header_keymap_is_provenance_not_execution_policy(void)
{
    static const char default_source[] =
        "# sagitta-macro: 1\n"
        "# keymap: default\n"
        "fn result() { return 23 }\nreturn result()\n";
    static const char custom_source[] =
        "# sagitta-macro: 1\n"
        "# keymap: radically-rebound\n"
        "fn result() { return 23 }\nreturn result()\n";
    SagMacroHeader default_header;
    SagMacroHeader custom_header;
    FlFix f;

    SAG_ASSERT_EQ_I64(sag_macro_header_parse(default_source,
                                               sizeof(default_source) - 1U,
                                               &default_header),
                      SAG_MACRO_HEADER_OK);
    SAG_ASSERT_EQ_I64(sag_macro_header_parse(custom_source,
                                               sizeof(custom_source) - 1U,
                                               &custom_header),
                      SAG_MACRO_HEADER_OK);
    assert_text(default_header.keymap, "default");
    assert_text(custom_header.keymap, "radically-rebound");
    flfix_open(&f);
    FL_EQ(&f, default_source, "23");
    FL_EQ(&f, custom_source, "23");
    flfix_close(&f);
}

