/* Sprint 38: sharing headers are metadata, never an execution policy. */

#include "flfix.h"

#include <string.h>

#include "edit/bind.h"
#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/macrolib.h"
#include "fl/origin.h"
#include "fl/record.h"

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

static Key text_key(char ch)
{
    Key key = {0};

    key.code = (u32)(u8)ch;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)ch;
    return key;
}

static void copy_buffer(const Ed *ed, Bytebuf *out)
{
    TextIter it;

    bytebuf_init(out);
    if (sag_textbuf_len(ed->buffer.tb) == 0U)
        return;
    SAG_ASSERT(sag_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U)));
    do {
        const u8 *bytes;
        u64 len;

        SAG_ASSERT(sag_textiter_chunk(&it, ed->buffer.tb, &bytes, &len));
        bytebuf_append(out, bytes, (size_t)len);
    } while (sag_textiter_advance(&it, ed->buffer.tb));
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
    SagMacroHeader recorded_header;
    Bytebuf actual;
    Bytebuf expected;
    Bytebuf source;
    const RegVal *recorded;
    CmdId insert;
    Ed origin;
    Ed fresh;
    u32 first_origin;
    u32 rebound_origin;

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

    sag_ed_init(&origin);
    SAG_ASSERT(sag_ed_open_scratch(&origin));
    insert = sag_cmd_lookup("ed.edit.insert.text", 19U);
    SAG_ASSERT(insert.v != 0U);
    first_origin = fl_origin_register(&origin, FL_ORIGIN_PLUGIN,
                                      "keymap:first", 0U);
    rebound_origin = fl_origin_register(&origin, FL_ORIGIN_WORKSPACE,
                                        "keymap:rebound", 0U);
    SAG_ASSERT(sag_bind_add(&origin, first_origin, SAG_MODE_L, "Z", insert,
                            0, "wrong-binding", FL_NIL_V) != 0U);
    SAG_ASSERT(sag_bind_add(&origin, rebound_origin, SAG_MODE_L, "Z", insert,
                            0, "resolved-effect", FL_NIL_V) != 0U);

    SAG_ASSERT(sag_record_start(&origin, (u8)'a'));
    sag_dispatch_key(&origin, text_key('Z'), 0);
    SAG_ASSERT_EQ_I64(origin.last_status, SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(origin.rec.ev.len, 1U);
    SAG_ASSERT_EQ_I64(sag_record_stop(&origin), SAG_CMD_OK);
    recorded = sag_reg_get(&origin.regs, (u8)'a');
    SAG_ASSERT_NOT_NULL(recorded);
    SAG_ASSERT(recorded->bytes.len != 0U);
    bytebuf_init(&source);
    bytebuf_append(&source, recorded->bytes.data, recorded->bytes.len);
    copy_buffer(&origin, &expected);
    SAG_ASSERT_EQ_U64(expected.len, sizeof("resolved-effect") - 1U);
    SAG_ASSERT_EQ_MEM(expected.data, "resolved-effect", expected.len);

    SAG_ASSERT_EQ_I64(sag_macro_header_parse((const char *)source.data,
                                              source.len,
                                              &recorded_header),
                      SAG_MACRO_HEADER_OK);
    assert_text(recorded_header.keymap, "default");

    sag_ed_init(&fresh);
    SAG_ASSERT(sag_ed_open_scratch(&fresh));
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&fresh), 0U);
    SAG_ASSERT_EQ_I64(sag_flapi_reg_write(&fresh, (u8)'a', source.data,
                                          (u32)source.len, false),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&fresh, (u8)'a', 1U), SAG_CMD_OK);
    copy_buffer(&fresh, &actual);
    SAG_ASSERT_EQ_U64(actual.len, expected.len);
    SAG_ASSERT_EQ_MEM(actual.data, expected.data, expected.len);

    bytebuf_free(&actual);
    bytebuf_free(&expected);
    bytebuf_free(&source);
    sag_ed_free(&fresh);
    sag_ed_free(&origin);
}
