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

static void assert_text(YewMacroText got, const char *want)
{
    size_t len = strlen(want);

    YEW_ASSERT(got.present);
    YEW_ASSERT_NOT_NULL(got.s);
    YEW_ASSERT_EQ_U64(got.len, len);
    YEW_ASSERT_EQ_MEM(got.s, want, len);
}

static void assert_missing(YewMacroText got)
{
    YEW_ASSERT(!got.present);
    YEW_ASSERT_NULL(got.s);
    YEW_ASSERT_EQ_U64(got.len, 0U);
}

static Key text_key(char ch)
{
    Key key = {0};

    key.code = (u32)(u8)ch;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)ch;
    return key;
}

static void copy_buffer(const Ed *ed, Bytebuf *out)
{
    TextIter it;

    bytebuf_init(out);
    if (yew_textbuf_len(ed->buffer.tb) == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U)));
    do {
        const u8 *bytes;
        u64 len;

        YEW_ASSERT(yew_textiter_chunk(&it, ed->buffer.tb, &bytes, &len));
        bytebuf_append(out, bytes, (size_t)len);
    } while (yew_textiter_advance(&it, ed->buffer.tb));
}

void test_macro_header_parses_all_five_fields(void)
{
    static const char source[] =
        "# yew-macro: 1\n"
        "# recorded-with:   yew 1.2.3  \n"
        "# keymap: custom-nav\n"
        "# recorded: 2026-07-31T14:22:07Z\n"
        "# describe: wrap selection in parens and reindent\n"
        "fn wrap() { return 7 }\n";
    YewMacroHeader h;

    YEW_ASSERT_EQ_I64(yew_macro_header_parse(source, sizeof(source) - 1U,
                                               &h),
                      YEW_MACRO_HEADER_OK);
    YEW_ASSERT(h.has_schema);
    YEW_ASSERT_EQ_U64(h.schema, 1U);
    assert_text(h.recorded_with, "yew 1.2.3");
    assert_text(h.keymap, "custom-nav");
    assert_text(h.recorded, "2026-07-31T14:22:07Z");
    assert_text(h.describe, "wrap selection in parens and reindent");
}

void test_macro_header_accepts_missing_optional_fields(void)
{
    static const char source[] =
        "# yew-macro: 1\n"
        "# describe: only one optional field\n"
        "fn sparse() { return 1 }\n";
    YewMacroHeader h;

    YEW_ASSERT_EQ_I64(yew_macro_header_parse(source, sizeof(source) - 1U,
                                               &h),
                      YEW_MACRO_HEADER_OK);
    YEW_ASSERT(h.has_schema);
    YEW_ASSERT_EQ_U64(h.schema, 1U);
    assert_missing(h.recorded_with);
    assert_missing(h.keymap);
    assert_missing(h.recorded);
    assert_text(h.describe, "only one optional field");
}

void test_macro_header_unknown_schema_is_skipped(void)
{
    static const char *const sources[] = {
        "# yew-macro: 0\nfn x() {}\n",
        "# yew-macro: 2\nfn x() {}\n",
        "# yew-macro: 9\nfn x() {}\n",
        "# yew-macro: 10\nfn x() {}\n",
        "# yew-macro: 42\nfn x() {}\n",
        "# yew-macro: 100\nfn x() {}\n"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(sources); i++) {
        YewMacroHeader h;

        YEW_ASSERT_EQ_I64(yew_macro_header_parse(sources[i],
                                                  strlen(sources[i]), &h),
                          YEW_MACRO_HEADER_UNSUPPORTED);
        YEW_ASSERT(h.has_schema);
        YEW_ASSERT(h.schema != 1U);
        assert_missing(h.recorded_with);
        assert_missing(h.keymap);
        assert_missing(h.recorded);
        assert_missing(h.describe);
    }
}

void test_macro_header_absent_header_still_loads(void)
{
    static const char source[] = "fn plain() { return 17 }\nreturn plain()\n";
    YewMacroHeader h;
    FlFix f;

    YEW_ASSERT_EQ_I64(yew_macro_header_parse(source, sizeof(source) - 1U,
                                               &h),
                      YEW_MACRO_HEADER_OK);
    YEW_ASSERT(!h.has_schema);
    YEW_ASSERT_EQ_U64(h.schema, 0U);
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
        "# yew-macro: 1\n"
        "# keymap: default\n"
        "fn result() { return 23 }\nreturn result()\n";
    static const char custom_source[] =
        "# yew-macro: 1\n"
        "# keymap: radically-rebound\n"
        "fn result() { return 23 }\nreturn result()\n";
    YewMacroHeader default_header;
    YewMacroHeader custom_header;
    YewMacroHeader recorded_header;
    Bytebuf actual;
    Bytebuf expected;
    Bytebuf source;
    const RegVal *recorded;
    CmdId insert;
    Ed origin;
    Ed fresh;
    u32 first_origin;
    u32 rebound_origin;

    YEW_ASSERT_EQ_I64(yew_macro_header_parse(default_source,
                                               sizeof(default_source) - 1U,
                                               &default_header),
                      YEW_MACRO_HEADER_OK);
    YEW_ASSERT_EQ_I64(yew_macro_header_parse(custom_source,
                                               sizeof(custom_source) - 1U,
                                               &custom_header),
                      YEW_MACRO_HEADER_OK);
    assert_text(default_header.keymap, "default");
    assert_text(custom_header.keymap, "radically-rebound");

    yew_ed_init(&origin);
    YEW_ASSERT(yew_ed_open_scratch(&origin));
    insert = yew_cmd_lookup("ed.edit.insert.text", 19U);
    YEW_ASSERT(insert.v != 0U);
    first_origin = fl_origin_register(&origin, FL_ORIGIN_PLUGIN,
                                      "keymap:first", 0U);
    rebound_origin = fl_origin_register(&origin, FL_ORIGIN_WORKSPACE,
                                        "keymap:rebound", 0U);
    YEW_ASSERT(yew_bind_add(&origin, first_origin, YEW_MODE_L, "Z", insert,
                            0, "wrong-binding", FL_NIL_V) != 0U);
    YEW_ASSERT(yew_bind_add(&origin, rebound_origin, YEW_MODE_L, "Z", insert,
                            0, "resolved-effect", FL_NIL_V) != 0U);

    YEW_ASSERT(yew_record_start(&origin, (u8)'a'));
    yew_dispatch_key(&origin, text_key('Z'), 0);
    YEW_ASSERT_EQ_I64(origin.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(origin.rec.ev.len, 1U);
    YEW_ASSERT_EQ_I64(yew_record_stop(&origin), YEW_CMD_OK);
    recorded = yew_reg_get(&origin.regs, (u8)'a');
    YEW_ASSERT_NOT_NULL(recorded);
    YEW_ASSERT(recorded->bytes.len != 0U);
    bytebuf_init(&source);
    bytebuf_append(&source, recorded->bytes.data, recorded->bytes.len);
    copy_buffer(&origin, &expected);
    YEW_ASSERT_EQ_U64(expected.len, sizeof("resolved-effect") - 1U);
    YEW_ASSERT_EQ_MEM(expected.data, "resolved-effect", expected.len);

    YEW_ASSERT_EQ_I64(yew_macro_header_parse((const char *)source.data,
                                              source.len,
                                              &recorded_header),
                      YEW_MACRO_HEADER_OK);
    assert_text(recorded_header.keymap, "default");

    yew_ed_init(&fresh);
    YEW_ASSERT(yew_ed_open_scratch(&fresh));
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&fresh), 0U);
    YEW_ASSERT_EQ_I64(yew_flapi_reg_write(&fresh, (u8)'a', source.data,
                                          (u32)source.len, false),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&fresh, (u8)'a', 1U), YEW_CMD_OK);
    copy_buffer(&fresh, &actual);
    YEW_ASSERT_EQ_U64(actual.len, expected.len);
    YEW_ASSERT_EQ_MEM(actual.data, expected.data, expected.len);

    bytebuf_free(&actual);
    bytebuf_free(&expected);
    bytebuf_free(&source);
    yew_ed_free(&fresh);
    yew_ed_free(&origin);
}
