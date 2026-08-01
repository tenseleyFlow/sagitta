#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "text/file.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/xattr.h>
#endif

#include "unicode/utf8.h"
#include "util/base.h"
#include "util/log.h"

#define SAG_FILE_NORMAL_MAX (UINT64_C(256) * 1024U * 1024U)
#define SAG_FILE_MAX (UINT64_C(2) * 1024U * 1024U * 1024U)
#define SAG_BINARY_SCAN_MAX 8192U

static const u8 bom[] = {0xEFU, 0xBBU, 0xBFU};
static const u8 lf[] = {'\n'};
static const u8 crlf[] = {'\r', '\n'};
static u64 temp_counter;

static struct timespec stat_mtime(const struct stat *st)
{
#if defined(__APPLE__)
    return st->st_mtimespec;
#else
    return st->st_mtim;
#endif
}

static bool timespec_equal(struct timespec a, struct timespec b)
{
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static char *string_copy(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *copy = sag_xmalloc(n);

    (void)memcpy(copy, s, n);
    return copy;
}

void sag_filemeta_init(FileMeta *meta)
{
    if (meta == NULL)
        SAG_BUG("sag_filemeta_init: NULL metadata");
    (void)memset(meta, 0, sizeof(*meta));
    meta->eol = SAG_EOL_LF;
    meta->dominant_eol = SAG_EOL_LF;
}

void sag_filemeta_dispose(FileMeta *meta)
{
    if (meta == NULL)
        return;
    free(meta->realpath);
    sag_filemeta_init(meta);
}

void sag_filemeta_eol_bytes(const FileMeta *meta, const u8 **bytes,
                            size_t *len)
{
    SagEol style;

    if (meta == NULL || bytes == NULL || len == NULL)
        SAG_BUG("sag_filemeta_eol_bytes: NULL argument");
    style = meta->eol == SAG_EOL_MIXED ? meta->dominant_eol : meta->eol;
    if (style == SAG_EOL_CRLF) {
        *bytes = crlf;
        *len = sizeof(crlf);
    } else {
        *bytes = lf;
        *len = sizeof(lf);
    }
}

static SagLoadErr load_errno(int error)
{
    if (error == ENOENT)
        return SAG_LOAD_ENOENT;
    if (error == EACCES || error == EPERM)
        return SAG_LOAD_EACCES;
    if (error == EISDIR)
        return SAG_LOAD_EISDIR;
    return SAG_LOAD_IO;
}

static bool read_exact_file(int fd, u8 *bytes, size_t len)
{
    size_t at = 0U;

    while (at < len) {
        ssize_t n = read(fd, bytes + at, len - at);

        if (n > 0) {
            at += (size_t)n;
        } else if (n == 0) {
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

static void classify_bytes(const u8 *bytes, size_t len, FileMeta *meta,
                           size_t *content_at)
{
    const u8 *at;
    const u8 *end = bytes + len;
    size_t binary_len = len < SAG_BINARY_SCAN_MAX ? len : SAG_BINARY_SCAN_MAX;
    u32 crlf_count = 0U;
    u32 bare_lf_count = 0U;

    meta->binary = memchr(bytes, 0, binary_len) != NULL;
    *content_at = 0U;
    if (meta->binary) {
        meta->eol = SAG_EOL_LF;
        meta->dominant_eol = SAG_EOL_LF;
        meta->missing_final_nl = len != 0U && bytes[len - 1U] != '\n';
        return;
    }
    if (len >= sizeof(bom) && memcmp(bytes, bom, sizeof(bom)) == 0) {
        meta->had_bom = true;
        *content_at = sizeof(bom);
    }
    at = bytes + *content_at;
    while (at < end) {
        const u8 *newline = memchr(at, '\n', (size_t)(end - at));

        if (newline == NULL)
            break;
        if (newline != bytes + *content_at && newline[-1] == '\r')
            crlf_count++;
        else
            bare_lf_count++;
        at = newline + 1U;
    }
    meta->crlf_count = crlf_count;
    meta->lf_count = bare_lf_count;
    meta->dominant_eol = crlf_count > bare_lf_count
                             ? SAG_EOL_CRLF
                             : SAG_EOL_LF;
    if (crlf_count != 0U && bare_lf_count != 0U)
        meta->eol = SAG_EOL_MIXED;
    else
        meta->eol = meta->dominant_eol;
    meta->had_invalid_utf8 =
        sag_utf8_validate(bytes + *content_at, len - *content_at) !=
        len - *content_at;
    meta->missing_final_nl =
        len != *content_at && bytes[len - 1U] != '\n';
}

static char *path_dirname(const char *path);
static const char *path_basename(const char *path);
static int open_temp(const char *dir, const char *base, mode_t mode,
                     char **path_out);

static char *canonical_new_path(const char *path)
{
    char *dir = path_dirname(path);
    char *resolved_dir = realpath(dir, NULL);
    char *result;

    free(dir);
    if (resolved_dir == NULL)
        return string_copy(path);
    {
        const char *base = path_basename(path);
        size_t n = strlen(resolved_dir) + strlen(base) + 2U;

        result = sag_xmalloc(n);
        (void)snprintf(result, n, "%s/%s", resolved_dir, base);
    }
    free(resolved_dir);
    return result;
}

static char *resolve_save_target_at(const char *path, unsigned int depth)
{
    struct stat st;
    char *resolved;
    char *link_text;
    char *next;
    size_t cap;
    ssize_t len;

    resolved = realpath(path, NULL);
    if (resolved != NULL)
        return resolved;
    if (errno != ENOENT || depth >= 40U)
        return NULL;
    if (lstat(path, &st) != 0) {
        if (errno != ENOENT)
            return NULL;
        return canonical_new_path(path);
    }
    if (!S_ISLNK(st.st_mode)) {
        errno = EINVAL;
        return NULL;
    }
    if (st.st_size > 0) {
        if ((u64)st.st_size > SIZE_MAX - 2U) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        cap = (size_t)st.st_size + 2U;
    } else {
        cap = (size_t)PATH_MAX + 2U;
    }
    link_text = sag_xmalloc(cap);
    len = readlink(path, link_text, cap - 1U);
    if (len < 0 || (size_t)len >= cap - 1U) {
        int saved_errno = len < 0 ? errno : ENAMETOOLONG;

        free(link_text);
        errno = saved_errno;
        return NULL;
    }
    link_text[len] = '\0';
    if (link_text[0] == '/') {
        next = link_text;
    } else {
        char *dir = path_dirname(path);
        size_t next_len = strlen(dir) + strlen(link_text) + 2U;

        next = sag_xmalloc(next_len);
        (void)snprintf(next, next_len, "%s/%s", dir, link_text);
        free(dir);
        free(link_text);
    }
    resolved = resolve_save_target_at(next, depth + 1U);
    free(next);
    return resolved;
}

static char *resolve_save_target(const char *path)
{
    return resolve_save_target_at(path, 0U);
}

static void warn_temp_leftovers(const char *path)
{
    char *dir = path_dirname(path);
    const char *base = path_basename(path);
    size_t prefix_len = strlen(base) + strlen(".sag--") + 1U;
    char *prefix = sag_xmalloc(prefix_len);
    DIR *stream;
    struct dirent *entry;

    (void)snprintf(prefix, prefix_len, ".sag-%s-", base);
    stream = opendir(dir);
    if (stream != NULL) {
        while ((entry = readdir(stream)) != NULL) {
            if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0)
                sag_log(SAG_LOG_WARN, "save temporary remains: %s/%s", dir,
                        entry->d_name);
        }
        (void)closedir(stream);
    }
    free(prefix);
    free(dir);
}

SagLoadErr sag_file_load(const char *path, TextBuf **out, FileMeta *meta)
{
    struct stat link_st;
    struct stat st;
    struct stat after;
    u8 *bytes;
    size_t size;
    size_t content_at;
    int fd;
    int saved_errno;

    if (path == NULL || out == NULL || meta == NULL)
        SAG_BUG("sag_file_load: NULL argument");
    *out = NULL;
    sag_filemeta_init(meta);
    warn_temp_leftovers(path);
    if (lstat(path, &link_st) == 0) {
        meta->via_symlink = S_ISLNK(link_st.st_mode);
        if (S_ISDIR(link_st.st_mode))
            return SAG_LOAD_EISDIR;
        if (!S_ISREG(link_st.st_mode) && !S_ISLNK(link_st.st_mode))
            return SAG_LOAD_IO;
    } else if (errno != ENOENT) {
        return load_errno(errno);
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SagLoadErr error = load_errno(errno);

        if (error == SAG_LOAD_ENOENT) {
            *out = sag_textbuf_new();
            meta->realpath = meta->via_symlink ? resolve_save_target(path)
                                               : canonical_new_path(path);
            if (meta->realpath == NULL) {
                sag_textbuf_free(*out);
                *out = NULL;
                return SAG_LOAD_IO;
            }
            meta->mode = 0666U;
        }
        return error;
    }
    if (fstat(fd, &st) != 0) {
        saved_errno = errno;
        (void)close(fd);
        return load_errno(saved_errno);
    }
    if (S_ISDIR(st.st_mode)) {
        (void)close(fd);
        return SAG_LOAD_EISDIR;
    }
    if (!S_ISREG(st.st_mode)) {
        (void)close(fd);
        return SAG_LOAD_IO;
    }
    if (st.st_size < 0 || (u64)st.st_size > SAG_FILE_MAX) {
        (void)close(fd);
        return SAG_LOAD_TOO_LARGE;
    }
    size = (size_t)st.st_size;
    bytes = sag_xmalloc(size);
    if (!read_exact_file(fd, bytes, size)) {
        saved_errno = errno;
        free(bytes);
        (void)close(fd);
        return saved_errno == EACCES ? SAG_LOAD_EACCES : SAG_LOAD_IO;
    }
    if (fstat(fd, &after) != 0 || after.st_dev != st.st_dev ||
        after.st_ino != st.st_ino || after.st_size != st.st_size ||
        !timespec_equal(stat_mtime(&after), stat_mtime(&st))) {
        free(bytes);
        (void)close(fd);
        return SAG_LOAD_IO;
    }
    if (close(fd) != 0) {
        free(bytes);
        return SAG_LOAD_IO;
    }
    if ((u64)size > SAG_FILE_NORMAL_MAX)
        sag_log(SAG_LOG_INFO, "loading large file: %llu bytes",
                (unsigned long long)size);

    meta->exists = true;
    meta->mode = st.st_mode;
    meta->uid = st.st_uid;
    meta->gid = st.st_gid;
    meta->nlink = st.st_nlink;
    meta->dev = st.st_dev;
    meta->ino = st.st_ino;
    meta->mtime = stat_mtime(&st);
    meta->size_on_disk = (u64)size;
    meta->realpath = realpath(path, NULL);
    if (meta->realpath == NULL)
        meta->realpath = string_copy(path);
    classify_bytes(bytes, size, meta, &content_at);
    if (content_at != 0U)
        (void)memmove(bytes, bytes + content_at, size - content_at);
    *out = sag_textbuf_from_owned_bytes(bytes, (u64)(size - content_at));
    return SAG_LOAD_OK;
}

static bool write_full(int fd, const u8 *bytes, size_t len)
{
    size_t at = 0U;

    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        if (n > 0)
            at += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else {
            if (n == 0)
                errno = EIO;
            return false;
        }
    }
    return true;
}

static bool fsync_full(int fd)
{
    int result;

    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool fsync_directory(const char *path)
{
    int fd = open(path, O_RDONLY
#ifdef O_DIRECTORY
                  | O_DIRECTORY
#endif
    );
    bool ok;

    if (fd < 0)
        return false;
    ok = fsync_full(fd);
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static SagSaveErr destination_matches(const FileMeta *meta, const char *path,
                                      bool *needs_inplace)
{
    struct stat st;

    *needs_inplace = false;
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || !meta->exists || st.st_size < 0 ||
            st.st_dev != meta->dev || st.st_ino != meta->ino ||
            (u64)st.st_size != meta->size_on_disk ||
            !timespec_equal(stat_mtime(&st), meta->mtime))
            return SAG_SAVE_CHANGED_ON_DISK;
        *needs_inplace = st.st_nlink > 1;
        return SAG_SAVE_OK;
    }
    if (errno == ENOENT)
        return meta->exists ? SAG_SAVE_CHANGED_ON_DISK : SAG_SAVE_OK;
    return errno == EACCES || errno == EPERM ? SAG_SAVE_PERM : SAG_SAVE_IO;
}

static bool stat_matches_meta(const struct stat *st, const FileMeta *meta)
{
    return S_ISREG(st->st_mode) && meta->exists && st->st_size >= 0 &&
           st->st_dev == meta->dev && st->st_ino == meta->ino &&
           (u64)st->st_size == meta->size_on_disk &&
           timespec_equal(stat_mtime(st), meta->mtime);
}

static bool write_text(int fd, const TextBuf *tb, bool had_bom)
{
    TextIter it;

    if (had_bom && !write_full(fd, bom, sizeof(bom)))
        return false;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(0U)))
        return true;
    do {
        const u8 *bytes;
        u64 len;

        if (!sag_textiter_chunk(&it, tb, &bytes, &len))
            return false;
        if (len > SIZE_MAX || !write_full(fd, bytes, (size_t)len))
            return false;
    } while (sag_textiter_advance(&it, tb));
    return true;
}

static char *path_dirname(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t len;
    char *dir;

    if (slash == NULL)
        return string_copy(".");
    if (slash == path)
        return string_copy("/");
    len = (size_t)(slash - path);
    dir = sag_xmalloc(len + 1U);
    (void)memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL ? path : slash + 1;
}

static bool directory_writable(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return false;
    if ((st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0)
        return false;
    return access(path, W_OK) == 0;
}

static bool make_dir(const char *path)
{
    struct stat st;

    if (mkdir(path, 0700) == 0)
        return true;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool make_parent_dirs(char *path)
{
    char *p;

    for (p = path + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!make_dir(path)) {
            *p = '/';
            return false;
        }
        *p = '/';
    }
    return true;
}

static char *state_backup_dir(void)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home;
    const char *suffix;
    size_t n;
    char *dir;

    if (state != NULL && state[0] != '\0') {
        suffix = "/sagitta/backup";
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return NULL;
        state = home;
        suffix = "/.local/state/sagitta/backup";
    }
    n = strlen(state) + strlen(suffix) + 1U;
    dir = sag_xmalloc(n);
    (void)snprintf(dir, n, "%s%s", state, suffix);
    if (!make_parent_dirs(dir) || !make_dir(dir)) {
        free(dir);
        return NULL;
    }
    return dir;
}

static u64 fnv64(const char *text)
{
    u64 hash = UINT64_C(14695981039346656037);

    while (*text != '\0') {
        hash ^= (u8)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static char *backup_path(const char *dst)
{
    char *dir = state_backup_dir();
    char resolved[PATH_MAX];
    const char *key;
    size_t n;
    char *path;

    if (dir == NULL)
        return NULL;
    key = realpath(dst, resolved) != NULL ? resolved : dst;
    n = strlen(dir) + 1U + 16U + strlen(".bak") + 1U;
    path = sag_xmalloc(n);
    (void)snprintf(path, n, "%s/%016llx.bak", dir,
                   (unsigned long long)fnv64(key));
    free(dir);
    return path;
}

static bool copy_fd(int src, int dst)
{
    u8 block[64U * 1024U];

    for (;;) {
        ssize_t n = read(src, block, sizeof(block));

        if (n > 0) {
            if (!write_full(dst, block, (size_t)n))
                return false;
        } else if (n == 0) {
            return true;
        } else if (errno != EINTR) {
            return false;
        }
    }
}

static bool copy_path_to_backup(const char *src_path, const char *bak_path,
                                const FileMeta *meta)
{
    struct stat src_st;
    struct stat bak_st;
    char *dir = path_dirname(bak_path);
    char *tmp = NULL;
    int src = -1;
    int bak = -1;
    bool ok = false;
    int saved_errno = 0;

    src = open(src_path, O_RDONLY
#ifdef O_NOFOLLOW
               | O_NOFOLLOW
#endif
    );
    if (src < 0)
        goto done;
    if (fstat(src, &src_st) != 0)
        goto done;
    if (!stat_matches_meta(&src_st, meta)) {
        errno = ESTALE;
        goto done;
    }
    bak = open_temp(dir, path_basename(bak_path), 0600, &tmp);
    if (bak < 0)
        goto done;
    if (fstat(bak, &bak_st) != 0)
        goto done;
    if (!S_ISREG(bak_st.st_mode) ||
        (bak_st.st_dev == src_st.st_dev && bak_st.st_ino == src_st.st_ino)) {
        errno = EINVAL;
        goto done;
    }
    if (!copy_fd(src, bak) || !fsync_full(bak))
        goto done;
    if (close(bak) != 0) {
        bak = -1;
        goto done;
    }
    bak = -1;
    if (rename(tmp, bak_path) != 0 || !fsync_directory(dir))
        goto done;
    ok = true;
done:
    if (!ok)
        saved_errno = errno == 0 ? EIO : errno;
    if (bak >= 0 && close(bak) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (src >= 0 && close(src) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok && tmp != NULL)
        (void)unlink(tmp);
    free(tmp);
    free(dir);
    if (!ok)
        errno = saved_errno;
    return ok;
}

static bool restore_backup(const char *bak_path, const char *dst_path)
{
    int bak = -1;
    int dst = -1;
    bool ok = false;

    bak = open(bak_path, O_RDONLY
#ifdef O_NOFOLLOW
               | O_NOFOLLOW
#endif
    );
    if (bak < 0)
        goto done;
    dst = open(dst_path, O_WRONLY
#ifdef O_NOFOLLOW
               | O_NOFOLLOW
#endif
    );
    if (dst < 0)
        goto done;
    if (!copy_fd(bak, dst))
        goto done;
    {
        off_t end = lseek(dst, 0, SEEK_CUR);

        if (end < 0 || ftruncate(dst, end) != 0 || !fsync_full(dst))
            goto done;
    }
    ok = true;
done:
    if (dst >= 0 && close(dst) != 0)
        ok = false;
    if (bak >= 0 && close(bak) != 0)
        ok = false;
    return ok;
}

static SagSaveErr inplace_save(const TextBuf *tb, const FileMeta *meta,
                               const char *dst, struct stat *saved_st)
{
    char *bak = backup_path(dst);
    struct stat st;
    int fd;
    off_t end;
    bool ok;

    if (bak == NULL)
        return SAG_SAVE_IO;
    if (!copy_path_to_backup(dst, bak, meta)) {
        int saved_errno = errno;

        free(bak);
        if (saved_errno == ESTALE)
            return SAG_SAVE_CHANGED_ON_DISK;
        return saved_errno == EACCES || saved_errno == EPERM ? SAG_SAVE_PERM
                                                              : SAG_SAVE_IO;
    }
    fd = open(dst, O_WRONLY
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0) {
        free(bak);
        return errno == EACCES || errno == EPERM ? SAG_SAVE_PERM
                                                  : SAG_SAVE_IO;
    }
    if (fstat(fd, &st) != 0 || !stat_matches_meta(&st, meta)) {
        (void)close(fd);
        free(bak);
        return SAG_SAVE_CHANGED_ON_DISK;
    }
    ok = write_text(fd, tb, meta->had_bom);
    end = ok ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
    if (end < 0 || ftruncate(fd, end) != 0 || !fsync_full(fd) ||
        fstat(fd, saved_st) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        (void)restore_backup(bak, dst);
        free(bak);
        return SAG_SAVE_IO;
    }
    free(bak);
    return SAG_SAVE_OK;
}

#ifdef __linux__
static void copy_xattrs(const char *src, int dst)
{
    ssize_t names_len = listxattr(src, NULL, 0U);
    char *names;
    char *name;

    if (names_len <= 0)
        return;
    names = sag_xmalloc((size_t)names_len);
    names_len = listxattr(src, names, (size_t)names_len);
    if (names_len <= 0) {
        free(names);
        return;
    }
    name = names;
    while (name < names + names_len) {
        size_t name_len = strlen(name) + 1U;
        ssize_t value_len = getxattr(src, name, NULL, 0U);

        if (value_len >= 0) {
            void *value = sag_xmalloc((size_t)value_len);

            value_len = getxattr(src, name, value, (size_t)value_len);
            if (value_len >= 0)
                (void)fsetxattr(dst, name, value, (size_t)value_len, 0);
            free(value);
        }
        name += name_len;
    }
    free(names);
}
#else
static void copy_xattrs(const char *src, int dst)
{
    (void)src;
    (void)dst;
}
#endif

static int open_temp(const char *dir, const char *base, mode_t mode,
                     char **path_out)
{
    unsigned int tries;

    for (tries = 0U; tries < 1000U; tries++) {
        unsigned long long counter =
            (unsigned long long)++temp_counter;
        size_t n = strlen(dir) + strlen(base) + 64U;
        char *path = sag_xmalloc(n);
        int flags = O_WRONLY | O_CREAT | O_EXCL;
        int fd;

        (void)snprintf(path, n, "%s/.sag-%s-%ld-%llu", dir, base,
                       (long)getpid(), counter);
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        fd = open(path, flags, mode);
        if (fd >= 0) {
            *path_out = path;
            return fd;
        }
        free(path);
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

static bool commit_temp(const char *tmp, const char *dst, const char *dir)
{
    if (rename(tmp, dst) != 0)
        return false;
    return fsync_directory(dir);
}

SagSaveErr sag_file_write_atomic(const char *path, const u8 *bytes,
                                 size_t len, mode_t mode)
{
    char *dir;
    char *tmp = NULL;
    int fd;
    int saved_errno;

    if (path == NULL || (bytes == NULL && len != 0U)) {
        errno = EINVAL;
        return SAG_SAVE_IO;
    }
    dir = path_dirname(path);
    fd = open_temp(dir, path_basename(path), mode, &tmp);
    if (fd < 0)
        goto fail;
    if (!write_full(fd, bytes, len) || !fsync_full(fd))
        goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (!commit_temp(tmp, path, dir))
        goto fail;
    free(tmp);
    free(dir);
    return SAG_SAVE_OK;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    if (fd >= 0)
        (void)close(fd);
    if (tmp != NULL)
        (void)unlink(tmp);
    free(tmp);
    free(dir);
    errno = saved_errno;
    return saved_errno == EACCES || saved_errno == EPERM ? SAG_SAVE_PERM
                                                          : SAG_SAVE_IO;
}

static SagSaveErr atomic_save(const TextBuf *tb, const FileMeta *meta,
                              const char *dst, struct stat *saved_st)
{
    char *dir = path_dirname(dst);
    char *tmp = NULL;
    mode_t mode = meta->exists ? meta->mode & 07777U : 0666U;
    int fd = open_temp(dir, path_basename(dst), mode, &tmp);
    bool foreign_owner;
    int saved_errno;
    SagSaveErr match;
    bool needs_inplace;

    if (fd < 0) {
        saved_errno = errno;
        free(dir);
        return saved_errno == EACCES || saved_errno == EPERM
                   ? SAG_SAVE_PERM
                   : SAG_SAVE_IO;
    }
    if (!write_text(fd, tb, meta->had_bom) || !fsync_full(fd))
        goto fail;
    if (meta->exists) {
        foreign_owner = meta->uid != geteuid() || meta->gid != getegid();
        if (fchown(fd, meta->uid, meta->gid) != 0 && foreign_owner) {
            (void)close(fd);
            (void)unlink(tmp);
            free(tmp);
            free(dir);
            return inplace_save(tb, meta, dst, saved_st);
        }
        if (fchmod(fd, meta->mode & 07777U) != 0)
            goto fail;
        copy_xattrs(dst, fd);
    }
    if (fstat(fd, saved_st) != 0)
        goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    match = destination_matches(meta, dst, &needs_inplace);
    if (match != SAG_SAVE_OK) {
        (void)unlink(tmp);
        free(tmp);
        free(dir);
        return match;
    }
    if (needs_inplace) {
        (void)unlink(tmp);
        free(tmp);
        free(dir);
        return inplace_save(tb, meta, dst, saved_st);
    }
    if (!commit_temp(tmp, dst, dir)) {
        saved_errno = errno;
        (void)unlink(tmp);
        free(tmp);
        free(dir);
        if (saved_errno == EXDEV)
            return inplace_save(tb, meta, dst, saved_st);
        return saved_errno == EACCES || saved_errno == EPERM
                   ? SAG_SAVE_PERM
                   : SAG_SAVE_IO;
    }
    free(tmp);
    free(dir);
    return SAG_SAVE_OK;

fail:
    saved_errno = errno;
    if (fd >= 0)
        (void)close(fd);
    (void)unlink(tmp);
    free(tmp);
    free(dir);
    return saved_errno == EACCES || saved_errno == EPERM ? SAG_SAVE_PERM
                                                          : SAG_SAVE_IO;
}

static void refresh_saved_meta(FileMeta *meta, const struct stat *st,
                               char *saved_path, bool via_symlink)
{
    free(meta->realpath);
    meta->realpath = saved_path;
    meta->exists = true;
    meta->via_symlink = via_symlink;
    meta->mode = st->st_mode;
    meta->uid = st->st_uid;
    meta->gid = st->st_gid;
    meta->nlink = st->st_nlink;
    meta->dev = st->st_dev;
    meta->ino = st->st_ino;
    meta->mtime = stat_mtime(st);
    meta->size_on_disk = (u64)st->st_size;
}

SagSaveErr sag_file_save(const TextBuf *tb, FileMeta *meta,
                         const char *path)
{
    struct stat link_st;
    struct stat saved_st;
    char *resolved = NULL;
    char *saved_realpath;
    const char *dst = path;
    char *dir;
    bool needs_inplace;
    bool is_symlink = false;
    SagSaveErr match;
    SagSaveErr result;

    if (tb == NULL || meta == NULL || path == NULL)
        SAG_BUG("sag_file_save: NULL argument");
    if (lstat(path, &link_st) == 0) {
        is_symlink = S_ISLNK(link_st.st_mode);
    } else if (errno != ENOENT) {
        return errno == EACCES || errno == EPERM ? SAG_SAVE_PERM
                                                  : SAG_SAVE_IO;
    }
    if (is_symlink) {
        resolved = resolve_save_target(path);
        if (resolved == NULL)
            return SAG_SAVE_IO;
        if (meta->realpath != NULL && strcmp(resolved, meta->realpath) != 0) {
            free(resolved);
            return SAG_SAVE_CHANGED_ON_DISK;
        }
        dst = resolved;
    }
    match = destination_matches(meta, dst, &needs_inplace);
    if (match != SAG_SAVE_OK) {
        free(resolved);
        return match;
    }
    saved_realpath = realpath(dst, NULL);
    if (saved_realpath == NULL)
        saved_realpath = canonical_new_path(dst);
    dir = path_dirname(dst);
    if (needs_inplace || !directory_writable(dir))
        result = inplace_save(tb, meta, dst, &saved_st);
    else
        result = atomic_save(tb, meta, dst, &saved_st);
    if (result == SAG_SAVE_OK) {
        refresh_saved_meta(meta, &saved_st, saved_realpath, is_symlink);
        saved_realpath = NULL;
    }
    free(saved_realpath);
    free(dir);
    free(resolved);
    return result;
}
