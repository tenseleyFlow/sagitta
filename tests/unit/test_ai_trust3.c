#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ws/trust.h"

typedef struct AiTrustFix {
    char root[160];
    char dbpath[208];
    char work[208];
    char config[240];
} AiTrustFix;

static void at_make(AiTrustFix *f)
{
    FILE *fp;

    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-ai-trust3-XXXXXX");
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

static void at_remove(const AiTrustFix *f)
{
    char old[240];
    char old_config[272];

    (void)snprintf(old, sizeof(old), "%s/old", f->root);
    (void)snprintf(old_config, sizeof(old_config), "%s/.yew.fl", old);
    (void)unlink(f->config);
    (void)unlink(old_config);
    (void)unlink(f->dbpath);
    (void)rmdir(f->work);
    (void)rmdir(old);
    (void)rmdir(f->root);
}

static void at_write(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    size_t n = strlen(text);

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, n, fp), n);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static char *at_read(const char *path)
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

static void at_trust(YewTrustDb *db, const AiTrustFix *f,
                     YewTrustProbe *probe)
{
    YEW_ASSERT_EQ_I64(yew_trust_check(db, f->work, true, false, probe),
                      YEW_TRUST_PROMPT_NEW);
    YEW_ASSERT(yew_trust_answer(db, probe, YEW_TRUST_ALWAYS, 1000));
    YEW_ASSERT_EQ_I64(yew_trust_check(db, f->work, true, false, probe),
                      YEW_TRUST_GRANTED);
}

void test_ai_trust3_grants_are_independent_allow_deny_unset(void)
{
    AiTrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;

    at_make(&f);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    at_trust(&db, &f, &probe);
    YEW_ASSERT_EQ_I64(yew_trust_ai_grant(&db, f.work), YEW_AI_WS_UNSET);
    YEW_ASSERT(yew_trust_ai_set(&db, f.work, YEW_AI_WS_DENY, 1001));
    YEW_ASSERT_EQ_I64(yew_trust_ai_grant(&db, f.work), YEW_AI_WS_DENY);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_GRANTED);
    YEW_ASSERT(yew_trust_ai_set(&db, f.work, YEW_AI_WS_ALLOW, 1002));
    YEW_ASSERT_EQ_I64(yew_trust_ai_grant(&db, f.work), YEW_AI_WS_ALLOW);
    YEW_ASSERT(yew_trust_ai_forget(&db, f.work));
    YEW_ASSERT_EQ_I64(yew_trust_ai_grant(&db, f.work), YEW_AI_WS_UNSET);
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_GRANTED);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    at_remove(&f);
}

void test_ai_trust3_schema2_plugins_block_survives_byte_identically(void)
{
    static const char doc[] =
        "{\n"
        "  schema: 2,\n"
        "  plugins: {\n"
        "    keep: {\n"
        "      enabled: true,\n"
        "      opaque: [\n"
        "        \"x\",\n"
        "        7,\n"
        "      ],\n"
        "    },\n"
        "  },\n"
        "  dirs: {\n"
        "  },\n"
        "}\n";
    static const char block[] =
        "  plugins: {\n"
        "    keep: {\n"
        "      enabled: true,\n"
        "      opaque: [\n"
        "        \"x\",\n"
        "        7,\n"
        "      ],\n"
        "    },\n"
        "  },\n";
    AiTrustFix f;
    YewTrustDb db;
    char *out;

    at_make(&f);
    at_write(f.dbpath, doc);
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    out = at_read(f.dbpath);
    YEW_ASSERT_NOT_NULL(strstr(out, "  schema: 3,"));
    YEW_ASSERT_NOT_NULL(strstr(out, block));
    free(out);
    yew_trust_db_free(&db);
    at_remove(&f);
}

void test_ai_trust3_future_schema_is_never_downgraded(void)
{
    AiTrustFix f;
    YewTrustDb db;
    char *out;

    at_make(&f);
    at_write(f.dbpath,
             "{schema: 12, future: {token: \"keep\"}, dirs: {}}\n");
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    out = at_read(f.dbpath);
    YEW_ASSERT_NOT_NULL(strstr(out, "schema: 12"));
    YEW_ASSERT_NOT_NULL(strstr(out, "future: {"));
    YEW_ASSERT_NOT_NULL(strstr(out, "token: \"keep\""));
    free(out);
    yew_trust_db_free(&db);
    at_remove(&f);
}

void test_ai_trust3_update_preserves_unknown_entry_fields(void)
{
    AiTrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;
    struct stat st;
    char *real;
    char doc[2048];
    char *out;

    at_make(&f);
    real = realpath(f.work, NULL);
    YEW_ASSERT_NOT_NULL(real);
    YEW_ASSERT_EQ_I64(stat(real, &st), 0);
    (void)snprintf(doc, sizeof(doc),
                   "{schema: 3, root_future: 41, dirs: {\n"
                   "  \"%s\": {state: \"denied\", ai: \"deny\", "
                   "dev: %lld, ino: %lld, at: 1, "
                   "future: {nested: [1, 2]}},\n"
                   "}}\n", real, (long long)st.st_dev,
                   (long long)st.st_ino);
    at_write(f.dbpath, doc);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_DENIED);
    YEW_ASSERT(yew_trust_answer(&db, &probe, YEW_TRUST_ALWAYS, 2999));
    YEW_ASSERT(yew_trust_ai_set(&db, f.work, YEW_AI_WS_ALLOW, 3000));
    YEW_ASSERT_EQ_I64(yew_trust_ai_grant(&db, f.work), YEW_AI_WS_ALLOW);
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 3000, 365U));
    out = at_read(f.dbpath);
    YEW_ASSERT_NOT_NULL(strstr(out, "root_future: 41"));
    YEW_ASSERT_NOT_NULL(strstr(out, "state: \"trusted\""));
    YEW_ASSERT_NOT_NULL(strstr(out, "ai: \"allow\""));
    YEW_ASSERT_NOT_NULL(strstr(out, "future: {"));
    YEW_ASSERT_NOT_NULL(strstr(out, "nested: ["));
    free(out);
    free(real);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    at_remove(&f);
}

void test_ai_trust3_writes_sorted_atomically_and_deterministically(void)
{
    AiTrustFix f;
    YewTrustDb db;
    char *first;
    char *second;
    const char *a;
    const char *z;
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    at_make(&f);
    at_write(f.dbpath,
             "{schema: 1, dirs: {\"/z-last\": \"denied\", "
             "\"/a-first\": \"denied\"}}\n");
    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load_path(&db, f.dbpath));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    first = at_read(f.dbpath);
    a = strstr(first, "/a-first");
    z = strstr(first, "/z-last");
    YEW_ASSERT_NOT_NULL(a);
    YEW_ASSERT_NOT_NULL(z);
    YEW_ASSERT(a < z);
    YEW_ASSERT_NOT_NULL(strstr(first, "schema: 3"));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 2000, 365U));
    second = at_read(f.dbpath);
    YEW_ASSERT_EQ_STR(first, second);
    YEW_ASSERT_EQ_I64(stat(f.dbpath, &st), 0);
    YEW_ASSERT_EQ_U64(st.st_mode & 0777U, 0600U);
    dir = opendir(f.root);
    YEW_ASSERT_NOT_NULL(dir);
    while ((entry = readdir(dir)) != NULL)
        YEW_ASSERT(strncmp(entry->d_name, ".yew-trust.fl-", 14U) != 0);
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    free(second);
    free(first);
    yew_trust_db_free(&db);
    at_remove(&f);
}

void test_ai_trust3_replacement_clears_state_and_ai(void)
{
    AiTrustFix f;
    YewTrustDb db;
    YewTrustProbe probe;
    char old[224];
    char *out;

    at_make(&f);
    yew_trust_db_init(&db);
    yew_trust_probe_init(&probe);
    at_trust(&db, &f, &probe);
    YEW_ASSERT(yew_trust_ai_set(&db, f.work, YEW_AI_WS_ALLOW, 1001));
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 1001, 365U));
    (void)snprintf(old, sizeof(old), "%s/old", f.root);
    YEW_ASSERT_EQ_I64(rename(f.work, old), 0);
    YEW_ASSERT_EQ_I64(mkdir(f.work, 0700), 0);
    at_write(f.config, "set({tabwidth: 8})\n");
    YEW_ASSERT_EQ_I64(yew_trust_check(&db, f.work, true, false, &probe),
                      YEW_TRUST_PROMPT_REPLACED);
    YEW_ASSERT_EQ_I64(yew_trust_ai_grant(&db, f.work), YEW_AI_WS_UNSET);
    YEW_ASSERT(yew_trust_db_write_path(&db, f.dbpath, 1002, 365U));
    out = at_read(f.dbpath);
    YEW_ASSERT(strstr(out, "state: \"trusted\"") == NULL);
    YEW_ASSERT(strstr(out, "ai: \"allow\"") == NULL);
    free(out);
    yew_trust_probe_free(&probe);
    yew_trust_db_free(&db);
    at_remove(&f);
}
