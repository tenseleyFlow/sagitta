#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "syn/defs.h"

typedef struct DetectFix {
    Arena arena;
    DiagCtx dc;
    SynDef *defs[24];
    u32 ndefs;
} DetectFix;

static void detect_quiet(void *ctx, FlDiagLevel level, FlSpan sp,
                         const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static void detect_open(DetectFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, detect_quiet, NULL);
}

static void detect_close(DetectFix *f)
{
    while (f->ndefs != 0U)
        yew_syn_def_dispose(f->defs[--f->ndefs]);
    arena_free_all(&f->arena);
}

static u32 detect_id(const char *name)
{
    u32 id;
    u32 limit = yew_syn_lang_count() + 32U;

    for (id = 1U; id <= limit; id++) {
        const SynLangDesc *lang = yew_syn_lang_desc(id);

        if (lang != NULL && strcmp(lang->name, name) == 0)
            return id;
    }
    return YEW_LANG_NONE;
}

static u32 detect_add(DetectFix *f, const char *name, const char *extensions,
                      const char *filenames, const char *shebangs,
                      const char *first_line, i32 priority)
{
    char src[2048];
    int n;
    u32 nerr;
    u32 nwarn;
    u32 file_id;
    SynDef *def;

    n = snprintf(src, sizeof(src),
                 "{ syntax: 1, language: { name: \"%s\", "
                 "extensions: %s, filenames: %s, shebangs: %s, "
                 "%s%s%s priority: %d }, "
                 "contexts: { main: { rules: [] } } }",
                 name, extensions, filenames, shebangs,
                 first_line[0] == '\0' ? "" : "first_line: ", first_line,
                 first_line[0] == '\0' ? "" : ",",
                 (int)priority);
    YEW_ASSERT(n > 0);
    YEW_ASSERT((size_t)n < sizeof(src));
    file_id = fl_diag_add_file(&f->dc, name, src, (size_t)n);
    def = yew_syn_def_compile(&f->arena, &f->dc, (const u8 *)src, (size_t)n,
                              file_id, &nerr, &nwarn);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 0U);
    YEW_ASSERT_EQ_U64(nwarn, 0U);
    YEW_ASSERT(f->ndefs < YEW_ARRAY_LEN(f->defs));
    f->defs[f->ndefs++] = def;
    YEW_ASSERT(detect_id(name) != YEW_LANG_NONE);
    return detect_id(name);
}

void test_syn_detect_builtin_ini_exact_glob_extension_and_path_basename(void)
{
    u32 ini = detect_id("ini");

    YEW_ASSERT(ini != YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for(".editorconfig", NULL, 0U), ini);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("/tmp/project/.editorconfig", NULL, 0U),
                      ini);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("x.desktop", NULL, 0U), ini);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("/usr/share/x.service", NULL, 0U), ini);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("settings.INI", NULL, 0U), ini);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("settings.properties", NULL, 0U), ini);
}

void test_syn_detect_exact_filename_precedes_glob_and_extension(void)
{
    DetectFix f;
    u32 exact;
    u32 glob;
    u32 ext;

    detect_open(&f);
    exact = detect_add(&f, "order-exact", "[]", "[\"Targetfile\"]", "[]",
                       "", -50);
    glob = detect_add(&f, "order-glob", "[]", "[\"*file\"]", "[]",
                      "", 50);
    ext = detect_add(&f, "order-ext", "[\"ord\"]", "[]", "[]", "",
                     100);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("/tmp/Targetfile", NULL, 0U), exact);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("/tmp/Otherfile", NULL, 0U), glob);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("Targetfile.ord", NULL, 0U), ext);
    YEW_ASSERT(exact != glob);
    YEW_ASSERT(glob != ext);
    detect_close(&f);
}

void test_syn_detect_longest_compound_extension_precedes_priority(void)
{
    DetectFix f;
    u32 compound;
    u32 short_ext;

    detect_open(&f);
    compound = detect_add(&f, "compound", "[\"tar.gz\"]", "[]", "[]",
                          "", -100);
    short_ext = detect_add(&f, "short", "[\"gz\"]", "[]", "[]", "",
                           100);
    YEW_ASSERT(compound != short_ext);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("archive.tar.gz", NULL, 0U), compound);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("archive.GZ", NULL, 0U), short_ext);
    detect_close(&f);
}

void test_syn_detect_priority_breaks_same_stage_ties(void)
{
    DetectFix f;
    u32 high;
    u32 low;

    detect_open(&f);
    low = detect_add(&f, "tie-low", "[\"prio\"]", "[]", "[]", "", -2);
    high = detect_add(&f, "tie-high", "[\"prio\"]", "[]", "[]", "", 9);
    YEW_ASSERT(high != low);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("x.prio", NULL, 0U), high);
    detect_close(&f);
}

void test_syn_detect_lexicographic_name_breaks_equal_priority_ties(void)
{
    DetectFix f;
    u32 alpha;
    u32 zeta;

    detect_open(&f);
    zeta = detect_add(&f, "zeta-tie", "[\"lexi\"]", "[]", "[]", "", 4);
    alpha = detect_add(&f, "alpha-tie", "[\"lexi\"]", "[]", "[]", "", 4);
    YEW_ASSERT(alpha != zeta);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("x.lexi", NULL, 0U), alpha);
    detect_close(&f);
}

void test_syn_detect_env_and_direct_shebang_interpreters(void)
{
    static const u8 env_line[] = "#!/usr/bin/env python3";
    static const u8 env_flags[] = "#!/usr/bin/env -S python3 -u";
    static const u8 direct[] = "#!/bin/sh -e";
    DetectFix f;
    u32 py;
    u32 sh;

    detect_open(&f);
    py = detect_add(&f, "python-probe", "[]", "[]", "[\"python3\"]",
                    "", 0);
    sh = detect_add(&f, "shell-probe", "[]", "[]", "[\"sh\"]", "", 0);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("script", env_line,
                                       (u32)strlen((const char *)env_line)), py);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("script", env_flags,
                                       (u32)strlen((const char *)env_flags)), py);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("script", direct,
                                       (u32)strlen((const char *)direct)), sh);
    detect_close(&f);
}

void test_syn_detect_shebang_precedes_first_line_regex(void)
{
    static const u8 line[] = "#!/usr/bin/env stagecmd";
    DetectFix f;
    u32 shebang;
    u32 regex;

    detect_open(&f);
    shebang = detect_add(&f, "stage-shebang", "[]", "[]", "[\"stagecmd\"]",
                         "", -100);
    regex = detect_add(&f, "stage-regex", "[]", "[]", "[]", "\"^#!\"", 100);
    YEW_ASSERT(shebang != regex);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("script", line,
                                       (u32)strlen((const char *)line)),
                      shebang);
    detect_close(&f);
}

void test_syn_detect_first_line_regex_is_last_positive_stage(void)
{
    static const u8 line[] = "MODE: detector";
    DetectFix f;
    u32 regex;

    detect_open(&f);
    regex = detect_add(&f, "line-probe", "[]", "[]", "[]", "\"^MODE:\\\\s\"",
                       0);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("README", line,
                                       (u32)strlen((const char *)line)), regex);
    detect_close(&f);
}

void test_syn_detect_unknown_path_and_empty_input_return_none(void)
{
    static const u8 ordinary[] = "ordinary text";

    YEW_ASSERT_EQ_U64(yew_syn_lang_for("file.xyz", ordinary,
                                       (u32)strlen((const char *)ordinary)),
                      YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for(NULL, NULL, 0U), YEW_LANG_NONE);
    YEW_ASSERT_NULL(yew_syn_lang_desc(YEW_LANG_NONE));
}
