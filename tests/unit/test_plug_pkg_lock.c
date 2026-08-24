#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/diag.h"
#include "fl/value.h"
#include "mod/plug/pkg.h"
#include "util/arena.h"
#include "util/buf.h"

typedef struct LockFix {
    char root[256];
    char yew[320];
    char path[384];
    char *old_data;
    Arena a;
    DiagCtx dc;
    u32 ndiag;
    char rendered[1024];
} LockFix;

static void lock_diag(void *ctx, FlDiagLevel level, FlSpan sp,
                      const char *msg, const char *rendered)
{
    LockFix *f = ctx;

    (void)level;
    (void)sp;
    (void)msg;
    f->ndiag++;
    (void)snprintf(f->rendered, sizeof(f->rendered), "%s", rendered);
}

static char *lock_env_copy(const char *name)
{
    const char *value = getenv(name);
    char *copy;

    if (value == NULL)
        return NULL;
    copy = malloc(strlen(value) + 1U);
    YEW_ASSERT_NOT_NULL(copy);
    (void)memcpy(copy, value, strlen(value) + 1U);
    return copy;
}

static void lock_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size_t len = strlen(text);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, len), (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static char *lock_read(const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char *text;

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(fstat(fd, &st), 0);
    text = malloc((size_t)st.st_size + 1U);
    YEW_ASSERT_NOT_NULL(text);
    YEW_ASSERT_EQ_I64(read(fd, text, (size_t)st.st_size), st.st_size);
    text[st.st_size] = '\0';
    YEW_ASSERT_EQ_I64(close(fd), 0);
    return text;
}

static void lock_fix_init(LockFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-pkg-lock-XXXXXX",
                 sizeof("/tmp/yew-pkg-lock-XXXXXX"));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)snprintf(f->yew, sizeof(f->yew), "%s/yew", f->root);
    (void)snprintf(f->path, sizeof(f->path), "%s/plugins.lock", f->yew);
    YEW_ASSERT_EQ_I64(mkdir(f->yew, 0700), 0);
    f->old_data = lock_env_copy("XDG_DATA_HOME");
    YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", f->root, 1), 0);
    arena_init(&f->a);
    fl_diag_init(&f->dc, &f->a);
    fl_diag_set_sink(&f->dc, lock_diag, f);
}

static void lock_fix_done(LockFix *f)
{
    (void)unlink(f->path);
    YEW_ASSERT_EQ_I64(rmdir(f->yew), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    if (f->old_data != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", f->old_data, 1), 0);
        free(f->old_data);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_DATA_HOME"), 0);
    }
    arena_free_all(&f->a);
}

static const char LOCK_SOURCE[] =
    "{\n"
    "  schema: 1,\n"
    "  plugins: {\n"
    "    zed: { url: \"file:///z\", shorthand: \"./z\", "
    "rev: \"0123456789abcdef0123456789abcdef01234567\", pin: \"head\", "
    "tree: \"0123456789abcdef\", installed_at: 1, updated_at: 2, "
    "future_entry: [1, 2,], },\n"
    "    alpha: { url: \"file:///a\", shorthand: \"./a\", "
    "rev: \"fedcba9876543210fedcba9876543210fedcba98\", "
    "pin: \"tag:v1\", tree: \"fedcba9876543210\", installed_at: 3, "
    "updated_at: 4, },\n"
    "  },\n"
    "  future_top: { x: true, },\n"
    "}\n";

void test_plug_pkg_lock_absent_is_empty_and_corrupt_is_not_overwritten(void)
{
    LockFix f;
    PkgLock lock = {0};
    char *before;
    char *after;

    lock_fix_init(&f);
    YEW_ASSERT(yew_pkg_lock_load(&lock, &f.dc));
    YEW_ASSERT_EQ_U64(lock.schema, 1U);
    YEW_ASSERT_EQ_U64(lock.v.len, 0U);
    YEW_ASSERT(!lock.corrupt);
    yew_pkg_lock_free(&lock);
    lock_write(f.path, "{ schema: 1, plugins: [ }\n");
    before = lock_read(f.path);
    YEW_ASSERT(!yew_pkg_lock_load(&lock, &f.dc));
    YEW_ASSERT(lock.corrupt);
    YEW_ASSERT(!yew_pkg_lock_save(&lock, &f.dc));
    after = lock_read(f.path);
    YEW_ASSERT_EQ_STR(before, after);
    free(after);
    free(before);
    yew_pkg_lock_free(&lock);
    lock_fix_done(&f);
}

void test_plug_pkg_lock_round_trip_sorts_and_preserves_unknown_keys(void)
{
    LockFix f;
    PkgLock lock = {0};
    PkgLock reread = {0};
    PkgEntry *zed;
    PkgEntry *alpha;
    char *text;
    char *pa;
    char *pz;

    lock_fix_init(&f);
    lock_write(f.path, LOCK_SOURCE);
    YEW_ASSERT(yew_pkg_lock_load(&lock, &f.dc));
    YEW_ASSERT_EQ_U64(lock.v.len, 2U);
    zed = yew_pkg_lock_find(&lock, "zed", 3U);
    alpha = yew_pkg_lock_find(&lock, "alpha", 5U);
    YEW_ASSERT_NOT_NULL(zed);
    YEW_ASSERT_NOT_NULL(alpha);
    YEW_ASSERT_EQ_STR(zed->url, "file:///z");
    YEW_ASSERT_EQ_STR(zed->shorthand, "./z");
    YEW_ASSERT_EQ_STR(zed->pin, "head");
    YEW_ASSERT_EQ_STR(zed->rev,
                      "0123456789abcdef0123456789abcdef01234567");
    YEW_ASSERT_EQ_STR(zed->tree, "0123456789abcdef");
    YEW_ASSERT_EQ_I64(zed->installed_at, 1);
    YEW_ASSERT_EQ_I64(zed->updated_at, 2);
    YEW_ASSERT_EQ_STR(alpha->pin, "tag:v1");
    YEW_ASSERT(yew_pkg_lock_save(&lock, &f.dc));
    text = lock_read(f.path);
    pa = strstr(text, "alpha:");
    pz = strstr(text, "zed:");
    YEW_ASSERT_NOT_NULL(pa);
    YEW_ASSERT_NOT_NULL(pz);
    YEW_ASSERT(pa < pz);
    YEW_ASSERT_NOT_NULL(strstr(text, "future_top"));
    YEW_ASSERT_NOT_NULL(strstr(text, "future_entry"));
    YEW_ASSERT_NOT_NULL(strstr(text, "url:"));
    YEW_ASSERT(strstr(text, "url:") < strstr(text, "shorthand:"));
    YEW_ASSERT(strstr(text, "shorthand:") < strstr(text, "rev:"));
    YEW_ASSERT(strstr(text, "rev:") < strstr(text, "pin:"));
    YEW_ASSERT(strstr(text, "pin:") < strstr(text, "tree:"));
    free(text);
    YEW_ASSERT(yew_pkg_lock_load(&reread, &f.dc));
    YEW_ASSERT_EQ_U64(reread.v.len, 2U);
    YEW_ASSERT_NOT_NULL(yew_pkg_lock_find(&reread, "alpha", 5U));
    YEW_ASSERT_NOT_NULL(yew_pkg_lock_find(&reread, "zed", 3U));
    YEW_ASSERT_NULL(yew_pkg_lock_find(&reread, "missing", 7U));
    YEW_ASSERT_EQ_U64(fl_diag_errors(&f.dc), 0U);
    yew_pkg_lock_free(&reread);
    yew_pkg_lock_free(&lock);
    lock_fix_done(&f);
}

void test_plug_pkg_lock_diag_source_outlives_lock(void)
{
    static const char expected[] =
        "<data>:2:3: note: delayed package diagnostic\n"
        "  schema: 1,\n"
        "  ^~~~~~\n";
    LockFix f;
    PkgLock lock = {0};

    lock_fix_init(&f);
    lock_write(f.path, LOCK_SOURCE);
    YEW_ASSERT(yew_pkg_lock_load(&lock, &f.dc));
    yew_pkg_lock_free(&lock);

    /* The CLI reuses this context after lock parsing for transaction I/O
     * diagnostics.  Rendering must not consult the released file Bytebuf or
     * the released PkgLock arena. */
    fl_diag_emit(&f.dc, FL_DIAG_NOTE, (FlSpan){0U, 2U, 3U, 6U},
                 "delayed package diagnostic");
    YEW_ASSERT_EQ_U64(f.ndiag, 1U);
    YEW_ASSERT_EQ_STR(f.rendered, expected);
    lock_fix_done(&f);
}

void test_plug_pkg_lock_twenty_entry_golden_order(void)
{
    LockFix f;
    PkgLock lock = {0};
    Bytebuf want;
    FILE *stream;
    char *text;
    int fd;
    int i;

    lock_fix_init(&f);
    fd = open(f.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    YEW_ASSERT(fd >= 0);
    stream = fdopen(fd, "w");
    YEW_ASSERT_NOT_NULL(stream);
    YEW_ASSERT(fputs("{ schema: 1, plugins: {\n", stream) >= 0);
    for (i = 19; i >= 0; i--)
        YEW_ASSERT(fprintf(stream,
                          "p%02d: { url: \"file:///p%02d\", shorthand: \"\", "
                          "rev: \"0123456789abcdef0123456789abcdef01234567\", "
                          "pin: \"head\", tree: \"0123456789abcdef\", "
                          "installed_at: %d, updated_at: %d, },\n",
                          i, i, i, i) > 0);
    YEW_ASSERT(fputs("}, }\n", stream) >= 0);
    YEW_ASSERT_EQ_I64(fclose(stream), 0);
    YEW_ASSERT(yew_pkg_lock_load(&lock, &f.dc));
    YEW_ASSERT_EQ_U64(lock.v.len, 20U);
    YEW_ASSERT(yew_pkg_lock_save(&lock, &f.dc));
    text = lock_read(f.path);
    bytebuf_init(&want);
    bytebuf_append(&want,
                   "# yew plugin lockfile.  Generated by `yew pkg`; "
                   "hand-editable, but\n"
                   "# `yew pkg doctor` is the only thing that will agree "
                   "with your edits.\n"
                   "{\n"
                   "  schema: 1,\n"
                   "  plugins: {\n",
                   sizeof("# yew plugin lockfile.  Generated by `yew pkg`; "
                          "hand-editable, but\n"
                          "# `yew pkg doctor` is the only thing that will "
                          "agree with your edits.\n"
                          "{\n"
                          "  schema: 1,\n"
                          "  plugins: {\n") - 1U);
    for (i = 0; i < 20; i++) {
        bytebuf_printf(&want,
                       "    p%02d: {\n"
                       "      url: \"file:///p%02d\",\n"
                       "      shorthand: \"\",\n"
                       "      rev: "
                       "\"0123456789abcdef0123456789abcdef01234567\",\n"
                       "      pin: \"head\",\n"
                       "      tree: \"0123456789abcdef\",\n"
                       "      installed_at: %d,\n"
                       "      updated_at: %d,\n"
                       "    },\n",
                       i, i, i, i);
    }
    bytebuf_append(&want, "  },\n}\n", sizeof("  },\n}\n") - 1U);
    YEW_ASSERT_EQ_U64(strlen(text), want.len);
    YEW_ASSERT_EQ_MEM(text, want.data, want.len);
    bytebuf_free(&want);
    free(text);
    yew_pkg_lock_free(&lock);
    lock_fix_done(&f);
}

void test_plug_pkg_lock_rejects_embedded_nul_without_rewrite(void)
{
    static const char source[] =
        "{ schema: 1, plugins: { bad: { "
        "url: \"file:///ok\\0hidden\", shorthand: \"\", "
        "rev: \"0123456789abcdef0123456789abcdef01234567\", "
        "pin: \"head\", tree: \"0123456789abcdef\", "
        "installed_at: 1, updated_at: 1, }, }, }\n";
    static const char control_source[] =
        "{ schema: 1, plugins: { bad: { "
        "url: \"file:///ok\\x1b]8;;bad\", shorthand: \"\", "
        "rev: \"0123456789abcdef0123456789abcdef01234567\", "
        "pin: \"head\", tree: \"0123456789abcdef\", "
        "installed_at: 1, updated_at: 1, }, }, }\n";
    LockFix f;
    PkgLock lock = {0};
    char *after;

    lock_fix_init(&f);
    lock_write(f.path, source);
    YEW_ASSERT(!yew_pkg_lock_load(&lock, &f.dc));
    YEW_ASSERT(lock.corrupt);
    YEW_ASSERT(!yew_pkg_lock_save(&lock, &f.dc));
    after = lock_read(f.path);
    YEW_ASSERT_EQ_STR(after, source);
    free(after);
    yew_pkg_lock_free(&lock);
    lock_write(f.path, control_source);
    YEW_ASSERT(!yew_pkg_lock_load(&lock, &f.dc));
    YEW_ASSERT(lock.corrupt);
    YEW_ASSERT(!yew_pkg_lock_save(&lock, &f.dc));
    after = lock_read(f.path);
    YEW_ASSERT_EQ_STR(after, control_source);
    free(after);
    yew_pkg_lock_free(&lock);
    lock_fix_done(&f);
}
