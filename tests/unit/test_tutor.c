#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "tutor.h"
#include "util/buf.h"

typedef struct TutorFix {
    char base[PATH_MAX];
    char *saved_state;
    bool had_state;
    char *root;
} TutorFix;

static int tutor_rm_one(const char *path, const struct stat *st, int flag,
                        struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return flag == FTW_DP ? rmdir(path) : unlink(path);
}

static void tutor_fix_make(TutorFix *fix)
{
    const char *tmp = getenv("TMPDIR");
    const char *old = getenv("XDG_STATE_HOME");
    int n;

    (void)memset(fix, 0, sizeof(*fix));
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(fix->base, sizeof(fix->base), "%s/yew-tutor-XXXXXX", tmp);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(fix->base));
    YEW_ASSERT_NOT_NULL(mkdtemp(fix->base));
    fix->had_state = old != NULL;
    fix->saved_state = old == NULL ? NULL : strdup(old);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fix->base, 1), 0);
    fix->root = yew_tutor_root(true);
    YEW_ASSERT_NOT_NULL(fix->root);
}

static void tutor_fix_free(TutorFix *fix)
{
    yew_xfree(fix->root);
    if (fix->had_state)
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fix->saved_state, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    free(fix->saved_state);
    (void)nftw(fix->base, tutor_rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

static void tutor_path(const TutorFix *fix, const char *name,
                       char out[PATH_MAX])
{
    int n = snprintf(out, PATH_MAX, "%s/%s", fix->root, name);

    YEW_ASSERT(n > 0 && n < PATH_MAX);
}

static void tutor_write(const char *path, const char *text)
{
    size_t len = strlen(text);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, len), (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void tutor_read(const char *path, Bytebuf *out)
{
    int fd = open(path, O_RDONLY);

    YEW_ASSERT(fd >= 0);
    bytebuf_init(out);
    for (;;) {
        u8 bytes[512];
        ssize_t n = read(fd, bytes, sizeof(bytes));

        YEW_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(out, bytes, (size_t)n);
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

void test_tutor_chapter_names_and_numbers_are_exact(void)
{
    static const char *const names[] = {
        "arrows", "units", "execute", "search", "workspace", "fletch",
        "fuss"
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char number[2] = {(char)('1' + i), '\0'};
        u8 chapter = 99U;

        YEW_ASSERT(yew_tutor_chapter_parse(number, &chapter));
        YEW_ASSERT_EQ_U64(chapter, i);
        chapter = 99U;
        YEW_ASSERT(yew_tutor_chapter_parse(names[i], &chapter));
        YEW_ASSERT_EQ_U64(chapter, i);
    }
    {
        u8 chapter = 99U;

        YEW_ASSERT(!yew_tutor_chapter_parse("0", &chapter));
        YEW_ASSERT(!yew_tutor_chapter_parse("8", &chapter));
        YEW_ASSERT(!yew_tutor_chapter_parse("Arrows", &chapter));
        YEW_ASSERT(!yew_tutor_chapter_parse("", &chapter));
        YEW_ASSERT(!yew_tutor_chapter_parse(NULL, &chapter));
    }
}

void test_tutor_seed_preserves_existing_work_and_reset_is_confined(void)
{
    TutorFix fix;
    char chapter[PATH_MAX];
    char marker[PATH_MAX];
    Bytebuf bytes;
    struct stat st;
    u32 i;

    tutor_fix_make(&fix);
    YEW_ASSERT(yew_tutor_seed(fix.root));
    for (i = 1U; i <= (u32)YEW_TUTOR_CHAPTERS; i++) {
        char name[16];

        (void)snprintf(name, sizeof(name), "ch%02u.txt", (unsigned)i);
        tutor_path(&fix, name, chapter);
        YEW_ASSERT_EQ_I64(stat(chapter, &st), 0);
        YEW_ASSERT(S_ISREG(st.st_mode));
    }
    tutor_path(&fix, "ch01.txt", chapter);
    tutor_write(chapter, "my unsaved lesson\n");
    YEW_ASSERT(yew_tutor_seed(fix.root));
    tutor_read(chapter, &bytes);
    YEW_ASSERT_EQ_U64(bytes.len, sizeof("my unsaved lesson\n") - 1U);
    YEW_ASSERT_EQ_MEM(bytes.data, "my unsaved lesson\n", bytes.len);
    bytebuf_free(&bytes);

    (void)snprintf(marker, sizeof(marker), "%s/keep", fix.base);
    tutor_write(marker, "outside\n");
    YEW_ASSERT(!yew_tutor_reset(fix.base));
    YEW_ASSERT(yew_tutor_reset(fix.root));
    YEW_ASSERT_EQ_I64(stat(marker, &st), 0);
    YEW_ASSERT_EQ_I64(stat(fix.root, &st), 0);
    YEW_ASSERT(S_ISDIR(st.st_mode));
    YEW_ASSERT_EQ_I64(rmdir(fix.root), 0);
    YEW_ASSERT_EQ_I64(symlink(fix.base, fix.root), 0);
    YEW_ASSERT(!yew_tutor_reset(fix.root));
    YEW_ASSERT(!yew_tutor_seed(fix.root));
    YEW_ASSERT_EQ_I64(stat(marker, &st), 0);
    tutor_fix_free(&fix);
}

void test_tutor_progress_roundtrips_canonically_and_rejects_corruption(void)
{
    TutorFix fix;
    YewTutorProgress want = {0};
    YewTutorProgress got;
    char path[PATH_MAX];
    Bytebuf first;
    Bytebuf second;
    bool missing;

    tutor_fix_make(&fix);
    want.done[0] = 0x7fU;
    want.done[1] = 0x05U;
    want.skipped[6] = true;
    YEW_ASSERT(yew_tutor_progress_save(fix.root, &want));
    YEW_ASSERT(yew_tutor_progress_load(fix.root, &got, &missing));
    YEW_ASSERT(!missing);
    YEW_ASSERT_EQ_MEM(&got, &want, sizeof(want));

    tutor_path(&fix, "tutor.fl", path);
    tutor_read(path, &first);
    YEW_ASSERT(yew_tutor_progress_save(fix.root, &got));
    tutor_read(path, &second);
    YEW_ASSERT_EQ_U64(first.len, second.len);
    YEW_ASSERT_EQ_MEM(first.data, second.data, first.len);
    bytebuf_free(&first);
    bytebuf_free(&second);

    got = want;
    got.done[6] = 1U;
    YEW_ASSERT(!yew_tutor_progress_save(fix.root, &got));
    got = want;
    got.done[2] = 0x80U;
    YEW_ASSERT(!yew_tutor_progress_save(fix.root, &got));

    tutor_write(path, "{schema: 1, chapters: nope}\n");
    YEW_ASSERT(!yew_tutor_progress_load(fix.root, &got, &missing));
    YEW_ASSERT(!missing);
    tutor_fix_free(&fix);
}
