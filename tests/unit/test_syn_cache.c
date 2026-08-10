#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/diag.h"
#include "syn/defs.h"
#include "util/arena.h"
#include "util/buf.h"

static const char cache_source_x[] =
    "{ syntax: 1, language: { name: \"cache-fixture\", "
    "extensions: [\"cache\"], }, contexts: { main: { default: \"text\", "
    "rules: [ { match: \"x\", attr: \"text\" }, ], }, }, }\n";

static const char cache_source_y[] =
    "{ syntax: 1, language: { name: \"cache-fixture\", "
    "extensions: [\"cache\"], }, contexts: { main: { default: \"text\", "
    "rules: [ { match: \"y\", attr: \"text\" }, ], }, }, }\n";

typedef struct CacheFixture {
    char root[64];
    char source[128];
    char cache[160];
    char *saved_xdg;
    char *saved_no_cache;
} CacheFixture;

typedef struct LoadedDef {
    Arena arena;
    DiagCtx dc;
    SynDef *def;
} LoadedDef;

static char *heap_copy(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL)
        return NULL;
    n = strlen(s) + 1U;
    copy = malloc(n);
    YEW_ASSERT_NOT_NULL(copy);
    (void)memcpy(copy, s, n);
    return copy;
}

static void write_exact(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    YEW_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        if (n < 0 && errno == EINTR)
            continue;
        YEW_ASSERT(n > 0);
        at += (size_t)n;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static Bytebuf read_exact(const char *path)
{
    Bytebuf out;
    u8 block[4096];
    int fd = open(path, O_RDONLY);

    bytebuf_init(&out);
    YEW_ASSERT(fd >= 0);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        if (n < 0 && errno == EINTR)
            continue;
        YEW_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(&out, block, (size_t)n);
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
    return out;
}

static void fixture_init(CacheFixture *f)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *no_cache = getenv("YEW_NO_SYN_CACHE");
    char *path;
    int n;

    (void)memset(f, 0, sizeof(*f));
    f->saved_xdg = heap_copy(xdg);
    f->saved_no_cache = heap_copy(no_cache);
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-syn-cache-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    YEW_ASSERT_EQ_I64(setenv("XDG_CACHE_HOME", f->root, 1), 0);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_NO_SYN_CACHE"), 0);
    yew_syn_cache_set_bypass(false);
    n = snprintf(f->source, sizeof(f->source), "%s/fixture.fl", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->source));
    write_exact(f->source, (const u8 *)cache_source_x,
                strlen(cache_source_x));
    path = yew_syn_cache_path("fixture");
    YEW_ASSERT_NOT_NULL(path);
    n = snprintf(f->cache, sizeof(f->cache), "%s", path);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->cache));
    free(path);
}

static void fixture_free(CacheFixture *f)
{
    char path[160];

    yew_syn_cache_set_bypass(false);
    if (f->saved_xdg != NULL)
        YEW_ASSERT_EQ_I64(setenv("XDG_CACHE_HOME", f->saved_xdg, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("XDG_CACHE_HOME"), 0);
    if (f->saved_no_cache != NULL)
        YEW_ASSERT_EQ_I64(setenv("YEW_NO_SYN_CACHE", f->saved_no_cache, 1),
                          0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("YEW_NO_SYN_CACHE"), 0);
    (void)unlink(f->cache);
    (void)snprintf(path, sizeof(path), "%s/yew/syn/keep.txt", f->root);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/yew/syn", f->root);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), "%s/yew", f->root);
    (void)unlink(path);
    (void)rmdir(path);
    YEW_ASSERT_EQ_I64(unlink(f->source), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    free(f->saved_xdg);
    free(f->saved_no_cache);
}

static LoadedDef load_def(const CacheFixture *f)
{
    LoadedDef loaded;

    arena_init(&loaded.arena);
    fl_diag_init(&loaded.dc, &loaded.arena);
    loaded.def = yew_syn_def_load(&loaded.arena, &loaded.dc, f->source);
    YEW_ASSERT_NOT_NULL(loaded.def);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&loaded.dc), 0U);
    return loaded;
}

static void loaded_free(LoadedDef *loaded)
{
    yew_syn_def_dispose(loaded->def);
    arena_free_all(&loaded->arena);
}

static void build_cold_cache(CacheFixture *f)
{
    LoadedDef loaded;

    yew_syn_compile_count_reset();
    loaded = load_def(f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT_EQ_I64(access(f->cache, F_OK), 0);
    loaded_free(&loaded);
}

static void touch_source(const CacheFixture *f)
{
    struct stat st;
    struct timespec times[2];

    YEW_ASSERT_EQ_I64(stat(f->source, &st), 0);
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    times[1].tv_sec += 2;
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, f->source, times, 0), 0);
}

static void corrupt_cache(CacheFixture *f, size_t off)
{
    Bytebuf bytes = read_exact(f->cache);

    YEW_ASSERT(bytes.len > YEW_SYN_CACHE_HEADER_SIZE);
    YEW_ASSERT(off < bytes.len);
    bytes.data[off] ^= UINT8_C(0x5a);
    write_exact(f->cache, bytes.data, bytes.len);
    bytebuf_free(&bytes);
}

static void assert_corruption_recompiles(CacheFixture *f)
{
    LoadedDef loaded;

    yew_test_capture_log();
    yew_syn_compile_count_reset();
    loaded = load_def(f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "syntax cache corrupt; recompiling"));
    YEW_ASSERT_EQ_I64(access(f->cache, F_OK), 0);
    loaded_free(&loaded);
}

void test_syn_cache_path_uses_temp_xdg_root(void)
{
    CacheFixture f;
    char expected[160];
    char *dir;
    char *path;

    fixture_init(&f);
    (void)snprintf(expected, sizeof(expected), "%s/yew/syn", f.root);
    dir = yew_syn_cache_dir();
    YEW_ASSERT_EQ_STR(dir, expected);
    (void)snprintf(expected, sizeof(expected), "%s/yew/syn/alpha.stab",
                   f.root);
    path = yew_syn_cache_path("alpha");
    YEW_ASSERT_EQ_STR(path, expected);
    YEW_ASSERT_NULL(yew_syn_cache_path("bad/name"));
    YEW_ASSERT_NULL(yew_syn_cache_path(""));
    free(dir);
    free(path);
    fixture_free(&f);
}

void test_syn_cache_cold_write_then_warm_load_avoids_recompile(void)
{
    CacheFixture f;
    LoadedDef loaded;
    struct stat st;

    fixture_init(&f);
    build_cold_cache(&f);
    YEW_ASSERT_EQ_I64(stat(f.cache, &st), 0);
    YEW_ASSERT((u64)st.st_size > YEW_SYN_CACHE_HEADER_SIZE);
    yew_syn_compile_count_reset();
    loaded = load_def(&f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U), "x");
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_touch_without_content_change_avoids_recompile(void)
{
    CacheFixture f;
    LoadedDef loaded;
    struct stat before;
    struct stat after;

    fixture_init(&f);
    build_cold_cache(&f);
    YEW_ASSERT_EQ_I64(stat(f.cache, &before), 0);
    touch_source(&f);
    yew_syn_compile_count_reset();
    loaded = load_def(&f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U), "x");
    YEW_ASSERT_EQ_I64(stat(f.cache, &after), 0);
    YEW_ASSERT(after.st_mtim.tv_sec >= before.st_mtim.tv_sec);
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_content_change_recompiles(void)
{
    CacheFixture f;
    LoadedDef loaded;

    _Static_assert(sizeof(cache_source_x) == sizeof(cache_source_y),
                   "cache content fixtures must have equal size");
    fixture_init(&f);
    build_cold_cache(&f);
    touch_source(&f);
    write_exact(f.source, (const u8 *)cache_source_y,
                strlen(cache_source_y));
    touch_source(&f);
    yew_syn_compile_count_reset();
    loaded = load_def(&f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U), "y");
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_truncation_recompiles_safely(void)
{
    CacheFixture f;
    Bytebuf bytes;

    fixture_init(&f);
    build_cold_cache(&f);
    bytes = read_exact(f.cache);
    YEW_ASSERT(bytes.len > 17U);
    write_exact(f.cache, bytes.data, 17U);
    bytebuf_free(&bytes);
    assert_corruption_recompiles(&f);
    fixture_free(&f);
}

void test_syn_cache_bad_magic_recompiles_safely(void)
{
    CacheFixture f;

    fixture_init(&f);
    build_cold_cache(&f);
    corrupt_cache(&f, 0U);
    assert_corruption_recompiles(&f);
    fixture_free(&f);
}

void test_syn_cache_bad_version_recompiles_safely(void)
{
    CacheFixture f;

    fixture_init(&f);
    build_cold_cache(&f);
    corrupt_cache(&f, 8U);
    assert_corruption_recompiles(&f);
    fixture_free(&f);
}

void test_syn_cache_bad_crc_recompiles_safely(void)
{
    CacheFixture f;

    fixture_init(&f);
    build_cold_cache(&f);
    corrupt_cache(&f, 60U);
    assert_corruption_recompiles(&f);
    fixture_free(&f);
}

void test_syn_cache_environment_bypass_never_reads_or_writes(void)
{
    CacheFixture f;
    LoadedDef loaded;

    fixture_init(&f);
    build_cold_cache(&f);
    YEW_ASSERT_EQ_I64(setenv("YEW_NO_SYN_CACHE", "1", 1), 0);
    YEW_ASSERT_EQ_I64(unlink(f.cache), 0);
    yew_syn_compile_count_reset();
    loaded = load_def(&f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT(access(f.cache, F_OK) != 0);
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_explicit_bypass_never_reads_or_writes(void)
{
    CacheFixture f;
    LoadedDef loaded;

    fixture_init(&f);
    build_cold_cache(&f);
    YEW_ASSERT_EQ_I64(unlink(f.cache), 0);
    yew_syn_cache_set_bypass(true);
    yew_syn_compile_count_reset();
    loaded = load_def(&f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT(access(f.cache, F_OK) != 0);
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_clear_removes_only_stab_entries(void)
{
    CacheFixture f;
    char keep[160];
    static const u8 marker[] = "keep";

    fixture_init(&f);
    build_cold_cache(&f);
    (void)snprintf(keep, sizeof(keep), "%s/yew/syn/keep.txt", f.root);
    write_exact(keep, marker, sizeof(marker) - 1U);
    YEW_ASSERT(yew_syn_cache_clear());
    YEW_ASSERT(access(f.cache, F_OK) != 0);
    YEW_ASSERT_EQ_I64(access(keep, F_OK), 0);
    YEW_ASSERT(yew_syn_cache_clear());
    fixture_free(&f);
}

void test_syn_cache_write_failure_is_best_effort(void)
{
    CacheFixture f;
    LoadedDef loaded;
    char blocker[160];
    static const u8 marker[] = "not a directory";

    fixture_init(&f);
    (void)snprintf(blocker, sizeof(blocker), "%s/yew", f.root);
    write_exact(blocker, marker, sizeof(marker) - 1U);
    yew_test_capture_log();
    yew_syn_compile_count_reset();
    loaded = load_def(&f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "syntax cache write failed"));
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(access(f.cache, F_OK) != 0);
    loaded_free(&loaded);
    fixture_free(&f);
}
