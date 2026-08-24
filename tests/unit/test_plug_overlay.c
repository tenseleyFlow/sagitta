#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/gc.h"
#include "fl/flruntime.h"
#include "fl/origin.h"
#include "fl/vm.h"
#include "mod/plug/internal.h"
#include "mod/plug/overlay.h"
#include "syn/attr.h"
#include "text/piece.h"
#include "util/intern.h"

typedef struct OverlayFix {
    Ed ed;
    PlugSys sys;
    Plug plug;
    Plug *row;
    PlugValueReg reg;
    FlNative native;
    u32 calls;
    i64 lo_line;
    i64 hi_line;
} OverlayFix;

typedef struct SeenSpan {
    Span span[8];
    u8 attr[8];
    u32 n;
} SeenSpan;

static OverlayFix *active_fix;
static u32 callback_mode;

static FlValue string_value(FlVm *vm, const char *text)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, text, (u32)strlen(text)));
}

static void row_field(FlVm *vm, FlMap *map, const char *name, i64 value)
{
    FlValue key = string_value(vm, name);

    fl_gc_protect(vm, key);
    YEW_ASSERT(fl_map_set(vm, map, key, FL_INT_V(value)));
    fl_gc_release(vm, 1U);
}

static void result_row(FlVm *vm, FlList *list, i64 lo, i64 hi, i64 attr)
{
    FlMap *map = fl_map_new(vm);
    FlValue value = FL_OBJ_V(FL_MAP, map);

    fl_gc_protect(vm, value);
    row_field(vm, map, "lo", lo);
    row_field(vm, map, "hi", hi);
    row_field(vm, map, "attr", attr);
    YEW_ASSERT(fl_list_push(vm, list, value));
    fl_gc_release(vm, 1U);
}

static bool overlay_native(FlVm *vm, FlValue *args, u32 nargs,
                           FlValue *out)
{
    FlList *list;

    YEW_ASSERT_NOT_NULL(active_fix);
    active_fix->calls++;
    YEW_ASSERT_EQ_U64(nargs, 4U);
    YEW_ASSERT_EQ_I64(args[0].t, FL_WIN);
    YEW_ASSERT_EQ_I64(args[1].t, FL_BUF);
    YEW_ASSERT_EQ_I64(args[2].t, FL_INT);
    YEW_ASSERT_EQ_I64(args[3].t, FL_INT);
    active_fix->lo_line = args[2].as.i;
    active_fix->hi_line = args[3].as.i;
    if (callback_mode == 1U)
        return fl_raise(vm, "test", "overlay exploded");
    if (callback_mode == 2U) {
        *out = FL_INT_V(7);
        return true;
    }
    list = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    result_row(vm, list, -9, 3, YEW_ATTR_WARNING);
    result_row(vm, list, 2, 99, YEW_ATTR_ERROR);
    result_row(vm, list, 99, 120, YEW_ATTR_STRING);
    *out = FL_OBJ_V(FL_LIST, list);
    fl_gc_release(vm, 1U);
    return true;
}

static void see_span(void *ctx, Span span, u8 attr)
{
    SeenSpan *seen = ctx;

    YEW_ASSERT(seen->n < (u32)YEW_ARRAY_LEN(seen->span));
    seen->span[seen->n] = span;
    seen->attr[seen->n] = attr;
    seen->n++;
}

static void overlay_open(OverlayFix *f)
{
    FlVm *vm;

    (void)memset(f, 0, sizeof(*f));
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_textbuf_insert(f->ed.buffer.tb, BYTEOFF(0U),
                       (const u8 *)"abc\ndef\n", 8U);
    vm = yew_fl_vm(&f->ed);
    YEW_ASSERT_NOT_NULL(vm);
    f->row = &f->plug;
    f->sys.v = &f->row;
    f->sys.n = 1U;
    f->sys.regs = &f->reg;
    f->sys.nregs = 1U;
    f->plug.mf.name_text = "overlay-test";
    f->plug.st = PLUG_ENABLED;
    f->plug.winner = true;
    f->plug.origin_id = fl_origin_register(&f->ed, FL_ORIGIN_PLUGIN,
                                            "overlay-test", 0U);
    f->native.h.t = (u8)FL_NATIVE;
    f->native.fn = overlay_native;
    f->native.name_id = yew_intern_cstr(vm->in, "overlay-test.callback");
    f->native.min_ar = 4U;
    f->native.max_ar = 4U;
    f->reg = (PlugValueReg){1U, f->plug.origin_id, (u8)REG_OVERLAY,
                            FL_OBJ_V(FL_NATIVE, &f->native), true};
    f->ed.plug = &f->sys;
    active_fix = f;
    callback_mode = 0U;
}

static void overlay_close(OverlayFix *f)
{
    active_fix = NULL;
    free(f->plug.last_error);
    f->plug.last_error = NULL;
    f->ed.plug = NULL;
    yew_ed_free(&f->ed);
}

void test_plug_overlay_shape_clips_to_exclusive_visible_lines(void)
{
    OverlayFix f;
    SeenSpan seen = {0};

    overlay_open(&f);
    yew_plug_overlay_run(&f.ed, f.ed.win, LINENO(0U), LINENO(1U),
                         see_span, &seen);
    YEW_ASSERT_EQ_U64(f.calls, 1U);
    YEW_ASSERT_EQ_I64(f.lo_line, 0);
    YEW_ASSERT_EQ_I64(f.hi_line, 1);
    YEW_ASSERT_EQ_U64(seen.n, 2U);
    YEW_ASSERT_EQ_U64(seen.span[0].lo, 0U);
    YEW_ASSERT_EQ_U64(seen.span[0].hi, 3U);
    YEW_ASSERT_EQ_I64(seen.attr[0], YEW_ATTR_WARNING);
    YEW_ASSERT_EQ_U64(seen.span[1].lo, 2U);
    YEW_ASSERT_EQ_U64(seen.span[1].hi, 4U);
    YEW_ASSERT_EQ_I64(seen.attr[1], YEW_ATTR_ERROR);
    overlay_close(&f);
}

void test_plug_overlay_errors_are_contained_and_disabled_regs_do_not_run(void)
{
    OverlayFix f;
    SeenSpan seen = {0};

    overlay_open(&f);
    callback_mode = 1U;
    yew_plug_overlay_run(&f.ed, f.ed.win, LINENO(0U), LINENO(2U),
                         see_span, &seen);
    YEW_ASSERT_EQ_U64(f.calls, 1U);
    YEW_ASSERT_EQ_U64(f.plug.err_count, 1U);
    YEW_ASSERT_EQ_U64(seen.n, 0U);

    callback_mode = 2U;
    yew_plug_overlay_run(&f.ed, f.ed.win, LINENO(0U), LINENO(2U),
                         see_span, &seen);
    YEW_ASSERT_EQ_U64(f.calls, 2U);
    YEW_ASSERT_EQ_U64(f.plug.err_count, 2U);

    f.reg.active = false;
    f.reg.value = FL_NIL_V;
    yew_plug_overlay_run(&f.ed, f.ed.win, LINENO(0U), LINENO(2U),
                         see_span, &seen);
    YEW_ASSERT_EQ_U64(f.calls, 2U);
    YEW_ASSERT_EQ_U64(seen.n, 0U);
    overlay_close(&f);
}
