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
    copy = sag_xmalloc(strlen(value) + 1U);
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
        SAG_BUG("config test fixture write failed");
}

static void cf_mkdir(const char *path)
{
    if (mkdir(path, 0700) != 0)
        SAG_BUG("config test fixture mkdir failed");
}

static void cf_init(ConfigFix *f, const SagEdStartup *startup)
{
    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-flconf-XXXXXX");
    if (mkdtemp(f->root) == NULL)
        SAG_BUG("config test fixture mkdtemp failed");
    (void)snprintf(f->runtime, sizeof(f->runtime), "%s/runtime", f->root);
    (void)snprintf(f->runtime_init, sizeof(f->runtime_init), "%s/init.fl",
                   f->runtime);
    (void)snprintf(f->xdg_config, sizeof(f->xdg_config), "%s/config",
                   f->root);
    (void)snprintf(f->user_dir, sizeof(f->user_dir), "%s/sagitta",
                   f->xdg_config);
    (void)snprintf(f->user_init, sizeof(f->user_init), "%s/init.fl",
                   f->user_dir);
    (void)snprintf(f->xdg_state, sizeof(f->xdg_state), "%s/state", f->root);
    (void)snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    (void)snprintf(f->workspace_init, sizeof(f->workspace_init),
                   "%s/.sagitta.fl", f->workspace);
    cf_mkdir(f->runtime);
    cf_mkdir(f->xdg_config);
    cf_mkdir(f->user_dir);
    cf_mkdir(f->xdg_state);
    cf_mkdir(f->workspace);
    cf_write(f->runtime_init, "\n");
    f->old_runtime = cf_env_copy("SAG_RUNTIME_DIR");
    f->old_config = cf_env_copy("XDG_CONFIG_HOME");
    f->old_state = cf_env_copy("XDG_STATE_HOME");
    if (setenv("SAG_RUNTIME_DIR", f->runtime, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", f->xdg_config, 1) != 0 ||
        setenv("XDG_STATE_HOME", f->xdg_state, 1) != 0)
        SAG_BUG("config test fixture environment failed");
    sag_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    if (!sag_ed_open_scratch(&f->ed))
        SAG_BUG("config test fixture editor failed");
    sag_config_init(&f->ed, startup);
}

static void cf_free(ConfigFix *f)
{
    char trust_dir[224];
    char trust_file[256];

    sag_ed_free(&f->ed);
    (void)unlink(f->workspace_init);
    (void)unlink(f->user_init);
    (void)unlink(f->runtime_init);
    (void)rmdir(f->workspace);
    (void)rmdir(f->user_dir);
    (void)rmdir(f->xdg_config);
    (void)snprintf(trust_dir, sizeof(trust_dir), "%s/sagitta", f->xdg_state);
    (void)snprintf(trust_file, sizeof(trust_file), "%s/trust.fl", trust_dir);
    (void)unlink(trust_file);
    (void)rmdir(trust_dir);
    (void)rmdir(f->xdg_state);
    (void)rmdir(f->runtime);
    (void)rmdir(f->root);
    cf_env_restore("SAG_RUNTIME_DIR", f->old_runtime);
    cf_env_restore("XDG_CONFIG_HOME", f->old_config);
    cf_env_restore("XDG_STATE_HOME", f->old_state);
}

static OptVal cf_opt(ConfigFix *f, const char *name)
{
    OptVal value = {0};

    SAG_ASSERT(sag_opt_get(&f->ed, sag_ed_doc(&f->ed), f->ed.win, name,
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
    SagEdStartup startup = {0};
    ConfigFix f;

    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "set({tabwidth: 5})\n");
    cf_write(f.user_init, "set({tabwidth: 6})\n");
    cf_write(f.workspace_init, "set({tabwidth: 7})\n");
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_OK);
    SAG_ASSERT_EQ_I64(cf_opt(&f, "tabwidth").as.i, 7);
    SAG_ASSERT_EQ_U64(f.ed.buffer.tabwidth, 7U);
    cf_free(&f);
}

void test_flconf_parse_error_isolated_to_its_source(void)
{
    SagEdStartup startup = {0};
    ConfigFix f;

    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "fn broken(\n");
    cf_write(f.user_init, "set({errorbells: true})\n");
    cf_write(f.workspace_init, "set({\"search.ignorecase\": true})\n");
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_PARSE);
    SAG_ASSERT(cf_opt(&f, "errorbells").as.b);
    SAG_ASSERT(cf_opt(&f, "search.ignorecase").as.b);
    cf_free(&f);
}

void test_flconf_reload_parse_error_keeps_old_binding_live(void)
{
    SagEdStartup startup = {0};
    ConfigFix f;
    KeyId key;
    const Binding *binding = NULL;
    u32 binds;
    u32 ledger;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init, "bind(\"L\", \"Z\", \"ed.nop\")\n");
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_OK);
    binds = sag_bind_active_count(&f.ed);
    ledger = cf_active_ledger(&f);
    SAG_ASSERT_EQ_U64(binds, 1U);
    cf_write(f.user_init, "bind(\"L\", \"Z\",\n");
    SAG_ASSERT_EQ_I64(sag_config_reload(&f.ed, NULL), SAG_CFG_PARSE);
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&f.ed), binds);
    SAG_ASSERT_EQ_U64(cf_active_ledger(&f), ledger);
    SAG_ASSERT_EQ_U64(sag_key_parse_seq("Z", &key, 1U), 1U);
    SAG_ASSERT_EQ_I64(sag_keymap_lookup(&f.ed.bind_keys[SAG_MODE_L], &key,
                                        1U, NULL, &binding),
                      SAG_MATCH_FULL);
    SAG_ASSERT_NOT_NULL(binding);
    SAG_ASSERT_EQ_STR(sag_cmd_desc(binding->cmd)->name, "ed.nop");
    cf_free(&f);
}

void test_flconf_runtime_error_tears_down_source_and_continues(void)
{
    SagEdStartup startup = {0};
    ConfigFix f;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init,
             "bind(\"L\", \"Z\", \"ed.nop\")\nerror(\"boom\")\n");
    cf_write(f.user_init, "set({errorbells: true})\n");
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_RUN);
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&f.ed), 0U);
    SAG_ASSERT(cf_opt(&f, "errorbells").as.b);
    cf_free(&f);
}

void test_flconf_twenty_reloads_leave_no_registry_residue(void)
{
    SagEdStartup startup = {0};
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
    rebuilds = sag_bind_rebuild_count(&f.ed);
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_OK);
    SAG_ASSERT_EQ_U64(sag_bind_rebuild_count(&f.ed), rebuilds + 1U);
    ledger_len = f.ed.hooks.ledger.n;
    hook_len = f.ed.hooks.n;
    for (i = 0U; i < 20U; i++) {
        rebuilds = sag_bind_rebuild_count(&f.ed);
        SAG_ASSERT_EQ_I64(sag_config_reload(&f.ed, NULL), SAG_CFG_OK);
        SAG_ASSERT_EQ_U64(sag_bind_rebuild_count(&f.ed), rebuilds + 1U);
        SAG_ASSERT_EQ_U64(f.ed.hooks.ledger.n, ledger_len);
        SAG_ASSERT_EQ_U64(f.ed.hooks.n, hook_len);
        SAG_ASSERT_EQ_U64(cf_active_ledger(&f), 2U);
        SAG_ASSERT_EQ_U64(sag_bind_active_count(&f.ed), 1U);
    }
    cf_free(&f);
}

void test_flconf_reloaded_hook_fires_once(void)
{
    SagEdStartup startup = {0};
    ConfigFix f;
    OptVal off = {SAG_OPT_BOOL, {.b = false}};
    const char *err = NULL;
    u32 i;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init,
             "on(\"ws.open\", fn(ws) set({errorbells: true}))\n");
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_OK);
    for (i = 0U; i < 20U; i++)
        SAG_ASSERT_EQ_I64(sag_config_reload(&f.ed, NULL), SAG_CFG_OK);
    SAG_ASSERT(sag_opt_set(&f.ed, SAG_OPT_GLOBAL, "errorbells", 10U, &off,
                           &err));
    sag_fl_hook_workspace(&f.ed, FL_EV_WS_OPEN);
    SAG_ASSERT(cf_opt(&f, "errorbells").as.b);
    SAG_ASSERT_EQ_U64(cf_active_ledger(&f), 1U);
    cf_free(&f);
}

void test_flconf_deleted_set_restores_default_on_reload(void)
{
    SagEdStartup startup = {0};
    ConfigFix f;

    startup.no_workspace_config = true;
    cf_init(&f, &startup);
    cf_write(f.user_init, "set({errorbells: true})\n");
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_OK);
    SAG_ASSERT(cf_opt(&f, "errorbells").as.b);
    cf_write(f.user_init, "\n");
    SAG_ASSERT_EQ_I64(sag_config_reload(&f.ed, NULL), SAG_CFG_OK);
    SAG_ASSERT(!cf_opt(&f, "errorbells").as.b);
    cf_free(&f);
}

void test_flconf_clean_skips_every_fletch_config_source(void)
{
    SagEdStartup startup = {0};
    ConfigFix f;
    u32 rebuilds;

    startup.clean = true;
    startup.trust_workspace = true;
    cf_init(&f, &startup);
    cf_write(f.runtime_init, "set({errorbells: true})\n");
    cf_write(f.user_init, "bind(\"L\", \"Z\", \"ed.nop\")\n");
    cf_write(f.workspace_init, "set({tabwidth: 9})\n");
    rebuilds = sag_bind_rebuild_count(&f.ed);
    SAG_ASSERT_EQ_I64(sag_config_load_all(&f.ed, NULL), SAG_CFG_OK);
    SAG_ASSERT(!cf_opt(&f, "errorbells").as.b);
    SAG_ASSERT_EQ_I64(cf_opt(&f, "tabwidth").as.i, 4);
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&f.ed), 0U);
    SAG_ASSERT_EQ_U64(cf_active_ledger(&f), 0U);
    SAG_ASSERT_EQ_U64(sag_bind_rebuild_count(&f.ed), rebuilds);
    cf_free(&f);
}
