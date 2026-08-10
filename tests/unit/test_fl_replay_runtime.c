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
    SAG_ASSERT(replay_nsources < (u32)SAG_ARRAY_LEN(replay_sources));
    replay_sources[replay_nsources++] = cx->source;
}

static void replay_runtime_open(Ed *ed)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    replay_nsources = 0U;
    sag_cmd_set_record_tap(replay_source_tap);
}

static void replay_runtime_close(Ed *ed)
{
    sag_cmd_set_record_tap(NULL);
    sag_ed_free(ed);
}

void test_fl_replay_runtime_propagates_command_source(void)
{
    static const u8 source[] =
        "ed.run(\"ed.mode.enter\", {\"sarg\": \"L\"})\n"
        "@[ > ]\n";
    Ed ed;
    FlFn *fn;

    replay_runtime_open(&ed);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, (const char *)source,
                                  (u32)(sizeof(source) - 1U)), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(replay_nsources, 2U);
    SAG_ASSERT_EQ_U64(replay_sources[0], SAG_SRC_FLETCH);
    SAG_ASSERT_EQ_U64(replay_sources[1], SAG_SRC_FLETCH);

    replay_nsources = 0U;
    fn = fl_compile_str(ed.fl, source, sizeof(source) - 1U,
                        "<source-propagation>");
    SAG_ASSERT_NOT_NULL(fn);
    SAG_ASSERT(fl_call_chunk(ed.fl, fn, SAG_SRC_REPLAY));
    SAG_ASSERT_EQ_U64(replay_nsources, 2U);
    SAG_ASSERT_EQ_U64(replay_sources[0], SAG_SRC_REPLAY);
    SAG_ASSERT_EQ_U64(replay_sources[1], SAG_SRC_REPLAY);
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
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    a = fl_macro_compile_cached(ed.fl, (u8)'a', first,
                                sizeof(first) - 1U);
    SAG_ASSERT_NOT_NULL(a);
    hit = fl_macro_compile_cached(ed.fl, (u8)'a', first,
                                  sizeof(first) - 1U);
    SAG_ASSERT(hit == a);

    changed = fl_macro_compile_cached(ed.fl, (u8)'a', second,
                                      sizeof(second) - 1U);
    SAG_ASSERT_NOT_NULL(changed);
    SAG_ASSERT(changed != a);
    fl_gc_collect(vm);
    hit = fl_macro_compile_cached(ed.fl, (u8)'a', second,
                                  sizeof(second) - 1U);
    SAG_ASSERT(hit == changed);
    SAG_ASSERT(fl_call_chunk(ed.fl, hit, SAG_SRC_REPLAY));

    fl_macro_cache_invalidate(ed.fl, (u8)'a');
    recompiled = fl_macro_compile_cached(ed.fl, (u8)'a', second,
                                         sizeof(second) - 1U);
    SAG_ASSERT_NOT_NULL(recompiled);
    SAG_ASSERT(recompiled != changed);
    replay_runtime_close(&ed);
}
