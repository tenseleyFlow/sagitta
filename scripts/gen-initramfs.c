#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57's deterministic initramfs writer.
 *
 * Host cpio implementations do not agree on how to normalize inode numbers,
 * ownership, or mtimes.  Writing the small SVR4 "newc" format directly keeps
 * the embedded image byte-identical on every rebuild without adding a build
 * dependency.  The source tree supplies only names, types, permission bits,
 * symlink targets, and regular-file bytes; all host identity is discarded.
 */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint32_t u32;

typedef enum EntryKind {
    ENTRY_DIR,
    ENTRY_FILE,
    ENTRY_LINK
} EntryKind;

typedef struct Entry {
    char *name;
    char *path;
    char *link;
    EntryKind kind;
    u32 mode;
    u32 size;
} Entry;

typedef struct Entries {
    Entry *data;
    size_t len;
    size_t cap;
} Entries;

static const char *program;

static void fail(const char *what, const char *path)
{
    if (path != NULL)
        (void)fprintf(stderr, "%s: %s: %s\n", program, what, path);
    else
        (void)fprintf(stderr, "%s: %s\n", program, what);
    exit(EXIT_FAILURE);
}

static void fail_errno(const char *what, const char *path)
{
    int saved = errno;

    (void)fprintf(stderr, "%s: %s %s: %s\n", program, what, path,
                  strerror(saved));
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size)
{
    void *p = malloc(size == 0U ? 1U : size);

    if (p == NULL)
        fail("out of memory", NULL);
    return p;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size == 0U ? 1U : size);

    if (p == NULL)
        fail("out of memory", NULL);
    return p;
}

static char *xstrdup(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static char *path_join(const char *left, const char *right)
{
    size_t nl = strlen(left);
    size_t nr = strlen(right);
    bool slash = nl != 0U && left[nl - 1U] != '/';
    char *joined;

    if (nl > SIZE_MAX - nr - (slash ? 2U : 1U))
        fail("path is too long", right);
    joined = xmalloc(nl + nr + (slash ? 2U : 1U));
    (void)memcpy(joined, left, nl);
    if (slash)
        joined[nl++] = '/';
    (void)memcpy(joined + nl, right, nr + 1U);
    return joined;
}

static bool valid_relative_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    size_t segment = 0U;

    if (p[0] == '\0' || p[0] == '/')
        return false;
    for (;;) {
        if (*p == '/' || *p == '\0') {
            const unsigned char *start = p - segment;

            if (segment == 0U ||
                (segment == 1U && start[0] == '.') ||
                (segment == 2U && start[0] == '.' && start[1] == '.'))
                return false;
            if (*p == '\0')
                return true;
            segment = 0U;
        } else {
            if (*p < 0x20U || *p == 0x7fU || *p == '\\')
                return false;
            segment++;
        }
        p++;
    }
}

static char *read_link(const char *path, const struct stat *st)
{
    size_t cap = st->st_size > 0 ? (size_t)st->st_size + 1U : 128U;
    char *target;

    if (cap < (size_t)st->st_size)
        fail("symbolic-link target is too large", path);
    for (;;) {
        ssize_t got;

        target = xmalloc(cap);
        got = readlink(path, target, cap);
        if (got < 0)
            fail_errno("cannot read symbolic link", path);
        if ((size_t)got < cap) {
            target[got] = '\0';
            return target;
        }
        free(target);
        if (cap > SIZE_MAX / 2U)
            fail("symbolic-link target is too large", path);
        cap *= 2U;
    }
}

static void entries_push(Entries *entries, const char *name, const char *path,
                         const struct stat *st)
{
    Entry *entry;
    uintmax_t size;

    if (entries->len == entries->cap) {
        size_t cap = entries->cap == 0U ? 32U : entries->cap * 2U;

        if (cap < entries->cap || cap > SIZE_MAX / sizeof(*entries->data))
            fail("too many initramfs entries", NULL);
        entries->data = xrealloc(entries->data,
                                 cap * sizeof(*entries->data));
        entries->cap = cap;
    }
    entry = &entries->data[entries->len++];
    entry->name = xstrdup(name);
    entry->path = xstrdup(path);
    entry->link = NULL;
    entry->mode = S_ISLNK(st->st_mode) ? 0777U
                                      : (u32)(st->st_mode & 07777U);
    entry->size = 0U;
    if (S_ISDIR(st->st_mode)) {
        entry->kind = ENTRY_DIR;
    } else if (S_ISREG(st->st_mode)) {
        size = (uintmax_t)st->st_size;
        if (st->st_size < 0 || size > UINT32_MAX)
            fail("regular file exceeds the newc size limit", name);
        entry->kind = ENTRY_FILE;
        entry->size = (u32)size;
    } else if (S_ISLNK(st->st_mode)) {
        size_t len;

        entry->kind = ENTRY_LINK;
        entry->link = read_link(path, st);
        len = strlen(entry->link);
        if (len > UINT32_MAX)
            fail("symbolic-link target exceeds the newc size limit", name);
        entry->size = (u32)len;
    } else {
        fail("unsupported initramfs entry", name);
    }
}

static void walk(const char *root, const char *relative, Entries *entries)
{
    char *directory = relative[0] == '\0' ? xstrdup(root)
                                            : path_join(root, relative);
    DIR *dir = opendir(directory);
    struct dirent *item;

    if (dir == NULL)
        fail_errno("cannot open directory", directory);
    errno = 0;
    while ((item = readdir(dir)) != NULL) {
        char *name;
        char *path;
        struct stat st;

        if (strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0)
            continue;
        name = relative[0] == '\0' ? xstrdup(item->d_name)
                                    : path_join(relative, item->d_name);
        if (!valid_relative_name(name))
            fail("invalid initramfs entry name", name);
        path = path_join(root, name);
        if (lstat(path, &st) != 0)
            fail_errno("cannot inspect", path);
        entries_push(entries, name, path, &st);
        if (S_ISDIR(st.st_mode))
            walk(root, name, entries);
        free(path);
        free(name);
        errno = 0;
    }
    if (errno != 0)
        fail_errno("cannot read directory", directory);
    if (closedir(dir) != 0)
        fail_errno("cannot close directory", directory);
    free(directory);
}

static void stable_sort_entries(Entries *entries)
{
    size_t i;

    for (i = 1U; i < entries->len; i++) {
        Entry moving = entries->data[i];
        size_t at = i;

        while (at != 0U &&
               strcmp(moving.name, entries->data[at - 1U].name) < 0) {
            entries->data[at] = entries->data[at - 1U];
            at--;
        }
        entries->data[at] = moving;
    }
}

static void write_exact(FILE *out, const void *data, size_t len,
                        const char *path)
{
    if (len != 0U && fwrite(data, 1U, len, out) != len)
        fail_errno("cannot write", path);
}

static void write_padding(FILE *out, size_t bytes, const char *path)
{
    static const u8 zero[4] = {0U, 0U, 0U, 0U};
    size_t pad = (4U - (bytes & 3U)) & 3U;

    write_exact(out, zero, pad, path);
}

static void copy_file(FILE *out, const Entry *entry, const char *output)
{
    FILE *in = fopen(entry->path, "rb");
    u8 bytes[16384];
    u32 left = entry->size;

    if (in == NULL)
        fail_errno("cannot open", entry->path);
    while (left != 0U) {
        size_t want = left < sizeof(bytes) ? (size_t)left : sizeof(bytes);
        size_t got = fread(bytes, 1U, want, in);

        if (got != want) {
            if (ferror(in))
                fail_errno("cannot read", entry->path);
            fail("regular file changed while building initramfs", entry->path);
        }
        write_exact(out, bytes, got, output);
        left -= (u32)got;
    }
    if (fgetc(in) != EOF)
        fail("regular file changed while building initramfs", entry->path);
    if (ferror(in))
        fail_errno("cannot read", entry->path);
    if (fclose(in) != 0)
        fail_errno("cannot close", entry->path);
}

static u32 archive_mode(const Entry *entry)
{
    u32 type;

    switch (entry->kind) {
    case ENTRY_DIR:
        type = 0040000U;
        break;
    case ENTRY_FILE:
        type = 0100000U;
        break;
    case ENTRY_LINK:
        type = 0120000U;
        break;
    default:
        fail("invalid initramfs entry type", entry->name);
        type = 0U;
        break;
    }
    return type | entry->mode;
}

static void write_record(FILE *out, const Entry *entry, u32 ino,
                         const char *output)
{
    char header[111];
    size_t namesize = strlen(entry->name) + 1U;
    int n;

    if (namesize > UINT32_MAX)
        fail("initramfs entry name is too long", entry->name);
    n = snprintf(header, sizeof(header),
                 "070701%08X%08X%08X%08X%08X%08X%08X"
                 "%08X%08X%08X%08X%08X%08X",
                 ino, archive_mode(entry), 0U, 0U,
                 entry->kind == ENTRY_DIR ? 2U : 1U, 0U, entry->size,
                 0U, 0U, 0U, 0U, (u32)namesize, 0U);
    if (n != 110)
        fail("cannot format initramfs header", entry->name);
    write_exact(out, header, 110U, output);
    write_exact(out, entry->name, namesize, output);
    write_padding(out, 110U + namesize, output);
    if (entry->kind == ENTRY_FILE && entry->size != 0U)
        copy_file(out, entry, output);
    else if (entry->kind == ENTRY_LINK)
        write_exact(out, entry->link, entry->size, output);
    write_padding(out, entry->size, output);
}

static void write_trailer(FILE *out, u32 ino, const char *output)
{
    Entry trailer;

    (void)memset(&trailer, 0, sizeof(trailer));
    trailer.name = (char *)"TRAILER!!!";
    trailer.kind = ENTRY_FILE;
    write_record(out, &trailer, ino, output);
}

static void write_list(const Entries *entries, const char *path)
{
    FILE *out;
    size_t i;

    if (path == NULL)
        return;
    out = fopen(path, "wb");
    if (out == NULL)
        fail_errno("cannot open", path);
    for (i = 0U; i < entries->len; i++) {
        const Entry *entry = &entries->data[i];
        char kind = entry->kind == ENTRY_DIR ? 'd'
                    : entry->kind == ENTRY_FILE ? 'f' : 'l';

        if (fprintf(out, "%c %04o %u %s", kind,
                    (unsigned)(entry->mode & 07777U), entry->size,
                    entry->name) < 0)
            fail_errno("cannot write", path);
        if (entry->kind == ENTRY_LINK &&
            fprintf(out, " -> %s", entry->link) < 0)
            fail_errno("cannot write", path);
        if (fputc('\n', out) == EOF)
            fail_errno("cannot write", path);
    }
    if (fclose(out) != 0)
        fail_errno("cannot close", path);
}

static void entries_free(Entries *entries)
{
    size_t i;

    for (i = 0U; i < entries->len; i++) {
        free(entries->data[i].link);
        free(entries->data[i].path);
        free(entries->data[i].name);
    }
    free(entries->data);
}

int main(int argc, char **argv)
{
    const char *root;
    const char *output;
    const char *list;
    struct stat st;
    Entries entries = {0};
    FILE *out;
    size_t i;

    program = argc == 0 ? "gen-initramfs" : argv[0];
    if (argc != 3 && argc != 4) {
        (void)fprintf(stderr,
                      "usage: %s ROOT OUTPUT [FILE-LIST]\n", program);
        return 2;
    }
    root = argv[1];
    output = argv[2];
    list = argc == 4 ? argv[3] : NULL;
    if (lstat(root, &st) != 0)
        fail_errno("cannot inspect", root);
    if (!S_ISDIR(st.st_mode))
        fail("initramfs root is not a directory", root);
    walk(root, "", &entries);
    stable_sort_entries(&entries);
    out = fopen(output, "wb");
    if (out == NULL)
        fail_errno("cannot open", output);
    for (i = 0U; i < entries.len; i++) {
        if (i >= UINT32_MAX - 1U)
            fail("too many initramfs entries", NULL);
        write_record(out, &entries.data[i], (u32)i + 1U, output);
    }
    write_trailer(out, (u32)entries.len + 1U, output);
    if (fclose(out) != 0)
        fail_errno("cannot close", output);
    write_list(&entries, list);
    entries_free(&entries);
    return 0;
}
