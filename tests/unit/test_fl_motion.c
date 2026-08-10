#include "flfix.h"

#include <stdio.h>
#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "fl/motion_tab.h"

typedef struct MotionTap {
    char word[20];
    u32 count;
    bool count_given;
    u8 sarg[16];
    u32 sarg_len;
    CmdSource source;
} MotionTap;

typedef struct MotionFix {
    FlFix fl;
    Ed ed;
} MotionFix;

static MotionTap taps[32];
static u32 ntaps;

static void motion_tap(CmdId id, const CmdCtx *cx)
{
    const CmdDesc *desc = yew_cmd_desc(id);
    MotionTap *tap;
    size_t word_len;

    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_NOT_NULL(desc->word);
    YEW_ASSERT(ntaps < YEW_ARRAY_LEN(taps));
    tap = &taps[ntaps++];
    (void)memset(tap, 0, sizeof(*tap));
    word_len = strlen(desc->word);
    YEW_ASSERT(word_len < sizeof(tap->word));
    (void)memcpy(tap->word, desc->word, word_len + 1U);
    tap->count = cx->count;
    tap->count_given = cx->count_given;
    tap->sarg_len = cx->sarg_len;
    YEW_ASSERT(cx->sarg_len <= sizeof(tap->sarg));
    if (cx->sarg_len != 0U)
        (void)memcpy(tap->sarg, cx->sarg, cx->sarg_len);
    tap->source = cx->source;
}

static void mf_open(MotionFix *f)
{
    flfix_open(&f->fl);
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    ntaps = 0U;
    yew_cmd_set_record_tap(motion_tap);
}

static void mf_close(MotionFix *f)
{
    yew_cmd_set_record_tap(NULL);
    yew_ed_free(&f->ed);
    flfix_close(&f->fl);
}

static FlMotionProg motion_prog(FlMotionOp *ops, u32 n)
{
    FlMotionProg prog = {0};

    prog.op = ops;
    prog.n = n;
    return prog;
}

static void assert_text(const TextBuf *tb, const u8 *want, u64 want_len)
{
    TextIter it;
    u64 done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

static void error_field(FlVm *vm, const char *key, char *out, size_t cap)
{
    FlValue got = FL_NIL_V;
    FlStr *k = fl_str_new(vm, key, (u32)strlen(key));

    out[0] = '\0';
    if (vm->err.t != (u8)FL_MAP)
        return;
    if (!fl_map_get((FlMap *)vm->err.as.o, FL_OBJ_V(FL_STR, k), &got) ||
        got.t != (u8)FL_STR)
        return;
    (void)snprintf(out, cap, "%.*s", (int)((FlStr *)got.as.o)->len,
                   ((FlStr *)got.as.o)->b);
}

void test_fl_motion_units_arrows_words_and_counts_use_cmdwords(void)
{
    MotionFix f;
    FlMotionOp ops[] = {
        {(u8)FL_MOTION_UNIT, (u8)'l', 0U, 1U, 0U},
        {(u8)FL_MOTION_UNIT, (u8)'w', 0U, 1U, 0U},
        {(u8)FL_MOTION_UNIT, (u8)'b', 0U, 1U, 0U},
        {(u8)FL_MOTION_UNIT, (u8)'c', 0U, 1U, 0U},
        {(u8)FL_MOTION_ARROW, (u8)'>',
         FL_MOTION_F_COUNT_GIVEN, 4U, 0U},
        {(u8)FL_MOTION_ARROW, (u8)'v', FL_MOTION_F_ALT, 1U, 0U},
        {(u8)FL_MOTION_WORD, 0U, FL_MOTION_F_COUNT_GIVEN, 1U, 0U}
    };
    FlMotionProg prog;

    mf_open(&f);
    ops[6].arg = yew_intern_cstr(&f.fl.in, "line_home");
    prog = motion_prog(ops, YEW_ARRAY_LEN(ops));
    YEW_ASSERT(fl_motion_exec(&f.fl.vm, &f.ed, f.ed.win, &prog));
    YEW_ASSERT_EQ_U64(ntaps, 7U);
    YEW_ASSERT_EQ_STR(taps[0].word, "mode");
    YEW_ASSERT_EQ_MEM(taps[0].sarg, "L", 1U);
    YEW_ASSERT_EQ_MEM(taps[1].sarg, "W", 1U);
    YEW_ASSERT_EQ_MEM(taps[2].sarg, "B", 1U);
    YEW_ASSERT_EQ_MEM(taps[3].sarg, "C", 1U);
    YEW_ASSERT_EQ_STR(taps[4].word, "unit_next");
    YEW_ASSERT_EQ_U64(taps[4].count, 4U);
    YEW_ASSERT(taps[4].count_given);
    YEW_ASSERT_EQ_STR(taps[5].word, "unit_down_alt");
    YEW_ASSERT_EQ_U64(taps[5].count, 1U);
    YEW_ASSERT(!taps[5].count_given);
    YEW_ASSERT_EQ_STR(taps[6].word, "line_home");
    YEW_ASSERT(taps[6].count_given);
    YEW_ASSERT_EQ_U64(taps[6].source, YEW_SRC_FLETCH);
    mf_close(&f);
}

void test_fl_motion_highlight_extent_escapes_before_following_word(void)
{
    MotionFix f;
    FlMotionOp ops[] = {
        {(u8)FL_MOTION_HIGHLIGHT, 0U, 0U, 1U, 1U},
        {(u8)FL_MOTION_ARROW, (u8)'>', 0U, 1U, 0U},
        {(u8)FL_MOTION_ARROW, (u8)'<', 0U, 1U, 0U}
    };
    FlMotionProg prog = motion_prog(ops, YEW_ARRAY_LEN(ops));

    mf_open(&f);
    YEW_ASSERT(fl_motion_exec(&f.fl.vm, &f.ed, f.ed.win, &prog));
    YEW_ASSERT_EQ_U64(ntaps, 4U);
    YEW_ASSERT_EQ_STR(taps[0].word, "mode");
    YEW_ASSERT_EQ_MEM(taps[0].sarg, "H", 1U);
    YEW_ASSERT_EQ_STR(taps[1].word, "unit_next");
    YEW_ASSERT_EQ_STR(taps[2].word, "escape");
    YEW_ASSERT_EQ_STR(taps[3].word, "unit_prev");
    mf_close(&f);
}

void test_fl_motion_insert_is_one_binary_safe_command(void)
{
    static const char bytes[] = {'a', '\0', 'b'};
    MotionFix f;
    FlMotionOp op = {(u8)FL_MOTION_INSERT, 0U, 0U, 1U, 0U};
    FlMotionProg prog = motion_prog(&op, 1U);

    mf_open(&f);
    op.arg = yew_intern(&f.fl.in, bytes, sizeof(bytes));
    YEW_ASSERT(fl_motion_exec(&f.fl.vm, &f.ed, f.ed.win, &prog));
    YEW_ASSERT_EQ_U64(ntaps, 1U);
    YEW_ASSERT_EQ_STR(taps[0].word, "insert");
    YEW_ASSERT_EQ_U64(taps[0].sarg_len, sizeof(bytes));
    YEW_ASSERT_EQ_MEM(taps[0].sarg, bytes, sizeof(bytes));
    assert_text(f.ed.buffer.tb, (const u8 *)bytes, sizeof(bytes));
    mf_close(&f);
}

void test_fl_motion_del_and_esc_route_through_registry(void)
{
    MotionFix f;
    FlMotionOp ops[] = {
        {(u8)FL_MOTION_DEL, 0U, 0U, 1U, 0U},
        {(u8)FL_MOTION_ESC, 0U, 0U, 1U, 0U}
    };
    FlMotionProg prog = motion_prog(ops, YEW_ARRAY_LEN(ops));

    mf_open(&f);
    YEW_ASSERT(fl_motion_exec(&f.fl.vm, &f.ed, f.ed.win, &prog));
    YEW_ASSERT_EQ_U64(ntaps, 2U);
    YEW_ASSERT_EQ_STR(taps[0].word, "delete_unit");
    YEW_ASSERT_EQ_STR(taps[1].word, "escape");
    mf_close(&f);
}

void test_fl_motion_unknown_word_raises_name(void)
{
    MotionFix f;
    FlMotionOp op = {(u8)FL_MOTION_WORD, 0U, 0U, 1U, 0U};
    FlMotionProg prog = motion_prog(&op, 1U);
    char kind[32];
    char msg[128];

    mf_open(&f);
    op.arg = yew_intern_cstr(&f.fl.in, "no_such_word");
    YEW_ASSERT(!fl_motion_exec(&f.fl.vm, &f.ed, f.ed.win, &prog));
    error_field(&f.fl.vm, "kind", kind, sizeof(kind));
    error_field(&f.fl.vm, "msg", msg, sizeof(msg));
    YEW_ASSERT_EQ_STR(kind, "name");
    YEW_ASSERT_EQ_STR(msg, "no command has word 'no_such_word'");
    YEW_ASSERT_EQ_U64(ntaps, 0U);
    mf_close(&f);
}
