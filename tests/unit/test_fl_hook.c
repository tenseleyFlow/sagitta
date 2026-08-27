#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/flhook.h"
#include "fl/gc.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct HookFix {
    FlHookTable hooks;
    FlNative fn[16];
    u32 calls[64];
    u32 ncalls;
    u32 fail_id;
    bool same_event;
    bool chain;
    u32 masked_origin;
    u32 notices[4];
} HookFix;

static FlValue fake_fn(HookFix *f, u32 id)
{
    f->fn[id].name_id = id;
    return FL_OBJ_V(FL_NATIVE, &f->fn[id]);
}

static bool hook_call(void *ctx, FlVm *vm, FlValue fn,
                      const FlValue *args, u8 nargs, FlValue *err)
{
    HookFix *f = (HookFix *)ctx;
    u32 id = ((FlNative *)fn.as.o)->name_id;

    (void)vm;
    (void)args;
    (void)nargs;
    f->calls[f->ncalls++] = id;
    if (f->same_event)
        fl_hook_fire(&f->hooks, NULL, FL_EV_BUF_CHANGE, NULL, 0U);
    if (f->chain && id < 9U)
        fl_hook_fire(&f->hooks, NULL, id + 1U, NULL, 0U);
    if (id == f->fail_id) {
        if (err != NULL)
            *err = FL_INT_V((i64)id);
        return false;
    }
    return true;
}

static bool hook_masked(void *ctx, u32 origin)
{
    HookFix *f = (HookFix *)ctx;

    return origin == f->masked_origin;
}

static void hook_notice(void *ctx, FlHookNotice what, u32 event,
                        u32 ledger_id, u32 errs, FlValue err)
{
    HookFix *f = (HookFix *)ctx;

    (void)event;
    (void)ledger_id;
    (void)errs;
    (void)err;
    f->notices[what]++;
}

static void hf_open(HookFix *f)
{
    FlHookOps ops;

    (void)memset(f, 0, sizeof(*f));
    (void)memset(&ops, 0, sizeof(ops));
    ops.call = hook_call;
    ops.masked = hook_masked;
    ops.notice = hook_notice;
    fl_hook_table_init(&f->hooks, &ops, f);
    f->fail_id = 0xFFFFFFFFU;
    f->masked_origin = 0xFFFFFFFFU;
}

static void hf_close(HookFix *f)
{
    fl_hook_table_free(&f->hooks);
}

void test_fl_hook_event_inventory(void)
{
    static const char *const names[] = {
        "buf.open",    "buf.change", "buf.save",   "buf.saved",
        "buf.close",   "mode.enter", "mode.leave", "win.focus",
        "cursor.move", "ws.open",    "ws.close",   "plug.enable",
        "plug.disable", "ed.idle"
    };
    u32 i;

    YEW_ASSERT_EQ_U64(FL_EV__N, 14U);
    for (i = 0U; i < (u32)YEW_ARRAY_LEN(names); i++) {
        u32 parsed = 99U;

        YEW_ASSERT_EQ_STR(fl_event_name(i), names[i]);
        YEW_ASSERT(fl_event_parse(names[i], (u32)strlen(names[i]), &parsed));
        YEW_ASSERT_EQ_U64(parsed, i);
    }
    YEW_ASSERT(fl_event_name(FL_EV__N) == NULL);
    YEW_ASSERT(!fl_event_parse("ws.quit", 7U, NULL));
}

void test_fl_hook_order_mask_and_remove(void)
{
    HookFix f;
    u32 removed;

    hf_open(&f);
    (void)fl_hook_add(&f.hooks, 2U, FL_EV_BUF_OPEN, fake_fn(&f, 1U));
    (void)fl_hook_add(&f.hooks, 0U, FL_EV_BUF_OPEN, fake_fn(&f, 2U));
    removed = fl_hook_add(&f.hooks, 1U, FL_EV_BUF_OPEN, fake_fn(&f, 3U));
    (void)fl_hook_add(&f.hooks, 0U, FL_EV_BUF_OPEN, fake_fn(&f, 4U));
    YEW_ASSERT_EQ_U64(fl_hook_origin(&f.hooks, removed), 1U);
    YEW_ASSERT_EQ_U64(fl_hook_origin(&f.hooks, 0U), FL_ORIGIN_ID_NONE);

    fl_hook_fire(&f.hooks, NULL, FL_EV_BUF_OPEN, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.ncalls, 4U);
    YEW_ASSERT_EQ_U64(f.calls[0], 2U);
    YEW_ASSERT_EQ_U64(f.calls[1], 4U);
    YEW_ASSERT_EQ_U64(f.calls[2], 1U);
    YEW_ASSERT_EQ_U64(f.calls[3], 3U);

    f.ncalls = 0U;
    f.masked_origin = 2U;
    YEW_ASSERT(fl_hook_remove(&f.hooks, removed));
    YEW_ASSERT_EQ_U64(fl_hook_origin(&f.hooks, removed),
                      FL_ORIGIN_ID_NONE);
    YEW_ASSERT(!fl_hook_remove(&f.hooks, removed));
    fl_hook_fire(&f.hooks, NULL, FL_EV_BUF_OPEN, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.ncalls, 2U);
    YEW_ASSERT_EQ_U64(f.calls[0], 2U);
    YEW_ASSERT_EQ_U64(f.calls[1], 4U);
    hf_close(&f);
}

void test_fl_hook_contains_and_disables_failures(void)
{
    u32 event;

    for (event = 0U; event < (u32)FL_EV__N; event++) {
        HookFix f;
        u32 bad;
        u32 i;

        hf_open(&f);
        f.fail_id = 1U;
        bad = fl_hook_add(&f.hooks, 0U, event, fake_fn(&f, 1U));
        (void)fl_hook_add(&f.hooks, 0U, event, fake_fn(&f, 2U));
        for (i = 0U; i < 6U; i++)
            fl_hook_fire(&f.hooks, NULL, event, NULL, 0U);

        YEW_ASSERT_EQ_U64(f.ncalls, 11U); /* bad five times; good six */
        YEW_ASSERT_EQ_U64(f.notices[FL_HOOK_NOTICE_ERROR], 5U);
        YEW_ASSERT_EQ_U64(f.notices[FL_HOOK_NOTICE_DISABLED], 1U);
        YEW_ASSERT(f.hooks.v[
            f.hooks.ledger.v[bad - 1U].handle - 1U].disabled);
        hf_close(&f);
    }
}

void test_fl_hook_drops_self_reentrancy_once(void)
{
    HookFix f;

    hf_open(&f);
    f.same_event = true;
    (void)fl_hook_add(&f.hooks, 0U, FL_EV_BUF_CHANGE, fake_fn(&f, 1U));
    fl_hook_fire(&f.hooks, NULL, FL_EV_BUF_CHANGE, NULL, 0U);
    fl_hook_fire(&f.hooks, NULL, FL_EV_BUF_CHANGE, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.ncalls, 2U);
    YEW_ASSERT_EQ_U64(f.notices[FL_HOOK_NOTICE_REENTRANT], 1U);
    hf_close(&f);
}

void test_fl_hook_listens_only_to_dispatchable_rows(void)
{
    HookFix f;
    u32 live;
    u32 disabled;

    hf_open(&f);
    YEW_ASSERT(!fl_hook_listens(&f.hooks, FL_EV_BUF_CHANGE));
    YEW_ASSERT(!fl_hook_listens(&f.hooks, FL_EV__N));
    live = fl_hook_add(&f.hooks, 2U, FL_EV_BUF_CHANGE,
                       fake_fn(&f, 1U));
    disabled = fl_hook_add(&f.hooks, 3U, FL_EV_BUF_CHANGE,
                           fake_fn(&f, 2U));
    YEW_ASSERT(fl_hook_listens(&f.hooks, FL_EV_BUF_CHANGE));
    YEW_ASSERT(!fl_hook_listens(&f.hooks, FL_EV_BUF_OPEN));
    f.masked_origin = 2U;
    YEW_ASSERT(fl_hook_listens(&f.hooks, FL_EV_BUF_CHANGE));
    f.hooks.v[f.hooks.ledger.v[disabled - 1U].handle - 1U].disabled = true;
    YEW_ASSERT(!fl_hook_listens(&f.hooks, FL_EV_BUF_CHANGE));
    f.masked_origin = 0U;
    YEW_ASSERT(fl_hook_listens(&f.hooks, FL_EV_BUF_CHANGE));
    YEW_ASSERT(fl_hook_remove(&f.hooks, live));
    YEW_ASSERT(!fl_hook_listens(&f.hooks, FL_EV_BUF_CHANGE));
    hf_close(&f);
}

void test_fl_hook_bounds_cross_event_depth(void)
{
    HookFix f;
    u32 i;

    hf_open(&f);
    f.chain = true;
    for (i = 0U; i < 9U; i++)
        (void)fl_hook_add(&f.hooks, 0U, i, fake_fn(&f, i));
    fl_hook_fire(&f.hooks, NULL, 0U, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.ncalls, YEW_HOOK_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(f.notices[FL_HOOK_NOTICE_DEPTH], 1U);
    YEW_ASSERT_EQ_U64(f.hooks.v[7].errs, 1U);
    YEW_ASSERT_EQ_U64(f.hooks.depth, 0U);
    hf_close(&f);
}

typedef struct HookGcFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlHookTable hooks;
} HookGcFix;

static void gc_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                    const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static bool object_live(const FlVm *vm, const FlObj *want)
{
    const FlObj *o;

    for (o = vm->gc.objects; o != NULL; o = o->gc_next) {
        if (o == want)
            return true;
    }
    return false;
}

void test_fl_hook_gc_mark_provider(void)
{
    HookGcFix f;
    FlNative *fn;
    u32 id;

    (void)memset(&f, 0, sizeof(f));
    arena_init(&f.arena);
    interner_init(&f.in, &f.arena);
    fl_diag_init(&f.dc, &f.arena);
    fl_diag_set_sink(&f.dc, gc_sink, NULL);
    YEW_ASSERT(fl_vm_init(&f.vm, &f.arena, &f.in, &f.dc));
    fl_hook_table_init(&f.hooks, NULL, NULL);

    fn = fl_gc_alloc(&f.vm, sizeof(*fn), FL_NATIVE);
    (void)memset((char *)fn + sizeof(fn->h), 0,
                 sizeof(*fn) - sizeof(fn->h));
    fn->min_ar = 0U;
    fn->max_ar = 0U;
    id = fl_hook_add(&f.hooks, 0U, FL_EV_ED_IDLE,
                     FL_OBJ_V(FL_NATIVE, fn));
    fl_gc_root_provider(&f.vm, fl_hook_mark, &f.hooks);
    fl_gc_collect(&f.vm);
    YEW_ASSERT(object_live(&f.vm, &fn->h));

    YEW_ASSERT(fl_hook_remove(&f.hooks, id));
    fl_gc_collect(&f.vm);
    YEW_ASSERT(!object_live(&f.vm, &fn->h));

    fl_hook_table_free(&f.hooks);
    fl_vm_free(&f.vm);
    interner_free(&f.in);
    arena_free_all(&f.arena);
}
