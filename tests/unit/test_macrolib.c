/* Sprint 38: deterministic macro-library discovery and reload. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/macrolib.h"

typedef struct MacroLibFix {
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    char root[160];
    char files[16][64];
    u32 nfiles;
    u32 errors;
    u32 warnings;
    char messages[2048];
} MacroLibFix;

static void mlf_diag(void *ctx, FlDiagLevel level, FlSpan sp,
                     const char *msg, const char *rendered)
{
    MacroLibFix *f = ctx;
    size_t used = strlen(f->messages);

    (void)sp;
    (void)rendered;
    if (level == FL_DIAG_ERROR)
        f->errors++;
    else if (level == FL_DIAG_WARNING)
        f->warnings++;
    if (used < sizeof(f->messages) - 1U)
        (void)snprintf(f->messages + used, sizeof(f->messages) - used,
                       "%s%s", used == 0U ? "" : "\n", msg);
}

static void mlf_open(MacroLibFix *f)
{
    OptVal value = {0};
    const char *err = NULL;

    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-macrolib-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->root));
    arena_init(&f->diag_arena);
    fl_diag_init(&f->dc, &f->diag_arena);
    fl_diag_set_sink(&f->dc, mlf_diag, f);
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    value.type = (u8)SAG_OPT_STR;
    value.as.str.s = f->root;
    value.as.str.len = (u32)strlen(f->root);
    SAG_ASSERT(sag_opt_set(&f->ed, SAG_OPT_GLOBAL, "macro.dir", 9U,
                           &value, &err));
    SAG_ASSERT_NULL(err);
}

static void mlf_write(MacroLibFix *f, const char *name, const char *source)
{
    char path[256];
    FILE *fp;
    size_t len = strlen(source);
    u32 i;

    (void)snprintf(path, sizeof(path), "%s/%s", f->root, name);
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_U64(fwrite(source, 1U, len, fp), len);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    for (i = 0U; i < f->nfiles; i++)
        if (strcmp(f->files[i], name) == 0)
            return;
    SAG_ASSERT(f->nfiles < SAG_ARRAY_LEN(f->files));
    (void)snprintf(f->files[f->nfiles], sizeof(f->files[f->nfiles]), "%s",
                   name);
    f->nfiles++;
}

static u32 mlf_scan(MacroLibFix *f)
{
    f->errors = 0U;
    f->warnings = 0U;
    f->messages[0] = '\0';
    fl_diag_init(&f->dc, &f->diag_arena);
    fl_diag_set_sink(&f->dc, mlf_diag, f);
    return sag_macrolib_scan(&f->ed, &f->dc);
}

static void mlf_close(MacroLibFix *f)
{
    u32 i;

    sag_ed_free(&f->ed);
    arena_free_all(&f->diag_arena);
    for (i = 0U; i < f->nfiles; i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", f->root, f->files[i]);
        SAG_ASSERT_EQ_I64(unlink(path), 0);
    }
    SAG_ASSERT_EQ_I64(rmdir(f->root), 0);
}

void test_macrolib_exports_dotted_names_and_matching_stem_alias(void)
{
    static const char edit[] = "fn dup_line() { return 1 }\n";
    static const char surround[] = "fn surround() { return 2 }\n";
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "surround.fl", surround);
    mlf_write(&f, "edit.fl", edit);
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 2U);
    SAG_ASSERT_EQ_U64(f.errors, 0U);
    SAG_ASSERT_EQ_U64(f.warnings, 0U);
    SAG_ASSERT_EQ_U64(sag_macrolib_count(&f.ed), 2U);
    SAG_ASSERT(sag_macrolib_find(&f.ed, "edit.dup_line", &view));
    SAG_ASSERT_EQ_STR(view.name, "edit.dup_line");
    SAG_ASSERT_NULL(view.alias);
    SAG_ASSERT_EQ_STR(view.binding, "dup_line");
    SAG_ASSERT_EQ_STR(view.stem, "edit");
    SAG_ASSERT_EQ_MEM(view.source, edit, sizeof(edit) - 1U);
    SAG_ASSERT_EQ_U64(view.source_len, sizeof(edit) - 1U);
    SAG_ASSERT_EQ_U64(view.arity, 0U);
    SAG_ASSERT(view.replayable);
    SAG_ASSERT(sag_macrolib_find(&f.ed, "surround", &view));
    SAG_ASSERT_EQ_STR(view.name, "surround.surround");
    SAG_ASSERT_EQ_STR(view.alias, "surround");
    SAG_ASSERT_EQ_STR(view.binding, "surround");
    SAG_ASSERT_EQ_STR(view.stem, "surround");
    SAG_ASSERT(sag_macrolib_find(&f.ed, "surround.surround", &view));
    mlf_close(&f);
}

void test_macrolib_excludes_private_and_nonzero_arity_from_macros(void)
{
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "mixed.fl",
              "fn _hidden() { return 1 }\n"
              "fn takes(value) { return value }\n"
              "fn zero() { return 0 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 2U);
    SAG_ASSERT_EQ_U64(f.errors, 0U);
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "mixed._hidden", &view));
    SAG_ASSERT(sag_macrolib_find(&f.ed, "mixed.takes", &view));
    SAG_ASSERT_EQ_U64(view.arity, 1U);
    SAG_ASSERT(!view.replayable);
    SAG_ASSERT_EQ_I64(sag_macrolib_call(&f.ed, "mixed.takes"),
                      SAG_CMD_ERR_ARG);
    SAG_ASSERT(sag_macrolib_find(&f.ed, "mixed.zero", &view));
    SAG_ASSERT_EQ_U64(view.arity, 0U);
    SAG_ASSERT(view.replayable);
    SAG_ASSERT_EQ_I64(sag_macrolib_call(&f.ed, "mixed.zero"), SAG_CMD_OK);
    mlf_close(&f);
}

void test_macrolib_broken_file_is_skipped_without_hiding_valid_files(void)
{
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "bad.fl", "fn broken(\n");
    mlf_write(&f, "good.fl", "fn good() { return 3 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    SAG_ASSERT(f.errors >= 1U);
    SAG_ASSERT(strstr(f.messages, "bad.fl") != NULL);
    SAG_ASSERT(strstr(f.messages, "skipped") != NULL);
    SAG_ASSERT_EQ_U64(sag_macrolib_count(&f.ed), 1U);
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "bad.broken", &view));
    SAG_ASSERT(sag_macrolib_find(&f.ed, "good.good", &view));
    SAG_ASSERT_EQ_STR(view.binding, "good");
    SAG_ASSERT(view.replayable);
    mlf_close(&f);
}

void test_macrolib_rejects_one_character_stem(void)
{
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "a.fl", "fn a() { return 1 }\n");
    mlf_write(&f, "valid.fl", "fn valid() { return 2 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    SAG_ASSERT_EQ_U64(f.errors, 1U);
    SAG_ASSERT(strstr(f.messages, "stem 'a'") != NULL);
    SAG_ASSERT(strstr(f.messages, "at least 2 bytes") != NULL);
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "a", &view));
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "a.a", &view));
    SAG_ASSERT(sag_macrolib_find(&f.ed, "valid", &view));
    mlf_close(&f);
}

void test_macrolib_unknown_schema_skips_only_that_file(void)
{
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "legacy.fl",
              "# sagitta-macro: 7\nfn legacy() { return 7 }\n");
    mlf_write(&f, "modern.fl",
              "# sagitta-macro: 1\nfn modern() { return 1 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    SAG_ASSERT_EQ_U64(f.errors, 0U);
    SAG_ASSERT_EQ_U64(f.warnings, 1U);
    SAG_ASSERT(strstr(f.messages, "unsupported sagitta-macro schema 7") !=
               NULL);
    SAG_ASSERT(strstr(f.messages, "skipped") != NULL);
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "legacy", &view));
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "legacy.legacy", &view));
    SAG_ASSERT(sag_macrolib_find(&f.ed, "modern", &view));
    SAG_ASSERT_EQ_STR(view.name, "modern.modern");
    SAG_ASSERT_EQ_U64(view.header.schema, 1U);
    mlf_close(&f);
}

void test_macrolib_scan_order_is_bytewise_and_stable(void)
{
    static const char *const want[] = {
        "Alpha.first", "Zed.second", "alpha.third"
    };
    MacroLibFix f;
    SagMacroEntryView view;
    u32 i;

    mlf_open(&f);
    mlf_write(&f, "alpha.fl", "fn third() { return 3 }\n");
    mlf_write(&f, "Zed.fl", "fn second() { return 2 }\n");
    mlf_write(&f, "Alpha.fl", "fn first() { return 1 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), SAG_ARRAY_LEN(want));
    for (i = 0U; i < SAG_ARRAY_LEN(want); i++) {
        SAG_ASSERT(sag_macrolib_at(&f.ed, i, &view));
        SAG_ASSERT_EQ_STR(view.name, want[i]);
    }
    SAG_ASSERT(!sag_macrolib_at(&f.ed, (u32)SAG_ARRAY_LEN(want), &view));
    SAG_ASSERT_EQ_U64(mlf_scan(&f), SAG_ARRAY_LEN(want));
    for (i = 0U; i < SAG_ARRAY_LEN(want); i++) {
        SAG_ASSERT(sag_macrolib_at(&f.ed, i, &view));
        SAG_ASSERT_EQ_STR(view.name, want[i]);
    }
    mlf_close(&f);
}

void test_macrolib_reload_replaces_entries_and_survives_gc(void)
{
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "reload.fl", "fn before() { return 1 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    SAG_ASSERT(sag_macrolib_find(&f.ed, "reload.before", &view));
    SAG_ASSERT_EQ_I64(sag_macrolib_call(&f.ed, "reload.before"), SAG_CMD_OK);
    mlf_write(&f, "reload.fl", "fn after() { return 2 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    SAG_ASSERT(!sag_macrolib_find(&f.ed, "reload.before", &view));
    SAG_ASSERT(sag_macrolib_find(&f.ed, "reload.after", &view));
    SAG_ASSERT_EQ_STR(view.binding, "after");
    fl_gc_collect(sag_fl_vm(&f.ed));
    SAG_ASSERT(sag_macrolib_find(&f.ed, "reload.after", &view));
    SAG_ASSERT(view.replayable);
    SAG_ASSERT_EQ_I64(sag_macrolib_call(&f.ed, "reload.after"), SAG_CMD_OK);
    mlf_close(&f);
}

void test_macro_header_major_mismatch_warns_but_still_loads(void)
{
    MacroLibFix f;
    SagMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "compat.fl",
              "# sagitta-macro: 1\n"
              "# recorded-with: sagitta 9.4.0\n"
              "fn compat() { return 9 }\n");
    SAG_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    SAG_ASSERT_EQ_U64(f.errors, 0U);
    SAG_ASSERT_EQ_U64(f.warnings, 1U);
    SAG_ASSERT(strstr(f.messages, "recorded-with") != NULL);
    SAG_ASSERT(strstr(f.messages, "9.4.0") != NULL);
    SAG_ASSERT(sag_macrolib_find(&f.ed, "compat", &view));
    SAG_ASSERT(view.header.recorded_with.present);
    SAG_ASSERT_EQ_MEM(view.header.recorded_with.s, "sagitta 9.4.0", 13U);
    SAG_ASSERT_EQ_I64(sag_macrolib_call(&f.ed, "compat"), SAG_CMD_OK);
    mlf_close(&f);
}
