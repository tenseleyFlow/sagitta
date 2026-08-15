#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "edit/option.h"
#include "fl/flconf.h"
#include "fl/flruntime.h"
#include "fl/value.h"

typedef struct ConfigFix {
    Ed ed;
    char root[160];
    char runtime[192];
    char runtime_init[224];
    char xdg_config[192];
    char user_dir[224];
    char user_init[256];
    char xdg_state[192];
    char workspace[192];
    char workspace_init[224];
    char *old_runtime;
    char *old_config;
    char *old_state;
} ConfigFix;

static char *cf_env_copy(const char *name)
{
    const char *value = getenv(name);
    char *copy;

    if (value == NULL)
        return NULL;
    copy = yew_xmalloc(strlen(value) + 1U);
    (void)memcpy(copy, value, strlen(value) + 1U);
    return copy;
}

static void cf_env_restore(const char *name, char *value)
{
    if (value == NULL)
        (void)unsetenv(name);
    else {
        (void)setenv(name, value, 1);
        free(value);
    }
}

static void cf_write(const char *path, const char *source)
{
    FILE *fp = fopen(path, "wb");
    size_t len = strlen(source);

    if (fp == NULL || fwrite(source, 1U, len, fp) != len || fclose(fp) != 0)
        YEW_BUG("config test fixture write failed");
}

static void cf_mkdir(const char *path)
{
    if (mkdir(path, 0700) != 0)
        YEW_BUG("config test fixture mkdir failed");
}

static void cf_init(ConfigFix *f, const YewEdStartup *startup)
{
    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-flconf-XXXXXX");
    if (mkdtemp(f->root) == NULL)
        YEW_BUG("config test fixture mkdtemp failed");
    (void)snprintf(f->runtime, sizeof(f->runtime), "%s/runtime", f->root);
    (void)snprintf(f->runtime_init, sizeof(f->runtime_init), "%s/init.fl",
                   f->runtime);
    (void)snprintf(f->xdg_config, sizeof(f->xdg_config), "%s/config",
                   f->root);
    (void)snprintf(f->user_dir, sizeof(f->user_dir), "%s/yew",
                   f->xdg_config);
    (void)snprintf(f->user_init, sizeof(f->user_init), "%s/init.fl",
                   f->user_dir);
    (void)snprintf(f->xdg_state, sizeof(f->xdg_state), "%s/state", f->root);
    (void)snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    (void)snprintf(f->workspace_init, sizeof(f->workspace_init),
                   "%s/.yew.fl", f->workspace);
    cf_mkdir(f->runtime);
    cf_mkdir(f->xdg_config);
    cf_mkdir(f->user_dir);
    cf_mkdir(f->xdg_state);
    cf_mkdir(f->workspace);
    cf_write(f->runtime_init, "\n");
    f->old_runtime = cf_env_copy("YEW_RUNTIME_DIR");
    f->old_config = cf_env_copy("XDG_CONFIG_HOME");
    f->old_state = cf_env_copy("XDG_STATE_HOME");
    if (setenv("YEW_RUNTIME_DIR", f->runtime, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", f->xdg_config, 1) != 0 ||
        setenv("XDG_STATE_HOME", f->xdg_state, 1) != 0)
        YEW_BUG("config test fixture environment failed");
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    if (!yew_ed_open_scratch(&f->ed))
        YEW_BUG("config test fixture editor failed");
    yew_config_init(&f->ed, startup);
}

static void cf_free(ConfigFix *f)
{
    char trust_dir[224];
    char trust_file[256];

    yew_ed_free(&f->ed);
    (void)unlink(f->workspace_init);
    (void)unlink(f->user_init);
    (void)unlink(f->runtime_init);
    (void)rmdir(f->workspace);
    (void)rmdir(f->user_dir);
    (void)rmdir(f->xdg_config);
    (void)snprintf(trust_dir, sizeof(trust_dir), "%s/yew", f->xdg_state);
    (void)snprintf(trust_file, sizeof(trust_file), "%s/trust.fl", trust_dir);
    (void)unlink(trust_file);
    (void)rmdir(trust_dir);
    (void)rmdir(f->xdg_state);
    (void)rmdir(f->runtime);
    (void)rmdir(f->root);
    cf_env_restore("YEW_RUNTIME_DIR", f->old_runtime);
    cf_env_restore("XDG_CONFIG_HOME", f->old_config);
    cf_env_restore("XDG_STATE_HOME", f->old_state);
}

static OptVal cf_opt(ConfigFix *f, const char *name)
{
    OptVal value = {0};

    YEW_ASSERT(yew_opt_get(&f->ed, yew_ed_doc(&f->ed), f->ed.win, name,
                           (u32)strlen(name), &value));
    return value;
}

static u32 cf_active_ledger(const ConfigFix *f)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < f->ed.hooks.ledger.n; i++)
        if (f->ed.hooks.ledger.v[i].active)
            active++;
    return active;
}

void test_flconf_loads_builtin_user_workspace_in_precedence_order(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;

    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "set({tabwidth: 5})\n");
    cf_write(f.user_init, "set({tabwidth: 6})\n");
    cf_write(f.workspace_init, "set({tabwidth: 7})\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT_EQ_I64(cf_opt(&f, "tabwidth").as.i, 7);
    YEW_ASSERT_EQ_U64(f.ed.buffer.tabwidth, 7U);
    cf_free(&f);
}

void test_flconf_global_accessor_reads_private_layers_in_precedence_order(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;
    FlValue value = FL_NIL_V;

    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init,
             "let shared = 1\nlet builtin_only = 2\nlet nil_value = nil\n");
    cf_write(f.user_init, "let shared = 3\nlet user_only = 4\n");
    cf_write(f.workspace_init, "let shared = 5\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&f.ed, "let vm_only = 99\n", 17U),
                      YEW_CMD_OK);

    YEW_ASSERT(yew_config_get_global(&f.ed, "shared", 6U, &value));
    YEW_ASSERT_EQ_I64(value.t, FL_INT);
    YEW_ASSERT_EQ_I64(value.as.i, 5);
    YEW_ASSERT(yew_config_get_global(&f.ed, "user_only", 9U, &value));
    YEW_ASSERT_EQ_I64(value.as.i, 4);
    YEW_ASSERT(yew_config_get_global(&f.ed, "builtin_only", 12U, &value));
    YEW_ASSERT_EQ_I64(value.as.i, 2);
    YEW_ASSERT(yew_config_get_global(&f.ed, "nil_value", 9U, &value));
    YEW_ASSERT_EQ_I64(value.t, FL_NIL);
    YEW_ASSERT(!yew_config_get_global(&f.ed, "missing", 7U, &value));
    YEW_ASSERT(!yew_config_get_global(&f.ed, "vm_only", 7U, &value));
    cf_free(&f);
}

void test_flconf_global_accessor_follows_successful_reload(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;
    FlValue value = FL_NIL_V;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "let setting = 1\n");
    cf_write(f.user_init, "let setting = 2\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(yew_config_get_global(&f.ed, "setting", 7U, &value));
    YEW_ASSERT_EQ_I64(value.as.i, 2);

    cf_write(f.user_init, "let replacement = 3\n");
    YEW_ASSERT_EQ_I64(yew_config_reload(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(yew_config_get_global(&f.ed, "setting", 7U, &value));
    YEW_ASSERT_EQ_I64(value.as.i, 1);
    YEW_ASSERT(yew_config_get_global(&f.ed, "replacement", 11U, &value));
    YEW_ASSERT_EQ_I64(value.as.i, 3);
    cf_free(&f);
}

void test_flconf_parse_error_isolated_to_its_source(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;

    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "fn broken(\n");
    cf_write(f.user_init, "set({errorbells: true})\n");
    cf_write(f.workspace_init, "set({\"search.ignorecase\": true})\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_PARSE);
    YEW_ASSERT(cf_opt(&f, "errorbells").as.b);
    YEW_ASSERT(cf_opt(&f, "search.ignorecase").as.b);
    cf_free(&f);
}

void test_flconf_reload_parse_error_keeps_old_binding_live(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;
    KeyId key;
    const Binding *binding = NULL;
    u32 binds;
    u32 ledger;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init, "bind(\"L\", \"Z\", \"ed.nop\")\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    binds = yew_bind_active_count(&f.ed);
    ledger = cf_active_ledger(&f);
    YEW_ASSERT_EQ_U64(binds, 1U);
    cf_write(f.user_init, "bind(\"L\", \"Z\",\n");
    YEW_ASSERT_EQ_I64(yew_config_reload(&f.ed, NULL), YEW_CFG_PARSE);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&f.ed), binds);
    YEW_ASSERT_EQ_U64(cf_active_ledger(&f), ledger);
    YEW_ASSERT_EQ_U64(yew_key_parse_seq("Z", &key, 1U), 1U);
    YEW_ASSERT_EQ_I64(yew_keymap_lookup(&f.ed.bind_keys[YEW_MODE_L], &key,
                                        1U, NULL, &binding),
                      YEW_MATCH_FULL);
    YEW_ASSERT_NOT_NULL(binding);
    YEW_ASSERT_EQ_STR(yew_cmd_desc(binding->cmd)->name, "ed.nop");
    cf_free(&f);
}

void test_flconf_runtime_error_tears_down_source_and_continues(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init,
             "bind(\"L\", \"Z\", \"ed.nop\")\nerror(\"boom\")\n");
    cf_write(f.user_init, "set({errorbells: true})\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_RUN);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&f.ed), 0U);
    YEW_ASSERT(cf_opt(&f, "errorbells").as.b);
    cf_free(&f);
}

void test_flconf_twenty_reloads_leave_no_registry_residue(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;
    u32 ledger_len;
    u32 hook_len;
    u32 rebuilds;
    u32 i;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init,
             "bind(\"L\", \"Z\", \"ed.nop\")\n"
             "on(\"ws.open\", fn(ws) set({errorbells: true}))\n");
    rebuilds = yew_bind_rebuild_count(&f.ed);
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT_EQ_U64(yew_bind_rebuild_count(&f.ed), rebuilds + 1U);
    ledger_len = f.ed.hooks.ledger.n;
    hook_len = f.ed.hooks.n;
    for (i = 0U; i < 100U; i++) {
        rebuilds = yew_bind_rebuild_count(&f.ed);
        YEW_ASSERT_EQ_I64(yew_config_reload(&f.ed, NULL), YEW_CFG_OK);
        YEW_ASSERT_EQ_U64(yew_bind_rebuild_count(&f.ed), rebuilds + 1U);
        YEW_ASSERT_EQ_U64(f.ed.hooks.ledger.n, ledger_len);
        YEW_ASSERT_EQ_U64(f.ed.hooks.n, hook_len);
        YEW_ASSERT_EQ_U64(cf_active_ledger(&f), 2U);
        YEW_ASSERT_EQ_U64(yew_bind_active_count(&f.ed), 1U);
    }
    cf_free(&f);
}

void test_flconf_reloaded_hook_fires_once(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;
    OptVal off = {YEW_OPT_BOOL, {.b = false}};
    const char *err = NULL;
    u32 active_before;
    u32 ledger_len;
    u32 i;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init,
             "on(\"ws.open\", fn(ws) set({errorbells: true}))\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    for (i = 0U; i < 20U; i++)
        YEW_ASSERT_EQ_I64(yew_config_reload(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(yew_opt_set(&f.ed, YEW_OPT_GLOBAL, "errorbells", 10U, &off,
                           &err));
    active_before = cf_active_ledger(&f);
    yew_fl_hook_workspace(&f.ed, FL_EV_WS_OPEN);
    YEW_ASSERT(cf_opt(&f, "errorbells").as.b);
    YEW_ASSERT_EQ_U64(cf_active_ledger(&f), active_before + 1U);
    ledger_len = f.ed.hooks.ledger.n;
    for (i = 0U; i < 20U; i++) {
        YEW_ASSERT(yew_opt_set(&f.ed, YEW_OPT_GLOBAL, "errorbells", 10U,
                               &off, &err));
        yew_fl_hook_workspace(&f.ed, FL_EV_WS_OPEN);
        YEW_ASSERT(cf_opt(&f, "errorbells").as.b);
        YEW_ASSERT_EQ_U64(cf_active_ledger(&f), active_before + 1U);
        YEW_ASSERT_EQ_U64(f.ed.hooks.ledger.n, ledger_len);
    }
    YEW_ASSERT(yew_opt_set(&f.ed, YEW_OPT_GLOBAL, "errorbells", 10U, &off,
                           &err));
    yew_origin_teardown(&f.ed, FL_ORIGIN_ID_CONFIG);
    YEW_ASSERT(!cf_opt(&f, "errorbells").as.b);
    cf_free(&f);
}

void test_flconf_deleted_set_restores_default_on_reload(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init, "set({errorbells: true})\n");
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(cf_opt(&f, "errorbells").as.b);
    cf_write(f.user_init, "\n");
    YEW_ASSERT_EQ_I64(yew_config_reload(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(!cf_opt(&f, "errorbells").as.b);
    cf_free(&f);
}

void test_flconf_clean_skips_every_fletch_config_source(void)
{
    YewEdStartup startup = {0};
    ConfigFix f;
    u32 rebuilds;

    startup.clean = true;
    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "set({errorbells: true})\n");
    cf_write(f.user_init, "bind(\"L\", \"Z\", \"ed.nop\")\n");
    cf_write(f.workspace_init, "set({tabwidth: 9})\n");
    rebuilds = yew_bind_rebuild_count(&f.ed);
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f.ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(!cf_opt(&f, "errorbells").as.b);
    YEW_ASSERT_EQ_I64(cf_opt(&f, "tabwidth").as.i, 4);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&f.ed), 0U);
    YEW_ASSERT_EQ_U64(cf_active_ledger(&f), 0U);
    YEW_ASSERT_EQ_U64(yew_bind_rebuild_count(&f.ed), rebuilds);
    cf_free(&f);
}
