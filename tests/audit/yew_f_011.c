/*
 * YEW-F-011 — identical source metadata can retain stale syntax tables.
 *
 * Correct behavior: Sprint 58 F10 requires the cache invalidation ladder to
 * detect an older same-sized definition even when its mtime is restored to
 * the cached value.  The source hash in the cache header must decide whether
 * the cached tables still describe the file.
 *
 * Baseline failure: yew_syn_def_load returns the cached tables immediately
 * when size and nanosecond mtime match.  It reads and hashes the source only
 * after either metadata value differs, so replacing "x" with same-sized "y"
 * while restoring the timestamp leaves the stale "x" rule active.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/diag.h"
#include "syn/defs.h"
#include "unit/stat_time.h"
#include "util/arena.h"
#include "util/base.h"

static const char source_x[] =
    "{ syntax: 1, language: { name: \"ini\" }, contexts: { "
    "main: { default: \"text\", rules: [ { match: \"x\", "
    "attr: \"number\" }, ], }, }, }\n";

static const char source_y[] =
    "{ syntax: 1, language: { name: \"ini\" }, contexts: { "
    "main: { default: \"text\", rules: [ { match: \"y\", "
    "attr: \"number\" }, ], }, }, }\n";

static char *copy_env(const char *value)
{
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value) + 1U;
    copy = malloc(len);
    if (copy != NULL)
        (void)memcpy(copy, value, len);
    return copy;
}

static bool write_source(const char *path, const char *source)
{
    size_t len = strlen(source);
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return false;
    while (at < len) {
        ssize_t n = write(fd, source + at, len - at);

        if (n <= 0) {
            (void)close(fd);
            return false;
        }
        at += (size_t)n;
    }
    return close(fd) == 0;
}

static bool load_rule(const char *path, const char *want, u64 *compiles)
{
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    bool matches;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    yew_syn_compile_count_reset();
    def = yew_syn_def_load(&arena, &dc, path);
    *compiles = yew_syn_compile_count();
    matches = def != NULL && fl_diag_errors(&dc) == 0U &&
              strcmp(yew_syn_rule_pattern(def, 0U), want) == 0;
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
    return matches;
}

bool test_yew_f_011(char *why, size_t why_cap)
{
    char root[] = "/tmp/yew-f011-XXXXXX";
    const char *source = "runtime/syntax/ini.fl";
    char absolute_source[160] = "";
    char runtime_dir[160] = "";
    char syntax_dir[160] = "";
    char syn_dir[160] = "";
    char yew_dir[160] = "";
    char old_cwd[1024] = "";
    char *cache = NULL;
    char *saved_xdg = copy_env(getenv("XDG_CACHE_HOME"));
    char *saved_bypass = copy_env(getenv("YEW_NO_SYN_CACHE"));
    struct stat original;
    struct timespec times[2];
    u64 cold_compiles = UINT64_MAX;
    u64 changed_compiles = UINT64_MAX;
    bool cold_ok = false;
    bool changed_ok = false;
    bool ready = false;
    bool correct;

    _Static_assert(sizeof(source_x) == sizeof(source_y),
                   "audit cache definitions must have identical size");
    if (getcwd(old_cwd, sizeof(old_cwd)) == NULL || mkdtemp(root) == NULL ||
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime", root) <= 0 ||
        snprintf(syntax_dir, sizeof(syntax_dir), "%s/runtime/syntax", root) <= 0 ||
        snprintf(absolute_source, sizeof(absolute_source),
                 "%s/runtime/syntax/ini.fl", root) <= 0 ||
        mkdir(runtime_dir, 0700) != 0 || mkdir(syntax_dir, 0700) != 0 ||
        setenv("XDG_CACHE_HOME", root, 1) != 0 ||
        unsetenv("YEW_NO_SYN_CACHE") != 0 || chdir(root) != 0)
        goto done;
    yew_syn_cache_set_bypass(false);
    if (!write_source(source, source_x) || stat(source, &original) != 0)
        goto done;
    cache = yew_syn_cache_path("ini");
    if (cache == NULL)
        goto done;
    cold_ok = load_rule(source, "x", &cold_compiles);
    if (!cold_ok || cold_compiles != 1U || access(cache, F_OK) != 0)
        goto done;
    if (!write_source(source, source_y))
        goto done;
    times[0] = yew_test_stat_atime(&original);
    times[1] = yew_test_stat_mtime(&original);
    if (utimensat(AT_FDCWD, source, times, 0) != 0)
        goto done;
    changed_ok = load_rule(source, "y", &changed_compiles);
    ready = true;

done:
    correct = ready && changed_ok && changed_compiles == 1U;
    if (!correct) {
        (void)snprintf(why, why_cap,
                       "ready=%u cold=%u/%llu changed=%u/%llu; matching size+mtime retained stale rule",
                       ready ? 1U : 0U, cold_ok ? 1U : 0U,
                       (unsigned long long)cold_compiles,
                       changed_ok ? 1U : 0U,
                       (unsigned long long)changed_compiles);
    }
    if (cache != NULL)
        (void)unlink(cache);
    if (old_cwd[0] != '\0')
        (void)chdir(old_cwd);
    (void)snprintf(syn_dir, sizeof(syn_dir), "%s/yew/syn", root);
    (void)snprintf(yew_dir, sizeof(yew_dir), "%s/yew", root);
    (void)rmdir(syn_dir);
    (void)rmdir(yew_dir);
    if (absolute_source[0] != '\0')
        (void)unlink(absolute_source);
    if (syntax_dir[0] != '\0')
        (void)rmdir(syntax_dir);
    if (runtime_dir[0] != '\0')
        (void)rmdir(runtime_dir);
    (void)rmdir(root);
    yew_xfree(cache);
    yew_syn_cache_set_bypass(false);
    if (saved_xdg != NULL)
        (void)setenv("XDG_CACHE_HOME", saved_xdg, 1);
    else
        (void)unsetenv("XDG_CACHE_HOME");
    if (saved_bypass != NULL)
        (void)setenv("YEW_NO_SYN_CACHE", saved_bypass, 1);
    else
        (void)unsetenv("YEW_NO_SYN_CACHE");
    free(saved_xdg);
    free(saved_bypass);
    return correct;
}
