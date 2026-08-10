#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/gc.h"

static CmdSource replay_sources[8];
static u32 replay_nsources;

static void replay_source_tap(CmdId id, const CmdCtx *cx)
{
    (void)id;
    YEW_ASSERT(replay_nsources < (u32)YEW_ARRAY_LEN(replay_sources));
    replay_sources[replay_nsources++] = cx->source;
}

static void replay_runtime_open(Ed *ed)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    replay_nsources = 0U;
    yew_cmd_set_record_tap(replay_source_tap);
}

static void replay_runtime_close(Ed *ed)
{
    yew_cmd_set_record_tap(NULL);
    yew_ed_free(ed);
}

void test_fl_replay_runtime_propagates_command_source(void)
{
    static const u8 source[] =
        "import ed\n"
        "ed.run(\"ed.mode.enter\", {\"sarg\": \"W\"})\n"
        "ed.run(\"ed.fl.eval\", {\"sarg\": \"@[ l ]\"})\n";
    Ed ed;
    FlFn *fn;

    replay_runtime_open(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, (const char *)source,
                                  (u32)(sizeof(source) - 1U)), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(replay_nsources, 2U);
    YEW_ASSERT_EQ_U64(replay_sources[0], YEW_SRC_FLETCH);
    YEW_ASSERT_EQ_U64(replay_sources[1], YEW_SRC_FLETCH);

    replay_nsources = 0U;
    fn = fl_compile_str(ed.fl, source, sizeof(source) - 1U,
                        "<source-propagation>");
    YEW_ASSERT_NOT_NULL(fn);
    YEW_ASSERT(fl_call_chunk(ed.fl, fn, YEW_SRC_REPLAY));
    YEW_ASSERT_EQ_U64(replay_nsources, 2U);
    YEW_ASSERT_EQ_U64(replay_sources[0], YEW_SRC_REPLAY);
    YEW_ASSERT_EQ_U64(replay_sources[1], YEW_SRC_REPLAY);
    replay_runtime_close(&ed);
}

void test_fl_replay_runtime_cache_is_exact_rooted_and_invalidated(void)
{
    static const u8 first[] = "@[ > ]\n";
    static const u8 second[] = "@[ < ]\n";
    Ed ed;
    FlVm *vm;
    FlFn *a;
    FlFn *hit;
    FlFn *changed;
    FlFn *recompiled;

    replay_runtime_open(&ed);
    vm = yew_fl_vm(&ed);
    YEW_ASSERT_NOT_NULL(vm);
    a = fl_macro_compile_cached(ed.fl, (u8)'a', first,
                                sizeof(first) - 1U);
    YEW_ASSERT_NOT_NULL(a);
    hit = fl_macro_compile_cached(ed.fl, (u8)'a', first,
                                  sizeof(first) - 1U);
    YEW_ASSERT(hit == a);

    changed = fl_macro_compile_cached(ed.fl, (u8)'a', second,
                                      sizeof(second) - 1U);
    YEW_ASSERT_NOT_NULL(changed);
    YEW_ASSERT(changed != a);
    fl_gc_collect(vm);
    hit = fl_macro_compile_cached(ed.fl, (u8)'a', second,
                                  sizeof(second) - 1U);
    YEW_ASSERT(hit == changed);
    YEW_ASSERT(fl_call_chunk(ed.fl, hit, YEW_SRC_REPLAY));

    fl_macro_cache_invalidate(ed.fl, (u8)'a');
    recompiled = fl_macro_compile_cached(ed.fl, (u8)'a', second,
                                         sizeof(second) - 1U);
    YEW_ASSERT_NOT_NULL(recompiled);
    YEW_ASSERT(recompiled != changed);
    replay_runtime_close(&ed);
}
