/* Sprint 34 §3: handle receiver calls are FIELD_GET sugar over fl_api. */

#include "flfix.h"

#include "edit/ed.h"
#include "fl/flapi.h"
#include "fl/gc.h"

typedef struct ReceiverFix {
    FlFix fl;
    Ed ed;
} ReceiverFix;

static void receiver_open(ReceiverFix *f)
{
    flfix_open(&f->fl);
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    fl_ed_attach(&f->fl.vm, &f->ed, NULL);
}

static void receiver_close(ReceiverFix *f)
{
    fl_ed_detach(&f->fl.vm);
    yew_ed_free(&f->ed);
    flfix_close(&f->fl);
}

void test_fl_receiver_field_get_calls_bound_handle(void)
{
    ReceiverFix f;

    receiver_open(&f);
    FL_EQ(&f.fl,
          "import buf\n"
          "let b = buf.current()\n"
          "let length = b.len\n"
          "return length()\n", "0");
    FL_EQ(&f.fl,
          "import win\n"
          "let w = win.current()\n"
          "return w.buf().len()\n", "0");
    receiver_close(&f);
}

void test_fl_receiver_headless_call_raises_handle(void)
{
    FlFix f;
    FlValue fake = {.t = FL_BUF, .as.i = 1};
    FlValue bound = FL_NIL_V;
    FlValue out = FL_NIL_V;
    FlValue kind = FL_NIL_V;
    FlValue key;

    flfix_open(&f);
    YEW_ASSERT(fl_api_bind_receiver(&f.vm, fake, "len", 3U, &bound));
    YEW_ASSERT(!fl_call(&f.vm, bound, NULL, 0U, &out));
    YEW_ASSERT_EQ_U64(f.vm.err.t, FL_MAP);
    key = FL_OBJ_V(FL_STR, fl_str_new(&f.vm, "kind", 4U));
    YEW_ASSERT(fl_map_get((FlMap *)f.vm.err.as.o, key, &kind));
    YEW_ASSERT_EQ_U64(kind.t, FL_STR);
    YEW_ASSERT_EQ_STR(((FlStr *)kind.as.o)->b, "handle");
    flfix_close(&f);
}

void test_fl_receiver_preserves_bound_arity_name(void)
{
    ReceiverFix f;

    receiver_open(&f);
    FL_EQ(&f.fl,
          "import buf\n"
          "let b = buf.current()\n"
          "return b.len(1)\n",
          "!arity: buf.len expects 0 arguments, got 1");
    receiver_close(&f);
}

void test_fl_receiver_survives_collection_without_editor_pointer(void)
{
    ReceiverFix f;
    FlValue handle;
    FlValue bound = FL_NIL_V;
    FlValue out = FL_NIL_V;
    FlNative *native;

    receiver_open(&f);
    handle = fl_h_buf_make(&f.ed, yew_ed_doc(&f.ed));
    YEW_ASSERT(fl_api_bind_receiver(&f.fl.vm, handle, "len", 3U, &bound));
    YEW_ASSERT_EQ_U64(bound.t, FL_NATIVE);
    native = (FlNative *)bound.as.o;
    YEW_ASSERT(native->has_recv != 0U);
    YEW_ASSERT_EQ_U64(native->recv.t, FL_BUF);
    YEW_ASSERT_EQ_U64(native->recv.as.i, handle.as.i);
    fl_gc_protect(&f.fl.vm, bound);
    fl_gc_collect(&f.fl.vm);
    YEW_ASSERT(fl_call(&f.fl.vm, bound, NULL, 0U, &out));
    YEW_ASSERT_EQ_U64(out.t, FL_INT);
    YEW_ASSERT_EQ_I64(out.as.i, 0);
    fl_gc_release(&f.fl.vm, 1U);
    receiver_close(&f);
}
