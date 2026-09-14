#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/value.h"
#include "fl/vm.h"

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

void test_fl_replay_runtime_cache_preserves_macro_defining_origin(void)
{
    static const char writer[] =
        "import ed\n"
        "ed.run(\"ed.reg.set\", {iarg: 97, sarg: \"@[ > ]\\n\"})\n";
    static const char yanker[] =
        "import ed\n"
        "ed.run(\"ed.edit.yank\", {sarg: \"A\", "
        "range_kind: \"span\", range_given: true, "
        "range_lo: 0, range_hi: 7})\n";
    static const u8 macro[] = "@[ > ]\n";
    Ed ed;
    FlVm *vm;
    FlValue exports = FL_NIL_V;
    FlOrigin plugin;
    const RegVal *value;
    FlFn *plugin_fn;
    FlFn *config_fn;

    _Static_assert(sizeof(macro) - 1U == 7U,
                   "yanker range must cover the complete macro source");
    replay_runtime_open(&ed);
    vm = yew_fl_vm(&ed);
    YEW_ASSERT_NOT_NULL(vm);
    plugin = (FlOrigin){
        (u8)FL_ORIGIN_PLUGIN,
        yew_intern_cstr(vm->in, "plugin-macro-writer"),
        0U,
        77U
    };
    YEW_ASSERT(fl_module_eval_source(vm, "plugin-macro-writer.fl", writer,
                                     sizeof(writer) - 1U, plugin,
                                     &exports));
    value = yew_reg_get(&ed.regs, (u8)'a');
    YEW_ASSERT_NOT_NULL(value);
    YEW_ASSERT_EQ_MEM(value->bytes.data, macro, sizeof(macro) - 1U);
    plugin_fn = fl_macro_compile_cached(ed.fl, (u8)'a', value->bytes.data,
                                        value->bytes.len);
    YEW_ASSERT_NOT_NULL(plugin_fn);
    YEW_ASSERT_EQ_U64(plugin_fn->origin.kind, FL_ORIGIN_PLUGIN);
    YEW_ASSERT_EQ_U64(plugin_fn->origin.caps, 0U);
    YEW_ASSERT_EQ_U64(plugin_fn->origin.principal_id, 77U);

    /* A host/user rewrite of identical bytes must still invalidate the
     * provenance-sensitive cache and restore config authority. */
    YEW_ASSERT_EQ_I64(yew_flapi_reg_write(&ed, (u8)'a', macro,
                                          sizeof(macro) - 1U, false),
                      YEW_CMD_OK);
    value = yew_reg_get(&ed.regs, (u8)'a');
    config_fn = fl_macro_compile_cached(ed.fl, (u8)'a', value->bytes.data,
                                        value->bytes.len);
    YEW_ASSERT_NOT_NULL(config_fn);
    YEW_ASSERT(config_fn != plugin_fn);
    YEW_ASSERT_EQ_U64(config_fn->origin.kind, FL_ORIGIN_CONFIG);
    YEW_ASSERT_EQ_U64(config_fn->origin.caps, FL_CAP_ALL);

    /* The other named-register door and its uppercase append spelling
     * carry the same provenance rule. */
    YEW_ASSERT(yew_ed_open_memory(&ed, macro, sizeof(macro) - 1U,
                                  "macro-source.fl"));
    YEW_ASSERT(fl_module_eval_source(vm, "plugin-macro-yanker.fl", yanker,
                                     sizeof(yanker) - 1U, plugin,
                                     &exports));
    value = yew_reg_get(&ed.regs, (u8)'a');
    YEW_ASSERT_NOT_NULL(value);
    YEW_ASSERT_EQ_U64(value->bytes.len, 2U * (sizeof(macro) - 1U));
    YEW_ASSERT_EQ_MEM(value->bytes.data, macro, sizeof(macro) - 1U);
    YEW_ASSERT_EQ_MEM(value->bytes.data + sizeof(macro) - 1U, macro,
                      sizeof(macro) - 1U);
    plugin_fn = fl_macro_compile_cached(ed.fl, (u8)'a', value->bytes.data,
                                        value->bytes.len);
    YEW_ASSERT_NOT_NULL(plugin_fn);
    YEW_ASSERT_EQ_U64(plugin_fn->origin.kind, FL_ORIGIN_PLUGIN);
    YEW_ASSERT_EQ_U64(plugin_fn->origin.principal_id, 77U);
    replay_runtime_close(&ed);
}
