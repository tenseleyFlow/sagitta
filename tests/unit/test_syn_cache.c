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
#include "syn/engine.h"
#include "text/journal.h"
#include "util/arena.h"
#include "util/buf.h"

static const char cache_source_x[] =
    "{ syntax: 1, language: { name: \"cache-fixture\", "
    "extensions: [\"cache\"], }, contexts: { main: { default: \"text\", "
    "rules: [ { match: \"x\", attr: \"number\" }, ], }, }, }\n";

static const char cache_source_y[] =
    "{ syntax: 1, language: { name: \"cache-fixture\", "
    "extensions: [\"cache\"], }, contexts: { main: { default: \"text\", "
    "rules: [ { match: \"y\", attr: \"number\" }, ], }, }, }\n";

static const char cache_source_other[] =
    "{ syntax: 1, language: { name: \"other-language\", "
    "extensions: [\"other\"], }, contexts: { main: { default: \"text\", "
    "rules: [ { match: \"z\", attr: \"number\" }, ], }, }, }\n";

static const char cache_source_complex[] =
    "{ syntax: 1, language: { name: \"cache-complex\", "
    "extensions: [\"cx\"], filenames: [\"Complexfile\"], priority: 9, }, "
    "root: \"main\", contexts: { main: { default: \"text\", rules: [ "
    "{ match: \"^([A-Z][a-z]+|[0-9][0-9])=(yes|no)$\", "
    "attr: \"operator\", captures: { 1: \"variable\", 2: \"boolean\" } "
    "}, ], }, tail: { default: \"comment\", rules: [], }, }, }\n";

static const char cache_source_aux[] =
    "{ syntax: 1, language: { name: \"cache-aux\" }, contexts: { "
    "main: { default: \"text\", rules: [ { match: \"^r(#+)\\\"\", "
    "attr: \"string\", set_aux: 1, push: \"raw\" }, ], }, "
    "raw: { default: \"string\", rules: [ { aux: \"literal\", "
    "aux_pre: \"\\\"\", aux_post: \"!\", attr: \"string.escape\", "
    "pop: 1 }, ], }, }, }\n";

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
    path = yew_syn_cache_path("cache-fixture");
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

static void load_def(LoadedDef *loaded, const CacheFixture *f)
{
    arena_init(&loaded->arena);
    fl_diag_init(&loaded->dc, &loaded->arena);
    loaded->def = yew_syn_def_load(&loaded->arena, &loaded->dc, f->source);
    YEW_ASSERT_NOT_NULL(loaded->def);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&loaded->dc), 0U);
}

static void loaded_free(LoadedDef *loaded)
{
    yew_syn_def_dispose(loaded->def);
    arena_free_all(&loaded->arena);
}

static void assert_loaded_regex_executes(LoadedDef *loaded, char byte)
{
    SynSpan spans[4];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynEngine *engine = yew_syn_engine_new(loaded->def);

    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)&byte, 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(out.n, 1U);
    YEW_ASSERT_EQ_U64(out.spans[0].start, 0U);
    YEW_ASSERT_EQ_U64(out.spans[0].len, 1U);
    YEW_ASSERT_EQ_U64(out.spans[0].attr, YEW_ATTR_NUMBER);
    yew_syn_engine_free(engine);
}

static void build_cold_cache(CacheFixture *f)
{
    LoadedDef loaded;
    char *path;
    int n;

    yew_syn_compile_count_reset();
    load_def(&loaded, f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    path = yew_syn_cache_path(loaded.def->name);
    YEW_ASSERT_NOT_NULL(path);
    n = snprintf(f->cache, sizeof(f->cache), "%s", path);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->cache));
    free(path);
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

static void put_u32_le(u8 *dst, u32 value)
{
    dst[0] = (u8)value;
    dst[1] = (u8)(value >> 8U);
    dst[2] = (u8)(value >> 16U);
    dst[3] = (u8)(value >> 24U);
}

static void assert_corruption_recompiles(CacheFixture *f)
{
    LoadedDef loaded;

    yew_test_capture_log();
    yew_syn_compile_count_reset();
    load_def(&loaded, f);
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
    char *ini;

    fixture_init(&f);
    (void)snprintf(expected, sizeof(expected), "%s/yew/syn", f.root);
    dir = yew_syn_cache_dir();
    YEW_ASSERT_EQ_STR(dir, expected);
    (void)snprintf(expected, sizeof(expected), "%s/yew/syn/alpha.stab",
                   f.root);
    path = yew_syn_cache_path("alpha");
    YEW_ASSERT_EQ_STR(path, expected);
    (void)snprintf(expected, sizeof(expected), "%s/yew/syn/ini.stab",
                   f.root);
    ini = yew_syn_cache_path("ini");
    YEW_ASSERT_EQ_STR(ini, expected);
    YEW_ASSERT_NULL(yew_syn_cache_path("bad/name"));
    YEW_ASSERT_NULL(yew_syn_cache_path(""));
    free(dir);
    free(path);
    free(ini);
    fixture_free(&f);
}

void test_syn_cache_same_stems_have_distinct_source_namespaces(void)
{
    CacheFixture f;
    char nested[128];
    char other_source[160];
    char *other_cache;
    LoadedDef loaded;

    fixture_init(&f);
    (void)snprintf(nested, sizeof(nested), "%s/nested", f.root);
    YEW_ASSERT_EQ_I64(mkdir(nested, 0700), 0);
    (void)snprintf(other_source, sizeof(other_source), "%s/fixture.fl",
                   nested);
    write_exact(other_source, (const u8 *)cache_source_other,
                strlen(cache_source_other));
    load_def(&loaded, &f);
    loaded_free(&loaded);
    arena_init(&loaded.arena);
    fl_diag_init(&loaded.dc, &loaded.arena);
    loaded.def = yew_syn_def_load(&loaded.arena, &loaded.dc, other_source);
    YEW_ASSERT_NOT_NULL(loaded.def);
    YEW_ASSERT_EQ_STR(loaded.def->name, "other-language");
    loaded_free(&loaded);
    other_cache = yew_syn_cache_path("other-language");
    YEW_ASSERT_NOT_NULL(other_cache);
    YEW_ASSERT_EQ_I64(access(f.cache, F_OK), 0);
    YEW_ASSERT_EQ_I64(access(other_cache, F_OK), 0);
    YEW_ASSERT(strcmp(f.cache, other_cache) != 0);
    YEW_ASSERT_EQ_I64(unlink(other_cache), 0);
    YEW_ASSERT_EQ_I64(unlink(other_source), 0);
    YEW_ASSERT_EQ_I64(rmdir(nested), 0);
    free(other_cache);
    fixture_free(&f);
}

void test_syn_cache_rejects_blob_copied_from_another_source(void)
{
    CacheFixture f;
    char second_source[128];
    char *second_cache;
    LoadedDef loaded;
    struct stat st;
    struct timespec times[2];

    fixture_init(&f);
    build_cold_cache(&f);
    (void)snprintf(second_source, sizeof(second_source), "%s/other.fl",
                   f.root);
    write_exact(second_source, (const u8 *)cache_source_y,
                strlen(cache_source_y));
    YEW_ASSERT_EQ_I64(stat(f.source, &st), 0);
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, second_source, times, 0), 0);
    second_cache = yew_syn_cache_path("cache-fixture");
    YEW_ASSERT_NOT_NULL(second_cache);
    YEW_ASSERT_EQ_STR(second_cache, f.cache);

    yew_syn_compile_count_reset();
    arena_init(&loaded.arena);
    fl_diag_init(&loaded.dc, &loaded.arena);
    loaded.def = yew_syn_def_load(&loaded.arena, &loaded.dc, second_source);
    YEW_ASSERT_NOT_NULL(loaded.def);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U), "y");
    loaded_free(&loaded);
    YEW_ASSERT_EQ_I64(unlink(second_source), 0);
    free(second_cache);
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
    load_def(&loaded, &f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U), "x");
    assert_loaded_regex_executes(&loaded, 'x');
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_warm_load_preserves_complex_regex_and_metadata(void)
{
    CacheFixture f;
    LoadedDef loaded;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynEngine *engine;
    const SynLangDesc *lang;
    static const u8 line[] = "Abc=yes";

    fixture_init(&f);
    write_exact(f.source, (const u8 *)cache_source_complex,
                strlen(cache_source_complex));
    build_cold_cache(&f);
    yew_syn_compile_count_reset();
    load_def(&loaded, &f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT_EQ_STR(loaded.def->name, "cache-complex");
    YEW_ASSERT_EQ_U64(loaded.def->root, 0U);
    YEW_ASSERT_EQ_U64(loaded.def->nctxs, 2U);
    YEW_ASSERT_EQ_U64(loaded.def->nrules, 1U);
    YEW_ASSERT_EQ_STR(yew_syn_ctx_name(loaded.def, 0U), "main");
    YEW_ASSERT_EQ_STR(yew_syn_ctx_name(loaded.def, 1U), "tail");
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U),
                      "^([A-Z][a-z]+|[0-9][0-9])=(yes|no)$");
    lang = yew_syn_lang_desc(yew_syn_lang_named("cache-complex"));
    YEW_ASSERT_NOT_NULL(lang);
    YEW_ASSERT_EQ_U64(lang->nextensions, 1U);
    YEW_ASSERT_EQ_STR(lang->extensions[0], "cx");
    YEW_ASSERT_EQ_U64(lang->nfilenames, 1U);
    YEW_ASSERT_EQ_STR(lang->filenames[0], "Complexfile");
    YEW_ASSERT_EQ_I64(lang->priority, 9);

    engine = yew_syn_engine_new(loaded.def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, line, sizeof(line) - 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(out.n, 3U);
    YEW_ASSERT_EQ_U64(out.spans[0].start, 0U);
    YEW_ASSERT_EQ_U64(out.spans[0].len, 3U);
    YEW_ASSERT_EQ_U64(out.spans[0].attr, YEW_ATTR_VARIABLE);
    YEW_ASSERT_EQ_U64(out.spans[1].start, 3U);
    YEW_ASSERT_EQ_U64(out.spans[1].len, 1U);
    YEW_ASSERT_EQ_U64(out.spans[1].attr, YEW_ATTR_OPERATOR);
    YEW_ASSERT_EQ_U64(out.spans[2].start, 4U);
    YEW_ASSERT_EQ_U64(out.spans[2].len, 3U);
    YEW_ASSERT_EQ_U64(out.spans[2].attr, YEW_ATTR_BOOLEAN);
    yew_syn_engine_free(engine);
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_warm_load_preserves_aux_literals_and_mutable_aux(void)
{
    CacheFixture f;
    LoadedDef loaded;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynEngine *engine;
    const SynState *state;
    static const u8 opener[] = "r##\"";
    static const u8 closer[] = "body\"##!tail";

    fixture_init(&f);
    write_exact(f.source, (const u8 *)cache_source_aux,
                strlen(cache_source_aux));
    build_cold_cache(&f);
    yew_syn_compile_count_reset();
    load_def(&loaded, &f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT_EQ_STR(yew_syn_ctx_name(loaded.def, 1U), "raw");
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(loaded.def, 0U), "^r(#+)\"");
    YEW_ASSERT_EQ_U64(loaded.def->rules[1].aux_match, SYN_AUXM_LITERAL);
    YEW_ASSERT_EQ_STR(yew_intern_str(loaded.def->aux,
                                     loaded.def->rules[1].aux_pre), "\"");
    YEW_ASSERT_EQ_STR(yew_intern_str(loaded.def->aux,
                                     loaded.def->rules[1].aux_post), "!");

    engine = yew_syn_engine_new(loaded.def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, opener, sizeof(opener) - 1U,
                 &out);
    state = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 2U);
    YEW_ASSERT_EQ_U64(state->ctx[1], 1U);
    YEW_ASSERT_EQ_STR(yew_intern_str(loaded.def->aux, state->aux), "##");

    yew_syn_line(engine, out.exit_state, closer, sizeof(closer) - 1U, &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    YEW_ASSERT_EQ_U64(out.n, 3U);
    YEW_ASSERT_EQ_U64(out.spans[0].attr, YEW_ATTR_STRING);
    YEW_ASSERT_EQ_U64(out.spans[0].len, 4U);
    YEW_ASSERT_EQ_U64(out.spans[1].attr, YEW_ATTR_STRING_ESCAPE);
    YEW_ASSERT_EQ_U64(out.spans[1].len, 4U);
    YEW_ASSERT_EQ_U64(out.spans[2].attr, YEW_ATTR_TEXT);
    YEW_ASSERT_EQ_U64(out.spans[2].len, 4U);
    state = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 1U);
    yew_syn_engine_free(engine);
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
    load_def(&loaded, &f);
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
    load_def(&loaded, &f);
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

void test_syn_cache_crc_valid_structural_corruption_recompiles_safely(void)
{
    CacheFixture f;
    Bytebuf bytes;
    LoadedDef loaded;
    size_t blob_len;
    u32 crc;

    fixture_init(&f);
    build_cold_cache(&f);
    bytes = read_exact(f.cache);
    YEW_ASSERT(bytes.len > YEW_SYN_CACHE_HEADER_SIZE + 12U);
    blob_len = bytes.len - YEW_SYN_CACHE_HEADER_SIZE;
    put_u32_le(bytes.data + YEW_SYN_CACHE_HEADER_SIZE + 8U, UINT32_MAX);
    crc = yew_crc32(bytes.data + YEW_SYN_CACHE_HEADER_SIZE, blob_len);
    put_u32_le(bytes.data + 60U, crc);
    write_exact(f.cache, bytes.data, bytes.len);
    bytebuf_free(&bytes);

    yew_test_capture_log();
    yew_syn_compile_count_reset();
    load_def(&loaded, &f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_WARN, "syntax cache tables invalid; recompiling"));
    assert_loaded_regex_executes(&loaded, 'x');
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_cache_restores_first_byte_filters_from_regex_programs(void)
{
    CacheFixture f;
    Bytebuf bytes;
    LoadedDef loaded;
    u8 first[32] = {0};
    size_t i;
    u32 changed = 0U;
    size_t blob_len;

    fixture_init(&f);
    build_cold_cache(&f);
    bytes = read_exact(f.cache);
    first[(u8)'x' >> 3U] = (u8)(1U << ((u8)'x' & 7U));
    for (i = YEW_SYN_CACHE_HEADER_SIZE;
         i + sizeof(first) <= bytes.len; i++) {
        if (memcmp(bytes.data + i, first, sizeof(first)) == 0) {
            (void)memset(bytes.data + i, 0, sizeof(first));
            changed++;
            i += sizeof(first) - 1U;
        }
    }
    YEW_ASSERT(changed >= 2U);
    blob_len = bytes.len - YEW_SYN_CACHE_HEADER_SIZE;
    put_u32_le(bytes.data + 60U,
               yew_crc32(bytes.data + YEW_SYN_CACHE_HEADER_SIZE, blob_len));
    write_exact(f.cache, bytes.data, bytes.len);
    bytebuf_free(&bytes);

    yew_syn_compile_count_reset();
    load_def(&loaded, &f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT(yew_syn_def_firstbyte_check(loaded.def, NULL, NULL));
    assert_loaded_regex_executes(&loaded, 'x');
    loaded_free(&loaded);
    fixture_free(&f);
}

void test_syn_definition_load_rejects_nonregular_and_oversized_files(void)
{
    CacheFixture f;
    Arena arena;
    DiagCtx dc;
    char large[128];
    int fd;

    fixture_init(&f);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    YEW_ASSERT_NULL(yew_syn_def_load(&arena, &dc, f.root));
    (void)snprintf(large, sizeof(large), "%s/large.fl", f.root);
    fd = open(large, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(ftruncate(fd, (off_t)(64U * 1024U * 1024U + 1U)), 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_NULL(yew_syn_def_load(&arena, &dc, large));
    YEW_ASSERT_EQ_I64(unlink(large), 0);
    arena_free_all(&arena);
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
    load_def(&loaded, &f);
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
    load_def(&loaded, &f);
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
    load_def(&loaded, &f);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "syntax cache write failed"));
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(access(f.cache, F_OK) != 0);
    loaded_free(&loaded);
    fixture_free(&f);
}
