#ifndef YEW_TEST_COMPSPEC_FIX_H
#define YEW_TEST_COMPSPEC_FIX_H

/*
 * Sprint 57.24: a hermetic spec environment.  The shipped runtime is the
 * checked-in runtime/ by absolute path, the user's config directory is a
 * fresh temporary one (never the developer's ~/.config), and HOME points
 * into it too, so nothing a test enumerates reads the real ~/.ssh.
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "ui/compgen.h"
#include "ui/compspec.h"
#include "util/buf.h"

typedef struct SpecFix {
    char root[64];
    char config[128];
    char home[128];
    char runtime[PATH_MAX];
    char *old_runtime;
    char *old_config;
    char *old_home;
} SpecFix;

static inline char *spec_env_copy(const char *name)
{
    const char *v = getenv(name);

    return v == NULL ? NULL : yew_xstrdup(v);
}

static inline void spec_env_restore(const char *name, char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
    yew_xfree(value);
}

static inline void spec_fix_init(SpecFix *f)
{
    char dir[160];
    char cwd[PATH_MAX];

    (void)memset(f, 0, sizeof(*f));
    f->old_runtime = spec_env_copy("YEW_RUNTIME_DIR");
    f->old_config = spec_env_copy("XDG_CONFIG_HOME");
    f->old_home = spec_env_copy("HOME");
    YEW_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    if (f->old_runtime != NULL && f->old_runtime[0] != '\0')
        (void)snprintf(f->runtime, sizeof(f->runtime), "%s", f->old_runtime);
    else
        (void)snprintf(f->runtime, sizeof(f->runtime), "%s/runtime", cwd);
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-compspec-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    (void)snprintf(f->home, sizeof(f->home), "%s/home", f->root);
    YEW_ASSERT_EQ_I64(mkdir(f->config, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(f->home, 0700), 0);
    (void)snprintf(dir, sizeof(dir), "%s/yew", f->config);
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    (void)snprintf(dir, sizeof(dir), "%s/yew/completions", f->config);
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", f->runtime, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", f->config, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("HOME", f->home, 1), 0);
    yew_compspec_invalidate_all();
    yew_compgen_cache_clear();
}

static inline void spec_fix_user(const SpecFix *f, const char *name,
                                 const char *text)
{
    char path[256];
    FILE *fp;

    (void)snprintf(path, sizeof(path), "%s/yew/completions/%s.fl",
                   f->config, name);
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, strlen(text), fp), strlen(text));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static inline void spec_read_file(const char *path, Bytebuf *out)
{
    char chunk[4096];
    FILE *fp = fopen(path, "rb");
    size_t n;

    YEW_ASSERT_NOT_NULL(fp);
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) != 0U)
        bytebuf_append(out, chunk, n);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

/* Remove a directory tree a test made under /tmp/yew-*. */
static inline void spec_rm_tree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;

    YEW_ASSERT(strncmp(path, "/tmp/yew-", 9U) == 0);
    if (lstat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        YEW_ASSERT_EQ_I64(unlink(path), 0);
        return;
    }
    d = opendir(path);
    YEW_ASSERT_NOT_NULL(d);
    while ((e = readdir(d)) != NULL) {
        char child[PATH_MAX];

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        (void)snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        spec_rm_tree(child);
    }
    YEW_ASSERT_EQ_I64(closedir(d), 0);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
}

static inline void spec_fix_drop(SpecFix *f)
{
    yew_compspec_invalidate_all();
    yew_compgen_cache_clear();
    spec_env_restore("YEW_RUNTIME_DIR", f->old_runtime);
    spec_env_restore("XDG_CONFIG_HOME", f->old_config);
    spec_env_restore("HOME", f->old_home);
    spec_rm_tree(f->root);
}

#endif
