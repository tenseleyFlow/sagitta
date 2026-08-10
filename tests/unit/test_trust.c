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

    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-trust-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)snprintf(f->dbpath, sizeof(f->dbpath), "%s/trust.fl", f->root);
    (void)snprintf(f->work, sizeof(f->work), "%s/work", f->root);
    SAG_ASSERT_EQ_I64(mkdir(f->work, 0700), 0);
    (void)snprintf(f->config, sizeof(f->config), "%s/.sagitta.fl", f->work);
    fp = fopen(f->config, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_U64(fwrite("set({tabwidth: 8})\n", 1U, 19U, fp), 19U);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
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

    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_U64(fwrite(bytes, 1U, strlen(bytes), fp), strlen(bytes));
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
}

static char *tf_read(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    char *out;

    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_END), 0);
    n = ftell(fp);
    SAG_ASSERT(n >= 0L);
    SAG_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_SET), 0);
    out = sag_xmalloc((size_t)n + 1U);
    SAG_ASSERT_EQ_U64(fread(out, 1U, (size_t)n, fp), (size_t)n);
    out[n] = '\0';
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    return out;
}

static void tf_trust(SagTrustDb *db, const char *work, time_t now,
                     SagTrustProbe *probe)
{
    SAG_ASSERT_EQ_I64(sag_trust_check(db, work, true, false, probe),
                      SAG_TRUST_PROMPT_NEW);
    SAG_ASSERT(sag_trust_answer(db, probe, SAG_TRUST_ALWAYS, now));
    SAG_ASSERT_EQ_I64(sag_trust_check(db, work, true, false, probe),
                      SAG_TRUST_GRANTED);
}

void test_trust_content_hash_invalidation_ignores_mtime(void)
{
    TrustFix f;
    SagTrustDb db;
    SagTrustProbe probe;
    struct timespec times[2] = {{1234, 0}, {1234, 0}};

    tf_make(&f);
    sag_trust_db_init(&db);
    sag_trust_probe_init(&probe);
    tf_trust(&db, f.work, 1000, &probe);
    SAG_ASSERT_EQ_I64(utimensat(AT_FDCWD, f.config, times, 0), 0);
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_GRANTED);
    tf_write(f.config, "set({tabwidth: 4})\n");
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_PROMPT_CHANGED);
    SAG_ASSERT_EQ_STR(sag_trust_decision_reason(SAG_TRUST_PROMPT_CHANGED),
                      "the config changed since you trusted it");
    sag_trust_probe_free(&probe);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_realpath_move_and_symlink_do_not_carry_grant(void)
{
    TrustFix f;
    SagTrustDb db;
    SagTrustProbe probe;
    char link[192];
    char moved[192];

    tf_make(&f);
    (void)snprintf(link, sizeof(link), "%s/link", f.root);
    (void)snprintf(moved, sizeof(moved), "%s/moved", f.root);
    SAG_ASSERT_EQ_I64(symlink(f.work, link), 0);
    sag_trust_db_init(&db);
    sag_trust_probe_init(&probe);
    tf_trust(&db, link, 1000, &probe);
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_GRANTED);
    SAG_ASSERT_EQ_I64(rename(f.work, moved), 0);
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, moved, true, false, &probe),
                      SAG_TRUST_PROMPT_NEW);
    sag_trust_probe_free(&probe);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_same_path_new_inode_reprompts(void)
{
    TrustFix f;
    SagTrustDb db;
    SagTrustProbe probe;
    char old[192];

    tf_make(&f);
    sag_trust_db_init(&db);
    sag_trust_probe_init(&probe);
    tf_trust(&db, f.work, 1000, &probe);
    (void)snprintf(old, sizeof(old), "%s/old", f.root);
    SAG_ASSERT_EQ_I64(rename(f.work, old), 0);
    SAG_ASSERT_EQ_I64(mkdir(f.work, 0700), 0);
    tf_write(f.config, "set({tabwidth: 8})\n");
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_PROMPT_REPLACED);
    sag_trust_probe_free(&probe);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_deleted_config_retains_entry(void)
{
    TrustFix f;
    SagTrustDb db;
    SagTrustProbe probe;

    tf_make(&f);
    sag_trust_db_init(&db);
    sag_trust_probe_init(&probe);
    tf_trust(&db, f.work, 1000, &probe);
    SAG_ASSERT_EQ_I64(unlink(f.config), 0);
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_NO_CONFIG);
    tf_write(f.config, "set({tabwidth: 8})\n");
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_GRANTED);
    sag_trust_probe_free(&probe);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_write_prunes_only_old_missing_dirs(void)
{
    TrustFix f;
    SagTrustDb db;
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
    sag_trust_db_init(&db);
    SAG_ASSERT(sag_trust_db_load_path(&db, f.dbpath));
    SAG_ASSERT(sag_trust_db_write_path(&db, f.dbpath, 200000, 1U));
    out = tf_read(f.dbpath);
    SAG_ASSERT(strstr(out, "/definitely/missing/old") == NULL);
    SAG_ASSERT_NOT_NULL(strstr(out, "/definitely/missing/new"));
    SAG_ASSERT_NOT_NULL(strstr(out, f.work));
    free(out);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_bare_string_upgrades_and_output_sorts(void)
{
    TrustFix f;
    SagTrustDb db;
    SagTrustProbe probe;
    char doc[1024];
    char *real;
    char *out;
    const char *a;
    const char *z;

    tf_make(&f);
    real = realpath(f.work, NULL);
    SAG_ASSERT_NOT_NULL(real);
    (void)snprintf(doc, sizeof(doc),
                   "{schema: 1, future: {keep: true}, dirs: {\n"
                   "  \"/z-last\": \"denied\",\n"
                   "  \"%s\": \"trusted\",\n"
                   "  \"/a-first\": \"denied\",\n"
                   "}}\n", real);
    tf_write(f.dbpath, doc);
    sag_trust_db_init(&db);
    sag_trust_probe_init(&probe);
    SAG_ASSERT(sag_trust_db_load_path(&db, f.dbpath));
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, true, false, &probe),
                      SAG_TRUST_GRANTED);
    SAG_ASSERT(sag_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    out = tf_read(f.dbpath);
    a = strstr(out, "/a-first");
    z = strstr(out, "/z-last");
    SAG_ASSERT_NOT_NULL(a);
    SAG_ASSERT_NOT_NULL(z);
    SAG_ASSERT(a < z);
    SAG_ASSERT_NOT_NULL(strstr(out, "future: {"));
    SAG_ASSERT_NOT_NULL(strstr(out, "hash: \""));
    SAG_ASSERT_NOT_NULL(strstr(out, "dev:"));
    SAG_ASSERT_NOT_NULL(strstr(out, "ino:"));
    free(out);
    free(real);
    sag_trust_probe_free(&probe);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_truncated_load_does_not_clobber_live_db(void)
{
    TrustFix f;
    SagTrustDb db;
    char *out;

    tf_make(&f);
    tf_write(f.dbpath,
             "{schema: 1, marker: \"survives\", dirs: {}}\n");
    sag_trust_db_init(&db);
    SAG_ASSERT(sag_trust_db_load_path(&db, f.dbpath));
    tf_write(f.dbpath, "{schema: 1, dirs: {");
    SAG_ASSERT(!sag_trust_db_load_path(&db, f.dbpath));
    out = tf_read(f.dbpath);
    SAG_ASSERT_EQ_STR(out, "{schema: 1, dirs: {");
    free(out);
    SAG_ASSERT(sag_trust_db_write_path(&db, f.dbpath, 1000, 365U));
    out = tf_read(f.dbpath);
    SAG_ASSERT_NOT_NULL(strstr(out, "marker: \"survives\""));
    free(out);
    sag_trust_db_free(&db);
    tf_remove(&f);
}

void test_trust_no_tty_never_requests_prompt(void)
{
    TrustFix f;
    SagTrustDb db;
    SagTrustProbe probe;

    tf_make(&f);
    sag_trust_db_init(&db);
    sag_trust_probe_init(&probe);
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, false, false, &probe),
                      SAG_TRUST_SKIP_NO_TTY);
    SAG_ASSERT_EQ_I64(sag_trust_check(&db, f.work, false, true, &probe),
                      SAG_TRUST_GRANTED);
    sag_trust_probe_free(&probe);
    sag_trust_db_free(&db);
    tf_remove(&f);
}
