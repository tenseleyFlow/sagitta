#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/diag.h"
#include "fl/flconf.h"
#include "mod/plug/internal.h"
#include "mod/plug/pkg.h"
#include "util/xdg.h"
#include "ws/trust.h"

typedef struct DiscoveryFix {
    char root[256];
    char data[320];
    char config[320];
    char state[320];
    char workspace[320];
    char *old_data;
    char *old_config;
    char *old_state;
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
} DiscoveryFix;

static char *test_strdup(const char *text)
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

static void write_all(const char *path, const char *text)
{
    size_t len = strlen(text);
    size_t off = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    YEW_ASSERT(fd >= 0);
    while (fd >= 0 && off < len) {
        ssize_t n = write(fd, text + off, len - off);

        YEW_ASSERT(n > 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    if (fd >= 0)
        YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void remove_tree(const char *path)
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
            remove_tree(child);
        else
            YEW_ASSERT_EQ_I64(unlink(child), 0);
    }
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
}

static void restore_env(const char *name, const char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
}

static void fixture_init(DiscoveryFix *f)
{
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-plug-discovery-XXXXXX",
                 sizeof("/tmp/yew-plug-discovery-XXXXXX"));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->data));
    n = snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->config));
    n = snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->state));
    n = snprintf(f->workspace, sizeof(f->workspace), "%s/workspace",
                 f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->workspace));
    YEW_ASSERT(yew_mkdirs(f->data, 0700U));
    YEW_ASSERT(yew_mkdirs(f->config, 0700U));
    YEW_ASSERT(yew_mkdirs(f->state, 0700U));
    YEW_ASSERT(yew_mkdirs(f->workspace, 0700U));
    f->old_data = test_strdup(getenv("XDG_DATA_HOME"));
    f->old_config = test_strdup(getenv("XDG_CONFIG_HOME"));
    f->old_state = test_strdup(getenv("XDG_STATE_HOME"));
    YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", f->data, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", f->config, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state, 1), 0);
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    arena_init(&f->diag_arena);
    fl_diag_init(&f->dc, &f->diag_arena);
    yew_trust_db_init(&f->trust);
}

static void fixture_done(DiscoveryFix *f)
{
    yew_trust_db_free(&f->trust);
    arena_free_all(&f->diag_arena);
    yew_ed_free(&f->ed);
    restore_env("XDG_DATA_HOME", f->old_data);
    restore_env("XDG_CONFIG_HOME", f->old_config);
    restore_env("XDG_STATE_HOME", f->old_state);
    free(f->old_data);
    free(f->old_config);
    free(f->old_state);
    remove_tree(f->root);
}

static void make_plugin_caps(const char *plugins_root, const char *name,
                             const char *version, const char *caps,
                             bool valid)
{
    char dir[640];
    char src[672];
    char entry[704];
    char manifest[704];
    char source[1024];
    int n;

    YEW_ASSERT(yew_mkdirs(plugins_root, 0700U));
    n = snprintf(dir, sizeof(dir), "%s/%s", plugins_root, name);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(dir));
    n = snprintf(src, sizeof(src), "%s/src", dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(src));
    YEW_ASSERT(yew_mkdirs(src, 0700U));
    n = snprintf(entry, sizeof(entry), "%s/main.fl", src);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(entry));
    n = snprintf(manifest, sizeof(manifest), "%s/plugin.fl", dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(manifest));
    write_all(entry, "{ init: fn(ctx) { nil } }\n");
    if (valid) {
        n = snprintf(source, sizeof(source),
                     "{name: \"%s\", version: \"%s\", api: 1, "
                     "entry: \"src/main.fl\", capabilities: %s, "
                     "events: [], description: \"fixture\"}\n",
                     name, version, caps);
    } else {
        n = snprintf(source, sizeof(source),
                     "{name: \"%s\", version: \"%s\", api: 1, "
                     "entry: \"src/main.fl\", capabilities: [], "
                     "events: [], mystery: true}\n", name, version);
    }
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(source));
    write_all(manifest, source);
}

static void make_plugin(const char *plugins_root, const char *name,
                        const char *version, bool valid)
{
    make_plugin_caps(plugins_root, name, version, "[]", valid);
}

static void fixture_restart_editor(DiscoveryFix *f)
{
    yew_trust_db_free(&f->trust);
    yew_ed_free(&f->ed);
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    yew_trust_db_init(&f->trust);
}

static void source_roots(const DiscoveryFix *f, char *data, size_t nd,
                         char *config, size_t nc, char *workspace,
                         size_t nw)
{
    int n;

    n = snprintf(data, nd, "%s/yew/plugins", f->data);
    YEW_ASSERT(n > 0 && (size_t)n < nd);
    n = snprintf(config, nc, "%s/yew/plugins", f->config);
    YEW_ASSERT(n > 0 && (size_t)n < nc);
    n = snprintf(workspace, nw, "%s/.yew/plugins", f->workspace);
    YEW_ASSERT(n > 0 && (size_t)n < nw);
}

static void write_lock_entry(const DiscoveryFix *f, const char *name,
                             const char tree[17])
{
    char path[640];
    char text[1536];
    int n;

    n = snprintf(path, sizeof(path), "%s/yew/plugins.lock", f->data);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(text, sizeof(text),
                 "{schema: 1, plugins: {\"%s\": {"
                 "url: \"file:///fixture\", shorthand: \"\", "
                 "rev: \"0123456789abcdef0123456789abcdef01234567\", "
                 "pin: \"head\", tree: \"%s\", "
                 "installed_at: 1, updated_at: 1}}}\n",
                 name, tree);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(text));
    write_all(path, text);
}

void test_plug_discovery_workspace_blocks_after_shadowing_lower_sources(void)
{
    DiscoveryFix f;
    char data[512], config[512], workspace[512];

    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin(data, "same", "1.0.0", true);
    make_plugin(config, "same", "2.0.0", true);
    make_plugin(workspace, "same", "3.0.0", true);
    /* A drifted managed lower-precedence plugin must not revoke or warn when
     * an unmanaged config/workspace winner shadows it. */
    write_lock_entry(&f, "same", "0000000000000000");
    yew_test_capture_log();

    YEW_ASSERT(yew_plug_discover_with_policy(&f.ed, false, &f.trust,
                                              &f.dc));
    YEW_ASSERT_NOT_NULL(f.ed.plug);
    YEW_ASSERT_EQ_U64(f.ed.plug->n, 3U);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->st, PLUG_SHADOWED);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[1]->st, PLUG_SHADOWED);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[2]->st, PLUG_BLOCKED);
    YEW_ASSERT(!f.ed.plug->v[0]->winner);
    YEW_ASSERT(!f.ed.plug->v[1]->winner);
    YEW_ASSERT(f.ed.plug->v[2]->winner);
    YEW_ASSERT_EQ_STR(f.ed.plug->v[2]->mf.version, "3.0.0");
    YEW_ASSERT(!yew_test_log_contains(YEW_LOG_WARN,
                                      "changed on disk since install"));
    fixture_done(&f);
}

void test_plug_discovery_trusted_workspace_obeys_explicit_desired_state(void)
{
    DiscoveryFix f;
    char data[512], config[512], workspace[512];

    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin(data, "toggle", "0.5.0", true);
    make_plugin(config, "toggle", "1.0.0", true);
    make_plugin(workspace, "toggle", "2.0.0", true);
    YEW_ASSERT(yew_trust_plugin_set_desired(
        &f.trust, "toggle", YEW_PLUGIN_DESIRED_DISABLED));

    YEW_ASSERT(yew_plug_discover_with_policy(&f.ed, true, &f.trust,
                                              &f.dc));
    YEW_ASSERT_EQ_U64(f.ed.plug->n, 3U);
    /* Trusted precedence is total: data < config < workspace. */
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->st, PLUG_SHADOWED);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[1]->st, PLUG_SHADOWED);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[2]->st, PLUG_DISABLED);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->source, PLUG_SOURCE_DATA);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[1]->source, PLUG_SOURCE_CONFIG);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[2]->source, PLUG_SOURCE_WORKSPACE);
    YEW_ASSERT(!f.ed.plug->v[0]->winner);
    YEW_ASSERT(!f.ed.plug->v[1]->winner);
    YEW_ASSERT(f.ed.plug->v[2]->winner);
    YEW_ASSERT_EQ_STR(f.ed.plug->v[2]->mf.version, "2.0.0");
    fixture_done(&f);
}

void test_plug_discovery_sorts_bytewise_and_keeps_manifest_errors(void)
{
    DiscoveryFix f;
    char data[512], config[512], workspace[512];

    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin(data, "zeta", "1.0.0", true);
    make_plugin(data, "broken", "1.0.0", false);
    make_plugin(data, "alpha", "1.0.0", true);

    YEW_ASSERT(yew_plug_discover_with_policy(&f.ed, false, &f.trust,
                                              &f.dc));
    YEW_ASSERT_EQ_U64(f.ed.plug->n, 3U);
    YEW_ASSERT_EQ_STR(f.ed.plug->v[0]->mf.name_text, "alpha");
    YEW_ASSERT_EQ_STR(f.ed.plug->v[1]->mf.name_text, "broken");
    YEW_ASSERT_EQ_STR(f.ed.plug->v[2]->mf.name_text, "zeta");
    YEW_ASSERT_EQ_U64(f.ed.plug->v[1]->st, PLUG_ERROR);
    YEW_ASSERT_NOT_NULL(f.ed.plug->v[1]->last_error);
    YEW_ASSERT(strstr(f.ed.plug->v[1]->last_error, "mystery") != NULL);
    fixture_done(&f);
}

void test_plug_discovery_clean_editor_scans_nothing(void)
{
    DiscoveryFix f;
    char data[512], config[512], workspace[512];

    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin(data, "ignored", "1.0.0", true);
    f.ed.clean = true;
    YEW_ASSERT(yew_plug_discover(&f.ed, &f.dc));
    YEW_ASSERT(f.ed.plug == NULL);
    fixture_done(&f);
}

void test_plug_discovery_public_seam_uses_loaded_config_policy(void)
{
    DiscoveryFix f;
    char data[512], config[512], workspace[512];

    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin(data, "configured", "1.0.0", true);
    yew_config_init(&f.ed, NULL);
    YEW_ASSERT(yew_config_plugin_set_desired(
        &f.ed, "configured", YEW_PLUGIN_DESIRED_DISABLED));

    YEW_ASSERT(yew_plug_discover(&f.ed, &f.dc));
    YEW_ASSERT_NOT_NULL(f.ed.plug);
    YEW_ASSERT_EQ_U64(f.ed.plug->n, 1U);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->st, PLUG_DISABLED);
    fixture_done(&f);
}

void test_plug_discovery_managed_drift_revokes_persisted_grants(void)
{
    DiscoveryFix f;
    YewTrustDb persisted;
    YewTrustDb check;
    char data[512], config[512], workspace[512];
    char plugin[640];
    char entry[704];
    char expected[17];
    int n;

    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin_caps(data, "drifter", "1.0.0", "[\"fs\"]", true);
    n = snprintf(plugin, sizeof(plugin), "%s/drifter", data);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(plugin));
    YEW_ASSERT(yew_pkg_tree_hash(plugin, expected, &f.dc));
    write_lock_entry(&f, "drifter", expected);

    yew_config_init(&f.ed, NULL);
    YEW_ASSERT(yew_config_plugin_set_capability(
        &f.ed, "drifter", YEW_PLUGIN_CAP_FS, YEW_PLUGIN_GRANT_ALLOW));
    YEW_ASSERT(yew_config_plugin_set_capability(
        &f.ed, "drifter", YEW_PLUGIN_CAP_SHELL, YEW_PLUGIN_GRANT_DENY));

    n = snprintf(entry, sizeof(entry), "%s/src/main.fl", plugin);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(entry));
    write_all(entry, "{ init: fn(ctx) { 1 } }\n");
    yew_test_capture_log();

    YEW_ASSERT(yew_plug_discover(&f.ed, &f.dc));
    YEW_ASSERT_NOT_NULL(f.ed.plug);
    YEW_ASSERT_EQ_U64(f.ed.plug->n, 1U);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->st, PLUG_DISCOVERED);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->session_allow, 0U);
    YEW_ASSERT_EQ_U64(f.ed.plug->v[0]->session_deny, 0U);
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_WARN);
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "plugin \"drifter\" changed on disk since install "
                      "(yew pkg doctor drifter)");
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_WARN,
        "plugin \"drifter\" changed on disk since install "
        "(yew pkg doctor drifter)"));
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_WARN,
        "revoked 2 persisted grant(s) for \"drifter\": code changed"));
    YEW_ASSERT_EQ_U64(yew_config_plugin_capability(
                          &f.ed, "drifter", YEW_PLUGIN_CAP_FS),
                      YEW_PLUGIN_GRANT_UNSET);
    YEW_ASSERT_EQ_U64(yew_config_plugin_capability(
                          &f.ed, "drifter", YEW_PLUGIN_CAP_SHELL),
                      YEW_PLUGIN_GRANT_UNSET);
    /* A later policy write must use the refreshed live DB and cannot
     * resurrect the grants that drift revoked. */
    YEW_ASSERT(yew_config_plugin_set_desired(
        &f.ed, "drifter", YEW_PLUGIN_DESIRED_ENABLED));

    yew_trust_db_init(&check);
    YEW_ASSERT(yew_trust_db_load(&check));
    YEW_ASSERT_EQ_U64(yew_trust_plugin_grant_count(&check, "drifter"), 0U);
    yew_trust_db_free(&check);

    /* Restart from only persisted state.  The changed plugin is still
     * desired, but the old fs answer must not return with it: enabling
     * reaches capability preflight and opens a fresh consent prompt. */
    fixture_restart_editor(&f);
    yew_config_init(&f.ed, NULL);
    YEW_ASSERT(yew_plug_discover(&f.ed, &f.dc));
    YEW_ASSERT(yew_plug_enable_desired(&f.ed, &f.dc));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_PLUGIN_CAP);
    YEW_ASSERT(f.ed.plug->prompt.active);
    YEW_ASSERT_EQ_U64(f.ed.plug->prompt.cap, YEW_CAP_FS);
    YEW_ASSERT_EQ_U64(yew_config_plugin_capability(
                          &f.ed, "drifter", YEW_PLUGIN_CAP_FS),
                      YEW_PLUGIN_GRANT_UNSET);
    fixture_done(&f);

    /* The global opt-out skips both hashing warnings and revocation. */
    fixture_init(&f);
    source_roots(&f, data, sizeof(data), config, sizeof(config), workspace,
                 sizeof(workspace));
    make_plugin(data, "drifter", "1.0.0", true);
    n = snprintf(plugin, sizeof(plugin), "%s/drifter", data);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(plugin));
    YEW_ASSERT(yew_pkg_tree_hash(plugin, expected, &f.dc));
    write_lock_entry(&f, "drifter", expected);
    yew_trust_db_init(&persisted);
    YEW_ASSERT(yew_trust_db_load(&persisted));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &persisted, "drifter", YEW_PLUGIN_CAP_FS, YEW_PLUGIN_GRANT_ALLOW));
    YEW_ASSERT(yew_trust_db_write(
        &persisted, 1001, YEW_TRUST_PRUNE_DAYS_DEFAULT));
    yew_trust_db_free(&persisted);
    {
        const OptVal off = {YEW_OPT_BOOL, {.b = false}};
        const char *err = NULL;

        YEW_ASSERT(yew_opt_set(&f.ed, YEW_OPT_GLOBAL,
                               "plug.verify_on_load", 19U, &off, &err));
    }
    n = snprintf(entry, sizeof(entry), "%s/src/main.fl", plugin);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(entry));
    write_all(entry, "{ init: fn(ctx) { 2 } }\n");
    yew_test_capture_log();
    YEW_ASSERT(yew_plug_discover_with_policy(&f.ed, false, &f.trust,
                                              &f.dc));
    YEW_ASSERT(!yew_test_log_contains(YEW_LOG_WARN,
                                      "changed on disk since install"));
    yew_trust_db_init(&check);
    YEW_ASSERT(yew_trust_db_load(&check));
    YEW_ASSERT_EQ_U64(yew_trust_plugin_grant_count(&check, "drifter"), 1U);
    yew_trust_db_free(&check);
    fixture_done(&f);
}
