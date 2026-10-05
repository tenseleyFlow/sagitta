#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "syn/theme.h"
#include "util/arena.h"
#include "util/runtime_asset.h"

/*
 * yew_runtime_file: a set $YEW_RUNTIME_DIR is the one runtime directory.
 * The fixture plants a DECOY install prefix (through the prefix seam)
 * holding every file the tests ask for, and an explicit runtime
 * directory holding none of them, so any per-file fallback to the
 * installed copy shows up as a decoy path.
 */
typedef struct RtFileFix {
    char root[PATH_MAX];
    char decoy[PATH_MAX];
    char env[PATH_MAX];
    char config[PATH_MAX];
    char *old_runtime;
    char *old_config;
} RtFileFix;

typedef struct RtThemeDiag {
    u32 n;
    char message[256];
} RtThemeDiag;

static char *rt_env_copy(const char *name)
{
    const char *value = getenv(name);

    return value == NULL ? NULL : yew_xstrdup(value);
}

static void rt_env_restore(const char *name, char *value)
{
    int rc = value == NULL ? unsetenv(name) : setenv(name, value, 1);

    yew_xfree(value);
    YEW_ASSERT_EQ_I64(rc, 0);
}

static void rt_join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);

    YEW_ASSERT(n > 0 && (size_t)n < cap);
}

static void rt_write(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t len = strlen(text);

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, len, file), len);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void rt_copy(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    FILE *out = fopen(to, "wb");
    char buf[4096];
    size_t n;

    YEW_ASSERT_NOT_NULL(in);
    YEW_ASSERT_NOT_NULL(out);
    while ((n = fread(buf, 1U, sizeof(buf), in)) != 0U)
        YEW_ASSERT_EQ_U64(fwrite(buf, 1U, n, out), n);
    YEW_ASSERT_EQ_I64(fclose(in), 0);
    YEW_ASSERT_EQ_I64(fclose(out), 0);
}

static void rt_fix_open(RtFileFix *f)
{
    char path[PATH_MAX];

    (void)memset(f, 0, sizeof(*f));
    f->old_runtime = rt_env_copy("YEW_RUNTIME_DIR");
    f->old_config = rt_env_copy("XDG_CONFIG_HOME");
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-rtfile-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    rt_join(f->decoy, sizeof(f->decoy), f->root, "decoy");
    rt_join(f->env, sizeof(f->env), f->root, "env");
    rt_join(f->config, sizeof(f->config), f->root, "config");
    YEW_ASSERT_EQ_I64(mkdir(f->decoy, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(f->env, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(f->config, 0700), 0);
    rt_join(path, sizeof(path), f->decoy, "syntax");
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    rt_join(path, sizeof(path), f->decoy, "syntax/python.fl");
    rt_write(path, "DECOY: not the runtime under test\n");
    rt_join(path, sizeof(path), f->decoy, "themes");
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    rt_join(path, sizeof(path), f->decoy, "themes/quiver-dark.fl");
    rt_copy("runtime/themes/quiver-dark.fl", path);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", f->config, 1), 0);
    yew_runtime_test_set_prefix(f->decoy);
}

static void rt_fix_close(RtFileFix *f)
{
    char path[PATH_MAX];

    yew_runtime_test_set_prefix(NULL);
    rt_join(path, sizeof(path), f->decoy, "themes/quiver-dark.fl");
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    rt_join(path, sizeof(path), f->decoy, "themes");
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
    rt_join(path, sizeof(path), f->decoy, "syntax/python.fl");
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    rt_join(path, sizeof(path), f->decoy, "syntax");
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->decoy), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->env), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->config), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    rt_env_restore("XDG_CONFIG_HOME", f->old_config);
    rt_env_restore("YEW_RUNTIME_DIR", f->old_runtime);
}

void test_runtime_file_env_is_the_only_directory(void)
{
    RtFileFix f;
    char want[PATH_MAX];
    char *path;

    rt_fix_open(&f);
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", f.env, 1), 0);
    /* Missing under $YEW_RUNTIME_DIR: still that path, so the load
     * fails naming it -- never the decoy prefix's readable copy. */
    path = yew_runtime_file("syntax/python.fl");
    rt_join(want, sizeof(want), f.env, "syntax/python.fl");
    YEW_ASSERT_NOT_NULL(path);
    YEW_ASSERT_EQ_STR(path, want);
    yew_xfree(path);
    YEW_ASSERT_NULL(yew_runtime_file(""));
    YEW_ASSERT_NULL(yew_runtime_file(NULL));
    rt_fix_close(&f);
}

void test_runtime_file_unset_env_prefers_prefix_then_source(void)
{
    RtFileFix f;
    char want[PATH_MAX];
    char *path;

    rt_fix_open(&f);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    path = yew_runtime_file("syntax/python.fl");
    rt_join(want, sizeof(want), f.decoy, "syntax/python.fl");
    YEW_ASSERT_NOT_NULL(path);
    YEW_ASSERT_EQ_STR(path, want);
    yew_xfree(path);
    /* Not in the prefix: the repository's copy (unit tests run from the
     * repository root), then nothing. */
    path = yew_runtime_file("syntax/c.fl");
    YEW_ASSERT_NOT_NULL(path);
    YEW_ASSERT_EQ_STR(path, "runtime/syntax/c.fl");
    yew_xfree(path);
    YEW_ASSERT_NULL(yew_runtime_file("syntax/no-such-language.fl"));
    rt_fix_close(&f);
}

static void rt_theme_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                          const char *msg, const char *rendered)
{
    RtThemeDiag *d = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    if (d->n++ == 0U)
        (void)snprintf(d->message, sizeof(d->message), "%s", msg);
}

static bool rt_theme_select(const char *runtime_dir, RtThemeDiag *d)
{
    Arena arena;
    DiagCtx dc;
    Theme theme;
    bool ok;

    (void)memset(d, 0, sizeof(*d));
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, rt_theme_sink, d);
    yew_theme_init(&theme);
    ok = yew_theme_select(&theme, "quiver-dark", runtime_dir, &dc);
    yew_theme_free(&theme);
    arena_free_all(&arena);
    return ok;
}

/* A theme missing from the explicit runtime directory is "not found",
 * not silently read from the installed prefix. */
void test_runtime_file_theme_never_falls_back_past_explicit_runtime(void)
{
    RtFileFix f;
    RtThemeDiag d;

    rt_fix_open(&f);
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", f.env, 1), 0);
    YEW_ASSERT(!rt_theme_select(NULL, &d));
    YEW_ASSERT_EQ_U64(d.n, 1U);
    YEW_ASSERT_NOT_NULL(strstr(d.message, "was not found"));
    YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    YEW_ASSERT(!rt_theme_select(f.env, &d));
    YEW_ASSERT_NOT_NULL(strstr(d.message, "was not found"));
    /* Unset and no explicit directory: the (decoy) prefix is the
     * installed runtime, and is used. */
    YEW_ASSERT(rt_theme_select(NULL, &d));
    YEW_ASSERT_EQ_U64(d.n, 0U);
    rt_fix_close(&f);
}
