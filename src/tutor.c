#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "tutor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/data.h"
#include "fl/diag.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "text/file.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/runtime_asset.h"
#include "util/xdg.h"

enum { YEW_TUTOR_FILE_MAX = 1024U * 1024U };

static const char tutor_version[] = "1.0.0";

static const char *const chapter_names[YEW_TUTOR_CHAPTERS] = {
    "arrows", "units", "execute", "search", "workspace", "fletch",
    "fuss"
};

static const char *const chapter_titles[YEW_TUTOR_CHAPTERS] = {
    "Arrows and the mode model",
    "Units, selections, and multiple cursors",
    "E mode and the shell",
    "Search, replace, and jumps",
    "Panes, tabs, groups, and resume",
    "Recordings and Fletch",
    "FUSS mode and git"
};

typedef struct TutorDoc {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} TutorDoc;

static bool path_join(char out[PATH_MAX], const char *left,
                      const char *right)
{
    int n;

    if (out == NULL || left == NULL || right == NULL)
        return false;
    n = snprintf(out, PATH_MAX, "%s/%s", left, right);
    return n >= 0 && n < PATH_MAX;
}

static void doc_init(TutorDoc *doc)
{
    arena_init(&doc->arena);
    interner_init(&doc->in, &doc->arena);
    fl_diag_init(&doc->dc, &doc->arena);
    (void)fl_vm_init(&doc->vm, &doc->arena, &doc->in, &doc->dc);
}

static void doc_free(TutorDoc *doc)
{
    fl_vm_free(&doc->vm);
    interner_free(&doc->in);
    arena_free_all(&doc->arena);
}

static bool key_eq(FlValue key, const char *want)
{
    const FlStr *s;
    size_t n;

    if (key.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)key.as.o;
    n = strlen(want);
    return s->len == n && memcmp(s->b, want, n) == 0;
}

static bool map_get(const FlMap *map, const char *name, FlValue *out)
{
    FlValue key;
    FlValue value;
    u32 cursor = 0U;

    while (fl_map_iter(map, &cursor, &key, &value)) {
        if (key_eq(key, name)) {
            if (out != NULL)
                *out = value;
            return true;
        }
    }
    return false;
}

static void map_set(TutorDoc *doc, FlMap *map, const char *key,
                    FlValue value)
{
    FlStr *name = fl_str_new(&doc->vm, key, (u32)strlen(key));

    (void)fl_map_set(&doc->vm, map, FL_OBJ_V(FL_STR, name), value);
}

static bool read_file(const char *path, Bytebuf *out, bool *missing)
{
    int fd;

    bytebuf_init(out);
    *missing = false;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *missing = errno == ENOENT;
        return false;
    }
    for (;;) {
        u8 chunk[8192];
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n > 0) {
            if (out->len > (size_t)YEW_TUTOR_FILE_MAX - (size_t)n) {
                (void)close(fd);
                bytebuf_free(out);
                errno = EFBIG;
                return false;
            }
            bytebuf_append(out, chunk, (size_t)n);
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            (void)close(fd);
            bytebuf_free(out);
            return false;
        }
    }
    if (close(fd) != 0) {
        bytebuf_free(out);
        return false;
    }
    return true;
}

static bool str_eq(FlValue value, const char *want)
{
    const FlStr *s;
    size_t n;

    if (value.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)value.as.o;
    n = strlen(want);
    return s->len == n && memcmp(s->b, want, n) == 0;
}

static bool task_number(u32 chapter, FlValue value, u32 *task)
{
    char want[24];
    u32 i;

    if (value.t != (u8)FL_STR)
        return false;
    for (i = 0U; i < (u32)YEW_TUTOR_TASKS_PER_CHAPTER; i++) {
        int n = snprintf(want, sizeof(want), "ch%02u.t%u",
                         (unsigned)(chapter + 1U), (unsigned)(i + 1U));

        if (n > 0 && (size_t)n < sizeof(want) && str_eq(value, want)) {
            *task = i;
            return true;
        }
    }
    return false;
}

static bool progress_decode(FlValue root, YewTutorProgress *out)
{
    const FlMap *root_map;
    const FlMap *chapters;
    FlValue value;
    u32 chapter;

    *out = (YewTutorProgress){0};
    if (root.t != (u8)FL_MAP)
        return false;
    root_map = (const FlMap *)root.as.o;
    if (!map_get(root_map, "schema", &value) || value.t != (u8)FL_INT ||
        value.as.i != 1 || !map_get(root_map, "version", &value) ||
        !str_eq(value, tutor_version) ||
        !map_get(root_map, "chapters", &value) || value.t != (u8)FL_MAP)
        return false;
    chapters = (const FlMap *)value.as.o;
    for (chapter = 0U; chapter < (u32)YEW_TUTOR_CHAPTERS; chapter++) {
        const FlMap *entry;
        const FlList *done;
        bool complete;
        bool skipped = false;
        u32 i;

        if (!map_get(chapters, chapter_names[chapter], &value))
            continue;
        if (value.t != (u8)FL_MAP)
            return false;
        entry = (const FlMap *)value.as.o;
        if (!map_get(entry, "done", &value) || value.t != (u8)FL_LIST)
            return false;
        done = (const FlList *)value.as.o;
        for (i = 0U; i < done->n; i++) {
            u32 task;
            u8 bit;

            if (!task_number(chapter, done->v[i], &task))
                return false;
            bit = (u8)(1U << task);
            if ((out->done[chapter] & bit) != 0U)
                return false;
            out->done[chapter] |= bit;
        }
        if (!map_get(entry, "complete", &value) ||
            value.t != (u8)FL_BOOL)
            return false;
        complete = value.as.b;
        if (map_get(entry, "skipped", &value)) {
            if (value.t != (u8)FL_BOOL)
                return false;
            skipped = value.as.b;
        }
        if (skipped && chapter + 1U != (u32)YEW_TUTOR_CHAPTERS)
            return false;
        if (complete !=
            (out->done[chapter] ==
             (u8)((1U << YEW_TUTOR_TASKS_PER_CHAPTER) - 1U)) ||
            (skipped && complete))
            return false;
        out->skipped[chapter] = skipped;
    }
    return true;
}

static FlValue progress_encode(TutorDoc *doc,
                               const YewTutorProgress *progress)
{
    FlMap *root = fl_map_new(&doc->vm);
    FlMap *chapters = fl_map_new(&doc->vm);
    u32 chapter;

    map_set(doc, root, "schema", FL_INT_V(1));
    map_set(doc, root, "version",
            FL_OBJ_V(FL_STR, fl_str_new(&doc->vm, tutor_version,
                                        (u32)(sizeof(tutor_version) - 1U))));
    for (chapter = 0U; chapter < (u32)YEW_TUTOR_CHAPTERS; chapter++) {
        FlMap *entry = fl_map_new(&doc->vm);
        FlList *done = fl_list_new(&doc->vm);
        u32 task;

        for (task = 0U; task < (u32)YEW_TUTOR_TASKS_PER_CHAPTER; task++) {
            char id[24];
            int n;

            if ((progress->done[chapter] & (u8)(1U << task)) == 0U)
                continue;
            n = snprintf(id, sizeof(id), "ch%02u.t%u",
                         (unsigned)(chapter + 1U), (unsigned)(task + 1U));
            if (n > 0 && (size_t)n < sizeof(id))
                (void)fl_list_push(
                    &doc->vm, done,
                    FL_OBJ_V(FL_STR,
                             fl_str_new(&doc->vm, id, (u32)n)));
        }
        map_set(doc, entry, "done", FL_OBJ_V(FL_LIST, done));
        map_set(doc, entry, "complete",
                FL_BOOL_V(progress->done[chapter] ==
                          (u8)((1U << YEW_TUTOR_TASKS_PER_CHAPTER) - 1U)));
        if (chapter + 1U == (u32)YEW_TUTOR_CHAPTERS)
            map_set(doc, entry, "skipped",
                    FL_BOOL_V(progress->skipped[chapter]));
        map_set(doc, chapters, chapter_names[chapter],
                FL_OBJ_V(FL_MAP, entry));
    }
    map_set(doc, root, "chapters", FL_OBJ_V(FL_MAP, chapters));
    return FL_OBJ_V(FL_MAP, root);
}

bool yew_tutor_chapter_parse(const char *text, u8 *chapter)
{
    u32 i;

    if (text == NULL || chapter == NULL)
        return false;
    for (i = 0U; i < (u32)YEW_TUTOR_CHAPTERS; i++) {
        char number[2] = {(char)('1' + i), '\0'};

        if (strcmp(text, number) == 0 || strcmp(text, chapter_names[i]) == 0) {
            *chapter = (u8)i;
            return true;
        }
    }
    return false;
}

char *yew_tutor_root(bool ensure)
{
    char *state = yew_xdg_state_dir();
    char *root;
    size_t n;

    if (state == NULL)
        return NULL;
    n = strlen(state) + sizeof("/tutor");
    root = yew_xmalloc(n);
    (void)snprintf(root, n, "%s/tutor", state);
    yew_xfree(state);
    if (ensure && !yew_mkdirs(root, 0700U)) {
        yew_xfree(root);
        return NULL;
    }
    return root;
}

static bool asset_read(const char *relative, Bytebuf *out)
{
    const char *runtime = NULL;
    char *path;
    bool missing;
    bool ok;

    if (yew_runtime_root(&runtime) == YEW_RUNTIME_ROOT_EMBEDDED) {
        bytebuf_init(out);
        return yew_runtime_asset_read(relative, out);
    }
    path = yew_runtime_file(relative);
    if (path == NULL)
        return false;
    ok = read_file(path, out, &missing);
    yew_xfree(path);
    return ok;
}

static bool write_new_file(const char *path, const u8 *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    size_t at = 0U;

    if (fd < 0)
        return errno == EEXIST;
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        if (n > 0)
            at += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else {
            (void)close(fd);
            (void)unlink(path);
            return false;
        }
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        (void)unlink(path);
        return false;
    }
    return true;
}

bool yew_tutor_seed(const char *root)
{
    struct stat st;
    u32 chapter;

    if (root == NULL || root[0] != '/')
        return false;
    if (lstat(root, &st) == 0) {
        if (!S_ISDIR(st.st_mode))
            return false;
    } else if (errno != ENOENT || !yew_mkdirs(root, 0700U)) {
        return false;
    }
    for (chapter = 0U; chapter < (u32)YEW_TUTOR_CHAPTERS; chapter++) {
        char relative[64];
        char target[PATH_MAX];
        Bytebuf bytes;
        int n;

        n = snprintf(relative, sizeof(relative), "tutor/text/ch%02u.txt",
                     (unsigned)(chapter + 1U));
        if (n <= 0 || (size_t)n >= sizeof(relative) ||
            !path_join(target, root, relative + sizeof("tutor/text/") - 1U) ||
            !asset_read(relative, &bytes))
            return false;
        if (!write_new_file(target, bytes.data, bytes.len)) {
            bytebuf_free(&bytes);
            return false;
        }
        bytebuf_free(&bytes);
    }
    return true;
}

static bool remove_tree(const char *path, u32 depth)
{
    DIR *dir;
    struct dirent *entry;
    bool ok = true;

    if (depth > 8U)
        return false;
    dir = opendir(path);
    if (dir == NULL)
        return errno == ENOENT;
    while ((entry = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (!path_join(child, path, entry->d_name) || lstat(child, &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode))
            ok = remove_tree(child, depth + 1U) && ok;
        else
            ok = (unlink(child) == 0) && ok;
    }
    if (closedir(dir) != 0)
        ok = false;
    return (rmdir(path) == 0 || errno == ENOENT) && ok;
}

bool yew_tutor_reset(const char *root)
{
    char *expected = yew_tutor_root(false);
    struct stat st;
    bool ok;

    if (root == NULL || expected == NULL || strcmp(root, expected) != 0) {
        yew_xfree(expected);
        return false;
    }
    yew_xfree(expected);
    if (lstat(root, &st) != 0)
        return errno == ENOENT && yew_mkdirs(root, 0700U);
    if (!S_ISDIR(st.st_mode))
        return false;
    ok = remove_tree(root, 0U);
    return ok && yew_mkdirs(root, 0700U);
}

bool yew_tutor_progress_load(const char *root, YewTutorProgress *out,
                             bool *missing)
{
    char path[PATH_MAX];
    Bytebuf bytes;
    TutorDoc doc;
    FlValue value;
    bool absent;
    bool ok;

    if (out == NULL || missing == NULL ||
        !path_join(path, root, "tutor.fl"))
        return false;
    *out = (YewTutorProgress){0};
    if (!read_file(path, &bytes, &absent)) {
        *missing = absent;
        return absent;
    }
    *missing = false;
    doc_init(&doc);
    value = fl_data_read(&doc.vm, (const char *)bytes.data, bytes.len,
                         &doc.dc);
    ok = fl_diag_errors(&doc.dc) == 0U && progress_decode(value, out);
    doc_free(&doc);
    bytebuf_free(&bytes);
    return ok;
}

bool yew_tutor_progress_save(const char *root,
                             const YewTutorProgress *progress)
{
    char path[PATH_MAX];
    TutorDoc doc;
    Bytebuf out;
    FlValue value;
    YewSaveErr error;
    u32 chapter;

    if (progress == NULL || !path_join(path, root, "tutor.fl"))
        return false;
    for (chapter = 0U; chapter < (u32)YEW_TUTOR_CHAPTERS; chapter++) {
        if ((progress->done[chapter] & (u8)~0x7fU) != 0U ||
            (progress->skipped[chapter] &&
             (chapter + 1U != (u32)YEW_TUTOR_CHAPTERS ||
              progress->done[chapter] != 0U)))
            return false;
    }
    doc_init(&doc);
    value = progress_encode(&doc, progress);
    bytebuf_init(&out);
    fl_data_write(&out, value, 0U);
    error = yew_file_write_atomic(path, out.data, out.len, 0600);
    bytebuf_free(&out);
    doc_free(&doc);
    return error == YEW_SAVE_OK;
}

static int usage_error(const char *message, const char *arg)
{
    (void)fprintf(stderr, "yew tutor: %s%s%s\n", message,
                  arg == NULL ? "" : ": ", arg == NULL ? "" : arg);
    return YEW_EXIT_ERR;
}

static int list_chapters(const char *root)
{
    YewTutorProgress progress;
    bool missing;
    u32 i;

    if (!yew_tutor_progress_load(root, &progress, &missing))
        return usage_error("cannot read tutor progress", NULL);
    for (i = 0U; i < (u32)YEW_TUTOR_CHAPTERS; i++) {
        const char *state = progress.skipped[i] ? "skipped" :
            progress.done[i] ==
                (u8)((1U << YEW_TUTOR_TASKS_PER_CHAPTER) - 1U) ?
                "complete" : "incomplete";

        (void)printf("%u %-9s %-10s %s\n", (unsigned)(i + 1U),
                     chapter_names[i], state, chapter_titles[i]);
    }
    return YEW_EXIT_OK;
}

int yew_tutor_main(int argc, char **argv)
{
    bool list = false;
    bool reset = false;
    bool check = false;
    bool clean = false;
    bool have_chapter = false;
    u8 chapter = 0U;
    char *root;
    int i;
    int result;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0)
            list = true;
        else if (strcmp(argv[i], "--reset") == 0)
            reset = true;
        else if (strcmp(argv[i], "--check") == 0)
            check = true;
        else if (strcmp(argv[i], "--clean") == 0)
            clean = true;
        else if (argv[i][0] == '-')
            return usage_error("unknown option", argv[i]);
        else if (have_chapter)
            return usage_error("more than one chapter", argv[i]);
        else if (!yew_tutor_chapter_parse(argv[i], &chapter))
            return usage_error("unknown chapter", argv[i]);
        else
            have_chapter = true;
    }
    (void)clean;
    if (check && list)
        return usage_error("--check and --list cannot be combined", NULL);
    root = yew_tutor_root(reset);
    if (root == NULL)
        return usage_error("no usable state directory", NULL);
    if (reset && (!yew_tutor_reset(root) || !yew_tutor_seed(root))) {
        yew_xfree(root);
        return usage_error("could not reset tutor workspace", NULL);
    }
    if (list) {
        result = list_chapters(root);
        yew_xfree(root);
        return result;
    }
    yew_xfree(root);
    (void)chapter;
    (void)have_chapter;
    return usage_error(check ?
        "--check replay lands in Sprint 59 tutor completion" :
        "interactive tutor lands in Sprint 59 tutor completion", NULL);
}
