#include "harness.h"

#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/flruntime.h"
#include "fl/record.h"
#include "text/undo.h"
#include "ui/cmdline.h"

typedef struct RecordFix {
    Ed ed;
} RecordFix;

static void rf_open(RecordFix *f)
{
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
}

static void rf_close(RecordFix *f)
{
    yew_ed_free(&f->ed);
}

static CmdStatus rf_run(RecordFix *f, const char *name, const void *sarg,
                        u32 sarg_len, u32 count, CmdSource source)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.count = count;
    cx.count_given = count != 1U;
    cx.sarg = sarg;
    cx.sarg_len = sarg_len;
    cx.source = source;
    return yew_ed_invoke(&f->ed, id, &cx);
}

static void assert_buffer(const Ed *ed, const void *want, u64 want_len)
{
    TextIter it;
    u64 done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed->buffer.tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, ed->buffer.tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, (const u8 *)want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, ed->buffer.tb));
    }
}

static void set_macro(Ed *ed, u8 reg, const void *source, size_t len)
{
    YEW_ASSERT_EQ_I64(yew_flapi_reg_write(ed, reg, source, (u32)len, false),
                      YEW_CMD_OK);
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
    YEW_ASSERT(!yew_record_active(&f.ed));
    YEW_ASSERT(!yew_record_start(&f.ed, (u8)'1'));
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'a'));
    YEW_ASSERT(yew_record_active(&f.ed));
    YEW_ASSERT(!yew_record_start(&f.ed, (u8)'b'));
    YEW_ASSERT_EQ_U64(yew_record_status(&f.ed, &status), 1U);
    YEW_ASSERT(status.active);
    YEW_ASSERT_EQ_U64(status.reg, (u8)'a');
    YEW_ASSERT_EQ_U64(status.nevents, 0U);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    YEW_ASSERT(!yew_record_active(&f.ed));
    YEW_ASSERT_EQ_U64(yew_record_status(&f.ed, &status), 1U);
    YEW_ASSERT(!status.active);
    YEW_ASSERT_EQ_U64(status.last_reg, (u8)'a');
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_ERR_STATE);
    rf_close(&f);
}

void test_record_tap_deep_copies_binary_arguments_and_resolved_context(void)
{
    u8 payload[] = {'a', '\0', 0x80U, 'z'};
    CmdCtx cx = {0};
    RecordFix f;
    const RecEvent *event;

    rf_open(&f);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'c'));
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
    cx.source = YEW_SRC_MOUSE;
    f.ed.mode = YEW_MODE_H;
    yew_record_tap(yew_cmd_lookup("ed.file.save", 12U), &cx);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    event = &f.ed.rec.ev.data[0];
    YEW_ASSERT_EQ_U64(event->count, 7U);
    YEW_ASSERT(event->count_given);
    YEW_ASSERT_EQ_I64(event->iarg, -42);
    YEW_ASSERT(event->bang);
    YEW_ASSERT_EQ_U64(event->range_kind, YEW_REC_RANGE_SPAN);
    YEW_ASSERT(event->range_given);
    YEW_ASSERT_EQ_U64(event->range_lo, 3U);
    YEW_ASSERT_EQ_U64(event->range_hi, 9U);
    YEW_ASSERT_EQ_U64(event->mode, YEW_MODE_H);
    YEW_ASSERT_EQ_U64(event->src, YEW_SRC_MOUSE);
    YEW_ASSERT_EQ_U64(event->sarg_len, sizeof(payload));
    payload[0] = 'X';
    payload[1] = 'Y';
    YEW_ASSERT_EQ_MEM(f.ed.rec.blob.data + event->sarg_at,
                      "a\0\x80z", sizeof(payload));
    rf_close(&f);
}

void test_record_tap_drops_replay_source(void)
{
    CmdCtx cx = {0};
    RecordFix f;

    rf_open(&f);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'d'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_REPLAY;
    yew_record_tap(yew_cmd_lookup("ed.move.unit.next", 17U), &cx);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    cx.source = YEW_SRC_FLETCH;
    yew_record_tap(yew_cmd_lookup("ed.move.unit.next", 17U), &cx);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);

    YEW_ASSERT(yew_record_start(&f.ed, (u8)'e'));
    cx.sarg = "E";
    cx.sarg_len = 1U;
    cx.source = YEW_SRC_KEY;
    yew_record_tap(yew_cmd_lookup("ed.mode.enter", 13U), &cx);
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, "");
    cx.sarg = "ignored";
    cx.sarg_len = 7U;
    yew_record_tap(yew_cmd_lookup("ed.edit.insert.text", 19U), &cx);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    cx.sarg = NULL;
    cx.sarg_len = 0U;
    cx.source = YEW_SRC_CMDLINE;
    yew_record_tap(yew_cmd_lookup("ed.move.unit.next", 17U), &cx);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.data[0].src, YEW_SRC_CMDLINE);
    yew_cmdline_close(&f.ed, false);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);

    YEW_ASSERT(yew_record_start(&f.ed, (u8)'f'));
    cx.sarg = "E";
    cx.sarg_len = 1U;
    cx.source = YEW_SRC_KEY;
    yew_record_tap(yew_cmd_lookup("ed.mode.enter", 13U), &cx);
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, "");
    yew_cmdline_close(&f.ed, false);
    /* Cancelling contributes no committed CMDLINE command. */
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    rf_close(&f);
}

void test_record_dispatch_ignores_nonrecordable_commands(void)
{
    RecordFix f;

    rf_open(&f);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'e'));
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.macro.list", NULL, 0U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    rf_close(&f);
}

void test_record_shadow_escape_records_only_the_mode_change(void)
{
    RecordFix f;
    CmdId escape;

    rf_open(&f);
    f.ed.mode = YEW_MODE_I;
    f.ed.prev_unit = YEW_MODE_I;
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'e'));
    f.ed.win->shadow.live = true;
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.shadow.dismiss", NULL, 0U, 1U,
                             YEW_SRC_KEY), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);

    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.shadow.dismiss", NULL, 0U, 1U,
                             YEW_SRC_KEY), YEW_CMD_OK);
    escape = yew_cmd_lookup("ed.mode.escape", 14U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.data[0].cmd.v, escape.v);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    rf_close(&f);
}

void test_record_multicursor_dispatch_captures_one_event(void)
{
    Cursor second = {{0U}, {0U}, {0U}};
    RecordFix f;

    rf_open(&f);
    YEW_ASSERT(yew_cset_add(&f.ed.win->cs, second) == false);
    second.pos = BYTEOFF(1U);
    second.anchor = BYTEOFF(1U);
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "ab", 2U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT(yew_cset_add(&f.ed.win->cs, second));
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'f'));
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "X", 1U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.win->cs.curs.len, 2U);
    rf_close(&f);
}

void test_record_rejects_nonportable_targets_before_execution(void)
{
    static const char payload[] = "x";
    CmdCtx cx = {0};
    RecordFix f;
    Win other = {0};

    rf_open(&f);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'g'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.sarg = payload;
    cx.sarg_len = sizeof(payload) - 1U;
    cx.cursor_given = true;
    cx.source = YEW_SRC_FLETCH;
    YEW_ASSERT_EQ_I64(
        yew_cmd_invoke(yew_cmd_lookup("ed.edit.insert.text", 19U), &cx),
        YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    assert_buffer(&f.ed, "", 0U);

    cx.cursor_given = false;
    cx.win = &other;
    YEW_ASSERT_EQ_I64(
        yew_cmd_invoke(yew_cmd_lookup("ed.edit.insert.text", 19U), &cx),
        YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    assert_buffer(&f.ed, "", 0U);
    rf_close(&f);
}

void test_record_cmdline_accepts_prompt_edits_and_keeps_status_messages(void)
{
    static const char start[] = "ed.macro.record a";
    static const char stop[] = "ed.macro.stop";
    CmdCtx cx = {0};
    RecordFix f;
    Key key = {0};

    rf_open(&f);
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;

    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, "");
    yew_cmdline_paste(&f.ed, (const u8 *)start, sizeof(start) - 1U);
    YEW_ASSERT_EQ_I64(yew_cmdline_cmd_accept(&cx), YEW_CMD_OK);
    YEW_ASSERT(yew_record_active(&f.ed));
    YEW_ASSERT(f.ed.msg.active);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "recording @a");

    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, "");
    yew_record_key(&f.ed, key);
    YEW_ASSERT(f.ed.rec.in_prompt);
    yew_cmdline_paste(&f.ed, (const u8 *)stop, sizeof(stop) - 1U);
    YEW_ASSERT_EQ_I64(yew_cmdline_cmd_accept(&cx), YEW_CMD_OK);
    YEW_ASSERT(!yew_record_active(&f.ed));
    YEW_ASSERT(f.ed.msg.active);
    YEW_ASSERT(strncmp(f.ed.msg.text, "recorded @a (0 events, ", 23U) == 0);

    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.macro.record", NULL, 0U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT(f.ed.capture_cmd.v != 0U);
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = (u32)'b';
    key.ntext = 1U;
    key.text[0] = (u8)'b';
    yew_dispatch_key(&f.ed, key, 0);
    YEW_ASSERT(yew_record_active(&f.ed));
    YEW_ASSERT_EQ_U64(f.ed.rec.reg, (u8)'b');
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.macro.record", NULL, 0U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT(!yew_record_active(&f.ed));
    rf_close(&f);
}

void test_record_prompt_close_keeps_the_next_nonkey_dispatch(void)
{
    RecordFix f;
    Key key = {0};

    rf_open(&f);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'b'));
    yew_cmdline_open(&f.ed, YEW_PROMPT_SEARCH_F, "");
    yew_record_key(&f.ed, key);
    YEW_ASSERT(f.ed.rec.in_prompt);
    yew_cmdline_close(&f.ed, false);
    YEW_ASSERT(!f.ed.cmdline.active);

    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.move.unit.next", NULL, 0U, 1U,
                             YEW_SRC_MOUSE), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.move.unit.prev", NULL, 0U, 1U,
                             YEW_SRC_FLETCH), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 2U);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.data[0].src, YEW_SRC_MOUSE);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.data[1].src, YEW_SRC_FLETCH);
    rf_close(&f);
}

void test_record_range_envelope_replays_the_same_span_edit(void)
{
    CmdCtx cx = {0};
    EditCtx ec;
    RecordFix f;

    rf_open(&f);
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "abcdef", 6U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'h'));
    cx.ed = &f.ed;
    cx.win = f.ed.win;
    cx.count = 1U;
    cx.range.given = true;
    cx.range.tok = (Span){1U, 3U};
    cx.source = YEW_SRC_FLETCH;
    YEW_ASSERT_EQ_I64(
        yew_ed_invoke(&f.ed,
                      yew_cmd_lookup("ed.edit.delete.span", 19U), &cx),
        YEW_CMD_OK);
    assert_buffer(&f.ed, "adef", 4U);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 1U);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.data[0].range_kind,
                      YEW_REC_RANGE_SPAN);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    assert_buffer(&f.ed, "abcdef", 6U);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'h', 1U), YEW_CMD_OK);
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
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'a'));
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "one", 3U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    value = yew_reg_get(&f.ed.regs, (u8)'a');
    YEW_ASSERT_NOT_NULL(value);
    YEW_ASSERT_EQ_U64(value->type, YEW_REG_CHARWISE);
    YEW_ASSERT(value->bytes.len > 32U);
    YEW_ASSERT(contains_bytes(&value->bytes, "i\"one\"", 6U));
    first_len = value->bytes.len;

    YEW_ASSERT(yew_record_start(&f.ed, (u8)'A'));
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "two", 3U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    value = yew_reg_get(&f.ed.regs, (u8)'a');
    YEW_ASSERT(value->bytes.len > first_len);
    YEW_ASSERT(contains_bytes(&value->bytes, "i\"one\"", 6U));
    YEW_ASSERT(contains_bytes(&value->bytes, "i\"two\"", 6U));
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    YEW_ASSERT(yew_undo(&ec));
    assert_buffer(&f.ed, "", 0U);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'a', 1U), YEW_CMD_OK);
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
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'a', 5U), YEW_CMD_OK);
    assert_buffer(&f.ed, "xxxxx", 5U);
    ec = yew_ed_edit_ctx(&f.ed);
    for (i = 5U; i != 0U; i--) {
        YEW_ASSERT(yew_undo(&ec));
        YEW_ASSERT_EQ_U64(yew_textbuf_len(f.ed.buffer.tb), i - 1U);
    }
    YEW_ASSERT(!yew_undo(&ec));
    rf_close(&f);
}

void test_macro_replay_rolls_back_on_runtime_error(void)
{
    static const char macro[] = "@[ i\"changed\" ]\nmissing_function()\n";
    RecordFix f;

    rf_open(&f);
    set_macro(&f.ed, (u8)'b', macro, sizeof(macro) - 1U);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'b', 1U),
                      YEW_CMD_ERR_STATE);
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
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'c', 1U), YEW_CMD_OK);
    assert_buffer(&f.ed, "a", 1U);
    set_macro(&f.ed, (u8)'c', second, sizeof(second) - 1U);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'c', 1U), YEW_CMD_OK);
    assert_buffer(&f.ed, "ab", 2U);
    rf_close(&f);
}

void test_recording_during_macro_replay_captures_no_inner_commands(void)
{
    static const char macro[] = "@[ i\"x\" > del ]\n";
    RecordFix f;

    rf_open(&f);
    set_macro(&f.ed, (u8)'a', macro, sizeof(macro) - 1U);
    YEW_ASSERT(yew_record_start(&f.ed, (u8)'b'));
    YEW_ASSERT_EQ_I64(yew_macro_replay(&f.ed, (u8)'a', 1U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.rec.ev.len, 0U);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
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
    n = yew_macro_list(&f.ed, info, YEW_ARRAY_LEN(info));
    YEW_ASSERT_EQ_U64(n, 2U);
    YEW_ASSERT_EQ_U64(info[0].type, YEW_REG_CHARWISE);
    YEW_ASSERT_EQ_U64(info[0].bytes, sizeof(macro) - 1U);
    YEW_ASSERT_EQ_U64(info[1].type, YEW_REG_CHARWISE);
    YEW_ASSERT_EQ_U64(info[1].bytes, sizeof(macro) - 1U);
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
    YEW_ASSERT_EQ_I64(yew_fl_eval(&f.ed, start_in_edit,
                                  sizeof(start_in_edit) - 1U),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT(!yew_record_active(&f.ed));
    YEW_ASSERT_EQ_I64(yew_fl_eval(&f.ed, start, sizeof(start) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT(yew_record_active(&f.ed));
    YEW_ASSERT_EQ_I64(yew_fl_eval(&f.ed, stop_in_edit,
                                  sizeof(stop_in_edit) - 1U),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT(yew_record_active(&f.ed));
    YEW_ASSERT_EQ_I64(yew_fl_eval(&f.ed, nested, sizeof(nested) - 1U),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(f.ed.rec.reg, (u8)'a');
    YEW_ASSERT_EQ_I64(rf_run(&f, "ed.edit.insert.text", "q", 1U, 1U,
                             YEW_SRC_TEST), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_record_stop(&f.ed), YEW_CMD_OK);
    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_undo(&ec));
    assert_buffer(&f.ed, "", 0U);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&f.ed, replay, sizeof(replay) - 1U),
                      YEW_CMD_OK);
    assert_buffer(&f.ed, "qq", 2U);
    YEW_ASSERT(yew_undo(&ec));
    assert_buffer(&f.ed, "q", 1U);
    YEW_ASSERT(yew_undo(&ec));
    assert_buffer(&f.ed, "", 0U);
    rf_close(&f);
}
