#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/buf.h"
#include "ws/trust.h"

typedef struct TrustFix {
    char root[128];
    char dbpath[192];
    char work[192];
    char config[224];
} TrustFix;

static void tf_make(TrustFix *f)
{
    FILE *fp;

    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-trust-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)snprintf(f->dbpath, sizeof(f->dbpath), "%s/trust.fl", f->root);
    (void)snprintf(f->work, sizeof(f->work), "%s/work", f->root);
    YEW_ASSERT_EQ_I64(mkdir(f->work, 0700), 0);
    (void)snprintf(f->config, sizeof(f->config), "%s/.yew.fl", f->work);
    fp = fopen(f->config, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite("set({tabwidth: 8})\n", 1U, 19U, fp), 19U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void tf_remove(const TrustFix *f)
{
    char cmd[320];
    int rc;

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->root);
    rc = system(cmd);
    (void)rc;
}

static void tf_write(const char *path, const char *bytes)
{
    FILE *fp = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(bytes, 1U, strlen(bytes), fp), strlen(bytes));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static char *tf_read(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    char *out;

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_END), 0);
    n = ftell(fp);
    YEW_ASSERT(n >= 0L);
    YEW_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_SET), 0);
    out = yew_xmalloc((size_t)n + 1U);
    YEW_ASSERT_EQ_U64(fread(out, 1U, (size_t)n, fp), (size_t)n);
    out[n] = '\0';
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    return out;
}

static void tf_trust(YewTrustDb *db, const char *work, time_t now,
                     YewTrustProbe *probe)
{
    YEW_ASSERT_EQ_I64(yew_trust_check(db, work, true, false, probe),
                      YEW_TRUST_PROMPT_NEW);
    YEW_ASSERT(yew_trust_answer(db, probe, YEW_TRUST_ALWAYS, now));
    YEW_ASSERT_EQ_I64(yew_trust_check(db, work, true, false, probe),
                      YEW_TRUST_GRANTED);
}

void test_trust_content_hash_invalidation_ignores_mtime(void)
{
    TrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;
    struct timespec times[2] = {{1234, 0}, {1234, 0}};

    tf_make(&f);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    tf_trust(&db, f.work, 1000, &probe);
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, f.config, times, 0), 0);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_GRANTED);
    tf_write(f.config, "set({tabwidth: 4})\n");
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_PROMPT_CHANGED);
    YEW_ASSERT_EQ_STR(yew_trust_decision_reason(YEW_TRUST_PROMPT_CHANGED),
                      "the config changed since you trusted it");
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_realpath_move_and_symlink_do_not_carry_grant(void)
{
    TrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;
    char link[192];
    char moved[192];

    tf_make(&f);
    (void)snprintf(link, sizeof(link), "%s/link", f.root);
    (void)snprintf(moved, sizeof(moved), "%s/moved", f.root);
    YEW_ASSERT_EQ_I64(symlink(f.work, link), 0);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    tf_trust(&db, link, 1000, &probe);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_GRANTED);
    YEW_ASSERT_EQ_I64(rename(f.work, moved), 0);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, moved, true, false, &probe),
                      YEW_TRUST_PROMPT_NEW);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_same_path_new_inode_reprompts(void)
{
    TrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;
    char old[192];

    tf_make(&f);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    tf_trust(&db, f.work, 1000, &probe);
    (void)snprintf(old, sizeof(old), "%s/old", f.root);
    YEW_ASSERT_EQ_I64(rename(f.work, old), 0);
    YEW_ASSERT_EQ_I64(mkdir(f.work, 0700), 0);
    tf_write(f.config, "set({tabwidth: 8})\n");
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_PROMPT_REPLACED);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_deleted_config_retains_entry(void)
{
    TrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;

    tf_make(&f);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    tf_trust(&db, f.work, 1000, &probe);
    YEW_ASSERT_EQ_I64(unlink(f.config), 0);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_NO_CONFIG);
    tf_write(f.config, "set({tabwidth: 8})\n");
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_GRANTED);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_write_prunes_only_old_missing_dirs(void)
{
    TrustFix f;
    YewTrustDb db;
    char doc[2048];
    char *out;

    tf_make(&f);
    (void)snprintf(doc, sizeof(doc),
        "{schema: 1, dirs: {\n"
        "  \"/definitely/missing/old\": {state: \"denied\", at: 1},\n"
        "  \"/definitely/missing/new\": {state: \"denied\", at: 199999},\n"
        "  \"%s\": {state: \"denied\", at: 1},\n"
        "}}\n", f.work);
    tf_write(f.dbpath, doc);
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 200000, 1U));
    out = tf_read(f.dbpath);
    YEW_ASSERT(strstr(out, "/definitely/missing/old") == NULL);
    YEW_ASSERT_NOT_NULL(strstr(out, "/definitely/missing/new"));
    YEW_ASSERT_NOT_NULL(strstr(out, f.work));
    free(out);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_bare_string_upgrades_and_output_sorts(void)
{
    TrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;
    char doc[1024];
    char *real;
    char *out;
    const char *a;
    const char *z;

    tf_make(&f);
    real = realpath(f.work, NULL);
    YEW_ASSERT_NOT_NULL(real);
    (void)snprintf(doc, sizeof(doc),
                   "{schema: 1, future: {keep: true}, dirs: {\n"
                   "  \"/z-last\": \"denied\",\n"
                   "  \"%s\": \"trusted\",\n"
                   "  \"/a-first\": \"denied\",\n"
                   "}}\n", real);
    tf_write(f.dbpath, doc);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_GRANTED);
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    out = tf_read(f.dbpath);
    a = strstr(out, "/a-first");
    z = strstr(out, "/z-last");
    YEW_ASSERT_NOT_NULL(a);
    YEW_ASSERT_NOT_NULL(z);
    YEW_ASSERT(a < z);
    YEW_ASSERT_NOT_NULL(strstr(out, "future: {"));
    YEW_ASSERT_NOT_NULL(strstr(out, "hash: \""));
    YEW_ASSERT_NOT_NULL(strstr(out, "dev:"));
    YEW_ASSERT_NOT_NULL(strstr(out, "ino:"));
    free(out);
    free(real);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_truncated_load_does_not_clobber_live_db(void)
{
    TrustFix f;
    YewTrustDb db;
    char *out;

    tf_make(&f);
    tf_write(f.dbpath,
             "{schema: 1, marker: \"survives\", dirs: {}}\n");
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    tf_write(f.dbpath, "{schema: 1, dirs: {");
    YEW_ASSERT(!yew_trust_db_load_path(&db, f.dbpath));
    out = tf_read(f.dbpath);
    YEW_ASSERT_EQ_STR(out, "{schema: 1, dirs: {");
    free(out);
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 1000, 365U));
    out = tf_read(f.dbpath);
    YEW_ASSERT_NOT_NULL(strstr(out, "marker: \"survives\""));
    free(out);
    yew_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_no_tty_never_requests_prompt(void)
{
    TrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;

    tf_make(&f);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, false, false, &probe),
                      YEW_TRUST_SKIP_NO_TTY);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, false, true, &probe),
                      YEW_TRUST_GRANTED);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    tf_remove(&f);
}
