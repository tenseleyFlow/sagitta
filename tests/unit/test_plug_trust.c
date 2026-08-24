#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ws/trust.h"

typedef struct PlugTrustFix {
    char root[160];
    char dbpath[208];
} PlugTrustFix;

static void pt_make(PlugTrustFix *f)
{
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-plug-trust-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)snprintf(f->dbpath, sizeof(f->dbpath), "%s/trust.fl", f->root);
}

static void pt_remove(const PlugTrustFix *f)
{
    (void)unlink(f->dbpath);
    (void)rmdir(f->root);
}

static void pt_write(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    size_t n = strlen(text);

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, n, fp), n);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static char *pt_read(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    char *text;

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_END), 0);
    n = ftell(fp);
    YEW_ASSERT(n >= 0L);
    YEW_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_SET), 0);
    text = yew_xmalloc((size_t)n + 1U);
    YEW_ASSERT_EQ_U64(fread(text, 1U, (size_t)n, fp), (size_t)n);
    text[n] = '\0';
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    return text;
}

void test_plug_trust_defaults_and_all_exact_capabilities_round_trip(void)
{
    PlugTrustFix f;
    YewTrustDb db;
    char *out;
    char *first;
    char *second;

    pt_make(&f);
    pt_write(f.dbpath,
             "{schema: 1, legacy_marker: \"kept\", "
             "dirs: {\"/legacy\": \"denied\"}}\n");
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT_EQ_I64(yew_trust_plugin_desired(&db, "notes"),
                      YEW_PLUGIN_DESIRED_DEFAULT);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "notes", YEW_PLUGIN_CAP_FS),
                      YEW_PLUGIN_GRANT_UNSET);
    YEW_ASSERT(yew_trust_plugin_set_desired(
        &db, "absent", YEW_PLUGIN_DESIRED_DEFAULT));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "absent", YEW_PLUGIN_CAP_NET, YEW_PLUGIN_GRANT_UNSET));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 1000, 365U));
    out = pt_read(f.dbpath);
    /* A dirs-only schema-1 file upgrades in place without inventing policy. */
    YEW_ASSERT_NOT_NULL(strstr(out, "schema: 3"));
    YEW_ASSERT_NOT_NULL(strstr(out, "legacy_marker: \"kept\""));
    YEW_ASSERT_NOT_NULL(strstr(out, "\"/legacy\": \"denied\""));
    YEW_ASSERT(strstr(out, "plugins:") == NULL);
    free(out);

    YEW_ASSERT(yew_trust_plugin_set_desired(
        &db, "notes", YEW_PLUGIN_DESIRED_DISABLED));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "notes", YEW_PLUGIN_CAP_FS, YEW_PLUGIN_GRANT_ALLOW));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "notes", YEW_PLUGIN_CAP_SHELL, YEW_PLUGIN_GRANT_DENY));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "notes", YEW_PLUGIN_CAP_NET, YEW_PLUGIN_GRANT_ALLOW));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "notes", YEW_PLUGIN_CAP_CLIPBOARD, YEW_PLUGIN_GRANT_DENY));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 1001, 365U));
    first = pt_read(f.dbpath);
    yew_trust_db_free(&db);

    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT_EQ_I64(yew_trust_plugin_desired(&db, "notes"),
                      YEW_PLUGIN_DESIRED_DISABLED);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "notes", YEW_PLUGIN_CAP_FS),
                      YEW_PLUGIN_GRANT_ALLOW);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "notes", YEW_PLUGIN_CAP_SHELL),
                      YEW_PLUGIN_GRANT_DENY);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "notes", YEW_PLUGIN_CAP_NET),
                      YEW_PLUGIN_GRANT_ALLOW);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "notes", YEW_PLUGIN_CAP_CLIPBOARD),
                      YEW_PLUGIN_GRANT_DENY);
    /* Current plugin policy survives reload and rewrites byte-identically. */
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 1001, 365U));
    second = pt_read(f.dbpath);
    YEW_ASSERT_EQ_STR(first, second);
    free(second);
    free(first);
    YEW_ASSERT(yew_trust_plugin_set_desired(
        &db, "notes", YEW_PLUGIN_DESIRED_DEFAULT));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "notes", YEW_PLUGIN_CAP_FS, YEW_PLUGIN_GRANT_UNSET));
    YEW_ASSERT_EQ_I64(yew_trust_plugin_desired(&db, "notes"),
                      YEW_PLUGIN_DESIRED_DEFAULT);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "notes", YEW_PLUGIN_CAP_FS),
                      YEW_PLUGIN_GRANT_UNSET);
    yew_trust_db_free(&db);
    pt_remove(&f);
}

void test_plug_trust_schema2_unknowns_and_sorted_output_round_trip(void)
{
    static const char input[] =
        "{schema: 2, root_future: {keep: 9}, dirs: {}, plugins: {\n"
        "  zeta: {shell: \"deny\", zz_future: [1, 2]},\n"
        "  alpha: {shell: \"allow\", net: \"deny\", fs: \"deny\", "
        "enabled: false, clipboard: \"allow\", zz_future: {x: true}},\n"
        "}}\n";
    PlugTrustFix f;
    YewTrustDb db;
    char *first;
    char *second;
    const char *alpha;
    const char *zeta;
    const char *clipboard;
    const char *enabled;
    const char *fs;
    const char *net;
    const char *shell;
    const char *unknown;

    pt_make(&f);
    pt_write(f.dbpath, input);
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT(yew_trust_plugin_set_capability(
        &db, "alpha", YEW_PLUGIN_CAP_FS, YEW_PLUGIN_GRANT_ALLOW));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    first = pt_read(f.dbpath);
    YEW_ASSERT_NOT_NULL(strstr(first, "schema: 3"));
    YEW_ASSERT_NOT_NULL(strstr(first, "root_future: {"));
    YEW_ASSERT_NOT_NULL(strstr(first, "keep: 9"));
    alpha = strstr(first, "alpha: {");
    zeta = strstr(first, "zeta: {");
    YEW_ASSERT_NOT_NULL(alpha);
    YEW_ASSERT_NOT_NULL(zeta);
    YEW_ASSERT(alpha < zeta);
    clipboard = strstr(alpha, "clipboard: \"allow\"");
    enabled = strstr(alpha, "enabled: false");
    fs = strstr(alpha, "fs: \"allow\"");
    net = strstr(alpha, "net: \"deny\"");
    shell = strstr(alpha, "shell: \"allow\"");
    unknown = strstr(alpha, "zz_future: {");
    YEW_ASSERT_NOT_NULL(clipboard);
    YEW_ASSERT_NOT_NULL(enabled);
    YEW_ASSERT_NOT_NULL(fs);
    YEW_ASSERT_NOT_NULL(net);
    YEW_ASSERT_NOT_NULL(shell);
    YEW_ASSERT_NOT_NULL(unknown);
    YEW_ASSERT(clipboard < enabled && enabled < fs && fs < net &&
               net < shell && shell < unknown && unknown < zeta);
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    second = pt_read(f.dbpath);
    YEW_ASSERT_EQ_STR(first, second);
    free(second);
    free(first);
    yew_trust_db_free(&db);
    pt_remove(&f);
}
