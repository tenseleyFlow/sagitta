#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "mod/plug/internal.h"
#include "ui/cmdcomp.h"
#include "util/xdg.h"
#include "ws/trust.h"

static FlValue plug_option_string(FlVm *vm, const char *text)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, text, (u32)strlen(text)));
}

static void plug_option_put(FlVm *vm, FlMap *map, const char *name,
                            FlValue value)
{
    YEW_ASSERT(fl_map_set(vm, map, plug_option_string(vm, name), value));
}

static u32 plug_option_declare(Ed *ed, u32 origin, const char *plugin,
                               FlMap *map)
{
    FlVm *vm = yew_fl_vm(ed);
    FlValue arg = FL_OBJ_V(FL_MAP, map);
    FlValue out = FL_BOOL_V(true);
    u32 first = 0U;
    u32 i;

    YEW_ASSERT_NOT_NULL(vm);
    YEW_ASSERT(fl_api_declare_plugin_options(
        vm, origin, plugin, (u32)strlen(plugin), &arg, 1U, &out));
    YEW_ASSERT_EQ_U64(out.t, FL_NIL);
    for (i = 0U; i < ed->hooks.ledger.n; i++)
        if (ed->hooks.ledger.v[i].active &&
            ed->hooks.ledger.v[i].origin_id == origin) {
            first = i + 1U;
            break;
        }
    YEW_ASSERT(first != 0U);
    return first;
}

static OptVal plug_option_get(Ed *ed, const char *name)
{
    OptVal value = {0};

    YEW_ASSERT(yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name),
                           &value));
    return value;
}

static bool completion_has(const Vec_CompItem *items, const char *name)
{
    u32 i;

    for (i = 0U; i < items->len; i++)
        if (strcmp(items->data[i].text, name) == 0)
            return true;
    return false;
}

void test_plug_options_declare_set_complete_teardown_and_reenable(void)
{
    Ed ed;
    FlVm *vm;
    FlMap *map;
    FlList *paths;
    Vec_CompItem items = {0};
    CmdCtx cx = {0};
    char *argv[] = {"ed.opt.set_many", "plug.option-life.limit", "17"};
    const OptDesc *desc;
    const OptProvider *provider;
    const char *names[128];
    u32 first_ledger;
    u32 listed;
    u32 i;
    bool found = false;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    vm = yew_fl_vm(&ed);
    YEW_ASSERT_NOT_NULL(vm);
    map = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, map));
    paths = fl_list_new(vm);
    YEW_ASSERT(fl_list_push(vm, paths, plug_option_string(vm, "src")));
    YEW_ASSERT(fl_list_push(vm, paths, plug_option_string(vm, "tests")));
    plug_option_put(vm, map, "enabled", FL_BOOL_V(true));
    plug_option_put(vm, map, "limit", FL_INT_V(9));
    plug_option_put(vm, map, "label", plug_option_string(vm, "first"));
    plug_option_put(vm, map, "paths", FL_OBJ_V(FL_LIST, paths));
    first_ledger = plug_option_declare(&ed, 701U, "option-life", map);

    YEW_ASSERT(plug_option_get(&ed, "plug.option-life.enabled").as.b);
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.option-life.limit").as.i,
                      9);
    YEW_ASSERT_EQ_STR(plug_option_get(&ed, "plug.option-life.label").as.str.s,
                      "first");
    YEW_ASSERT_EQ_U64(
        plug_option_get(&ed, "plug.option-life.paths").as.list.len, 2U);
    desc = yew_opt_desc_for(&ed, "plug.option-life.limit",
                            (u32)strlen("plug.option-life.limit"));
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_EQ_U64(desc->scope, YEW_OPT_GLOBAL);
    YEW_ASSERT_EQ_U64(desc->type, YEW_OPT_INT);

    cx.ed = &ed;
    cx.argv = (CmdArgv){argv, 3U};
    YEW_ASSERT_EQ_I64(yew_opt_cmdline_set(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.option-life.limit").as.i,
                      17);
    {
        const char *err = NULL;
        OptVal changed = {YEW_OPT_STR, {.str = {"changed", 7U}}};

        YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_GLOBAL,
                               "plug.option-life.label",
                               (u32)strlen("plug.option-life.label"),
                               &changed, &err));
        YEW_ASSERT_NULL(err);
        YEW_ASSERT_EQ_STR(
            plug_option_get(&ed, "plug.option-life.label").as.str.s,
            "changed");
    }
    yew_opt_reset(&ed);
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.option-life.limit").as.i,
                      9);
    YEW_ASSERT_EQ_STR(plug_option_get(&ed,
                                      "plug.option-life.label").as.str.s,
                      "first");
    provider = yew_opt_provider(&ed);
    listed = provider->list(&ed, names, YEW_ARRAY_LEN(names));
    for (i = 0U; i < listed; i++)
        if (strcmp(names[i], "plug.option-life.limit") == 0)
            found = true;
    YEW_ASSERT(found);
    (void)yew_comp_enumerate(&ed, YEW_COMP_OPTION, "plug.option", &items);
    YEW_ASSERT(completion_has(&items, "plug.option-life.enabled"));
    YEW_ASSERT(completion_has(&items, "plug.option-life.limit"));
    Vec_CompItem_free(&items);

    for (i = ed.hooks.ledger.n; i >= first_ledger; i--)
        if (ed.hooks.ledger.v[i - 1U].active &&
            ed.hooks.ledger.v[i - 1U].origin_id == 701U)
            YEW_ASSERT(yew_opt_remove(&ed, i));
    YEW_ASSERT(!yew_opt_get(&ed, NULL, NULL, "plug.option-life.limit",
                            (u32)strlen("plug.option-life.limit"),
                            &(OptVal){0}));
    YEW_ASSERT_NULL(yew_opt_desc_for(&ed, "plug.option-life.limit",
                                     (u32)strlen("plug.option-life.limit")));
    (void)yew_comp_enumerate(&ed, YEW_COMP_OPTION, "plug.option", &items);
    YEW_ASSERT(!completion_has(&items, "plug.option-life.limit"));
    Vec_CompItem_free(&items);

    first_ledger = plug_option_declare(&ed, 701U, "option-life", map);
    (void)first_ledger;
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.option-life.limit").as.i,
                      9);
    fl_gc_release(vm, 1U);
    yew_ed_free(&ed);
}

void test_plug_options_collisions_are_atomic_and_origin_owned(void)
{
    Ed ed;
    FlVm *vm;
    FlMap *initial;
    FlMap *collision;
    FlValue arg;
    FlValue out = FL_NIL_V;
    u32 ledger_before;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    vm = yew_fl_vm(&ed);
    YEW_ASSERT_NOT_NULL(vm);
    initial = fl_map_new(vm);
    plug_option_put(vm, initial, "kept", FL_INT_V(3));
    (void)plug_option_declare(&ed, 801U, "collision", initial);
    ledger_before = ed.hooks.ledger.n;

    collision = fl_map_new(vm);
    plug_option_put(vm, collision, "temporary", FL_BOOL_V(true));
    plug_option_put(vm, collision, "kept", FL_INT_V(4));
    arg = FL_OBJ_V(FL_MAP, collision);
    YEW_ASSERT(!fl_api_declare_plugin_options(
        vm, 802U, "collision", (u32)strlen("collision"), &arg, 1U, &out));
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.n, ledger_before + 1U);
    YEW_ASSERT(!ed.hooks.ledger.v[ledger_before].active);
    YEW_ASSERT(!yew_opt_get(&ed, NULL, NULL, "plug.collision.temporary",
                            (u32)strlen("plug.collision.temporary"),
                            &(OptVal){0}));
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.collision.kept").as.i,
                      3);
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.v[ledger_before - 1U].origin_id, 801U);
    yew_ed_free(&ed);
}

static char *plug_option_env_copy(const char *name)
{
    const char *value = getenv(name);
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value);
    copy = yew_xmalloc(len + 1U);
    (void)memcpy(copy, value, len + 1U);
    return copy;
}

static void plug_option_env_restore(const char *name, const char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
}

static void plug_option_write(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    size_t len = strlen(text);

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, len, fp), len);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

void test_plug_options_ctx_set_uses_manifest_namespace_and_lifecycle(void)
{
    char root[] = "/tmp/yew-plug-options-XXXXXX";
    char data[320];
    char config[320];
    char state[320];
    char workspace[320];
    char source_dir[512];
    char manifest[600];
    char entry[600];
    char plugin_dir[448];
    char *old_data = plug_option_env_copy("XDG_DATA_HOME");
    char *old_config = plug_option_env_copy("XDG_CONFIG_HOME");
    char *old_state = plug_option_env_copy("XDG_STATE_HOME");
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
    Plug *plug;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(data, sizeof(data), "%s/data", root);
    (void)snprintf(config, sizeof(config), "%s/config", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(workspace, sizeof(workspace), "%s/work", root);
    (void)snprintf(plugin_dir, sizeof(plugin_dir),
                   "%s/yew/plugins/ctx-options", data);
    (void)snprintf(source_dir, sizeof(source_dir), "%s/src", plugin_dir);
    (void)snprintf(manifest, sizeof(manifest), "%s/plugin.fl", plugin_dir);
    (void)snprintf(entry, sizeof(entry), "%s/main.fl", source_dir);
    YEW_ASSERT(yew_mkdirs(source_dir, 0700U));
    YEW_ASSERT(yew_mkdirs(config, 0700U));
    YEW_ASSERT(yew_mkdirs(state, 0700U));
    YEW_ASSERT(yew_mkdirs(workspace, 0700U));
    plug_option_write(
        manifest,
        "{name: \"ctx-options\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [], "
        "description: \"dynamic options\"}\n");
    plug_option_write(
        entry,
        "fn init(ctx) { ctx.set({enabled: true, limit: 54}) }\n");
    YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", data, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", config, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state, 1), 0);
    yew_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, workspace);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    arena_init(&diag_arena);
    fl_diag_init(&dc, &diag_arena);
    yew_trust_db_init(&trust);
    YEW_ASSERT(yew_plug_discover_with_policy(&ed, true, &trust, &dc));
    plug = yew_plug_find(&ed, "ctx-options");
    YEW_ASSERT_NOT_NULL(plug);
    YEW_ASSERT(yew_plug_enable(&ed, plug, &dc));
    YEW_ASSERT(plug_option_get(&ed, "plug.ctx-options.enabled").as.b);
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.ctx-options.limit").as.i,
                      54);
    YEW_ASSERT(yew_plug_disable(&ed, plug));
    YEW_ASSERT(!yew_opt_get(&ed, NULL, NULL, "plug.ctx-options.limit",
                            (u32)strlen("plug.ctx-options.limit"),
                            &(OptVal){0}));
    YEW_ASSERT(yew_plug_enable(&ed, plug, &dc));
    YEW_ASSERT_EQ_I64(plug_option_get(&ed, "plug.ctx-options.limit").as.i,
                      54);

    yew_trust_db_free(&trust);
    arena_free_all(&diag_arena);
    yew_ed_free(&ed);
    plug_option_env_restore("XDG_DATA_HOME", old_data);
    plug_option_env_restore("XDG_CONFIG_HOME", old_config);
    plug_option_env_restore("XDG_STATE_HOME", old_state);
    free(old_data);
    free(old_config);
    free(old_state);
    YEW_ASSERT_EQ_I64(unlink(entry), 0);
    YEW_ASSERT_EQ_I64(unlink(manifest), 0);
    YEW_ASSERT_EQ_I64(rmdir(source_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(plugin_dir), 0);
    {
        char plugins[400];
        char yew[360];

        (void)snprintf(plugins, sizeof(plugins), "%s/yew/plugins", data);
        (void)snprintf(yew, sizeof(yew), "%s/yew", data);
        YEW_ASSERT_EQ_I64(rmdir(plugins), 0);
        YEW_ASSERT_EQ_I64(rmdir(yew), 0);
    }
    YEW_ASSERT_EQ_I64(rmdir(data), 0);
    YEW_ASSERT_EQ_I64(rmdir(config), 0);
    YEW_ASSERT_EQ_I64(rmdir(state), 0);
    YEW_ASSERT_EQ_I64(rmdir(workspace), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}
