#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flconf.h"
#include "flfix.h"
#include "mod/ai/policy.h"
#include "syn/defs.h"
#include "syn/theme.h"
#include "util/arena.h"
#include "util/runtime_asset.h"

#if YEW_EMBED_RUNTIME
typedef struct RuntimeFix {
    char root[PATH_MAX];
    char cwd[PATH_MAX];
    char home[PATH_MAX];
    char config[PATH_MAX];
    char state[PATH_MAX];
    char *old_runtime;
    char *old_home;
    char *old_config;
    char *old_state;
    char *old_no_cache;
} RuntimeFix;

static char *env_copy(const char *name)
{
    const char *value = getenv(name);

    return value == NULL ? NULL : yew_xstrdup(value);
}

static void env_restore(const char *name, char *value)
{
    int rc = value == NULL ? unsetenv(name) : setenv(name, value, 1);

    yew_xfree(value);
    YEW_ASSERT_EQ_I64(rc, 0);
}

static void path_make(const char *path)
{
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void path_suffix(char *out, size_t cap, const char *root,
                        const char *suffix)
{
    int n = snprintf(out, cap, "%s%s", root, suffix);

    YEW_ASSERT(n >= 0 && (size_t)n < cap);
}

static void runtime_fix_init(RuntimeFix *fix)
{
    (void)memset(fix, 0, sizeof(*fix));
    YEW_ASSERT_NOT_NULL(getcwd(fix->cwd, sizeof(fix->cwd)));
    fix->old_runtime = env_copy("YEW_RUNTIME_DIR");
    fix->old_home = env_copy("HOME");
    fix->old_config = env_copy("XDG_CONFIG_HOME");
    fix->old_state = env_copy("XDG_STATE_HOME");
    fix->old_no_cache = env_copy("YEW_NO_SYN_CACHE");
    (void)snprintf(fix->root, sizeof(fix->root),
                   "/tmp/yew-runtime-consumer-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fix->root));
    path_suffix(fix->home, sizeof(fix->home), fix->root, "/home");
    path_suffix(fix->config, sizeof(fix->config), fix->root, "/config");
    path_suffix(fix->state, sizeof(fix->state), fix->root, "/state");
    path_make(fix->home);
    path_make(fix->config);
    path_make(fix->state);
    YEW_ASSERT_EQ_I64(setenv("HOME", fix->home, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", fix->config, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fix->state, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_NO_SYN_CACHE", "1", 1), 0);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    YEW_ASSERT_EQ_I64(chdir(fix->root), 0);
}

static void runtime_fix_drop(RuntimeFix *fix)
{
    char path[PATH_MAX];

    YEW_ASSERT_EQ_I64(chdir(fix->cwd), 0);
    path_suffix(path, sizeof(path), fix->config, "/yew/fl/ai-deny.fl");
    (void)unlink(path);
    path_suffix(path, sizeof(path), fix->config, "/yew/fl");
    (void)rmdir(path);
    path_suffix(path, sizeof(path), fix->config, "/yew");
    (void)rmdir(path);
    path_suffix(path, sizeof(path), fix->state, "/yew/log");
    (void)unlink(path);
    path_suffix(path, sizeof(path), fix->state, "/yew");
    (void)rmdir(path);
    YEW_ASSERT_EQ_I64(rmdir(fix->state), 0);
    YEW_ASSERT_EQ_I64(rmdir(fix->config), 0);
    YEW_ASSERT_EQ_I64(rmdir(fix->home), 0);
    YEW_ASSERT_EQ_I64(rmdir(fix->root), 0);
    env_restore("YEW_NO_SYN_CACHE", fix->old_no_cache);
    env_restore("XDG_STATE_HOME", fix->old_state);
    env_restore("XDG_CONFIG_HOME", fix->old_config);
    env_restore("HOME", fix->old_home);
    env_restore("YEW_RUNTIME_DIR", fix->old_runtime);
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t len = strlen(text);

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, len, file), len);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}
#endif

void test_runtime_consumer_syntax_theme_and_policy_use_embedded_bytes(void)
{
#if YEW_EMBED_RUNTIME
    RuntimeFix fix;
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    Theme theme;
    AiPolicyBundle bundle;

    runtime_fix_init(&fix);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    def = yew_syn_def_load(&arena, &dc, "runtime/syntax/c.fl");
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&dc), 0U);
    yew_syn_def_dispose(def);
    yew_theme_init(&theme);
    YEW_ASSERT(yew_theme_select(&theme, "quiver-light", NULL, &dc));
    YEW_ASSERT_EQ_STR(yew_theme_name(&theme), "quiver-light");
    yew_theme_free(&theme);
    YEW_ASSERT(yew_ai_policy_load_paths("runtime/ai-deny.fl", NULL,
                                        false, false, &bundle));
    YEW_ASSERT(yew_ai_redact_policy_len(bundle.redact) > 0U);
    yew_ai_policy_bundle_drop(&bundle);
    arena_free_all(&arena);

    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", "runtime", 1), 0);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    def = yew_syn_def_load(&arena, &dc, "runtime/syntax/c.fl");
    YEW_ASSERT_NULL(def);
    YEW_ASSERT(fl_diag_errors(&dc) > 0U);
    yew_test_capture_log();
    YEW_ASSERT(yew_ai_policy_load_paths("runtime/ai-deny.fl", NULL,
                                        false, false, &bundle));
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_WARN, "shipped file unusable; retaining compiled protection"));
    yew_ai_policy_bundle_drop(&bundle);
    arena_free_all(&arena);
    runtime_fix_drop(&fix);
#else
    YEW_ASSERT_EQ_U64(yew_runtime_asset_count(), 0U);
#endif
}

void test_runtime_consumer_fletch_precedence_and_explicit_override(void)
{
#if YEW_EMBED_RUNTIME
    RuntimeFix fix;
    FlFix fl;
    char path[PATH_MAX];
    char got[8192];

    runtime_fix_init(&fix);
    flfix_open(&fl);
    fl.origin.path_id = yew_intern(&fl.in, "runtime/syntax/importer.fl",
                                   sizeof("runtime/syntax/importer.fl") - 1U);
    FL_EQ(&fl, "import \"../ai-deny.fl\" as policy\nreturn 42\n", "42");
    flfix_close(&fl);

    flfix_open(&fl);
    flfix_write(&fl, "ai-deny.fl", "let source = \"importer\"\n");
    FL_EQ(&fl, "import \"ai-deny.fl\" as policy\nreturn policy.source\n",
          "importer");
    flfix_close(&fl);

    path_suffix(path, sizeof(path), fix.config, "/yew");
    path_make(path);
    path_suffix(path, sizeof(path), fix.config, "/yew/fl");
    path_make(path);
    path_suffix(path, sizeof(path), fix.config, "/yew/fl/ai-deny.fl");
    write_text(path, "let source = \"user\"\n");
    flfix_open(&fl);
    (void)flfix_tmpdir(&fl);
    FL_EQ(&fl, "import \"ai-deny.fl\" as policy\nreturn policy.source\n",
          "user");
    flfix_close(&fl);
    YEW_ASSERT_EQ_I64(unlink(path), 0);

    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", "runtime", 1), 0);
    flfix_open(&fl);
    (void)flfix_tmpdir(&fl);
    flfix_run(&fl, "import \"ai-deny.fl\" as policy\nreturn 1\n",
              got, sizeof(got));
    YEW_ASSERT_NOT_NULL(strstr(got, "!import: cannot find 'ai-deny.fl'"));
    flfix_close(&fl);
    runtime_fix_drop(&fix);
#else
    YEW_ASSERT_EQ_U64(yew_runtime_asset_count(), 0U);
#endif
}

void test_runtime_consumer_builtin_config_honors_explicit_override(void)
{
#if YEW_EMBED_RUNTIME
    RuntimeFix fix;
    YewEdStartup startup = {0};
    Ed ed;
    OptVal tabwidth;

    runtime_fix_init(&fix);
    startup.no_workspace_config = true;
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_config_init(&ed, &startup);
    YEW_ASSERT_EQ_I64(yew_config_load_all(&ed, NULL), YEW_CFG_OK);
    YEW_ASSERT(yew_opt_get(&ed, yew_ed_doc(&ed), ed.win, "tabwidth", 8U,
                           &tabwidth));
    YEW_ASSERT_EQ_I64(tabwidth.as.i, 4);
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", "runtime", 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_config_init(&ed, &startup);
    YEW_ASSERT_EQ_I64(yew_config_load_all(&ed, NULL), YEW_CFG_MISSING);
    yew_ed_free(&ed);
    runtime_fix_drop(&fix);
#else
    YEW_ASSERT_EQ_U64(yew_runtime_asset_count(), 0U);
#endif
}
