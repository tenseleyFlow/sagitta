#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/record.h"
#include "text/undo.h"

typedef struct NestedReplayFix {
    Ed ed;
} NestedReplayFix;

static void nrf_open(NestedReplayFix *f)
{
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
}

static void nrf_close(NestedReplayFix *f)
{
    sag_ed_free(&f->ed);
}

static void nrf_set_macro(Ed *ed, u8 reg, const char *source)
{
    SAG_ASSERT_EQ_I64(sag_flapi_reg_write(ed, reg, (const u8 *)source,
                                          (u32)strlen(source), false),
                      SAG_CMD_OK);
}

static CmdStatus nrf_run(NestedReplayFix *f, const char *name,
                         const char *sarg, CmdSource source)
{
    CmdCtx cx = {0};
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));

    SAG_ASSERT(id.v != 0U);
    cx.ed = &f->ed;
    cx.win = f->ed.win;
    cx.count = 1U;
    cx.sarg = sarg;
    cx.sarg_len = sarg == NULL ? 0U : (u32)strlen(sarg);
    cx.source = source;
    return sag_ed_invoke(&f->ed, id, &cx);
}

static void nrf_assert_buffer(const Ed *ed, const char *want)
{
    TextIter it;
    u64 done = 0U;
    u64 want_len = (u64)strlen(want);

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
        SAG_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            SAG_ASSERT(sag_textiter_advance(&it, ed->buffer.tb));
    }
}

static bool nrf_contains(const Bytebuf *bytes, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    if (n > bytes->len)
        return false;
    for (i = 0U; i <= bytes->len - n; i++)
        if (memcmp(bytes->data + i, needle, n) == 0)
            return true;
    return false;
}

void test_macro_nested_replay_commits_as_outer_undo_step(void)
{
    static const char inner[] = "@[ i\"B\" ]\n";
    static const char outer[] =
        "import ed\n"
        "@[ i\"A\" ]\n"
        "ed.run(\"ed.macro.replay\", { sarg: \"b\" })\n"
        "@[ i\"C\" ]\n";
    EditCtx ec;
    NestedReplayFix f;

    nrf_open(&f);
    nrf_set_macro(&f.ed, (u8)'b', inner);
    nrf_set_macro(&f.ed, (u8)'a', outer);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'a', 1U), SAG_CMD_OK);
    nrf_assert_buffer(&f.ed, "ABC");
    ec = sag_ed_edit_ctx(&f.ed);
    SAG_ASSERT(sag_undo(&ec));
    nrf_assert_buffer(&f.ed, "");
    SAG_ASSERT(!sag_undo(&ec));
    nrf_close(&f);
}

void test_macro_nested_replay_outer_error_rolls_back_every_edit(void)
{
    static const char inner[] = "@[ i\"B\" ]\n";
    static const char outer[] =
        "import ed\n"
        "@[ i\"A\" ]\n"
        "ed.run(\"ed.macro.replay\", { sarg: \"b\" })\n"
        "@[ i\"C\" ]\n"
        "missing_function()\n";
    NestedReplayFix f;

    nrf_open(&f);
    nrf_set_macro(&f.ed, (u8)'b', inner);
    nrf_set_macro(&f.ed, (u8)'a', outer);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'a', 1U),
                      SAG_CMD_ERR_STATE);
    nrf_assert_buffer(&f.ed, "");
    nrf_close(&f);
}

void test_record_replay_last_captures_resolved_register(void)
{
    static const char original[] = "@[ i\"x\" ]\n";
    const RegVal *recorded;
    EditCtx ec;
    NestedReplayFix f;

    nrf_open(&f);
    nrf_set_macro(&f.ed, (u8)'a', original);
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'a', 1U), SAG_CMD_OK);
    ec = sag_ed_edit_ctx(&f.ed);
    SAG_ASSERT(sag_undo(&ec));
    nrf_assert_buffer(&f.ed, "");

    SAG_ASSERT(sag_record_start(&f.ed, (u8)'b'));
    SAG_ASSERT_EQ_I64(nrf_run(&f, "ed.macro.replay_last", NULL,
                              SAG_SRC_KEY), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_record_stop(&f.ed), SAG_CMD_OK);
    recorded = sag_reg_get(&f.ed.regs, (u8)'b');
    SAG_ASSERT_NOT_NULL(recorded);
    SAG_ASSERT(nrf_contains(&recorded->bytes, "ed.macro.replay"));
    SAG_ASSERT(nrf_contains(&recorded->bytes, "sarg: \"a\""));
    SAG_ASSERT(!nrf_contains(&recorded->bytes, "replay_last"));

    SAG_ASSERT(sag_undo(&ec));
    nrf_assert_buffer(&f.ed, "");
    SAG_ASSERT_EQ_U64(f.ed.rec.last_reg, (u8)'b');
    SAG_ASSERT_EQ_I64(sag_macro_replay(&f.ed, (u8)'b', 1U), SAG_CMD_OK);
    nrf_assert_buffer(&f.ed, "x");
    nrf_close(&f);
}
