#include "harness.h"
#include "flfix.h"

#include <string.h>
#include <sys/stat.h>

#include "edit/ed.h"
#include "fl/flapi.h"
#include "fl/gc.h"
#include "ui/layout.h"

static CmdStatus run_cmd(Ed *ed, Win *win, const char *name,
                         const char *sarg, u32 sarg_len, u32 count,
                         Span range, const CmdCursorArg *cursors,
                         u32 ncursors)
{
    CmdCtx cx = {0};
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));

    SAG_ASSERT(id.v != 0U);
    cx.ed = ed;
    cx.win = win;
    cx.count = count;
    cx.count_given = count != 1U;
    cx.sarg = sarg;
    cx.sarg_len = sarg_len;
    cx.range.given = range.lo != range.hi;
    cx.range.tok = range;
    cx.cursor_args = cursors;
    cx.cursor_args_len = ncursors;
    cx.source = SAG_SRC_TEST;
    return sag_cmd_invoke(id, &cx);
}

void test_fl_api_win_split_and_focus_are_registry_commands(void)
{
    Ed ed;
    Win *old;
    Win *created;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    old = ed.win;
    SAG_ASSERT_EQ_I64(run_cmd(&ed, old, "ed.win.split", "h", 1U, 1U,
                              (Span){0U, 0U}, NULL, 0U), SAG_CMD_OK);
    created = ed.win;
    SAG_ASSERT(created != old);
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(ed.pane_root), 2U);
    SAG_ASSERT_EQ_I64(run_cmd(&ed, old, "ed.win.focus", NULL, 0U, 1U,
                              (Span){0U, 0U}, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT(ed.win == old);
    SAG_ASSERT_EQ_I64(run_cmd(&ed, old, "ed.win.split", "diagonal", 8U,
                              1U, (Span){0U, 0U}, NULL, 0U),
                      SAG_CMD_ERR_ARG);
    sag_ed_free(&ed);
}

void test_fl_api_set_cursors_replaces_and_normalizes_once(void)
{
    static const CmdCursorArg cursors[] = {
        {{3U}, {3U}, 3U},
        {{0U}, {0U}, 0U},
        {{3U}, {3U}, 3U}
    };
    Ed ed;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.edit.insert.at", "abcd", 4U,
                              1U, (Span){0U, 0U}, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.cursor.set_many", NULL, 0U,
                              1U, (Span){0U, 0U}, cursors,
                              SAG_ARRAY_LEN(cursors)), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.len, 2U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 0U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[1].pos.v, 3U);
    sag_ed_free(&ed);
}

void test_fl_api_cursor_move_targets_one_cursor_and_honors_count(void)
{
    static const CmdCursorArg cursors[] = {
        {{0U}, {0U}, 0U},
        {{3U}, {3U}, 3U}
    };
    CmdCtx cx = {0};
    Ed ed;
    CmdId id;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.edit.insert.at", "abcd", 4U,
                              1U, (Span){0U, 0U}, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.cursor.set_many", NULL, 0U,
                              1U, (Span){0U, 0U}, cursors,
                              SAG_ARRAY_LEN(cursors)), SAG_CMD_OK);
    id = sag_cmd_lookup("ed.cursor.move", 14U);
    SAG_ASSERT(id.v != 0U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.cursor_given = true;
    cx.cursor_index = 0U;
    cx.count = 2U;
    cx.count_given = true;
    cx.sarg = "char:next";
    cx.sarg_len = 9U;
    cx.source = SAG_SRC_TEST;
    SAG_ASSERT_EQ_I64(sag_cmd_invoke(id, &cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 2U);
    SAG_ASSERT_EQ_U64(ed.win->cs.curs.data[1].pos.v, 3U);
    sag_ed_free(&ed);
}

void test_fl_api_span_yank_uses_requested_register(void)
{
    Ed ed;
    RegVal *named;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.edit.insert.at", "alpha", 5U,
                              1U, (Span){0U, 0U}, NULL, 0U), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.edit.yank", "a", 1U, 1U,
                              (Span){1U, 4U}, NULL, 0U), SAG_CMD_OK);
    named = sag_reg_get(&ed.regs, (u8)'a');
    SAG_ASSERT_NOT_NULL(named);
    SAG_ASSERT_EQ_U64(named->bytes.len, 3U);
    SAG_ASSERT_EQ_MEM(named->bytes.data, "lph", 3U);
    SAG_ASSERT_EQ_I64(run_cmd(&ed, ed.win, "ed.edit.yank", "0", 1U, 1U,
                              (Span){1U, 4U}, NULL, 0U), SAG_CMD_ERR_ARG);
    sag_ed_free(&ed);
}

typedef struct ApiFix {
    FlFix fl;
    Ed ed;
} ApiFix;

static void api_open(ApiFix *f)
{
    flfix_open(&f->fl);
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    fl_ed_attach(&f->fl.vm, &f->ed, NULL);
    fl_api_init();
}

static void api_close(ApiFix *f)
{
    fl_ed_detach(&f->fl.vm);
    sag_ed_free(&f->ed);
    flfix_close(&f->fl);
}

static FlValue api_string(FlVm *vm, const char *s)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s, (u32)strlen(s)));
}

static bool api_invoke(ApiFix *f, const char *name, FlValue *args, u32 n,
                       FlValue *out)
{
    const FlBindDesc *d = fl_api_find(name, (u32)strlen(name));

    SAG_ASSERT_NOT_NULL(d);
    return fl_api_invoke(&f->fl.vm, d, args, n, out);
}

void test_fl_api_buf_metadata_queries_are_truthful(void)
{
    ApiFix f;
    FlValue args[1];
    FlValue out = FL_NIL_V;
    Buffer *buffer;

    api_open(&f);
    buffer = sag_ed_doc(&f.ed);
    args[0] = fl_h_buf_make(&f.ed, buffer);
    SAG_ASSERT(api_invoke(&f, "buf.lang", args, 1U, &out));
    SAG_ASSERT_EQ_U64(out.t, FL_NIL);
    buffer->meta.exists = true;
    buffer->meta.mode = S_IRUSR;
    SAG_ASSERT(api_invoke(&f, "buf.readonly", args, 1U, &out));
    SAG_ASSERT_EQ_U64(out.t, FL_BOOL);
    SAG_ASSERT(out.as.b);
    buffer->meta.mode |= S_IWUSR;
    SAG_ASSERT(api_invoke(&f, "buf.readonly", args, 1U, &out));
    SAG_ASSERT(!out.as.b);
    api_close(&f);
}

void test_fl_api_missing_rows_have_typed_argmaps(void)
{
    const FlBindDesc *d;
    u32 i;

    sag_cmd_init();
    fl_api_init();
    d = fl_api_find("win.split", 9U);
    SAG_ASSERT_NOT_NULL(d);
    SAG_ASSERT_EQ_U64(d->argmap[0], FL_ARG_STR);
    d = fl_api_find("win.set_cursors", 15U);
    SAG_ASSERT_NOT_NULL(d);
    SAG_ASSERT_EQ_U64(d->argmap[0], FL_ARG_LIST);
    d = fl_api_find("cur.move", 8U);
    SAG_ASSERT_NOT_NULL(d);
    SAG_ASSERT_EQ_U64(d->argmap[0], FL_ARG_STR);
    SAG_ASSERT_EQ_U64(d->argmap[1], FL_ARG_STR);
    SAG_ASSERT_EQ_U64(d->argmap[2], FL_ARG_COUNT);
    d = fl_api_find("span.replace_lines", 18U);
    SAG_ASSERT_NOT_NULL(d);
    SAG_ASSERT_EQ_U64(d->argmap[0], FL_ARG_LIST);
    d = fl_api_find("span.yank", 9U);
    SAG_ASSERT_NOT_NULL(d);
    SAG_ASSERT_EQ_U64(d->argmap[0], FL_ARG_STR);
    for (i = 0U; i < fl_api_len; i++) {
        u32 base = fl_api[i].recv == (u8)FL_H_NONE ? 0U : 1U;
        u32 arg;

        if (fl_api[i].cmd == NULL)
            continue;
        SAG_ASSERT(fl_api[i].resolved_id.v != 0U);
        SAG_ASSERT(fl_api[i].nmax >= base);
        SAG_ASSERT(fl_api[i].nmax - base <=
                   SAG_ARRAY_LEN(fl_api[i].argmap));
        for (arg = 0U; arg < fl_api[i].nmax - base; arg++)
            SAG_ASSERT(fl_api[i].argmap[arg] != (u8)FL_ARG_NONE);
    }
    sag_cmd_shutdown();
}

void test_fl_api_rows_replace_lines_yank_and_move(void)
{
    ApiFix f;
    FlList *lines;
    FlValue args[4];
    FlValue out = FL_NIL_V;
    FlValue buf;
    FlValue span;
    FlValue cursor;
    RegVal *named;

    api_open(&f);
    buf = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));
    args[0] = buf;
    args[1] = FL_INT_V(0);
    args[2] = api_string(&f.fl.vm, "old");
    SAG_ASSERT(api_invoke(&f, "buf.insert", args, 3U, &out));
    span = fl_h_span_make(&f.ed, sag_ed_doc(&f.ed), 0U, 3U);
    lines = fl_list_new(&f.fl.vm);
    fl_gc_protect(&f.fl.vm, FL_OBJ_V(FL_LIST, lines));
    SAG_ASSERT(fl_list_push(&f.fl.vm, lines, api_string(&f.fl.vm, "one")));
    SAG_ASSERT(fl_list_push(&f.fl.vm, lines, api_string(&f.fl.vm, "two")));
    args[0] = span;
    args[1] = FL_OBJ_V(FL_LIST, lines);
    SAG_ASSERT(api_invoke(&f, "span.replace_lines", args, 2U, &out));
    args[0] = buf;
    SAG_ASSERT(api_invoke(&f, "buf.text", args, 1U, &out));
    SAG_ASSERT_EQ_U64(((FlStr *)out.as.o)->len, 7U);
    SAG_ASSERT_EQ_MEM(((FlStr *)out.as.o)->b, "one\ntwo", 7U);
    span = fl_h_span_make(&f.ed, sag_ed_doc(&f.ed), 0U, 3U);
    args[0] = span;
    args[1] = api_string(&f.fl.vm, "a");
    SAG_ASSERT(api_invoke(&f, "span.yank", args, 2U, &out));
    named = sag_reg_get(&f.ed.regs, (u8)'a');
    SAG_ASSERT_EQ_U64(named->bytes.len, 3U);
    SAG_ASSERT_EQ_MEM(named->bytes.data, "one", 3U);
    cursor = fl_h_cur_make(&f.ed, f.ed.win, f.ed.win->cs.primary);
    args[0] = cursor;
    args[1] = FL_INT_V(0);
    SAG_ASSERT(api_invoke(&f, "cur.goto", args, 2U, &out));
    args[0] = cursor;
    args[1] = api_string(&f.fl.vm, "char");
    args[2] = api_string(&f.fl.vm, "next");
    args[3] = FL_INT_V(2);
    SAG_ASSERT(api_invoke(&f, "cur.move", args, 4U, &out));
    args[0] = cursor;
    SAG_ASSERT(api_invoke(&f, "cur.pos", args, 1U, &out));
    SAG_ASSERT_EQ_I64(out.as.i, 2);
    fl_gc_release(&f.fl.vm, 1U);
    api_close(&f);
}

void test_fl_api_rows_split_focus_and_set_cursors(void)
{
    ApiFix f;
    FlList *cursors;
    FlValue args[2];
    FlValue out = FL_NIL_V;
    FlValue old_win;
    FlValue new_win;
    FlValue cursor;
    Win *created;

    api_open(&f);
    sag_layout_compute(f.ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    old_win = fl_h_win_make(&f.ed, f.ed.win);
    cursor = fl_h_cur_make(&f.ed, f.ed.win, f.ed.win->cs.primary);
    args[0] = old_win;
    args[1] = api_string(&f.fl.vm, "h");
    SAG_ASSERT(api_invoke(&f, "win.split", args, 2U, &new_win));
    created = fl_h_win(&f.fl.vm, new_win);
    SAG_ASSERT_NOT_NULL(created);
    SAG_ASSERT(created == f.ed.win);
    args[0] = old_win;
    SAG_ASSERT(api_invoke(&f, "win.focus", args, 1U, &out));
    SAG_ASSERT(f.ed.win != created);
    cursors = fl_list_new(&f.fl.vm);
    fl_gc_protect(&f.fl.vm, FL_OBJ_V(FL_LIST, cursors));
    SAG_ASSERT(fl_list_push(&f.fl.vm, cursors, cursor));
    args[0] = new_win;
    args[1] = FL_OBJ_V(FL_LIST, cursors);
    SAG_ASSERT(api_invoke(&f, "win.set_cursors", args, 2U, &out));
    SAG_ASSERT_EQ_U64(created->cs.curs.len, 1U);
    fl_gc_release(&f.fl.vm, 1U);
    api_close(&f);
}
