#define _POSIX_C_SOURCE 200809L

/* Sprint 34 §4: descriptor dispatch and generic CmdCtx marshalling. */

#include "flfix.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "fl/flapi.h"
#include "fl/gc.h"
#include "fl/handle.h"
#include "fl/std.h"
#include "util/buf.h"

typedef struct MarshalFix {
    FlFix fl;
    Ed ed;
} MarshalFix;

static void mf_open(MarshalFix *f)
{
    flfix_open(&f->fl);
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    fl_ed_attach(&f->fl.vm, &f->ed, NULL);
    fl_api_init();
}

static void mf_close(MarshalFix *f)
{
    fl_ed_detach(&f->fl.vm);
    sag_ed_free(&f->ed);
    flfix_close(&f->fl);
}

static FlValue strv(FlVm *vm, const char *s, u32 n)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s, n));
}

static void mapv(FlVm *vm, FlMap *m, const char *key, FlValue value)
{
    (void)fl_map_set(vm, m, strv(vm, key, (u32)strlen(key)), value);
}

static bool error_is(FlVm *vm, const char *want)
{
    FlValue got = FL_NIL_V;
    FlValue key = strv(vm, "kind", 4U);
    const FlStr *kind;
    size_t n = strlen(want);

    if (vm->err.t != (u8)FL_MAP ||
        !fl_map_get((FlMap *)vm->err.as.o, key, &got) ||
        got.t != (u8)FL_STR)
        return false;
    kind = (const FlStr *)got.as.o;
    return kind->len == n && memcmp(kind->b, want, n) == 0;
}

static bool invoke(FlVm *vm, const char *name, FlValue *args, u32 n,
                   FlValue *out)
{
    const FlBindDesc *d = fl_api_find(name, (u32)strlen(name));
    SAG_ASSERT_NOT_NULL(d);
    return fl_api_invoke(vm, d, args, n, out);
}

void test_fl_marshal_all_command_rows_resolve_once(void)
{
    u32 i;

    sag_cmd_init();
    fl_api_init();
    for (i = 0U; i < fl_api_len; i++) {
        if (fl_api[i].cmd != NULL)
            SAG_ASSERT(fl_api[i].resolved_id.v != 0U);
        else
            SAG_ASSERT(fl_api[i].query != NULL);
    }
    sag_cmd_shutdown();
}

void test_fl_marshal_receiver_lookup_is_kind_scoped(void)
{
    MarshalFix f;
    FlValue b;
    const FlBindDesc *d;

    mf_open(&f);
    b = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));
    d = fl_api_find_receiver(b, "len", 3U);
    SAG_ASSERT_NOT_NULL(d);
    SAG_ASSERT_EQ_STR(d->fl_name, "buf.len");
    SAG_ASSERT_NULL(fl_api_find_receiver(b, "pos", 3U));
    SAG_ASSERT_NULL(fl_api_find_receiver(FL_INT_V(1), "len", 3U));
    mf_close(&f);
}

void test_fl_marshal_insert_and_text_preserve_embedded_nul(void)
{
    static const char bytes[] = {'a', '\0', 'b'};
    MarshalFix f;
    FlValue args[3];
    FlValue out = FL_NIL_V;
    FlStr *text;

    mf_open(&f);
    args[0] = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));
    args[1] = FL_INT_V(0);
    args[2] = strv(&f.fl.vm, bytes, sizeof(bytes));
    SAG_ASSERT(invoke(&f.fl.vm, "buf.insert", args, 3U, &out));
    SAG_ASSERT_EQ_U64(out.t, FL_NIL);
    SAG_ASSERT(invoke(&f.fl.vm, "buf.text", args, 1U, &out));
    SAG_ASSERT_EQ_U64(out.t, FL_STR);
    text = (FlStr *)out.as.o;
    SAG_ASSERT_EQ_U64(text->len, sizeof(bytes));
    SAG_ASSERT_EQ_MEM(text->b, bytes, sizeof(bytes));
    mf_close(&f);
}

void test_fl_marshal_ed_run_accepts_every_key_and_preserves_sarg_len(void)
{
    static const char bytes[] = {'x', '\0', 'y'};
    MarshalFix f;
    FlMap *map;
    FlValue args[2];
    FlValue out = FL_NIL_V;
    FlValue text_args[1];
    FlStr *text;

    mf_open(&f);
    map = fl_map_new(&f.fl.vm);
    fl_gc_protect(&f.fl.vm, FL_OBJ_V(FL_MAP, map));
    mapv(&f.fl.vm, map, "count", FL_INT_V(1));
    mapv(&f.fl.vm, map, "iarg", FL_INT_V(0));
    mapv(&f.fl.vm, map, "sarg", strv(&f.fl.vm, bytes, sizeof(bytes)));
    mapv(&f.fl.vm, map, "bang", FL_BOOL_V(true));
    mapv(&f.fl.vm, map, "win", fl_h_win_make(&f.ed, f.ed.win));
    args[0] = strv(&f.fl.vm, "ed.edit.insert.at", 17U);
    args[1] = FL_OBJ_V(FL_MAP, map);
    SAG_ASSERT(fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    text_args[0] = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));
    SAG_ASSERT(invoke(&f.fl.vm, "buf.text", text_args, 1U, &out));
    text = (FlStr *)out.as.o;
    SAG_ASSERT_EQ_U64(text->len, sizeof(bytes));
    SAG_ASSERT_EQ_MEM(text->b, bytes, sizeof(bytes));

    fl_map_clear(map);
    mapv(&f.fl.vm, map, "range_kind", strv(&f.fl.vm, "span", 4U));
    mapv(&f.fl.vm, map, "range_given", FL_BOOL_V(true));
    mapv(&f.fl.vm, map, "range_lo", FL_INT_V(1));
    mapv(&f.fl.vm, map, "range_hi", FL_INT_V(2));
    args[0] = strv(&f.fl.vm, "ed.edit.delete.span", 19U);
    SAG_ASSERT(fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(invoke(&f.fl.vm, "buf.text", text_args, 1U, &out));
    text = (FlStr *)out.as.o;
    SAG_ASSERT_EQ_U64(text->len, 2U);
    SAG_ASSERT_EQ_MEM(text->b, "xy", 2U);
    fl_gc_release(&f.fl.vm, 1U);
    mf_close(&f);
}

void test_fl_marshal_ed_run_rejects_unknown_key_and_bad_count(void)
{
    MarshalFix f;
    FlMap *map;
    FlValue args[2];
    FlValue out = FL_NIL_V;

    mf_open(&f);
    map = fl_map_new(&f.fl.vm);
    fl_gc_protect(&f.fl.vm, FL_OBJ_V(FL_MAP, map));
    mapv(&f.fl.vm, map, "mystery", FL_INT_V(1));
    args[0] = strv(&f.fl.vm, "ed.nop", 6U);
    args[1] = FL_OBJ_V(FL_MAP, map);
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "key"));
    f.fl.vm.err = FL_NIL_V;
    fl_map_clear(map);
    mapv(&f.fl.vm, map, "count", FL_INT_V(0));
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "type"));
    f.fl.vm.err = FL_NIL_V;
    fl_map_clear(map);
    mapv(&f.fl.vm, map, "range_kind", strv(&f.fl.vm, "span", 4U));
    mapv(&f.fl.vm, map, "range_lo", FL_INT_V(0));
    mapv(&f.fl.vm, map, "range_hi", FL_INT_V(1));
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "type"));
    fl_gc_release(&f.fl.vm, 1U);
    mf_close(&f);
}

void test_fl_marshal_ed_run_status_mapping(void)
{
    MarshalFix f;
    FlMap *map;
    FlValue args[2];
    FlValue out = FL_NIL_V;

    mf_open(&f);
    map = fl_map_new(&f.fl.vm);
    fl_gc_protect(&f.fl.vm, FL_OBJ_V(FL_MAP, map));
    args[0] = strv(&f.fl.vm, "ed.definitely.missing", 21U);
    args[1] = FL_OBJ_V(FL_MAP, map);
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "name"));
    f.fl.vm.err = FL_NIL_V;
    args[0] = strv(&f.fl.vm, "ed.repeat", 9U);
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "user"));
    f.fl.vm.err = FL_NIL_V;
    args[0] = strv(&f.fl.vm, "ed.edit.insert.text", 19U);
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "type"));
    fl_gc_release(&f.fl.vm, 1U);
    mf_close(&f);
}

void test_fl_marshal_ed_commands_is_deterministic_registry_order(void)
{
    MarshalFix f;
    FlValue first = FL_NIL_V;
    FlValue second = FL_NIL_V;
    Bytebuf a;
    Bytebuf b;

    mf_open(&f);
    bytebuf_init(&a);
    bytebuf_init(&b);
    SAG_ASSERT(fl_api_ed_commands(&f.fl.vm, NULL, 0U, &first));
    fl_gc_protect(&f.fl.vm, first);
    SAG_ASSERT(fl_fmt_repr(&f.fl.vm, &a, first));
    SAG_ASSERT(fl_api_ed_commands(&f.fl.vm, NULL, 0U, &second));
    SAG_ASSERT(fl_fmt_repr(&f.fl.vm, &b, second));
    SAG_ASSERT_EQ_U64(a.len, b.len);
    SAG_ASSERT_EQ_MEM(a.data, b.data, a.len);
    SAG_ASSERT(((FlList *)first.as.o)->n == sag_cmd_count());
    fl_gc_release(&f.fl.vm, 1U);
    bytebuf_free(&b);
    bytebuf_free(&a);
    mf_close(&f);
}

void test_fl_marshal_ed_run_enforces_caps_and_rejects_nul_paths(void)
{
    static const char bad_path[] = {'x', '\0', 'y'};
    MarshalFix f;
    FlMap *map;
    FlValue args[2];
    FlValue out = FL_NIL_V;

    mf_open(&f);
    map = fl_map_new(&f.fl.vm);
    fl_gc_protect(&f.fl.vm, FL_OBJ_V(FL_MAP, map));
    mapv(&f.fl.vm, map, "sarg", strv(&f.fl.vm, "x", 1U));
    args[0] = strv(&f.fl.vm, "ed.buf.open", 11U);
    args[1] = FL_OBJ_V(FL_MAP, map);
    f.fl.vm.root_origin.caps = 0U;
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "capability"));
    f.fl.vm.err = FL_NIL_V;
    f.fl.vm.root_origin.caps = FL_CAP_FS_READ;
    fl_map_clear(map);
    mapv(&f.fl.vm, map, "sarg",
         strv(&f.fl.vm, bad_path, (u32)sizeof(bad_path)));
    SAG_ASSERT(!fl_api_ed_run(&f.fl.vm, args, 2U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "type"));
    fl_gc_release(&f.fl.vm, 1U);
    mf_close(&f);
}

void test_fl_marshal_cursor_mutation_targets_the_receiver(void)
{
    MarshalFix f;
    FlValue args[2];
    FlValue out = FL_NIL_V;
    Cursor second = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    u32 target;
    u32 other;
    u64 other_before;

    mf_open(&f);
    args[0] = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));
    args[1] = FL_INT_V(0);
    {
        FlValue insert[3] = {args[0], FL_INT_V(0),
                             strv(&f.fl.vm, "abcd", 4U)};
        SAG_ASSERT(invoke(&f.fl.vm, "buf.insert", insert, 3U, &out));
    }
    second.pos = BYTEOFF(2U);
    second.anchor = BYTEOFF(2U);
    SAG_ASSERT(sag_cset_add(&f.ed.win->cs, second));
    target = f.ed.win->cs.primary == 0U ? 1U : 0U;
    other = target == 0U ? 1U : 0U;
    other_before = f.ed.win->cs.curs.data[other].pos.v;
    args[0] = fl_h_cur_make(&f.ed, f.ed.win, target);
    args[1] = FL_INT_V(3);
    SAG_ASSERT(invoke(&f.fl.vm, "cur.goto", args, 2U, &out));
    SAG_ASSERT_EQ_U64(f.ed.win->cs.curs.data[other].pos.v, other_before);
    SAG_ASSERT_EQ_U64(f.ed.win->cs.curs.data[target].pos.v, 3U);
    mf_close(&f);
}

void test_fl_marshal_teardown_blocks_mutation_but_not_queries(void)
{
    MarshalFix f;
    FlValue args[3];
    FlValue out = FL_NIL_V;

    mf_open(&f);
    args[0] = fl_h_buf_make(&f.ed, sag_ed_doc(&f.ed));
    args[1] = FL_INT_V(0);
    args[2] = strv(&f.fl.vm, "x", 1U);
    f.ed.fl_model_teardown = true;
    SAG_ASSERT(!invoke(&f.fl.vm, "buf.insert", args, 3U, &out));
    SAG_ASSERT(error_is(&f.fl.vm, "user"));
    f.fl.vm.err = FL_NIL_V;
    SAG_ASSERT(invoke(&f.fl.vm, "buf.len", args, 1U, &out));
    SAG_ASSERT_EQ_I64(out.as.i, 0);
    f.ed.fl_model_teardown = false;
    mf_close(&f);
}
