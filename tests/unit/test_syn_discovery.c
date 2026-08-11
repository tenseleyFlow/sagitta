#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "syn/defs.h"
#include "syn/engine.h"
#include "text/piece.h"

typedef struct DiscoveryFix {
    char root[160];
    char yew[192];
    char syntax[224];
    char files[8][256];
    u32 nfiles;
    char *old_config;
    char *old_no_cache;
    u32 builtin_count;
} DiscoveryFix;

static char *discovery_env_copy(const char *name)
{
    const char *value = getenv(name);
    char *copy;

    if (value == NULL)
        return NULL;
    copy = yew_xmalloc(strlen(value) + 1U);
    (void)memcpy(copy, value, strlen(value) + 1U);
    return copy;
}

static void discovery_env_restore(const char *name, char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else {
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
        free(value);
    }
}

static void discovery_open(DiscoveryFix *f, bool make_syntax)
{
    YEW_ASSERT_NOT_NULL(f);
    if (f == NULL)
        return;
    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-syn-discovery-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)snprintf(f->yew, sizeof(f->yew), "%s/yew", f->root);
    (void)snprintf(f->syntax, sizeof(f->syntax), "%s/syntax", f->yew);
    f->old_config = discovery_env_copy("XDG_CONFIG_HOME");
    f->old_no_cache = discovery_env_copy("YEW_NO_SYN_CACHE");
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", f->root, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_NO_SYN_CACHE", "1", 1), 0);
    yew_syn_discovery_reset();
    f->builtin_count = yew_syn_lang_count();
    if (make_syntax) {
        YEW_ASSERT_EQ_I64(mkdir(f->yew, 0700), 0);
        YEW_ASSERT_EQ_I64(mkdir(f->syntax, 0700), 0);
        yew_syn_discovery_reset();
    }
}

static void discovery_write(DiscoveryFix *f, const char *name,
                            const char *source)
{
    FILE *fp;
    size_t len = strlen(source);

    YEW_ASSERT_NOT_NULL(f);
    if (f == NULL)
        return;
    YEW_ASSERT(f->nfiles < YEW_ARRAY_LEN(f->files));
    (void)snprintf(f->files[f->nfiles], sizeof(f->files[f->nfiles]),
                   "%s/%s", f->syntax, name);
    fp = fopen(f->files[f->nfiles], "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(source, 1U, len, fp), len);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    f->nfiles++;
}

static void discovery_close(DiscoveryFix *f, bool made_syntax)
{
    u32 i;

    yew_syn_discovery_reset();
    for (i = 0U; i < f->nfiles; i++)
        YEW_ASSERT_EQ_I64(unlink(f->files[i]), 0);
    if (made_syntax) {
        YEW_ASSERT_EQ_I64(rmdir(f->syntax), 0);
        YEW_ASSERT_EQ_I64(rmdir(f->yew), 0);
    }
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    /* Pin an empty scan before restoring the operator's environment so later
     * registry tests cannot import definitions from outside the fixture. */
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f->builtin_count);
    discovery_env_restore("XDG_CONFIG_HOME", f->old_config);
    discovery_env_restore("YEW_NO_SYN_CACHE", f->old_no_cache);
}

static const char alpha_def[] =
    "{ syntax: 1, language: { name: \"user-alpha\", "
    "extensions: [\"ualpha\"], filenames: [\"Alpha.project\"], "
    "shebangs: [\"ualpha\"], priority: 4 }, contexts: { main: { "
    "rules: [ { match: \"x\", attr: \"keyword\" } ] } } }";

static const char zeta_def[] =
    "{ syntax: 1, language: { name: \"user-zeta\", "
    "extensions: [\"uzeta\"], filenames: [], shebangs: [], priority: 2 }, "
    "contexts: { main: { rules: [] } } }";

void test_syn_discovery_absent_config_directory_is_silent(void)
{
    DiscoveryFix f;

    discovery_open(&f, false);
    yew_test_capture_log();
    yew_syn_discovery_reset();
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f.builtin_count);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 0U);
    discovery_close(&f, false);
}

void test_syn_discovery_is_sorted_and_integrates_every_registry_surface(void)
{
    DiscoveryFix f;
    u32 alpha;
    u32 zeta;
    const SynLangDesc *desc;
    u64 compiled;

    discovery_open(&f, true);
    /* Creation order deliberately opposes the required bytewise scan order. */
    discovery_write(&f, "z-last.fl", zeta_def);
    discovery_write(&f, "README", "ignored");
    discovery_write(&f, "a-first.fl", alpha_def);
    yew_syn_compile_count_reset();
    yew_syn_discovery_reset();
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f.builtin_count + 2U);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 2U);
    alpha = yew_syn_lang_named("user-alpha");
    zeta = yew_syn_lang_named("user-zeta");
    YEW_ASSERT(alpha != YEW_LANG_NONE);
    YEW_ASSERT(zeta != YEW_LANG_NONE);
    YEW_ASSERT(alpha < zeta);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("example.ualpha", NULL, 0U), alpha);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("Alpha.project", NULL, 0U), alpha);
    desc = yew_syn_lang_desc(alpha);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_EQ_STR(desc->name, "user-alpha");
    YEW_ASSERT(strstr(desc->source, "/a-first.fl") != NULL);
    compiled = yew_syn_compile_count();
    YEW_ASSERT_NOT_NULL(yew_syn_def_for(alpha));
    YEW_ASSERT_NOT_NULL(yew_syn_engine_for(alpha));
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), compiled);
    /* Looking up built-in metadata must not compile a built-in definition. */
    YEW_ASSERT(yew_syn_lang_named("ini") != YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), compiled);
    discovery_close(&f, true);
}

void test_syn_discovery_invalid_file_warns_once_and_reset_drops_user_state(void)
{
    DiscoveryFix f;
    u32 ini;

    discovery_open(&f, true);
    discovery_write(&f, "broken.fl", "{ syntax: 1, language: [ }");
    yew_test_capture_log();
    yew_syn_discovery_reset();
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f.builtin_count);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "ignoring invalid syntax definition"));
    ini = yew_syn_lang_named("ini");
    YEW_ASSERT(ini != YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("settings.ini", NULL, 0U), ini);
    YEW_ASSERT_EQ_I64(unlink(f.files[0]), 0);
    f.nfiles = 0U;
    discovery_write(&f, "user.fl", alpha_def);
    yew_syn_discovery_reset();
    YEW_ASSERT(yew_syn_lang_named("user-alpha") != YEW_LANG_NONE);
    YEW_ASSERT_EQ_I64(unlink(f.files[0]), 0);
    f.nfiles = 0U;
    yew_syn_discovery_reset();
    YEW_ASSERT_EQ_U64(yew_syn_lang_named("user-alpha"), YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f.builtin_count);
    discovery_close(&f, true);
}

void test_syn_discovery_duplicate_name_keeps_first_sorted_definition(void)
{
    static const char duplicate[] =
        "{ syntax: 1, language: { name: \"user-alpha\", "
        "extensions: [\"duplicate\"], filenames: [], shebangs: [] }, "
        "contexts: { main: { rules: [] } } }";
    DiscoveryFix f;
    u32 alpha;
    const SynLangDesc *desc;

    discovery_open(&f, true);
    discovery_write(&f, "01-first.fl", alpha_def);
    discovery_write(&f, "02-duplicate.fl", duplicate);
    yew_test_capture_log();
    yew_syn_discovery_reset();
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f.builtin_count + 1U);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "duplicate syntax language 'user-alpha'"));
    alpha = yew_syn_lang_named("user-alpha");
    desc = yew_syn_lang_desc(alpha);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT(strstr(desc->source, "/01-first.fl") != NULL);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("x.ualpha", NULL, 0U), alpha);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("x.duplicate", NULL, 0U),
                      YEW_LANG_NONE);
    discovery_close(&f, true);
}

void test_syn_discovery_rejects_builtin_language_name_collision(void)
{
    static const char builtin_collision[] =
        "{ syntax: 1, language: { name: \"ini\", extensions: [\"hijack\"] }, "
        "contexts: { main: { rules: [] } } }";
    DiscoveryFix f;
    u32 ini;
    const SynLangDesc *desc;

    discovery_open(&f, true);
    discovery_write(&f, "hijack.fl", builtin_collision);
    yew_test_capture_log();
    yew_syn_discovery_reset();
    YEW_ASSERT_EQ_U64(yew_syn_lang_count(), f.builtin_count);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "duplicate syntax language 'ini'"));
    ini = yew_syn_lang_named("ini");
    desc = yew_syn_lang_desc(ini);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_EQ_STR(desc->source, "runtime/syntax/ini.fl");
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("x.hijack", NULL, 0U),
                      YEW_LANG_NONE);
    discovery_close(&f, true);
}

void test_syn_discovery_engine_snapshots_user_name_for_pure_embed_lookup(void)
{
    static const char host_src[] =
        "{syntax:1,language:{name:\"snapshot-host\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"user-alpha\",end:\"inline\"}}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    DiscoveryFix f;
    Arena arena;
    DiagCtx dc;
    SynDef *host;
    SynEngine *engine;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *pending;
    u32 alpha;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 file;
    u64 compiled;

    discovery_open(&f, true);
    discovery_write(&f, "user.fl", alpha_def);
    yew_syn_discovery_reset();
    alpha = yew_syn_lang_named("user-alpha");
    YEW_ASSERT(alpha != YEW_LANG_NONE);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    file = fl_diag_add_file(&dc, "snapshot-host.fl", host_src,
                            strlen(host_src));
    host = yew_syn_def_compile(&arena, &dc, (const u8 *)host_src,
                               strlen(host_src), file, &errors, &warnings);
    YEW_ASSERT_NOT_NULL(host);
    YEW_ASSERT_EQ_U64(errors, 0U);
    YEW_ASSERT_EQ_U64(warnings, 0U);
    engine = yew_syn_engine_new(host);
    compiled = yew_syn_compile_count();
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"OPENx", 5U,
                 &out);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), compiled);
    pending = yew_syn_state_get(yew_syn_engine_states(engine),
                                out.exit_state);
    YEW_ASSERT_NOT_NULL(pending);
    YEW_ASSERT((pending->flags & YEW_SYN_F_EMBED_PEND) != 0U);
    YEW_ASSERT_EQ_U64(pending->aux[pending->ndef], alpha);
    yew_syn_engine_free(engine);
    yew_syn_def_dispose(host);
    arena_free_all(&arena);
    discovery_close(&f, true);
}

void test_syn_discovery_reset_keeps_pumped_guest_runtime_alive(void)
{
    static const char host_src[] =
        "{syntax:1,language:{name:\"reset-host\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"user-alpha\",end:\"inline\"}}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    DiscoveryFix f;
    Arena arena;
    DiagCtx dc;
    SynDef *host;
    SynEngine *engine;
    SynBuf syn;
    TextBuf *tb;
    SynSettleReport report;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 file;
    u32 alpha;

    discovery_open(&f, true);
    discovery_write(&f, "user.fl", alpha_def);
    yew_syn_discovery_reset();
    alpha = yew_syn_lang_named("user-alpha");
    YEW_ASSERT(alpha != YEW_LANG_NONE);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    file = fl_diag_add_file(&dc, "reset-host.fl", host_src,
                            strlen(host_src));
    host = yew_syn_def_compile(&arena, &dc, (const u8 *)host_src,
                               strlen(host_src), file, &errors, &warnings);
    YEW_ASSERT_NOT_NULL(host);
    YEW_ASSERT_EQ_U64(errors, 0U);
    YEW_ASSERT_EQ_U64(warnings, 0U);
    engine = yew_syn_engine_new(host);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, engine);
    tb = yew_textbuf_from_bytes((const u8 *)"OPENxEND\n", 9U);
    YEW_ASSERT_NOT_NULL(tb);
    yew_syn_attach(&syn, 1U, tb);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(1U), INT64_MAX, &report);
    YEW_ASSERT_EQ_U64(syn.embed_pending, alpha);
    YEW_ASSERT(yew_syn_embed_pump(&syn, engine,
                                  YEW_SYN_EMBED_LOAD_BUDGET_US));
    YEW_ASSERT_NOT_NULL(yew_syn_def_resident(engine, alpha));

    yew_syn_discovery_reset();
    yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(1U), INT64_MAX, &report);
    YEW_ASSERT(report.fixpoint);
    YEW_ASSERT_NOT_NULL(yew_syn_def_resident(engine, alpha));
    YEW_ASSERT_EQ_STR(yew_syn_def_resident(engine, alpha)->name,
                      "user-alpha");

    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    yew_syn_engine_free(engine);
    yew_syn_def_dispose(host);
    arena_free_all(&arena);
    discovery_close(&f, true);
}
