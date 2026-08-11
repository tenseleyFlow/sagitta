#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syn/langs_gen.h"
#include "syn/registry.h"

typedef struct SeedFix {
    SynLangSeed *seed;
    char (*names)[32];
    char (*sources)[64];
    char (*first_lines)[32];
    size_t len;
} SeedFix;

static void seed_fix_open(SeedFix *fix, size_t len, bool first_lines)
{
    size_t i;

    (void)memset(fix, 0, sizeof(*fix));
    fix->seed = yew_xcalloc(len, sizeof(*fix->seed));
    fix->names = yew_xcalloc(len, sizeof(*fix->names));
    fix->sources = yew_xcalloc(len, sizeof(*fix->sources));
    if (first_lines)
        fix->first_lines = yew_xcalloc(len, sizeof(*fix->first_lines));
    fix->len = len;
    for (i = 0U; i < len; i++) {
        (void)snprintf(fix->names[i], sizeof(fix->names[i]),
                       "synthetic-%zu", i + 1U);
        (void)snprintf(fix->sources[i], sizeof(fix->sources[i]),
                       "runtime/syntax/synthetic-%zu.fl", i + 1U);
        if (first_lines) {
            (void)snprintf(fix->first_lines[i], sizeof(fix->first_lines[i]),
                           "^needle-%zu$", i + 1U);
        }
        fix->seed[i].id = (u32)(i + 1U);
        fix->seed[i].name = fix->names[i];
        fix->seed[i].source = fix->sources[i];
        fix->seed[i].first_line = first_lines ? fix->first_lines[i] : NULL;
    }
}

static void seed_fix_close(SeedFix *fix)
{
    free(fix->first_lines);
    free(fix->sources);
    free(fix->names);
    free(fix->seed);
    (void)memset(fix, 0, sizeof(*fix));
}

void test_syn_registry_exact_sizes_and_stable_descriptors(void)
{
    static const size_t sizes[] = {0U, 1U, 19U, 32U, 33U, 48U, 257U};
    size_t s;

    for (s = 0U; s < YEW_ARRAY_LEN(sizes); s++) {
        BuiltinRegistry registry;
        SeedFix fix;
        size_t i;

        seed_fix_open(&fix, sizes[s], false);
        yew_syn_builtin_registry_build(&registry, fix.seed, fix.len);
        YEW_ASSERT(registry.ready);
        YEW_ASSERT_EQ_U64(registry.len, fix.len);
        YEW_ASSERT_EQ_U64(registry.desc != NULL, fix.len != 0U);
        YEW_ASSERT_EQ_U64(registry.loaded != NULL, fix.len != 0U);
        YEW_ASSERT_EQ_U64(registry.first_line_re != NULL, fix.len != 0U);
        for (i = 0U; i < fix.len; i++) {
            const SynLangDesc *first =
                yew_syn_builtin_registry_desc_at(&registry, i);
            const SynLangDesc *second =
                yew_syn_builtin_registry_desc_at(&registry, i);

            YEW_ASSERT_NOT_NULL(first);
            YEW_ASSERT(first == second);
            YEW_ASSERT_EQ_U64(first->id, i + 1U);
            YEW_ASSERT_EQ_STR(first->name, fix.names[i]);
            YEW_ASSERT_EQ_STR(first->source, fix.sources[i]);
        }
        YEW_ASSERT_NULL(yew_syn_builtin_registry_desc_at(&registry, fix.len));
        yew_syn_builtin_registry_free(&registry);
        YEW_ASSERT(!registry.ready);
        YEW_ASSERT_EQ_U64(registry.len, 0U);
        seed_fix_close(&fix);
    }
}

void test_syn_registry_more_than_32_first_lines_reach_the_last_slot(void)
{
    BuiltinRegistry registry;
    SeedFix fix;
    YewRe *first;
    YewRe *second;
    YewReInput input;
    YewReMatch match;
    size_t i;

    seed_fix_open(&fix, 33U, true);
    yew_syn_builtin_registry_build(&registry, fix.seed, fix.len);
    for (i = 0U; i < fix.len; i++)
        YEW_ASSERT_NOT_NULL(yew_syn_builtin_registry_first_line(&registry, i));
    first = yew_syn_builtin_registry_first_line(&registry, fix.len - 1U);
    second = yew_syn_builtin_registry_first_line(&registry, fix.len - 1U);
    YEW_ASSERT(first == second);
    input = yew_re_input_bytes((const u8 *)"needle-33", 9U);
    YEW_ASSERT(yew_re_match_at(first, &input, BYTEOFF(0U), &match));
    yew_syn_builtin_registry_free(&registry);
    seed_fix_close(&fix);
}

typedef struct LoadCount {
    u32 calls;
} LoadCount;

static SynDef *registry_test_load(Arena *arena, DiagCtx *dc,
                                  const SynLangSeed *seed, void *ctx)
{
    LoadCount *count = ctx;
    char text[512];
    char *source;
    int n;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 file;

    count->calls++;
    n = snprintf(text, sizeof(text),
                 "{syntax:1,language:{name:\"%s\"},"
                 "contexts:{main:{rules:[]}}}", seed->name);
    YEW_ASSERT(n > 0);
    YEW_ASSERT((size_t)n < sizeof(text));
    source = arena_strdup(arena, text);
    file = fl_diag_add_file(dc, seed->source, source, (size_t)n);
    return yew_syn_def_compile(arena, dc, (const u8 *)source, (size_t)n,
                               file, &errors, &warnings);
}

void test_syn_registry_loads_requested_definition_once(void)
{
    BuiltinRegistry registry;
    SynLangSeed seed = {7001U, "registry-lazy", "registry-lazy.fl",
                        NULL, 0U, NULL, 0U, NULL, 0U, NULL, 0,
                        {NULL, NULL, NULL}};
    LoadCount count = {0U};
    SynDef *first;
    SynDef *second;

    yew_syn_builtin_registry_build(&registry, &seed, 1U);
    first = yew_syn_builtin_registry_load(&registry, &seed, 1U, seed.id,
                                           registry_test_load, &count);
    second = yew_syn_builtin_registry_load(&registry, &seed, 1U, seed.id,
                                            registry_test_load, &count);
    YEW_ASSERT_NOT_NULL(first);
    YEW_ASSERT(first == second);
    YEW_ASSERT_EQ_U64(count.calls, 1U);
    YEW_ASSERT_NULL(yew_syn_builtin_registry_load(
        &registry, &seed, 1U, seed.id + 1U, registry_test_load, &count));
    YEW_ASSERT_EQ_U64(count.calls, 1U);
    yew_syn_builtin_registry_free(&registry);
}

void test_syn_registry_allocation_overflow_is_a_bug(void)
{
    SynLangSeed seed = {0};
    pid_t child;
    pid_t waited;
    int status;

    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        BuiltinRegistry registry;
        size_t len = SIZE_MAX / sizeof(SynLangDesc) + 1U;

        if (freopen("/dev/null", "w", stderr) == NULL)
            _exit(126);
        yew_syn_builtin_registry_build(&registry, &seed, len);
        _exit(0);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
}

void test_syn_registry_builtin_id_ledger_matches_generated_table(void)
{
    FILE *fp = fopen("tests/syn/builtin-ids.txt", "rb");
    char line[256];
    size_t row = 0U;

    YEW_ASSERT_NOT_NULL(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned int id;
        char name[64];
        char stem[64];
        char source[96];

        if (line[0] == '#')
            continue;
        YEW_ASSERT_EQ_I64(sscanf(line, "%u|%63[^|]|%63[^\n]",
                                 &id, name, stem), 3);
        YEW_ASSERT(row < yew_syn_builtin_langs_len);
        YEW_ASSERT_EQ_U64(yew_syn_builtin_langs[row].id, id);
        YEW_ASSERT_EQ_STR(yew_syn_builtin_langs[row].name, name);
        (void)snprintf(source, sizeof(source), "runtime/syntax/%s.fl", stem);
        YEW_ASSERT_EQ_STR(yew_syn_builtin_langs[row].source, source);
        YEW_ASSERT_EQ_U64(yew_syn_lang_named(name), id);
        YEW_ASSERT_EQ_U64(yew_syn_lang_by_name((const u8 *)name,
                                               (u32)strlen(name)), id);
        YEW_ASSERT_NOT_NULL(yew_syn_lang_desc(id));
        YEW_ASSERT_EQ_STR(yew_syn_lang_desc(id)->name, name);
        row++;
    }
    YEW_ASSERT(!ferror(fp));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_U64(row, yew_syn_builtin_langs_len);
    YEW_ASSERT_EQ_U64(yew_syn_builtin_langs_len, 48U);
}
