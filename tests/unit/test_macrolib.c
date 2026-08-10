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
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-macrolib-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    arena_init(&f->diag_arena);
    fl_diag_init(&f->dc, &f->diag_arena);
    fl_diag_set_sink(&f->dc, mlf_diag, f);
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    value.type = (u8)YEW_OPT_STR;
    value.as.str.s = f->root;
    value.as.str.len = (u32)strlen(f->root);
    YEW_ASSERT(yew_opt_set(&f->ed, YEW_OPT_GLOBAL, "macro.dir", 9U,
                           &value, &err));
    YEW_ASSERT_NULL(err);
}

static void mlf_write(MacroLibFix *f, const char *name, const char *source)
{
    char path[256];
    FILE *fp;
    size_t len = strlen(source);
    u32 i;

    (void)snprintf(path, sizeof(path), "%s/%s", f->root, name);
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(source, 1U, len, fp), len);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    for (i = 0U; i < f->nfiles; i++)
        if (strcmp(f->files[i], name) == 0)
            return;
    YEW_ASSERT(f->nfiles < YEW_ARRAY_LEN(f->files));
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
    return yew_macrolib_scan(&f->ed, &f->dc);
}

static void mlf_close(MacroLibFix *f)
{
    u32 i;

    yew_ed_free(&f->ed);
    arena_free_all(&f->diag_arena);
    for (i = 0U; i < f->nfiles; i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", f->root, f->files[i]);
        YEW_ASSERT_EQ_I64(unlink(path), 0);
    }
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
}

void test_macrolib_exports_dotted_names_and_matching_stem_alias(void)
{
    static const char edit[] = "fn dup_line() { return 1 }\n";
    static const char surround[] = "fn surround() { return 2 }\n";
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "surround.fl", surround);
    mlf_write(&f, "edit.fl", edit);
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 2U);
    YEW_ASSERT_EQ_U64(f.errors, 0U);
    YEW_ASSERT_EQ_U64(f.warnings, 0U);
    YEW_ASSERT_EQ_U64(yew_macrolib_count(&f.ed), 2U);
    YEW_ASSERT(yew_macrolib_find(&f.ed, "edit.dup_line", &view));
    YEW_ASSERT_EQ_STR(view.name, "edit.dup_line");
    YEW_ASSERT_NULL(view.alias);
    YEW_ASSERT_EQ_STR(view.binding, "dup_line");
    YEW_ASSERT_EQ_STR(view.stem, "edit");
    YEW_ASSERT_EQ_MEM(view.source, edit, sizeof(edit) - 1U);
    YEW_ASSERT_EQ_U64(view.source_len, sizeof(edit) - 1U);
    YEW_ASSERT_EQ_U64(view.arity, 0U);
    YEW_ASSERT(view.replayable);
    YEW_ASSERT(yew_macrolib_find(&f.ed, "surround", &view));
    YEW_ASSERT_EQ_STR(view.name, "surround.surround");
    YEW_ASSERT_EQ_STR(view.alias, "surround");
    YEW_ASSERT_EQ_STR(view.binding, "surround");
    YEW_ASSERT_EQ_STR(view.stem, "surround");
    YEW_ASSERT(yew_macrolib_find(&f.ed, "surround.surround", &view));
    mlf_close(&f);
}

void test_macrolib_excludes_private_and_nonzero_arity_from_macros(void)
{
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "mixed.fl",
              "fn _hidden() { return 1 }\n"
              "fn takes(value) { return value }\n"
              "fn zero() { return 0 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 2U);
    YEW_ASSERT_EQ_U64(f.errors, 0U);
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "mixed._hidden", &view));
    YEW_ASSERT(yew_macrolib_find(&f.ed, "mixed.takes", &view));
    YEW_ASSERT_EQ_U64(view.arity, 1U);
    YEW_ASSERT(!view.replayable);
    YEW_ASSERT_EQ_I64(yew_macrolib_call(&f.ed, "mixed.takes"),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT(yew_macrolib_find(&f.ed, "mixed.zero", &view));
    YEW_ASSERT_EQ_U64(view.arity, 0U);
    YEW_ASSERT(view.replayable);
    YEW_ASSERT_EQ_I64(yew_macrolib_call(&f.ed, "mixed.zero"), YEW_CMD_OK);
    mlf_close(&f);
}

void test_macrolib_broken_file_is_skipped_without_hiding_valid_files(void)
{
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "bad.fl", "fn broken(\n");
    mlf_write(&f, "good.fl", "fn good() { return 3 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    YEW_ASSERT(f.errors >= 1U);
    YEW_ASSERT(strstr(f.messages, "bad.fl") != NULL);
    YEW_ASSERT(strstr(f.messages, "skipped") != NULL);
    YEW_ASSERT_EQ_U64(yew_macrolib_count(&f.ed), 1U);
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "bad.broken", &view));
    YEW_ASSERT(yew_macrolib_find(&f.ed, "good.good", &view));
    YEW_ASSERT_EQ_STR(view.binding, "good");
    YEW_ASSERT(view.replayable);
    mlf_close(&f);
}

void test_macrolib_rejects_one_character_stem(void)
{
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "a.fl", "fn a() { return 1 }\n");
    mlf_write(&f, "valid.fl", "fn valid() { return 2 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    YEW_ASSERT_EQ_U64(f.errors, 1U);
    YEW_ASSERT(strstr(f.messages, "stem 'a'") != NULL);
    YEW_ASSERT(strstr(f.messages, "at least 2 bytes") != NULL);
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "a", &view));
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "a.a", &view));
    YEW_ASSERT(yew_macrolib_find(&f.ed, "valid", &view));
    mlf_close(&f);
}

void test_macrolib_unknown_schema_skips_only_that_file(void)
{
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "legacy.fl",
              "# yew-macro: 7\nfn legacy() { return 7 }\n");
    mlf_write(&f, "modern.fl",
              "# yew-macro: 1\nfn modern() { return 1 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    YEW_ASSERT_EQ_U64(f.errors, 0U);
    YEW_ASSERT_EQ_U64(f.warnings, 1U);
    YEW_ASSERT(strstr(f.messages, "unsupported yew-macro schema 7") !=
               NULL);
    YEW_ASSERT(strstr(f.messages, "skipped") != NULL);
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "legacy", &view));
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "legacy.legacy", &view));
    YEW_ASSERT(yew_macrolib_find(&f.ed, "modern", &view));
    YEW_ASSERT_EQ_STR(view.name, "modern.modern");
    YEW_ASSERT_EQ_U64(view.header.schema, 1U);
    mlf_close(&f);
}

void test_macrolib_scan_order_is_bytewise_and_stable(void)
{
    static const char *const want[] = {
        "Alpha.first", "Zed.second", "alpha.third"
    };
    MacroLibFix f;
    YewMacroEntryView view;
    u32 i;

    mlf_open(&f);
    mlf_write(&f, "alpha.fl", "fn third() { return 3 }\n");
    mlf_write(&f, "Zed.fl", "fn second() { return 2 }\n");
    mlf_write(&f, "Alpha.fl", "fn first() { return 1 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), YEW_ARRAY_LEN(want));
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
        YEW_ASSERT(yew_macrolib_at(&f.ed, i, &view));
        YEW_ASSERT_EQ_STR(view.name, want[i]);
    }
    YEW_ASSERT(!yew_macrolib_at(&f.ed, (u32)YEW_ARRAY_LEN(want), &view));
    YEW_ASSERT_EQ_U64(mlf_scan(&f), YEW_ARRAY_LEN(want));
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
        YEW_ASSERT(yew_macrolib_at(&f.ed, i, &view));
        YEW_ASSERT_EQ_STR(view.name, want[i]);
    }
    mlf_close(&f);
}

void test_macrolib_reload_replaces_entries_and_survives_gc(void)
{
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "reload.fl", "fn before() { return 1 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    YEW_ASSERT(yew_macrolib_find(&f.ed, "reload.before", &view));
    YEW_ASSERT_EQ_I64(yew_macrolib_call(&f.ed, "reload.before"), YEW_CMD_OK);
    mlf_write(&f, "reload.fl", "fn after() { return 2 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    YEW_ASSERT(!yew_macrolib_find(&f.ed, "reload.before", &view));
    YEW_ASSERT(yew_macrolib_find(&f.ed, "reload.after", &view));
    YEW_ASSERT_EQ_STR(view.binding, "after");
    fl_gc_collect(yew_fl_vm(&f.ed));
    YEW_ASSERT(yew_macrolib_find(&f.ed, "reload.after", &view));
    YEW_ASSERT(view.replayable);
    YEW_ASSERT_EQ_I64(yew_macrolib_call(&f.ed, "reload.after"), YEW_CMD_OK);
    mlf_close(&f);
}

void test_macro_header_major_mismatch_warns_but_still_loads(void)
{
    MacroLibFix f;
    YewMacroEntryView view;

    mlf_open(&f);
    mlf_write(&f, "compat.fl",
              "# yew-macro: 1\n"
              "# recorded-with: yew 9.4.0\n"
              "fn compat() { return 9 }\n");
    YEW_ASSERT_EQ_U64(mlf_scan(&f), 1U);
    YEW_ASSERT_EQ_U64(f.errors, 0U);
    YEW_ASSERT_EQ_U64(f.warnings, 1U);
    YEW_ASSERT(strstr(f.messages, "recorded-with") != NULL);
    YEW_ASSERT(strstr(f.messages, "9.4.0") != NULL);
    YEW_ASSERT(yew_macrolib_find(&f.ed, "compat", &view));
    YEW_ASSERT(view.header.recorded_with.present);
    YEW_ASSERT_EQ_MEM(view.header.recorded_with.s, "yew 9.4.0", 9U);
    YEW_ASSERT_EQ_I64(yew_macrolib_call(&f.ed, "compat"), YEW_CMD_OK);
    mlf_close(&f);
}
