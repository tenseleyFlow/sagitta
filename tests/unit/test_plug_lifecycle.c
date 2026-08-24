#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/module.h"
#include "mod/plug/internal.h"
#include "util/xdg.h"
#include "ws/trust.h"

typedef struct LifecycleCounts {
    u32 commands;
    u32 binds;
    u32 hooks;
    u32 ledger;
    u32 plug_commands;
    u32 plug_values;
} LifecycleCounts;

typedef struct LifecycleFix {
    char root[256];
    char data[320];
    char config[320];
    char state[320];
    char workspace[320];
    char plugins[384];
    char plugin_dir[448];
    char source_dir[480];
    char entry[512];
    char helper[512];
    char manifest[512];
    char *old_data;
    char *old_config;
    char *old_state;
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
    Plug *plug;
} LifecycleFix;

static char *life_strdup(const char *text)
{
    size_t len;
    char *copy;

    if (text == NULL)
        return NULL;
    len = strlen(text);
    copy = malloc(len + 1U);
    YEW_ASSERT_NOT_NULL(copy);
    (void)memcpy(copy, text, len + 1U);
    return copy;
}

static void life_restore_env(const char *name, const char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
}

static void life_remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (dir == NULL) {
        (void)unlink(path);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char child[768];
        struct stat st;
        int n;

        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;
        n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        YEW_ASSERT(n > 0 && (size_t)n < sizeof(child));
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            life_remove_tree(child);
        else
            YEW_ASSERT_EQ_I64(unlink(child), 0);
    }
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
}

static void life_write(const char *path, const char *source)
{
    FILE *fp = fopen(path, "wb");
    size_t len = strlen(source);

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(source, 1U, len, fp), len);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static u32 life_active_ledger(const Ed *ed)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < ed->hooks.ledger.n; i++)
        if (ed->hooks.ledger.v[i].active)
            active++;
    return active;
}

static u32 life_active_hooks(const Ed *ed)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < ed->hooks.n; i++)
        if (ed->hooks.v[i].active)
            active++;
    return active;
}

static u32 life_active_plug_commands(const PlugSys *sys)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; sys != NULL && i < sys->ncmds; i++)
        if (sys->cmds[i].active)
            active++;
    return active;
}

static u32 life_active_plug_values(const PlugSys *sys)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; sys != NULL && i < sys->nregs; i++)
        if (sys->regs[i].active)
            active++;
    return active;
}

static LifecycleCounts life_counts(const LifecycleFix *f)
{
    LifecycleCounts counts;

    counts.commands = yew_cmd_active_count();
    counts.binds = yew_bind_active_count(&f->ed);
    counts.hooks = life_active_hooks(&f->ed);
    counts.ledger = life_active_ledger(&f->ed);
    counts.plug_commands = life_active_plug_commands(f->ed.plug);
    counts.plug_values = life_active_plug_values(f->ed.plug);
    return counts;
}

static void life_assert_counts(LifecycleCounts got, LifecycleCounts want)
{
    YEW_ASSERT_EQ_U64(got.commands, want.commands);
    YEW_ASSERT_EQ_U64(got.binds, want.binds);
    YEW_ASSERT_EQ_U64(got.hooks, want.hooks);
    YEW_ASSERT_EQ_U64(got.ledger, want.ledger);
    YEW_ASSERT_EQ_U64(got.plug_commands, want.plug_commands);
    YEW_ASSERT_EQ_U64(got.plug_values, want.plug_values);
}

static void life_open(LifecycleFix *f, const char *name,
                      const char *events, const char *entry_source,
                      const char *helper_source)
{
    char manifest_source[1024];
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-plug-life-XXXXXX",
                 sizeof("/tmp/yew-plug-life-XXXXXX"));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->data));
    n = snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->config));
    n = snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->state));
    n = snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->workspace));
    n = snprintf(f->plugins, sizeof(f->plugins), "%s/yew/plugins", f->data);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->plugins));
    n = snprintf(f->plugin_dir, sizeof(f->plugin_dir), "%s/%s",
                 f->plugins, name);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->plugin_dir));
    n = snprintf(f->source_dir, sizeof(f->source_dir), "%s/src",
                 f->plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->source_dir));
    n = snprintf(f->entry, sizeof(f->entry), "%s/main.fl", f->source_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->entry));
    n = snprintf(f->helper, sizeof(f->helper), "%s/helper.fl",
                 f->source_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->helper));
    n = snprintf(f->manifest, sizeof(f->manifest), "%s/plugin.fl",
                 f->plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->manifest));
    YEW_ASSERT(yew_mkdirs(f->source_dir, 0700U));
    YEW_ASSERT(yew_mkdirs(f->config, 0700U));
    YEW_ASSERT(yew_mkdirs(f->state, 0700U));
    YEW_ASSERT(yew_mkdirs(f->workspace, 0700U));
    life_write(f->entry, entry_source);
    if (helper_source != NULL)
        life_write(f->helper, helper_source);
    n = snprintf(manifest_source, sizeof(manifest_source),
                 "{name: \"%s\", version: \"1.0.0\", api: 1, "
                 "entry: \"src/main.fl\", capabilities: [], events: %s, "
                 "description: \"lifecycle fixture\"}\n",
                 name, events);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(manifest_source));
    life_write(f->manifest, manifest_source);

    f->old_data = life_strdup(getenv("XDG_DATA_HOME"));
    f->old_config = life_strdup(getenv("XDG_CONFIG_HOME"));
    f->old_state = life_strdup(getenv("XDG_STATE_HOME"));
    YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", f->data, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", f->config, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state, 1), 0);
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    arena_init(&f->diag_arena);
    fl_diag_init(&f->dc, &f->diag_arena);
    yew_trust_db_init(&f->trust);
    YEW_ASSERT(yew_plug_discover_with_policy(&f->ed, true, &f->trust,
                                              &f->dc));
    f->plug = yew_plug_find(&f->ed, name);
    YEW_ASSERT_NOT_NULL(f->plug);
    YEW_ASSERT_EQ_U64(f->plug->st, PLUG_DISCOVERED);
}

static void life_close(LifecycleFix *f)
{
    yew_trust_db_free(&f->trust);
    arena_free_all(&f->diag_arena);
    yew_ed_free(&f->ed);
    life_restore_env("XDG_DATA_HOME", f->old_data);
    life_restore_env("XDG_CONFIG_HOME", f->old_config);
    life_restore_env("XDG_STATE_HOME", f->old_state);
    free(f->old_data);
    free(f->old_config);
    free(f->old_state);
    life_remove_tree(f->root);
}

static u32 life_principal_modules(const LifecycleFix *f, u8 state)
{
    const FlVm *vm = yew_fl_vm((Ed *)&f->ed);
    u32 count = 0U;
    u32 i;

    for (i = 0U; i < vm->mods.n; i++)
        if (vm->mods.v[i].origin.principal_id == f->plug->origin_id &&
            vm->mods.v[i].state == (u8)state)
            count++;
    return count;
}

void test_plug_lifecycle_enable_passes_ctx_and_registers_command_and_hook(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  if ctx.name != \"life-basic\" { error(\"wrong ctx\") }\n"
        "  ctx.command(\"ping\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    CmdId command;

    life_open(&f, "life-basic", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
    YEW_ASSERT(f.plug->rooted);
    YEW_ASSERT_EQ_U64(f.plug->module.t, FL_MAP);
    command = yew_cmd_lookup("ed.plug.life_basic.ping",
                             strlen("ed.plug.life_basic.ping"));
    YEW_ASSERT(command.v != YEW_CMD_NONE.v);
    YEW_ASSERT_EQ_U64(life_counts(&f).commands, before.commands + 1U);
    YEW_ASSERT_EQ_U64(life_counts(&f).hooks, before.hooks + 1U);
    YEW_ASSERT_EQ_U64(life_counts(&f).ledger, before.ledger + 2U);
    life_close(&f);
}

void test_plug_lifecycle_failing_init_leaves_zero_residue_and_trace(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.command(\"partial\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "  error(\"init exploded\")\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;

    life_open(&f, "life-failing", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(!yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ERROR);
    YEW_ASSERT(!f.plug->rooted);
    YEW_ASSERT_EQ_U64(f.plug->module.t, FL_NIL);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT(strstr(f.plug->last_error, "init exploded") != NULL);
    YEW_ASSERT(strstr(f.plug->last_error, "at init") != NULL);
    YEW_ASSERT(strstr(f.plug->last_error, "^") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_ctx_on_rejects_event_missing_from_manifest(void)
{
    static const char source[] =
        "fn init(ctx) { ctx.on(\"ed.idle\", fn() nil) }\n";
    LifecycleFix f;
    LifecycleCounts before;

    life_open(&f, "life-events", "[]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(!yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ERROR);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT(strstr(f.plug->last_error,
                      "did not declare event ed.idle") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_reverse_disable_leaves_zero_registry_residue(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.command(\"remove\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    u32 first = 0U;
    u32 second = 0U;
    u32 i;

    life_open(&f, "life-disable", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    for (i = 0U; i < f.ed.hooks.ledger.n; i++) {
        const FlRegistration *reg = &f.ed.hooks.ledger.v[i];

        if (!reg->active || reg->origin_id != f.plug->origin_id)
            continue;
        if (first == 0U)
            first = i + 1U;
        else
            second = i + 1U;
    }
    YEW_ASSERT(first != 0U);
    YEW_ASSERT(second != 0U);
    YEW_ASSERT_EQ_U64(f.ed.hooks.ledger.v[first - 1U].kind, REG_CMD);
    YEW_ASSERT_EQ_U64(f.ed.hooks.ledger.v[second - 1U].kind, REG_HOOK);
    YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_reenable_keeps_imported_helper_principal(void)
{
    static const char source[] =
        "import \"helper.fl\" as helper\n"
        "fn init(ctx) {\n"
        "  if helper.answer != 54 { error(\"bad helper\") }\n"
        "  ctx.command(\"again\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    LifecycleCounts enabled;
    u32 principal;
    u32 principal_modules;

    life_open(&f, "life-reenable", "[\"ed.idle\"]", source,
              "let answer = 54\n");
    before = life_counts(&f);
    principal = f.plug->origin_id;
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    enabled = life_counts(&f);
    principal_modules = life_principal_modules(&f, FL_MOD_READY);
    YEW_ASSERT(principal_modules >= 2U);
    YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    YEW_ASSERT_EQ_U64(f.plug->origin_id, principal);
    YEW_ASSERT_EQ_U64(life_principal_modules(&f, FL_MOD_READY), 0U);
    YEW_ASSERT_EQ_U64(life_principal_modules(&f, FL_MOD_DROPPED),
                      principal_modules);
    life_assert_counts(life_counts(&f), before);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
    YEW_ASSERT_EQ_U64(f.plug->origin_id, principal);
    YEW_ASSERT_EQ_U64(life_principal_modules(&f, FL_MOD_READY),
                      principal_modules);
    life_assert_counts(life_counts(&f), enabled);
    life_close(&f);
}

void test_plug_lifecycle_fifth_hook_error_auto_disables_plugin(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.on(\"ed.idle\", fn() error(\"hook exploded\"))\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    u32 i;

    life_open(&f, "life-errors", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    for (i = 0U; i < 4U; i++) {
        yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
        YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
        YEW_ASSERT_EQ_U64(f.plug->err_count, i + 1U);
    }
    yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.plug->err_count, 5U);
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT(strstr(f.plug->last_error, "hook exploded") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}
