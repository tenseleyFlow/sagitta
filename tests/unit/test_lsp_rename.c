#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "mod/lsp/rename.h"
#include "mod/lsp/sync.h"
#include "text/piece.h"
#include "text/undo.h"
#include "util/arena.h"
#include "util/buf.h"

typedef struct RenameFix {
    Ed ed;
    Arena json;
    char root[96];
    char state[128];
    char a[128];
    char b[128];
    char c[128];
    char d[128];
    char e[128];
    char f[128];
    char dir[128];
    char *saved_xdg_state;
} RenameFix;

static const u8 rename_a_text[] = "alpha alpha\nA\xF0\x9F\x8C\xB2" "B\n";
static const u8 rename_b_text[] = "alpha beta alpha\n";
static const u8 rename_c_text[] = "alpha gamma\n";
static const u8 rename_d_text[] = "alpha delta\n";
static const u8 rename_e_text[] = "alpha epsilon\n";
static const u8 rename_f_text[] = "alpha phi\n";

static void rename_write(const char *path, const u8 *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t at = 0U;

    if (fd < 0)
        YEW_BUG("rename fixture open failed: %s", strerror(errno));
    while (at < len) {
        ssize_t wrote = write(fd, bytes + at, len - at);

        if (wrote <= 0)
            YEW_BUG("rename fixture write failed: %s", strerror(errno));
        at += (size_t)wrote;
    }
    if (close(fd) != 0)
        YEW_BUG("rename fixture close failed: %s", strerror(errno));
}

static void rename_fix_init(RenameFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root),
                   "/tmp/yew-lsp-rename-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    YEW_ASSERT(yew_test_canonicalize_path(f->root, sizeof(f->root)));
    YEW_ASSERT(snprintf(f->state, sizeof(f->state), "%s/state",
                        f->root) > 0);
    if (getenv("XDG_STATE_HOME") != NULL)
        f->saved_xdg_state = strdup(getenv("XDG_STATE_HOME"));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state, 1), 0);
    YEW_ASSERT(snprintf(f->a, sizeof(f->a), "%s/a.c", f->root) > 0);
    YEW_ASSERT(snprintf(f->b, sizeof(f->b), "%s/b.c", f->root) > 0);
    YEW_ASSERT(snprintf(f->c, sizeof(f->c), "%s/c.c", f->root) > 0);
    YEW_ASSERT(snprintf(f->d, sizeof(f->d), "%s/d.c", f->root) > 0);
    YEW_ASSERT(snprintf(f->e, sizeof(f->e), "%s/e.c", f->root) > 0);
    YEW_ASSERT(snprintf(f->f, sizeof(f->f), "%s/f.c", f->root) > 0);
    YEW_ASSERT(snprintf(f->dir, sizeof(f->dir), "%s/not-regular", f->root)
               > 0);
    rename_write(f->a, rename_a_text, sizeof(rename_a_text) - 1U);
    rename_write(f->b, rename_b_text, sizeof(rename_b_text) - 1U);
    rename_write(f->c, rename_c_text, sizeof(rename_c_text) - 1U);
    rename_write(f->d, rename_d_text, sizeof(rename_d_text) - 1U);
    rename_write(f->e, rename_e_text, sizeof(rename_e_text) - 1U);
    rename_write(f->f, rename_f_text, sizeof(rename_f_text) - 1U);
    YEW_ASSERT_EQ_I64(mkdir(f->dir, 0700), 0);

    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->root);
    YEW_ASSERT_EQ_I64(yew_ed_open(&f->ed, f->a), YEW_LOAD_OK);
    arena_init(&f->json);
}

static void rename_fix_free(RenameFix *f)
{
    char path[192];
    u32 i;

    arena_free_all(&f->json);
    for (i = 0U; i < f->ed.ws.nbufs; i++) {
        Buffer *buffer = f->ed.ws.bufs[i];

        if (buffer != NULL && buffer->jrn != NULL) {
            yew_journal_discard(buffer->jrn);
            buffer->jrn = NULL;
        }
    }
    yew_ed_free(&f->ed);
    if (f->saved_xdg_state != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->saved_xdg_state, 1),
                          0);
        free(f->saved_xdg_state);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    YEW_ASSERT(snprintf(path, sizeof(path), "%s/yew/journal", f->state) > 0);
    if (rmdir(path) != 0)
        YEW_ASSERT_EQ_I64(errno, ENOENT);
    YEW_ASSERT(snprintf(path, sizeof(path), "%s/yew", f->state) > 0);
    if (rmdir(path) != 0)
        YEW_ASSERT_EQ_I64(errno, ENOENT);
    if (rmdir(f->state) != 0)
        YEW_ASSERT_EQ_I64(errno, ENOENT);
    YEW_ASSERT_EQ_I64(unlink(f->a), 0);
    YEW_ASSERT_EQ_I64(unlink(f->b), 0);
    YEW_ASSERT_EQ_I64(unlink(f->c), 0);
    YEW_ASSERT_EQ_I64(unlink(f->d), 0);
    YEW_ASSERT_EQ_I64(unlink(f->e), 0);
    YEW_ASSERT_EQ_I64(unlink(f->f), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
}

static JsonValue *rename_json(RenameFix *f, const char *json)
{
    JsonErr err;
    JsonValue *value;

    arena_free_all(&f->json);
    arena_init(&f->json);
    value = yew_json_parse(&f->json, (const u8 *)json,
                           (u64)strlen(json), &err);
    YEW_ASSERT_NOT_NULL(value);
    return value;
}

static void rename_assert_text(const TextBuf *tb, const u8 *want,
                               size_t want_len)
{
    TextIter it;
    u64 done = 0U;

    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        u64 take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        take = len < want_len - done ? len : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

static Buffer *rename_find_path(RenameFix *f, const char *path)
{
    u32 i;

    for (i = 0U; i < f->ed.ws.nbufs; i++) {
        Buffer *b = f->ed.ws.bufs[i];

        if (b->path != NULL && strcmp(b->path, path) == 0)
            return b;
    }
    return NULL;
}

static void rename_assert_sources_unchanged(RenameFix *f)
{
    Buffer *a = rename_find_path(f, f->a);
    Buffer *b = rename_find_path(f, f->b);
    Buffer *c = rename_find_path(f, f->c);

    YEW_ASSERT_NOT_NULL(a);
    rename_assert_text(a->tb, rename_a_text, sizeof(rename_a_text) - 1U);
    if (b != NULL && yew_buf_resident(b))
        rename_assert_text(b->tb, rename_b_text,
                           sizeof(rename_b_text) - 1U);
    if (c != NULL && yew_buf_resident(c))
        rename_assert_text(c->tb, rename_c_text,
                           sizeof(rename_c_text) - 1U);
}

static void rename_assert_disk(const char *path, const u8 *want,
                               size_t want_len)
{
    u8 bytes[64];
    size_t got = 0U;
    int fd = open(path, O_RDONLY);

    YEW_ASSERT(fd >= 0);
    while (got < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + got, sizeof(bytes) - got);

        YEW_ASSERT(n >= 0);
        if (n == 0)
            break;
        got += (size_t)n;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_U64(got, want_len);
    YEW_ASSERT_EQ_MEM(bytes, want, want_len);
}

static void rename_assert_fixture_disk(RenameFix *f)
{
    rename_assert_disk(f->a, rename_a_text, sizeof(rename_a_text) - 1U);
    rename_assert_disk(f->b, rename_b_text, sizeof(rename_b_text) - 1U);
    rename_assert_disk(f->c, rename_c_text, sizeof(rename_c_text) - 1U);
    rename_assert_disk(f->d, rename_d_text, sizeof(rename_d_text) - 1U);
    rename_assert_disk(f->e, rename_e_text, sizeof(rename_e_text) - 1U);
    rename_assert_disk(f->f, rename_f_text, sizeof(rename_f_text) - 1U);
}

static void rename_expect_reject(RenameFix *f, const char *json, u8 pos_enc,
                                 const char *want)
{
    RenamePlan plan;
    char err[YEW_RENAME_ERROR_MAX] = {0};

    yew_lsp_rename_plan_init(&plan);
    YEW_ASSERT(!yew_lsp_rename_preflight(&f->ed, rename_json(f, json),
                                          pos_enc, "alpha", "omega",
                                          &plan, err));
    YEW_ASSERT_EQ_STR(err, want);
    rename_assert_sources_unchanged(f);
    yew_lsp_rename_plan_free(&plan);
}

static char *rename_one_change(const char *uri, i64 sl, i64 sc,
                               i64 el, i64 ec)
{
    int need = snprintf(NULL, 0,
        "{\"changes\":{\"%s\":[{\"range\":{"
        "\"start\":{\"line\":%lld,\"character\":%lld},"
        "\"end\":{\"line\":%lld,\"character\":%lld}},"
        "\"newText\":\"omega\"}]}}",
        uri, (long long)sl, (long long)sc, (long long)el, (long long)ec);
    char *json;

    YEW_ASSERT(need >= 0);
    json = yew_xmalloc((size_t)need + 1U);
    YEW_ASSERT_EQ_I64(snprintf(json, (size_t)need + 1U,
        "{\"changes\":{\"%s\":[{\"range\":{"
        "\"start\":{\"line\":%lld,\"character\":%lld},"
        "\"end\":{\"line\":%lld,\"character\":%lld}},"
        "\"newText\":\"omega\"}]}}",
        uri, (long long)sl, (long long)sc, (long long)el, (long long)ec),
        need);
    return json;
}

void test_lsp_rename_preflight_refuses_non_file_uri_without_mutation(void)
{
    RenameFix f;
    char *json;

    rename_fix_init(&f);
    json = rename_one_change("https://example.test/a.c", 0, 0, 0, 5);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8,
                         "rename touches a non-file URI; refusing");
    free(json);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_resource_operations_without_mutation(void)
{
    RenameFix f;
    char json[512];

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"documentChanges\":[{\"kind\":\"create\","
        "\"uri\":\"file://%s/new.c\"}]}", f.root) > 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8,
        "server asked to create or delete files; refusing (not supported in 1.0)");
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_rename_file_without_mutation(void)
{
    RenameFix f;
    char json[768];

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"documentChanges\":[{\"kind\":\"rename\","
        "\"oldUri\":\"file://%s\",\"newUri\":\"file://%s/new.c\"}]}",
        f.a, f.root) > 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8,
        "server asked to create or delete files; refusing (not supported in 1.0)");
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_delete_file_without_mutation(void)
{
    RenameFix f;
    char json[512];

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"documentChanges\":[{\"kind\":\"delete\","
        "\"uri\":\"file://%s\"}]}", f.a) > 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8,
        "server asked to create or delete files; refusing (not supported in 1.0)");
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_outside_workspace_without_mutation(void)
{
    RenameFix f;
    char outside[PATH_MAX] = "/tmp/yew-lsp-rename-outside-XXXXXX";
    char uri[160];
    char want[256];
    char *json;
    int fd;

    rename_fix_init(&f);
    fd = mkstemp(outside);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT(yew_test_canonicalize_path(outside, sizeof(outside)));
    YEW_ASSERT(snprintf(uri, sizeof(uri), "file://%s", outside) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want),
                       "rename would edit outside the workspace: %s",
                       outside) > 0);
    json = rename_one_change(uri, 0, 0, 0, 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8, want);
    free(json);
    YEW_ASSERT_EQ_I64(unlink(outside), 0);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_nonregular_path_without_mutation(void)
{
    RenameFix f;
    char uri[160];
    char want[256];
    char *json;

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(uri, sizeof(uri), "file://%s", f.dir) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want), "cannot edit %s: %s",
                       f.dir, strerror(EINVAL)) > 0);
    json = rename_one_change(uri, 0, 0, 0, 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8, want);
    free(json);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_unreadable_path_without_mutation(void)
{
    RenameFix f;
    char uri[160];
    char want[256];
    char *json;

    rename_fix_init(&f);
    YEW_ASSERT_EQ_I64(chmod(f.b, 0000), 0);
    YEW_ASSERT(snprintf(uri, sizeof(uri), "file://%s", f.b) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want), "cannot edit %s: %s",
                       f.b, strerror(EACCES)) > 0);
    json = rename_one_change(uri, 0, 0, 0, 5);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8, want);
    free(json);
    YEW_ASSERT_EQ_I64(chmod(f.b, 0600), 0);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_file_cap_without_mutation(void)
{
    RenameFix f;
    Bytebuf json;
    char paths[YEW_RENAME_MAX_FILES + 1U][160];
    u32 i;

    rename_fix_init(&f);
    bytebuf_init(&json);
    bytebuf_printf(&json, "{\"changes\":{");
    for (i = 0U; i <= YEW_RENAME_MAX_FILES; i++) {
        YEW_ASSERT(snprintf(paths[i], sizeof(paths[i]), "%s/f%03u.c",
                           f.root, i) > 0);
        rename_write(paths[i], (const u8 *)"x\n", 2U);
        bytebuf_printf(&json,
            "%s\"file://%s\":[{\"range\":{"
            "\"start\":{\"line\":0,\"character\":0},"
            "\"end\":{\"line\":0,\"character\":0}},"
            "\"newText\":\"x\"}]",
            i == 0U ? "" : ",", paths[i]);
    }
    bytebuf_printf(&json, "}}");
    bytebuf_push_u8(&json, 0U);
    rename_expect_reject(&f, (const char *)json.data, YEW_POSENC_UTF8,
                         "rename spans 201 files; refusing (limit 200)");
    bytebuf_free(&json);
    for (i = 0U; i <= YEW_RENAME_MAX_FILES; i++)
        YEW_ASSERT_EQ_I64(unlink(paths[i]), 0);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_edit_cap_without_mutation(void)
{
    RenameFix f;
    Bytebuf json;
    u32 i;

    rename_fix_init(&f);
    bytebuf_init(&json);
    bytebuf_printf(&json, "{\"changes\":{\"file://%s\":[", f.a);
    for (i = 0U; i <= YEW_RENAME_MAX_EDITS; i++) {
        bytebuf_printf(&json,
            "%s{\"range\":{\"start\":{\"line\":0,\"character\":0},"
            "\"end\":{\"line\":0,\"character\":0}},"
            "\"newText\":\"x\"}", i == 0U ? "" : ",");
    }
    bytebuf_printf(&json, "]}}");
    bytebuf_push_u8(&json, 0U);
    rename_expect_reject(&f, (const char *)json.data, YEW_POSENC_UTF8,
                         "rename spans 20001 edits; refusing (limit 20000)");
    bytebuf_free(&json);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_stale_version_without_mutation(void)
{
    RenameFix f;
    char json[768];

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"documentChanges\":[{\"textDocument\":{"
        "\"uri\":\"file://%s\",\"version\":999},\"edits\":[{"
        "\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},"
        "\"newText\":\"omega\"}]}]}", f.a) > 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8,
                         "rename is stale; the buffer changed. try again");
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_out_of_range_position_without_mutation(void)
{
    RenameFix f;
    char uri[160];
    char want[256];
    char *json;

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(uri, sizeof(uri), "file://%s", f.a) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want),
                       "rename produced an invalid range in %s:100", f.a)
               > 0);
    json = rename_one_change(uri, 99, 0, 99, 1);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8, want);
    free(json);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_inexact_utf16_position_without_mutation(void)
{
    RenameFix f;
    char uri[160];
    char want[256];
    char *json;

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(uri, sizeof(uri), "file://%s", f.a) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want),
                       "rename produced an invalid range in %s:2", f.a) > 0);
    json = rename_one_change(uri, 1, 2, 1, 3);
    rename_expect_reject(&f, json, YEW_POSENC_UTF16, want);
    free(json);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_grapheme_splitting_range_without_mutation(void)
{
    static const u8 combining[] = "e\xCC\x81x\n";
    RenameFix f;
    char uri[160];
    char want[256];
    char *json;

    rename_fix_init(&f);
    rename_write(f.b, combining, sizeof(combining) - 1U);
    YEW_ASSERT(snprintf(uri, sizeof(uri), "file://%s", f.b) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want),
                       "rename produced an invalid range in %s:1", f.b) > 0);
    json = rename_one_change(uri, 0, 1, 0, 3);
    {
        RenamePlan plan;
        char err[YEW_RENAME_ERROR_MAX] = {0};

        yew_lsp_rename_plan_init(&plan);
        YEW_ASSERT(!yew_lsp_rename_preflight(&f.ed, rename_json(&f, json),
                                              YEW_POSENC_UTF8, "e", "z",
                                              &plan, err));
        YEW_ASSERT_EQ_STR(err, want);
        rename_assert_text(yew_ed_doc(&f.ed)->tb, rename_a_text,
                           sizeof(rename_a_text) - 1U);
        rename_assert_text(rename_find_path(&f, f.b)->tb, combining,
                           sizeof(combining) - 1U);
        yew_lsp_rename_plan_free(&plan);
    }
    free(json);
    rename_write(f.b, rename_b_text, sizeof(rename_b_text) - 1U);
    rename_fix_free(&f);
}

void test_lsp_rename_preflight_refuses_overlapping_edits_without_mutation(void)
{
    RenameFix f;
    char json[1024];
    char want[256];

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"changes\":{\"file://%s\":["
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},\"newText\":\"x\"},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":4},"
        "\"end\":{\"line\":0,\"character\":11}},\"newText\":\"y\"}"
        "]}}", f.a) > 0);
    YEW_ASSERT(snprintf(want, sizeof(want),
        "server sent overlapping edits for %s; refusing", f.a) > 0);
    rename_expect_reject(&f, json, YEW_POSENC_UTF8, want);
    rename_fix_free(&f);
}

static void rename_assert_same_plan(const RenamePlan *a, const RenamePlan *b)
{
    size_t i;

    YEW_ASSERT_EQ_U64(a->files.len, b->files.len);
    YEW_ASSERT_EQ_U64(a->nedits, b->nedits);
    for (i = 0U; i < a->files.len; i++) {
        const RenameFile *af = &a->files.data[i];
        const RenameFile *bf = &b->files.data[i];
        size_t j;

        YEW_ASSERT_EQ_STR(af->path, bf->path);
        YEW_ASSERT_EQ_U64(af->edits.len, bf->edits.len);
        for (j = 0U; j < af->edits.len; j++) {
            YEW_ASSERT_EQ_U64(af->edits.data[j].lo.v,
                              bf->edits.data[j].lo.v);
            YEW_ASSERT_EQ_U64(af->edits.data[j].hi.v,
                              bf->edits.data[j].hi.v);
            YEW_ASSERT_EQ_U64(af->edits.data[j].len,
                              bf->edits.data[j].len);
            YEW_ASSERT_EQ_MEM(af->edits.data[j].text,
                              bf->edits.data[j].text,
                              af->edits.data[j].len);
        }
    }
}

void test_lsp_rename_preflight_normalizes_changes_and_document_changes_equally(void)
{
    RenameFix f;
    RenamePlan changes;
    RenamePlan docs;
    char plain[2048];
    char versioned[2048];
    char err[YEW_RENAME_ERROR_MAX] = {0};

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(plain, sizeof(plain),
        "{\"changes\":{\"file://%s\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},\"newText\":\"one\"}],"
        "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
        "\"newText\":\"one\"},{\"range\":{\"start\":{\"line\":0,"
        "\"character\":6},\"end\":{\"line\":0,\"character\":11}},"
        "\"newText\":\"two\"}]}}", f.b, f.a) > 0);
    YEW_ASSERT(snprintf(versioned, sizeof(versioned),
        "{\"documentChanges\":[{\"textDocument\":{\"uri\":\"file://%s\"},"
        "\"edits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},\"newText\":\"one\"}]},"
        "{\"textDocument\":{\"uri\":\"file://%s\"},\"edits\":["
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},\"newText\":\"one\"},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":6},"
        "\"end\":{\"line\":0,\"character\":11}},\"newText\":\"two\"}]}]}",
        f.b, f.a) > 0);

    yew_lsp_rename_plan_init(&changes);
    yew_lsp_rename_plan_init(&docs);
    YEW_ASSERT(yew_lsp_rename_preflight(&f.ed, rename_json(&f, plain),
                                         YEW_POSENC_UTF8, "alpha", "new",
                                         &changes, err));
    err[0] = '\0';
    YEW_ASSERT(yew_lsp_rename_preflight(&f.ed, rename_json(&f, versioned),
                                         YEW_POSENC_UTF8, "alpha", "new",
                                         &docs, err));
    rename_assert_same_plan(&changes, &docs);
    YEW_ASSERT_EQ_STR(changes.files.data[0].path, f.a);
    YEW_ASSERT_EQ_U64(changes.files.data[0].edits.data[0].lo.v, 6U);
    YEW_ASSERT_EQ_U64(changes.files.data[0].edits.data[1].lo.v, 0U);
    yew_lsp_rename_plan_free(&docs);
    yew_lsp_rename_plan_free(&changes);
    rename_fix_free(&f);
}

void test_lsp_rename_apply_commits_one_lsp_undo_node_per_buffer(void)
{
    RenameFix f;
    RenamePlan plan;
    Buffer *a;
    Buffer *b;
    char json[2048];
    char err[YEW_RENAME_ERROR_MAX] = {0};
    u32 a_nodes;
    u32 b_nodes;

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"changes\":{\"file://%s\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},\"newText\":\"omega\"}],"
        "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
        "\"newText\":\"omega\"},{\"range\":{\"start\":{\"line\":0,"
        "\"character\":6},\"end\":{\"line\":0,\"character\":11}},"
        "\"newText\":\"omega\"}]}}", f.b, f.a) > 0);
    yew_lsp_rename_plan_init(&plan);
    YEW_ASSERT(yew_lsp_rename_preflight(&f.ed, rename_json(&f, json),
                                         YEW_POSENC_UTF8, "alpha", "omega",
                                         &plan, err));
    a = rename_find_path(&f, f.a);
    b = rename_find_path(&f, f.b);
    YEW_ASSERT_NOT_NULL(a);
    YEW_ASSERT_NOT_NULL(b);
    a_nodes = (u32)a->undo->nodes.len;
    b_nodes = (u32)b->undo->nodes.len;

    if (!yew_lsp_rename_apply(&f.ed, &plan, err))
        yew_test_fail(__FILE__, __LINE__, err);
    YEW_ASSERT_EQ_U64(a->undo->nodes.len, a_nodes + 1U);
    YEW_ASSERT_EQ_U64(b->undo->nodes.len, b_nodes + 1U);
    YEW_ASSERT_EQ_U64(a->undo->nodes.data[a->undo->cur - 1U].reason,
                      YEW_TXN_LSP);
    YEW_ASSERT_EQ_U64(b->undo->nodes.data[b->undo->cur - 1U].reason,
                      YEW_TXN_LSP);
    rename_assert_text(a->tb,
                       (const u8 *)"omega omega\nA\xF0\x9F\x8C\xB2" "B\n",
                       sizeof("omega omega\nA\xF0\x9F\x8C\xB2" "B\n") - 1U);
    rename_assert_text(b->tb, (const u8 *)"omega beta alpha\n",
                       sizeof("omega beta alpha\n") - 1U);
    yew_lsp_rename_plan_free(&plan);
    rename_fix_free(&f);
}

void test_lsp_rename_multiline_delete_does_not_touch_registers(void)
{
    static const u8 expected[] =
        "alpha omega\xF0\x9F\x8C\xB2" "B\n";
    RenameFix f;
    RenamePlan plan;
    RegVal numbered;
    RegVal small;
    Buffer *buffer;
    char json[1024];
    char err[YEW_RENAME_ERROR_MAX] = {0};
    u32 undo_before;

    rename_fix_init(&f);
    f.ed.regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    yew_regval_init(&numbered);
    numbered.type = YEW_REG_LINEWISE;
    bytebuf_append(&numbered.bytes, "sentinel\n", 9U);
    yew_reg_delete(&f.ed.regs, 0U, &numbered);
    yew_regval_init(&small);
    small.type = YEW_REG_CHARWISE;
    bytebuf_append(&small.bytes, "x", 1U);
    yew_reg_delete(&f.ed.regs, 0U, &small);
    YEW_ASSERT_EQ_U64(f.ed.regs.ring_len, 2U);

    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"changes\":{\"file://%s\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":6},"
        "\"end\":{\"line\":1,\"character\":1}},"
        "\"newText\":\"omega\"}]}}", f.a) > 0);
    yew_lsp_rename_plan_init(&plan);
    YEW_ASSERT(yew_lsp_rename_preflight(&f.ed, rename_json(&f, json),
                                         YEW_POSENC_UTF8, "alpha", "omega",
                                         &plan, err));
    buffer = rename_find_path(&f, f.a);
    YEW_ASSERT_NOT_NULL(buffer);
    undo_before = yew_undo_current(buffer->undo);
    if (!yew_lsp_rename_apply(&f.ed, &plan, err))
        yew_test_fail(__FILE__, __LINE__, err);

    rename_assert_text(buffer->tb, expected, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(buffer->undo), undo_before + 1U);
    YEW_ASSERT_EQ_U64(f.ed.regs.numbered[1].bytes.len, 9U);
    YEW_ASSERT_EQ_MEM(f.ed.regs.numbered[1].bytes.data, "sentinel\n", 9U);
    YEW_ASSERT_EQ_U64(f.ed.regs.small_del.bytes.len, 1U);
    YEW_ASSERT_EQ_MEM(f.ed.regs.small_del.bytes.data, "x", 1U);
    YEW_ASSERT_EQ_U64(f.ed.regs.unnamed.bytes.len, 1U);
    YEW_ASSERT_EQ_MEM(f.ed.regs.unnamed.bytes.data, "x", 1U);
    YEW_ASSERT_EQ_U64(f.ed.regs.ring_len, 2U);

    yew_regval_free(&small);
    yew_regval_free(&numbered);
    yew_lsp_rename_plan_free(&plan);
    rename_fix_free(&f);
}

void test_lsp_rename_apply_uses_descending_original_offsets(void)
{
    static const u8 source[] = "one two three\n";
    static const u8 changed[] = "1 TWO-TWO 3\n";
    RenameFix f;
    RenamePlan plan;
    Buffer *buffer;
    EditCtx ec;
    char json[1536];
    char err[YEW_RENAME_ERROR_MAX] = {0};
    u32 undo_before;

    rename_fix_init(&f);
    rename_write(f.c, source, sizeof(source) - 1U);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"changes\":{\"file://%s\":["
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"1\"},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":4},"
        "\"end\":{\"line\":0,\"character\":7}},"
        "\"newText\":\"TWO-TWO\"},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":8},"
        "\"end\":{\"line\":0,\"character\":13}},\"newText\":\"3\"}"
        "]}}", f.c) > 0);
    yew_lsp_rename_plan_init(&plan);
    YEW_ASSERT(yew_lsp_rename_preflight(&f.ed, rename_json(&f, json),
                                         YEW_POSENC_UTF8, "one", "1",
                                         &plan, err));
    buffer = rename_find_path(&f, f.c);
    YEW_ASSERT_NOT_NULL(buffer);
    undo_before = yew_undo_current(buffer->undo);

    YEW_ASSERT(yew_lsp_rename_apply(&f.ed, &plan, err));
    rename_assert_text(buffer->tb, changed, sizeof(changed) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(buffer->undo), undo_before + 1U);
    ec = yew_ed_edit_ctx_buffer(&f.ed, buffer);
    YEW_ASSERT(yew_undo(&ec));
    yew_ed_finish_edit(&f.ed, &ec);
    rename_assert_text(buffer->tb, source, sizeof(source) - 1U);

    yew_lsp_rename_plan_free(&plan);
    rename_fix_free(&f);
}

void test_lsp_rename_apply_six_files_undo_and_leave_disk_unchanged(void)
{
    RenameFix f;
    RenamePlan plan;
    Bytebuf json;
    const char *paths[6];
    const u8 *original[6] = {
        rename_a_text, rename_b_text, rename_c_text,
        rename_d_text, rename_e_text, rename_f_text
    };
    const size_t original_len[6] = {
        sizeof(rename_a_text) - 1U, sizeof(rename_b_text) - 1U,
        sizeof(rename_c_text) - 1U, sizeof(rename_d_text) - 1U,
        sizeof(rename_e_text) - 1U, sizeof(rename_f_text) - 1U
    };
    Buffer *buffers[6];
    u32 undo_before[6];
    char err[YEW_RENAME_ERROR_MAX] = {0};
    size_t i;

    rename_fix_init(&f);
    paths[0] = f.a;
    paths[1] = f.b;
    paths[2] = f.c;
    paths[3] = f.d;
    paths[4] = f.e;
    paths[5] = f.f;
    bytebuf_init(&json);
    bytebuf_append(&json, "{\"changes\":{", sizeof("{\"changes\":{") - 1U);
    for (i = 0U; i < 6U; i++) {
        bytebuf_printf(&json,
            "%s\"file://%s\":[{\"range\":{"
            "\"start\":{\"line\":0,\"character\":0},"
            "\"end\":{\"line\":0,\"character\":5}},"
            "\"newText\":\"omega\"}]", i == 0U ? "" : ",", paths[i]);
    }
    bytebuf_append(&json, "}}", 2U);
    bytebuf_push_u8(&json, 0U);
    rename_assert_fixture_disk(&f);
    yew_lsp_rename_plan_init(&plan);
    YEW_ASSERT(yew_lsp_rename_preflight(
        &f.ed, rename_json(&f, (const char *)json.data), YEW_POSENC_UTF8,
        "alpha", "omega", &plan, err));
    YEW_ASSERT_EQ_U64(plan.files.len, 6U);
    for (i = 0U; i < 6U; i++) {
        YEW_ASSERT_EQ_STR(plan.files.data[i].path, paths[i]);
        buffers[i] = rename_find_path(&f, paths[i]);
        YEW_ASSERT_NOT_NULL(buffers[i]);
        undo_before[i] = yew_undo_current(buffers[i]->undo);
    }

    YEW_ASSERT(yew_lsp_rename_apply(&f.ed, &plan, err));
    for (i = 0U; i < 6U; i++) {
        YEW_ASSERT_EQ_U64(yew_undo_current(buffers[i]->undo),
                          undo_before[i] + 1U);
        YEW_ASSERT_EQ_U64(
            buffers[i]->undo->nodes.data[buffers[i]->undo->cur - 1U].reason,
            YEW_TXN_LSP);
        YEW_ASSERT(yew_buf_dirty(buffers[i]));
    }
    rename_assert_fixture_disk(&f);

    for (i = 0U; i < 6U; i++) {
        EditCtx ec = yew_ed_edit_ctx_buffer(&f.ed, buffers[i]);

        YEW_ASSERT(yew_undo(&ec));
        yew_ed_finish_edit(&f.ed, &ec);
        YEW_ASSERT_EQ_U64(yew_undo_current(buffers[i]->undo),
                          undo_before[i]);
        YEW_ASSERT(!yew_buf_dirty(buffers[i]));
        rename_assert_text(buffers[i]->tb, original[i], original_len[i]);
    }
    rename_assert_fixture_disk(&f);

    bytebuf_free(&json);
    yew_lsp_rename_plan_free(&plan);
    rename_fix_free(&f);
}

void test_lsp_rename_apply_failure_rolls_back_committed_buffers(void)
{
    RenameFix f;
    RenamePlan plan;
    Buffer *buffers[3];
    u32 undo_before[3];
    char json[3072];
    char want[256];
    char err[YEW_RENAME_ERROR_MAX] = {0};
    size_t i;

    rename_fix_init(&f);
    YEW_ASSERT(snprintf(json, sizeof(json),
        "{\"changes\":{\"file://%s\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},"
        "\"newText\":\"omega\"}],"
        "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
        "\"newText\":\"omega\"}],"
        "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
        "\"newText\":\"omega\"}]}}", f.c, f.a, f.b) > 0);
    yew_lsp_rename_plan_init(&plan);
    YEW_ASSERT(yew_lsp_rename_preflight(&f.ed, rename_json(&f, json),
                                         YEW_POSENC_UTF8, "alpha", "omega",
                                         &plan, err));
    YEW_ASSERT_EQ_U64(plan.files.len, 3U);
    YEW_ASSERT_EQ_STR(plan.files.data[0].path, f.a);
    YEW_ASSERT_EQ_STR(plan.files.data[1].path, f.b);
    YEW_ASSERT_EQ_STR(plan.files.data[2].path, f.c);
    for (i = 0U; i < 3U; i++) {
        buffers[i] = rename_find_path(&f, plan.files.data[i].path);
        YEW_ASSERT_NOT_NULL(buffers[i]);
        undo_before[i] = yew_undo_current(buffers[i]->undo);
    }

    yew_lsp_rename_plan_test_fail_at(&plan, 2U, 0U);
    YEW_ASSERT(!yew_lsp_rename_apply(&f.ed, &plan, err));
    YEW_ASSERT(snprintf(want, sizeof(want),
        "rename aborted: injected edit failure in %s; 2 files rolled back",
        f.c) > 0);
    YEW_ASSERT_EQ_STR(err, want);
    rename_assert_sources_unchanged(&f);
    for (i = 0U; i < 3U; i++) {
        EditCtx ec = yew_ed_edit_ctx_buffer(&f.ed, buffers[i]);

        YEW_ASSERT_EQ_U64(yew_undo_current(buffers[i]->undo),
                          undo_before[i]);
        YEW_ASSERT(!yew_buf_dirty(buffers[i]));
        YEW_ASSERT(!yew_redo(&ec));
    }

    yew_lsp_rename_plan_free(&plan);
    rename_fix_free(&f);

    {
        static const u8 user_text[] =
            "alpha alpha\nA\xF0\x9F\x8C\xB2" "B\n!";
        RenameFix noop;
        RenamePlan noop_plan;
        Buffer *a;
        Buffer *b;
        Buffer *c;
        Buffer *d;
        EditCtx ec;
        u32 user_node;
        char noop_json[3072];
        char noop_err[YEW_RENAME_ERROR_MAX] = {0};

        rename_fix_init(&noop);
        a = rename_find_path(&noop, noop.a);
        YEW_ASSERT_NOT_NULL(a);
        ec = yew_ed_edit_ctx_buffer(&noop.ed, a);
        yew_undo_begin(&ec, YEW_TXN_PASTE);
        YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(yew_textbuf_len(a->tb)),
                                   (const u8 *)"!", 1U));
        yew_undo_end(&ec);
        yew_ed_finish_edit(&noop.ed, &ec);
        user_node = yew_undo_current(a->undo);
        YEW_ASSERT(user_node != 0U);
        rename_assert_text(a->tb, user_text, sizeof(user_text) - 1U);

        YEW_ASSERT(snprintf(noop_json, sizeof(noop_json),
            "{\"changes\":{\"file://%s\":[],"
            "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
            "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
            "\"newText\":\"alpha\"}],"
            "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
            "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
            "\"newText\":\"omega\"}],"
            "\"file://%s\":[{\"range\":{\"start\":{\"line\":0,"
            "\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
            "\"newText\":\"omega\"}]}}",
            noop.a, noop.b, noop.c, noop.d) > 0);
        yew_lsp_rename_plan_init(&noop_plan);
        YEW_ASSERT(yew_lsp_rename_preflight(
            &noop.ed, rename_json(&noop, noop_json), YEW_POSENC_UTF8,
            "alpha", "omega", &noop_plan, noop_err));
        YEW_ASSERT_EQ_U64(noop_plan.files.len, 2U);
        YEW_ASSERT_EQ_U64(noop_plan.nedits, 2U);
        YEW_ASSERT_EQ_STR(noop_plan.files.data[0].path, noop.c);
        YEW_ASSERT_EQ_STR(noop_plan.files.data[1].path, noop.d);
        b = rename_find_path(&noop, noop.b);
        c = rename_find_path(&noop, noop.c);
        d = rename_find_path(&noop, noop.d);
        YEW_ASSERT_NOT_NULL(b);
        YEW_ASSERT_NOT_NULL(c);
        YEW_ASSERT_NOT_NULL(d);

        yew_lsp_rename_plan_test_fail_at(&noop_plan, 1U, 0U);
        YEW_ASSERT(!yew_lsp_rename_apply(&noop.ed, &noop_plan, noop_err));
        YEW_ASSERT_EQ_U64(yew_undo_current(a->undo), user_node);
        rename_assert_text(a->tb, user_text, sizeof(user_text) - 1U);
        rename_assert_text(b->tb, rename_b_text, sizeof(rename_b_text) - 1U);
        rename_assert_text(c->tb, rename_c_text, sizeof(rename_c_text) - 1U);
        rename_assert_text(d->tb, rename_d_text, sizeof(rename_d_text) - 1U);
        ec = yew_ed_edit_ctx_buffer(&noop.ed, a);
        YEW_ASSERT(yew_undo(&ec));
        yew_ed_finish_edit(&noop.ed, &ec);
        rename_assert_text(a->tb, rename_a_text,
                           sizeof(rename_a_text) - 1U);

        yew_lsp_rename_plan_free(&noop_plan);
        rename_fix_free(&noop);
    }
}
