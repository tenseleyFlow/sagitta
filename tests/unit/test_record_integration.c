#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/flruntime.h"
#include "fl/record.h"
#include "text/undo.h"

typedef struct RecordFix {
    Ed ed;
} RecordFix;

static void rf_open(RecordFix *f)
{
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
}

static void rf_close(RecordFix *f)
{
    sag_ed_free(&f->ed);
}

static CmdStatus rf_run(RecordFix *f, const char *name, const void *sarg,
                        u32 sarg_len, u32 count, CmdSource source)
{
    CmdCtx cx = {0};
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));

    SAG_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.count = count;
    cx.count_given = count != 1U;
    cx.sarg = sarg;
    cx.sarg_len = sarg_len;
    cx.source = source;
    return sag_ed_invoke(&f->ed, id, &cx);
}

static void assert_buffer(const Ed *ed, const void *want, u64 want_len)
{
    TextIter it;
    u64 done = 0U;

    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed->buffer.tb), want_len);
    if (want_len == 0U)
        return;
    SAG_ASSERT(sag_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        SAG_ASSERT(sag_textiter_chunk(&it, ed->buffer.tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        SAG_ASSERT_EQ_MEM(bytes, (const u8 *)want + done, take);
        done += take;
        if (done < want_len)
            SAG_ASSERT(sag_textiter_advance(&it, ed->buffer.tb));
    }
}

static void set_macro(Ed *ed, u8 reg, const void *source, size_t len)
{
    SAG_ASSERT_EQ_I64(sag_flapi_reg_write(ed, reg, source, (u32)len, false),
                      SAG_CMD_OK);
}

static bool contains_bytes(const Bytebuf *haystack, const void *needle,
                           size_t needle_len)
{
    size_t i;

    if (needle_len == 0U)
        return true;
    if (needle_len > haystack->len)
        return false;
    for (i = 0U; i <= haystack->len - needle_len; i++) {
        if (memcmp(haystack->data + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

void test_record_state_machine_reports_start_stop_and_last_register(void)
{
    RecordFix f;
    RecStatus status = {0};

    rf_open(&f);
    SAG_ASSERT(!sag_record_active(&f.ed));
    SAG_ASSERT(!sag_record_start(&f.ed, (u8)'1'));
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'a'));
    SAG_ASSERT(sag_record_active(&f.ed));
    SAG_ASSERT(!sag_record_start(&f.ed, (u8)'b'));
    SAG_ASSERT_EQ_U64(sag_record_status(&f.ed, &status), 1U);
    SAG_ASSERT(status.active);
    SAG_ASSERT_EQ_U64(status.reg, (u8)'a');
    SAG_ASSERT_EQ_U64(status.nevents, 0U);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    SAG_ASSERT(!sag_record_active(&f.ed));
    SAG_ASSERT_EQ_U64(sag_record_status(&f.ed, &status), 1U);
    SAG_ASSERT(!status.active);
    SAG_ASSERT_EQ_U64(status.last_reg, (u8)'a');
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_ERR_STATE);
    rf_close(&f);
}

void test_record_tap_deep_copies_binary_arguments_and_resolved_context(void)
{
    u8 payload[] = {'a', '\0', 0x80U, 'z'};
    CmdCtx cx = {0};
    RecordFix f;
    const RecEvent *event;

    rf_open(&f);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'c'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 7U;
    cx.count_given = true;
    cx.iarg = -42;
    cx.bang = true;
    cx.range.given = true;
    cx.range.tok = (Span){3U, 9U};
    cx.sarg = (const char *)payload;
    cx.sarg_len = sizeof(payload);
    cx.source = SAG_SRC_MOUSE;
    f.ed.mode = SAG_MODE_H;
    sag_record_tap(sag_cmd_lookup("ed.file.save", 12U), &cx);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    event = &f.ed.rec.ev.data[0];
    SAG_ASSERT_EQ_U64(event->count, 7U);
    SAG_ASSERT(event->count_given);
    SAG_ASSERT_EQ_I64(event->iarg, -42);
    SAG_ASSERT(event->bang);
    SAG_ASSERT_EQ_U64(event->range_kind, SAG_REC_RANGE_SPAN);
    SAG_ASSERT(event->range_given);
    SAG_ASSERT_EQ_U64(event->range_lo, 3U);
    SAG_ASSERT_EQ_U64(event->range_hi, 9U);
    SAG_ASSERT_EQ_U64(event->mode, SAG_MODE_H);
    SAG_ASSERT_EQ_U64(event->src, SAG_SRC_MOUSE);
    SAG_ASSERT_EQ_U64(event->sarg_len, sizeof(payload));
    payload[0] = 'X';
    payload[1] = 'Y';
    SAG_ASSERT_EQ_MEM(f.ed.rec.blob.data + event->sarg_at,
                      "a\0\x80z", sizeof(payload));
    rf_close(&f);
}

void test_record_tap_drops_replay_source(void)
{
    CmdCtx cx = {0};
    RecordFix f;

    rf_open(&f);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'d'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.source = SAG_SRC_REPLAY;
    sag_record_tap(sag_cmd_lookup("ed.move.unit.next", 17U), &cx);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    cx.source = SAG_SRC_FLETCH;
    sag_record_tap(sag_cmd_lookup("ed.move.unit.next", 17U), &cx);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);

    SAG_ASSERT(sag_record_start(&f.ed, (u8)'e'));
    cx.sarg = "E";
    cx.sarg_len = 1U;
    cx.source = SAG_SRC_KEY;
    sag_record_tap(sag_cmd_lookup("ed.mode.enter", 13U), &cx);
    cx.sarg = "ignored";
    cx.sarg_len = 7U;
    sag_record_tap(sag_cmd_lookup("ed.edit.insert.text", 19U), &cx);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    cx.sarg = NULL;
    cx.sarg_len = 0U;
    cx.source = SAG_SRC_CMDLINE;
    sag_record_tap(sag_cmd_lookup("ed.move.unit.next", 17U), &cx);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.data[0].src, SAG_SRC_CMDLINE);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);

    SAG_ASSERT(sag_record_start(&f.ed, (u8)'f'));
    cx.sarg = "E";
    cx.sarg_len = 1U;
    cx.source = SAG_SRC_KEY;
    sag_record_tap(sag_cmd_lookup("ed.mode.enter", 13U), &cx);
    /* Cancelling contributes no committed CMDLINE command. */
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    rf_close(&f);
}

void test_record_dispatch_ignores_nonrecordable_commands(void)
{
    RecordFix f;

    rf_open(&f);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'e'));
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.macro.list", NULL, 0U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    rf_close(&f);
}

void test_record_multicursor_dispatch_captures_one_event(void)
{
    Cursor second = {{0U}, {0U}, {0U}};
    RecordFix f;

    rf_open(&f);
    SAG_ASSERT(sag_cset_add(&f.ed.win->cs, second) == false);
    second.pos = BYTEOFF(1U);
    second.anchor = BYTEOFF(1U);
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "ab", 2U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT(sag_cset_add(&f.ed.win->cs, second));
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'f'));
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "X", 1U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    SAG_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 2U);
    rf_close(&f);
}

void test_record_rejects_nonportable_targets_before_execution(void)
{
    static const char payload[] = "x";
    CmdCtx cx = {0};
    RecordFix f;
    Win other = {0};

    rf_open(&f);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'g'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.sarg = payload;
    cx.sarg_len = sizeof(payload) - 1U;
    cx.cursor_given = true;
    cx.source = SAG_SRC_FLETCH;
    SAG_ASSERT_EQ_I64(
        sag_cmd_invoke(sag_cmd_lookup("ed.edit.insert.text", 19U), &cx),
        SAG_CMD_ERR_STATE);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    assert_buffer(&f.ed, "", 0U);

    cx.cursor_given = false;
    cx.win = &other;
    SAG_ASSERT_EQ_I64(
        sag_cmd_invoke(sag_cmd_lookup("ed.edit.insert.text", 19U), &cx),
        SAG_CMD_ERR_STATE);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    assert_buffer(&f.ed, "", 0U);
    rf_close(&f);
}

void test_record_range_envelope_replays_the_same_span_edit(void)
{
    CmdCtx cx = {0};
    EditCtx ec;
    RecordFix f;

    rf_open(&f);
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "abcdef", 6U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'h'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.range.given = true;
    cx.range.tok = (Span){1U, 3U};
    cx.source = SAG_SRC_FLETCH;
    SAG_ASSERT_EQ_I64(
        sag_ed_invoke(&f.ed,
                      sag_cmd_lookup("ed.edit.delete.span", 19U), &cx),
        SAG_CMD_OK);
    assert_buffer(&f.ed, "adef", 4U);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.data[0].range_kind,
                      SAG_REC_RANGE_SPAN);
    ec = sag_ed_edit_ctx(&f.ed);
    SAG_ASSERT(sag_undo(&ec));
    assert_buffer(&f.ed, "abcdef", 6U);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'h', 1U), SAG_CMD_OK);
    assert_buffer(&f.ed, "adef", 4U);
    rf_close(&f);
}

void test_record_stop_stores_parseable_source_and_uppercase_appends(void)
{
    EditCtx ec;
    RecordFix f;
    RegVal *value;
    size_t first_len;

    rf_open(&f);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'a'));
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "one", 3U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    value = sag_reg_get(&f.ed.regs, (u8)'a');
    SAG_ASSERT_NOT_NULL(value);
    SAG_ASSERT_EQ_U64(value->type, SAG_REG_CHARWISE);
    SAG_ASSERT(value->bytes.len > 32U);
    SAG_ASSERT(contains_bytes(&value->bytes, "i\"one\"", 6U));
    first_len = value->bytes.len;

    SAG_ASSERT(sag_record_start(&f.ed, (u8)'A'));
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "two", 3U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    value = sag_reg_get(&f.ed.regs, (u8)'a');
    SAG_ASSERT(value->bytes.len > first_len);
    SAG_ASSERT(contains_bytes(&value->bytes, "i\"one\"", 6U));
    SAG_ASSERT(contains_bytes(&value->bytes, "i\"two\"", 6U));
    ec = sag_ed_edit_ctx(&f.ed);
    SAG_ASSERT(sag_undo(&ec));
    SAG_ASSERT(sag_undo(&ec));
    assert_buffer(&f.ed, "", 0U);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'a', 1U), SAG_CMD_OK);
    assert_buffer(&f.ed, "onetwo", 6U);
    rf_close(&f);
}

void test_macro_replay_uses_one_undo_node_per_run(void)
{
    static const char macro[] = "@[ i\"x\" ]\n";
    EditCtx ec;
    RecordFix f;
    u32 i;

    rf_open(&f);
    set_macro(&f.ed, (u8)'a', macro, sizeof(macro) - 1U);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'a', 5U), SAG_CMD_OK);
    assert_buffer(&f.ed, "xxxxx", 5U);
    ec = sag_ed_edit_ctx(&f.ed);
    for (i = 5U; i != 0U; i--) {
        SAG_ASSERT(sag_undo(&ec));
        SAG_ASSERT_EQ_U64(sag_textbuf_len(f.ed.buffer.tb), i - 1U);
    }
    SAG_ASSERT(!sag_undo(&ec));
    rf_close(&f);
}

void test_macro_replay_rolls_back_on_runtime_error(void)
{
    static const char macro[] = "@[ i\"changed\" ]\nmissing_function()\n";
    RecordFix f;

    rf_open(&f);
    set_macro(&f.ed, (u8)'b', macro, sizeof(macro) - 1U);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'b', 1U),
                      SAG_CMD_ERR_STATE);
    assert_buffer(&f.ed, "", 0U);
    rf_close(&f);
}

void test_macro_replay_cache_invalidates_after_register_write(void)
{
    static const char first[] = "@[ i\"a\" ]\n";
    static const char second[] = "@[ i\"b\" ]\n";
    RecordFix f;

    rf_open(&f);
    set_macro(&f.ed, (u8)'c', first, sizeof(first) - 1U);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'c', 1U), SAG_CMD_OK);
    assert_buffer(&f.ed, "a", 1U);
    set_macro(&f.ed, (u8)'c', second, sizeof(second) - 1U);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'c', 1U), SAG_CMD_OK);
    assert_buffer(&f.ed, "ab", 2U);
    rf_close(&f);
}

void test_recording_during_macro_replay_captures_no_inner_commands(void)
{
    static const char macro[] = "@[ i\"x\" > del ]\n";
    RecordFix f;

    rf_open(&f);
    set_macro(&f.ed, (u8)'a', macro, sizeof(macro) - 1U);
    SAG_ASSERT(sag_record_start(&f.ed, (u8)'b'));
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'a', 1U), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    rf_close(&f);
}

void test_macro_list_reports_only_nonempty_named_registers(void)
{
    static const char macro[] = "@[ > ]\n";
    RegInfo info[26];
    RecordFix f;
    u32 n;

    rf_open(&f);
    set_macro(&f.ed, (u8)'a', macro, sizeof(macro) - 1U);
    set_macro(&f.ed, (u8)'z', macro, sizeof(macro) - 1U);
    n = sag_macro_list(&f.ed, info, SAG_ARRAY_LEN(info));
    SAG_ASSERT_EQ_U64(n, 2U);
    SAG_ASSERT_EQ_U64(info[0].type, SAG_REG_CHARWISE);
    SAG_ASSERT_EQ_U64(info[0].bytes, sizeof(macro) - 1U);
    SAG_ASSERT_EQ_U64(info[1].type, SAG_REG_CHARWISE);
    SAG_ASSERT_EQ_U64(info[1].bytes, sizeof(macro) - 1U);
    rf_close(&f);
}

void test_fletch_record_and_replay_prelude_route_through_macro_commands(void)
{
    static const char start[] = "record(\"a\")";
    static const char start_in_edit[] = "edit { record(\"c\") }";
    static const char stop_in_edit[] =
        "import ed\nedit { ed.run(\"ed.macro.stop\") }";
    static const char nested[] = "record(\"b\")";
    static const char replay[] = "replay(\"a\", 2)";
    EditCtx ec;
    RecordFix f;

    rf_open(&f);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&f.ed, start_in_edit,
                                  sizeof(start_in_edit) - 1U),
                      SAG_CMD_ERR_STATE);
    SAG_ASSERT(!sag_record_active(&f.ed));
    SAG_ASSERT_EQ_I64(sag_fl_eval(&f.ed, start, sizeof(start) - 1U),
                      SAG_CMD_OK);
    SAG_ASSERT(sag_record_active(&f.ed));
    SAG_ASSERT_EQ_I64(sag_fl_eval(&f.ed, stop_in_edit,
                                  sizeof(stop_in_edit) - 1U),
                      SAG_CMD_ERR_STATE);
    SAG_ASSERT(sag_record_active(&f.ed));
    SAG_ASSERT_EQ_I64(sag_fl_eval(&f.ed, nested, sizeof(nested) - 1U),
                      SAG_CMD_ERR_STATE);
    SAG_ASSERT_EQ_U64(f.ed.rec.reg, (u8)'a');
    SAG_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "q", 1U, 1U,
                             SAG_SRC_TEST), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    ec = sag_ed_edit_ctx(&f.ed);
    SAG_ASSERT(sag_undo(&ec));
    assert_buffer(&f.ed, "", 0U);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&f.ed, replay, sizeof(replay) - 1U),
                      SAG_CMD_OK);
    assert_buffer(&f.ed, "qq", 2U);
    SAG_ASSERT(sag_undo(&ec));
    assert_buffer(&f.ed, "q", 1U);
    SAG_ASSERT(sag_undo(&ec));
    assert_buffer(&f.ed, "", 0U);
    rf_close(&f);
}
