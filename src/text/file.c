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

#define YEW_FILE_NORMAL_MAX (UINT64_C(256) * 1024U * 1024U)
#define YEW_FILE_MAX (UINT64_C(2) * 1024U * 1024U * 1024U)
#define YEW_BINARY_SCAN_MAX 8192U

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
    char *copy = yew_xmalloc(n);

    (void)memcpy(copy, s, n);
    return copy;
}

static void filemeta_snapshot_set(FileMeta *meta, const TextBuf *tb)
{
    if (meta->disk_snapshot_valid)
        yew_textsnap_release(NULL, &meta->disk_snapshot);
    meta->disk_snapshot = yew_textbuf_snap((TextBuf *)tb);
    meta->disk_snapshot_valid = true;
}

void yew_filemeta_init(FileMeta *meta)
{
    if (meta == NULL)
        YEW_BUG("yew_filemeta_init: NULL metadata");
    (void)memset(meta, 0, sizeof(*meta));
    meta->eol = YEW_EOL_LF;
    meta->dominant_eol = YEW_EOL_LF;
}

void yew_filemeta_dispose(FileMeta *meta)
{
    if (meta == NULL)
        return;
    yew_filemeta_content_forget(meta);
    yew_xfree(meta->realpath);
    yew_filemeta_init(meta);
}

void yew_filemeta_content_forget(FileMeta *meta)
{
    if (meta == NULL || !meta->disk_snapshot_valid)
        return;
    yew_textsnap_release(NULL, &meta->disk_snapshot);
    meta->disk_snapshot_valid = false;
}

void yew_filemeta_eol_bytes(const FileMeta *meta, const u8 **bytes,
                            size_t *len)
{
    YewEol style;

    if (meta == NULL || bytes == NULL || len == NULL)
        YEW_BUG("yew_filemeta_eol_bytes: NULL argument");
    style = meta->eol == YEW_EOL_MIXED ? meta->dominant_eol : meta->eol;
    if (style == YEW_EOL_CRLF) {
        *bytes = crlf;
        *len = sizeof(crlf);
    } else {
        *bytes = lf;
        *len = sizeof(lf);
    }
}

static YewLoadErr load_errno(int error)
{
    if (error == ENOENT)
        return YEW_LOAD_ENOENT;
    if (error == EACCES || error == EPERM)
        return YEW_LOAD_EACCES;
    if (error == EISDIR)
        return YEW_LOAD_EISDIR;
    return YEW_LOAD_IO;
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
                           size_t *content_at, bool *simple_ascii)
{
    const u8 *at;
    const u8 *end = bytes + len;
    size_t binary_len = len < YEW_BINARY_SCAN_MAX ? len : YEW_BINARY_SCAN_MAX;
    u32 crlf_count = 0U;
    u32 bare_lf_count = 0U;

    meta->binary = memchr(bytes, 0, binary_len) != NULL;
    *content_at = 0U;
    *simple_ascii = false;
    if (meta->binary) {
        meta->eol = YEW_EOL_LF;
        meta->dominant_eol = YEW_EOL_LF;
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
                             ? YEW_EOL_CRLF
                             : YEW_EOL_LF;
    if (crlf_count != 0U && bare_lf_count != 0U)
        meta->eol = YEW_EOL_MIXED;
    else
        meta->eol = meta->dominant_eol;
    meta->had_invalid_utf8 = yew_utf8_validate_simple(
        bytes + *content_at, len - *content_at, simple_ascii) !=
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
    char *resolved_dir = yew_xrealpath(dir);
    char *result;

    yew_xfree(dir);
    if (resolved_dir == NULL)
        return string_copy(path);
    {
        const char *base = path_basename(path);
        size_t n = strlen(resolved_dir) + strlen(base) + 2U;

        result = yew_xmalloc(n);
        (void)snprintf(result, n, "%s/%s", resolved_dir, base);
    }
    yew_xfree(resolved_dir);
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

    resolved = yew_xrealpath(path);
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
    link_text = yew_xmalloc(cap);
    len = readlink(path, link_text, cap - 1U);
    if (len < 0 || (size_t)len >= cap - 1U) {
        int saved_errno = len < 0 ? errno : ENAMETOOLONG;

        yew_xfree(link_text);
        errno = saved_errno;
        return NULL;
    }
    link_text[len] = '\0';
    if (link_text[0] == '/') {
        next = link_text;
    } else {
        char *dir = path_dirname(path);
        size_t next_len = strlen(dir) + strlen(link_text) + 2U;

        next = yew_xmalloc(next_len);
        (void)snprintf(next, next_len, "%s/%s", dir, link_text);
        yew_xfree(dir);
        yew_xfree(link_text);
    }
    resolved = resolve_save_target_at(next, depth + 1U);
    yew_xfree(next);
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
    size_t prefix_len = strlen(base) + strlen(".yew--") + 1U;
    char *prefix = yew_xmalloc(prefix_len);
    DIR *stream;
    struct dirent *entry;

    (void)snprintf(prefix, prefix_len, ".yew-%s-", base);
    stream = opendir(dir);
    if (stream != NULL) {
        while ((entry = readdir(stream)) != NULL) {
            if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0)
                yew_log(YEW_LOG_WARN, "save temporary remains: %s/%s", dir,
                        entry->d_name);
        }
        (void)closedir(stream);
    }
    yew_xfree(prefix);
    yew_xfree(dir);
}

/*
 * Sprint 24 D4: how many times the editor has actually read a file.
 *
 * A test hook, and deliberately a COUNTER rather than a flag: the claim
 * being defended is "opening a 40-file group costs one read", which is
 * only checkable by counting.  Nothing outside tests reads it, and it
 * is never reset by the editor itself.
 */
static u64 file_load_calls;

u64 yew_file_load_count(void)
{
    return file_load_calls;
}

void yew_file_load_count_reset(void)
{
    file_load_calls = 0U;
}

YewLoadErr yew_file_load(const char *path, TextBuf **out, FileMeta *meta)
{
    struct stat link_st;
    struct stat st;
    struct stat after;
    u8 *bytes;
    size_t size;
    size_t content_at;
    bool simple_ascii;
    int fd;
    int saved_errno;

    if (path == NULL || out == NULL || meta == NULL)
        YEW_BUG("yew_file_load: NULL argument");
    file_load_calls++;
    *out = NULL;
    yew_filemeta_init(meta);
    warn_temp_leftovers(path);
    if (lstat(path, &link_st) == 0) {
        meta->via_symlink = S_ISLNK(link_st.st_mode);
        if (S_ISDIR(link_st.st_mode))
            return YEW_LOAD_EISDIR;
        if (!S_ISREG(link_st.st_mode) && !S_ISLNK(link_st.st_mode))
            return YEW_LOAD_IO;
    } else if (errno != ENOENT) {
        return load_errno(errno);
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        YewLoadErr error = load_errno(errno);

        if (error == YEW_LOAD_ENOENT) {
            *out = yew_textbuf_new();
            meta->realpath = meta->via_symlink ? resolve_save_target(path)
                                               : canonical_new_path(path);
            if (meta->realpath == NULL) {
                yew_textbuf_free(*out);
                *out = NULL;
                return YEW_LOAD_IO;
            }
            meta->mode = 0666U;
            filemeta_snapshot_set(meta, *out);
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
        return YEW_LOAD_EISDIR;
    }
    if (!S_ISREG(st.st_mode)) {
        (void)close(fd);
        return YEW_LOAD_IO;
    }
    if (st.st_size < 0 || (u64)st.st_size > YEW_FILE_MAX) {
        (void)close(fd);
        return YEW_LOAD_TOO_LARGE;
    }
    size = (size_t)st.st_size;
    bytes = yew_xmalloc(size);
    if (!read_exact_file(fd, bytes, size)) {
        saved_errno = errno;
        yew_xfree(bytes);
        (void)close(fd);
        return saved_errno == EACCES ? YEW_LOAD_EACCES : YEW_LOAD_IO;
    }
    if (fstat(fd, &after) != 0 || after.st_dev != st.st_dev ||
        after.st_ino != st.st_ino || after.st_size != st.st_size ||
        !timespec_equal(stat_mtime(&after), stat_mtime(&st))) {
        yew_xfree(bytes);
        (void)close(fd);
        return YEW_LOAD_IO;
    }
    if (close(fd) != 0) {
        yew_xfree(bytes);
        return YEW_LOAD_IO;
    }
    if ((u64)size > YEW_FILE_NORMAL_MAX)
        yew_log(YEW_LOG_INFO, "loading large file: %llu bytes",
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
    meta->realpath = yew_xrealpath(path);
    if (meta->realpath == NULL)
        meta->realpath = string_copy(path);
    classify_bytes(bytes, size, meta, &content_at, &simple_ascii);
    if (content_at != 0U)
        (void)memmove(bytes, bytes + content_at, size - content_at);
    *out = yew_textbuf_from_owned_bytes_simple(
        bytes, (u64)(size - content_at), simple_ascii);
    filemeta_snapshot_set(meta, *out);
    return YEW_LOAD_OK;
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

static void meta_accept_stat(FileMeta *meta, const struct stat *st)
{
    meta->exists = true;
    meta->mode = st->st_mode;
    meta->uid = st->st_uid;
    meta->gid = st->st_gid;
    meta->nlink = st->st_nlink;
    meta->dev = st->st_dev;
    meta->ino = st->st_ino;
    meta->mtime = stat_mtime(st);
    meta->size_on_disk = (u64)st->st_size;
}

static bool read_matches(int fd, const u8 *want, size_t len, bool *io_error)
{
    u8 block[64U * 1024U];
    size_t at = 0U;

    while (at < len) {
        size_t ask = len - at < sizeof(block) ? len - at : sizeof(block);
        ssize_t n = read(fd, block, ask);

        if (n > 0) {
            if (memcmp(block, want + at, (size_t)n) != 0)
                return false;
            at += (size_t)n;
        } else if (n == 0) {
            return false;
        } else if (errno != EINTR) {
            *io_error = true;
            return false;
        }
    }
    return true;
}

static bool content_matches_disk(const FileMeta *meta, const char *path,
                                 const struct stat *before, bool *io_error)
{
    TextIter it;
    struct stat after;
    u8 extra;
    int fd;
    bool ok = true;

    *io_error = false;
    if (!meta->disk_snapshot_valid)
        return false;
    fd = open(path, O_RDONLY
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0) {
        *io_error = true;
        return false;
    }
    if (meta->had_bom && !read_matches(fd, bom, sizeof(bom), io_error))
        ok = false;
    if (ok && yew_textsnap_iter(&it, &meta->disk_snapshot, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            if (!yew_textiter_chunk(&it, NULL, &bytes, &len) ||
                len > SIZE_MAX ||
                !read_matches(fd, bytes, (size_t)len, io_error)) {
                ok = false;
                break;
            }
        } while (yew_textiter_advance(&it, NULL));
    }
    if (ok) {
        ssize_t n;

        do {
            n = read(fd, &extra, 1U);
        } while (n < 0 && errno == EINTR);
        if (n != 0) {
            if (n < 0)
                *io_error = true;
            ok = false;
        }
    }
    if (fstat(fd, &after) != 0) {
        *io_error = true;
        ok = false;
    } else if (after.st_dev != before->st_dev ||
               after.st_ino != before->st_ino ||
               after.st_size != before->st_size ||
               !timespec_equal(stat_mtime(&after), stat_mtime(before))) {
        ok = false;
    }
    if (close(fd) != 0) {
        *io_error = true;
        ok = false;
    }
    return ok;
}

static YewSaveErr destination_matches(const FileMeta *meta, const char *path,
                                      const YewSaveOpts *opts,
                                      FileMeta *accepted,
                                      bool *needs_inplace)
{
    struct stat st;
    bool metadata_matches;

    *accepted = *meta;
    *needs_inplace = false;
    if (stat(path, &st) == 0) {
        bool io_error;

        if (!S_ISREG(st.st_mode) || st.st_size < 0)
            return YEW_SAVE_CHANGED_ON_DISK;
        metadata_matches = meta->exists && st.st_dev == meta->dev &&
                           st.st_ino == meta->ino &&
                           (u64)st.st_size == meta->size_on_disk &&
                           timespec_equal(stat_mtime(&st), meta->mtime);
        if (!metadata_matches && opts->check_disk == YEW_SAVE_CHECK_MTIME)
            return YEW_SAVE_CHANGED_ON_DISK;
        if (!metadata_matches && opts->check_disk == YEW_SAVE_CHECK_CONTENT) {
            if ((u64)st.st_size > opts->check_disk_max) {
                yew_log(YEW_LOG_INFO,
                        "content save check degraded to mtime: %llu bytes exceeds %llu",
                        (unsigned long long)st.st_size,
                        (unsigned long long)opts->check_disk_max);
                return YEW_SAVE_CHANGED_ON_DISK;
            }
            if (!content_matches_disk(meta, path, &st, &io_error))
                return io_error ? YEW_SAVE_IO : YEW_SAVE_CHANGED_ON_DISK;
        }
        if (!metadata_matches)
            meta_accept_stat(accepted, &st);
        *needs_inplace = st.st_nlink > 1;
        return YEW_SAVE_OK;
    }
    if (errno == ENOENT) {
        if (meta->exists && opts->check_disk != YEW_SAVE_CHECK_OFF)
            return YEW_SAVE_CHANGED_ON_DISK;
        accepted->exists = false;
        accepted->mode = 0666U;
        accepted->nlink = 0U;
        accepted->size_on_disk = 0U;
        return YEW_SAVE_OK;
    }
    return errno == EACCES || errno == EPERM ? YEW_SAVE_PERM : YEW_SAVE_IO;
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
    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return true;
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            return false;
        if (len > SIZE_MAX || !write_full(fd, bytes, (size_t)len))
            return false;
    } while (yew_textiter_advance(&it, tb));
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
    dir = yew_xmalloc(len + 1U);
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

void yew_file_save_opts_default(YewSaveOpts *opts)
{
    if (opts == NULL)
        YEW_BUG("yew_file_save_opts_default: NULL options");
    *opts = (YewSaveOpts){YEW_SAVE_STRATEGY_DEFAULT,
                          YEW_SAVE_CHECK_DISK_DEFAULT,
                          YEW_SAVE_CHECK_DISK_MAX_DEFAULT,
                          YEW_SAVE_BACKUP_KEEP_DEFAULT,
                          YEW_SAVE_BACKUP_DIR_DEFAULT};
}

static char *state_backup_dir(const char *configured)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home;
    const char *suffix;
    size_t n;
    char *dir;

    if (configured != NULL && configured[0] != '\0') {
        dir = string_copy(configured);
        if (!make_parent_dirs(dir) || !make_dir(dir)) {
            yew_xfree(dir);
            return NULL;
        }
        return dir;
    }
    if (state != NULL && state[0] != '\0') {
        suffix = "/yew/backup";
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return NULL;
        state = home;
        suffix = "/.local/state/yew/backup";
    }
    n = strlen(state) + strlen(suffix) + 1U;
    dir = yew_xmalloc(n);
    (void)snprintf(dir, n, "%s%s", state, suffix);
    if (!make_parent_dirs(dir) || !make_dir(dir)) {
        yew_xfree(dir);
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

static char *backup_path(const char *dst, const YewSaveOpts *opts)
{
    char *dir = state_backup_dir(opts->backup_dir);
    char resolved[PATH_MAX];
    const char *key;
    size_t n;
    char *path;

    if (dir == NULL)
        return NULL;
    key = realpath(dst, resolved) != NULL ? resolved : dst;
    n = strlen(dir) + 1U + 16U + strlen(".bak") + 1U;
    path = yew_xmalloc(n);
    (void)snprintf(path, n, "%s/%016llx.bak", dir,
                   (unsigned long long)fnv64(key));
    yew_xfree(dir);
    return path;
}

static char *numbered_backup_path(const char *base, u32 number)
{
    size_t base_len = strlen(base);
    size_t stem_len = base_len - strlen(".bak");
    size_t n = base_len + 32U;
    char *path = yew_xmalloc(n);

    (void)snprintf(path, n, "%.*s.%u.bak", (int)stem_len, base, number);
    return path;
}

static char *pending_backup_path(const char *base, bool primary)
{
    size_t base_len = strlen(base);
    size_t stem_len = base_len - strlen(".bak");
    size_t n = base_len + 80U;
    char *path = yew_xmalloc(n);
    if (primary) {
        (void)snprintf(path, n, "%.*s.pending.bak", (int)stem_len, base);
    } else {
        unsigned long long serial = (unsigned long long)++temp_counter;

        (void)snprintf(path, n, "%.*s.pending-%ld-%llu.bak", (int)stem_len,
                       base, (long)getpid(), serial);
    }
    return path;
}

static bool rotate_backups(const char *base, u32 keep)
{
    u32 number;

    if (keep <= 1U)
        return true;
    for (number = keep - 1U; number != 0U; number--) {
        char *from = number == 1U ? string_copy(base) :
                     numbered_backup_path(base, number - 1U);
        char *to = numbered_backup_path(base, number);

        if (rename(from, to) != 0 && errno != ENOENT) {
            yew_xfree(to);
            yew_xfree(from);
            return false;
        }
        yew_xfree(to);
        yew_xfree(from);
    }
    return true;
}

static bool prune_backups(const char *base, u32 keep)
{
    u32 number;
    bool ok = true;

    if (keep == 0U && unlink(base) != 0 && errno != ENOENT)
        ok = false;
    for (number = keep == 0U ? 1U : keep;
         number < YEW_SAVE_BACKUP_KEEP_MAX; number++) {
        char *path = numbered_backup_path(base, number);

        if (unlink(path) != 0 && errno != ENOENT)
            ok = false;
        yew_xfree(path);
    }
    return ok;
}

static bool discard_backup(const char *path)
{
    char *dir = path_dirname(path);
    bool ok = unlink(path) == 0 || errno == ENOENT;

    if (ok)
        ok = fsync_directory(dir);
    yew_xfree(dir);
    return ok;
}

typedef struct BackupRecovery {
    char *path;
} BackupRecovery;

static bool recovery_paths_dispose(BackupRecovery *recoveries, u32 count,
                                   bool unlink_paths)
{
    u32 i;
    bool ok = true;

    for (i = 0U; i < count; i++) {
        if (unlink_paths && unlink(recoveries[i].path) != 0 &&
            errno != ENOENT)
            ok = false;
        yew_xfree(recoveries[i].path);
        recoveries[i].path = NULL;
    }
    return ok;
}

static bool link_recovery_unique(const char *source, char **path_out)
{
    unsigned int tries;

    for (tries = 0U; tries < 1000U; tries++) {
        size_t n = strlen(source) + 80U;
        char *recovery = yew_xmalloc(n);

        if (tries == 0U) {
            (void)snprintf(recovery, n, "%s.recover.bak", source);
        } else {
            unsigned long long serial = (unsigned long long)++temp_counter;

            (void)snprintf(recovery, n, "%s.recover-%ld-%llu.bak", source,
                           (long)getpid(), serial);
        }
        if (link(source, recovery) == 0) {
            *path_out = recovery;
            return true;
        }
        yew_xfree(recovery);
        if (errno != EEXIST)
            return false;
    }
    errno = EEXIST;
    return false;
}

static bool preserve_rotation_history(const char *base, u32 keep,
                                      const char *dir,
                                      BackupRecovery *recoveries,
                                      u32 *recovery_count)
{
    u32 number;

    *recovery_count = 0U;
    for (number = 0U; number < keep; number++) {
        char *source = number == 0U ? string_copy(base) :
                       numbered_backup_path(base, number);
        struct stat st;

        if (lstat(source, &st) != 0) {
            int saved_errno = errno;

            yew_xfree(source);
            if (saved_errno == ENOENT)
                continue;
            errno = saved_errno;
            goto fail;
        }
        if (!S_ISREG(st.st_mode)) {
            if (S_ISLNK(st.st_mode)) {
                if (unlink(source) != 0) {
                    int saved_errno = errno;

                    yew_xfree(source);
                    errno = saved_errno;
                    goto fail;
                }
                yew_xfree(source);
                continue;
            }
            yew_xfree(source);
            errno = EINVAL;
            goto fail;
        }
        if (!link_recovery_unique(source,
                                  &recoveries[*recovery_count].path)) {
            int saved_errno = errno;

            yew_xfree(source);
            errno = saved_errno;
            goto fail;
        }
        (*recovery_count)++;
        yew_xfree(source);
    }
    if (!fsync_directory(dir))
        return false;
    return true;

fail:
    {
        int saved_errno = errno == 0 ? EIO : errno;

        (void)recovery_paths_dispose(recoveries, *recovery_count, true);
        *recovery_count = 0U;
        (void)fsync_directory(dir);
        errno = saved_errno;
    }
    return false;
}

static bool commit_backup_rotation(const char *pending, const char *base,
                                   u32 keep)
{
    BackupRecovery recoveries[YEW_SAVE_BACKUP_KEEP_MAX];
    char *dir = path_dirname(base);
    u32 recovery_count = 0U;
    bool ok = true;

    (void)memset(recoveries, 0, sizeof(recoveries));
    if (keep == 0U) {
        ok = unlink(pending) == 0 || errno == ENOENT;
    } else {
        ok = preserve_rotation_history(base, keep, dir, recoveries,
                                       &recovery_count);
        if (ok && (!rotate_backups(base, keep) ||
                   rename(pending, base) != 0))
            ok = false;
    }
    if (ok) {
        bool pruned = prune_backups(base, keep);
        bool synced = fsync_directory(dir);

        ok = pruned && synced;
    }
    if (ok && recovery_count != 0U) {
        ok = recovery_paths_dispose(recoveries, recovery_count, true);
        recovery_count = 0U;
        if (!fsync_directory(dir))
            ok = false;
    }
    if (recovery_count != 0U)
        (void)recovery_paths_dispose(recoveries, recovery_count, false);
    yew_xfree(dir);
    return ok;
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
    /* Publish without replacing stale recovery evidence from a reused PID. */
    if (link(tmp, bak_path) != 0)
        goto done;
    if (unlink(tmp) != 0)
        goto done;
    yew_xfree(tmp);
    tmp = NULL;
    if (!fsync_directory(dir))
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
    yew_xfree(tmp);
    yew_xfree(dir);
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

static YewSaveErr inplace_save(const TextBuf *tb, const FileMeta *meta,
                               const char *dst, const YewSaveOpts *opts,
                               struct stat *saved_st)
{
    char *bak;
    char *pending;
    struct stat st;
    int fd;
    off_t end;
    bool ok;
    bool backup_copied = false;

    if (!meta->exists) {
        char *dir;

        fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666U);
        if (fd < 0)
            return errno == EACCES || errno == EPERM ? YEW_SAVE_PERM
                                                      : YEW_SAVE_IO;
        ok = write_text(fd, tb, meta->had_bom);
        end = ok ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
        if (end < 0 || ftruncate(fd, end) != 0 || !fsync_full(fd) ||
            fstat(fd, saved_st) != 0)
            ok = false;
        if (close(fd) != 0)
            ok = false;
        dir = path_dirname(dst);
        if (ok && !fsync_directory(dir))
            ok = false;
        yew_xfree(dir);
        if (!ok) {
            (void)unlink(dst);
            return YEW_SAVE_IO;
        }
        return YEW_SAVE_OK;
    }
    bak = backup_path(dst, opts);
    if (bak == NULL)
        return YEW_SAVE_IO;
    pending = NULL;
    {
        unsigned int tries;

        for (tries = 0U; tries < 1000U; tries++) {
            pending = pending_backup_path(bak, tries == 0U);
            if (copy_path_to_backup(dst, pending, meta)) {
                backup_copied = true;
                break;
            }
            if (errno != EEXIST)
                break;
            yew_xfree(pending);
            pending = NULL;
        }
    }
    if (!backup_copied) {
        int saved_errno = errno;

        yew_xfree(pending);
        yew_xfree(bak);
        if (saved_errno == ESTALE)
            return YEW_SAVE_CHANGED_ON_DISK;
        return saved_errno == EACCES || saved_errno == EPERM ? YEW_SAVE_PERM
                                                              : YEW_SAVE_IO;
    }
    fd = open(dst, O_WRONLY
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0) {
        int saved_errno = errno;

        (void)discard_backup(pending);
        yew_xfree(pending);
        yew_xfree(bak);
        return saved_errno == EACCES || saved_errno == EPERM
                   ? YEW_SAVE_PERM
                   : YEW_SAVE_IO;
    }
    if (fstat(fd, &st) != 0 || !stat_matches_meta(&st, meta)) {
        (void)close(fd);
        (void)discard_backup(pending);
        yew_xfree(pending);
        yew_xfree(bak);
        return YEW_SAVE_CHANGED_ON_DISK;
    }
    ok = write_text(fd, tb, meta->had_bom);
    end = ok ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
    if (end < 0 || ftruncate(fd, end) != 0 || !fsync_full(fd) ||
        fstat(fd, saved_st) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        if (restore_backup(pending, dst))
            (void)discard_backup(pending);
        yew_xfree(pending);
        yew_xfree(bak);
        return YEW_SAVE_IO;
    }
    if (!commit_backup_rotation(pending, bak, opts->backup_keep)) {
        yew_log(YEW_LOG_WARN,
                "save completed but backup retention failed; recovery evidence was preserved where possible");
        yew_xfree(pending);
        yew_xfree(bak);
        return YEW_SAVE_BACKUP_FAILED;
    }
    yew_xfree(pending);
    yew_xfree(bak);
    return YEW_SAVE_OK;
}

#ifdef __linux__
static void copy_xattrs(const char *src, int dst)
{
    ssize_t names_len = listxattr(src, NULL, 0U);
    char *names;
    char *name;

    if (names_len <= 0)
        return;
    names = yew_xmalloc((size_t)names_len);
    names_len = listxattr(src, names, (size_t)names_len);
    if (names_len <= 0) {
        yew_xfree(names);
        return;
    }
    name = names;
    while (name < names + names_len) {
        size_t name_len = strlen(name) + 1U;
        ssize_t value_len = getxattr(src, name, NULL, 0U);

        if (value_len >= 0) {
            void *value = yew_xmalloc((size_t)value_len);

            value_len = getxattr(src, name, value, (size_t)value_len);
            if (value_len >= 0)
                (void)fsetxattr(dst, name, value, (size_t)value_len, 0);
            yew_xfree(value);
        }
        name += name_len;
    }
    yew_xfree(names);
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
        char *path = yew_xmalloc(n);
        int flags = O_WRONLY | O_CREAT | O_EXCL;
        int fd;

        (void)snprintf(path, n, "%s/.yew-%s-%ld-%llu", dir, base,
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
        yew_xfree(path);
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

YewAtomicWriteResult yew_file_write_atomic_result(const char *path,
                                                  const u8 *bytes,
                                                  size_t len, mode_t mode)
{
    YewAtomicWriteResult result = {YEW_SAVE_IO, false};
    char *dir;
    char *tmp = NULL;
    int fd;
    int saved_errno;

    if (path == NULL || (bytes == NULL && len != 0U)) {
        errno = EINVAL;
        return result;
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
    if (rename(tmp, path) != 0)
        goto fail;
    result.committed = true;
    if (!fsync_directory(dir))
        goto fail;
    yew_xfree(tmp);
    yew_xfree(dir);
    result.error = YEW_SAVE_OK;
    return result;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    if (fd >= 0)
        (void)close(fd);
    if (tmp != NULL)
        (void)unlink(tmp);
    yew_xfree(tmp);
    yew_xfree(dir);
    errno = saved_errno;
    result.error = saved_errno == EACCES || saved_errno == EPERM
                       ? YEW_SAVE_PERM
                       : YEW_SAVE_IO;
    return result;
}

YewSaveErr yew_file_write_atomic(const char *path, const u8 *bytes,
                                 size_t len, mode_t mode)
{
    return yew_file_write_atomic_result(path, bytes, len, mode).error;
}

bool yew_save_committed(YewSaveErr error)
{
    return error == YEW_SAVE_OK || error == YEW_SAVE_BACKUP_FAILED;
}

YewSaveErr yew_file_move_aside(const char *from, const char *to)
{
    if (from == NULL || to == NULL) {
        errno = EINVAL;
        return YEW_SAVE_IO;
    }
    /*
     * link + unlink, NOT rename.
     *
     * rename(2) replaces the destination without a word.  The caller is
     * moving a file precisely because it may still matter to somebody,
     * and its name carries a one-second timestamp — so two of them in
     * the same second is not hypothetical, and the loser would be gone
     * with no record that it ever existed.  link fails with EEXIST
     * instead, which is the answer the caller can act on.
     */
    if (link(from, to) != 0) {
        return errno == EACCES || errno == EPERM ? YEW_SAVE_PERM
                                                 : YEW_SAVE_IO;
    }
    if (unlink(from) != 0) {
        /* The copy is in place; failing to remove the original leaves
         * two names for one inode, which is untidy but loses nothing. */
        return YEW_SAVE_IO;
    }
    return YEW_SAVE_OK;
}

static YewSaveErr atomic_save(const TextBuf *tb, const FileMeta *meta,
                              const char *dst, const YewSaveOpts *opts,
                              bool force_atomic, struct stat *saved_st)
{
    char *dir = path_dirname(dst);
    char *tmp = NULL;
    mode_t mode = meta->exists ? meta->mode & 07777U : 0666U;
    int fd = open_temp(dir, path_basename(dst), mode, &tmp);
    bool foreign_owner;
    int saved_errno;
    YewSaveErr match;
    bool needs_inplace;
    FileMeta accepted;

    if (fd < 0) {
        saved_errno = errno;
        yew_xfree(dir);
        return saved_errno == EACCES || saved_errno == EPERM
                   ? YEW_SAVE_PERM
                   : YEW_SAVE_IO;
    }
    if (!write_text(fd, tb, meta->had_bom) || !fsync_full(fd))
        goto fail;
    if (meta->exists) {
        foreign_owner = meta->uid != geteuid() || meta->gid != getegid();
        if (fchown(fd, meta->uid, meta->gid) != 0 && foreign_owner) {
            (void)close(fd);
            (void)unlink(tmp);
            yew_xfree(tmp);
            yew_xfree(dir);
            return force_atomic ? YEW_SAVE_IO :
                   inplace_save(tb, meta, dst, opts, saved_st);
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
    match = destination_matches(meta, dst, opts, &accepted, &needs_inplace);
    if (match != YEW_SAVE_OK) {
        (void)unlink(tmp);
        yew_xfree(tmp);
        yew_xfree(dir);
        return match;
    }
    if (needs_inplace && !force_atomic) {
        (void)unlink(tmp);
        yew_xfree(tmp);
        yew_xfree(dir);
        return inplace_save(tb, &accepted, dst, opts, saved_st);
    }
    if (!commit_temp(tmp, dst, dir)) {
        saved_errno = errno;
        (void)unlink(tmp);
        yew_xfree(tmp);
        yew_xfree(dir);
        if (saved_errno == EXDEV && !force_atomic)
            return inplace_save(tb, &accepted, dst, opts, saved_st);
        return saved_errno == EACCES || saved_errno == EPERM
                   ? YEW_SAVE_PERM
                   : YEW_SAVE_IO;
    }
    yew_xfree(tmp);
    yew_xfree(dir);
    return YEW_SAVE_OK;

fail:
    saved_errno = errno;
    if (fd >= 0)
        (void)close(fd);
    (void)unlink(tmp);
    yew_xfree(tmp);
    yew_xfree(dir);
    return saved_errno == EACCES || saved_errno == EPERM ? YEW_SAVE_PERM
                                                          : YEW_SAVE_IO;
}

static void refresh_saved_meta(FileMeta *meta, const struct stat *st,
                               char *saved_path, bool via_symlink)
{
    yew_xfree(meta->realpath);
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

static YewSaveErr accept_destination(FileMeta *accepted, const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_size < 0)
            return YEW_SAVE_IO;
        accepted->exists = true;
        accepted->mode = st.st_mode;
        accepted->uid = st.st_uid;
        accepted->gid = st.st_gid;
        accepted->nlink = st.st_nlink;
        accepted->dev = st.st_dev;
        accepted->ino = st.st_ino;
        accepted->mtime = stat_mtime(&st);
        accepted->size_on_disk = (u64)st.st_size;
        return YEW_SAVE_OK;
    }
    if (errno != ENOENT)
        return errno == EACCES || errno == EPERM ? YEW_SAVE_PERM
                                                  : YEW_SAVE_IO;
    accepted->exists = false;
    accepted->mode = 0666U;
    accepted->uid = 0;
    accepted->gid = 0;
    accepted->nlink = 0;
    accepted->dev = 0;
    accepted->ino = 0;
    accepted->mtime = (struct timespec){0, 0};
    accepted->size_on_disk = 0U;
    return YEW_SAVE_OK;
}

static YewSaveErr file_save(const TextBuf *tb, FileMeta *meta,
                            const char *path, const YewSaveOpts *requested,
                            bool force)
{
    struct stat link_st;
    struct stat saved_st;
    FileMeta accepted;
    const FileMeta *expected = meta;
    char *resolved = NULL;
    char *saved_realpath;
    const char *dst = path;
    char *dir;
    bool needs_inplace;
    bool is_symlink = false;
    YewSaveErr match;
    YewSaveErr result;
    YewSaveOpts defaults;
    YewSaveOpts effective;
    const YewSaveOpts *opts;

    if (tb == NULL || meta == NULL || path == NULL)
        YEW_BUG("yew_file_save: NULL argument");
    if (requested == NULL) {
        yew_file_save_opts_default(&defaults);
        opts = &defaults;
    } else {
        effective = *requested;
        opts = &effective;
    }
    if (opts->strategy > YEW_SAVE_STRATEGY_INPLACE ||
        opts->check_disk > YEW_SAVE_CHECK_CONTENT ||
        opts->check_disk_max > YEW_SAVE_CHECK_DISK_MAX_LIMIT ||
        opts->backup_keep > YEW_SAVE_BACKUP_KEEP_MAX)
        return YEW_SAVE_IO;
    if (force) {
        effective = *opts;
        effective.check_disk = YEW_SAVE_CHECK_OFF;
        opts = &effective;
    }
    if (lstat(path, &link_st) == 0) {
        is_symlink = S_ISLNK(link_st.st_mode);
    } else if (errno != ENOENT) {
        return errno == EACCES || errno == EPERM ? YEW_SAVE_PERM
                                                  : YEW_SAVE_IO;
    }
    if (is_symlink) {
        resolved = resolve_save_target(path);
        if (resolved == NULL)
            return YEW_SAVE_IO;
        if (!force && meta->realpath != NULL &&
            strcmp(resolved, meta->realpath) != 0) {
            yew_xfree(resolved);
            return YEW_SAVE_CHANGED_ON_DISK;
        }
        dst = resolved;
    }
    if (force) {
        accepted = *meta;
        match = accept_destination(&accepted, dst);
        if (match != YEW_SAVE_OK) {
            yew_xfree(resolved);
            return match;
        }
        expected = &accepted;
    }
    match = destination_matches(expected, dst, opts, &accepted,
                                &needs_inplace);
    if (match != YEW_SAVE_OK) {
        yew_xfree(resolved);
        return match;
    }
    expected = &accepted;
    saved_realpath = yew_xrealpath(dst);
    if (saved_realpath == NULL)
        saved_realpath = canonical_new_path(dst);
    dir = path_dirname(dst);
    if (opts->strategy == YEW_SAVE_STRATEGY_INPLACE)
        result = inplace_save(tb, expected, dst, opts, &saved_st);
    else if (opts->strategy == YEW_SAVE_STRATEGY_ATOMIC)
        result = atomic_save(tb, expected, dst, opts, true, &saved_st);
    else if (needs_inplace || !directory_writable(dir))
        result = inplace_save(tb, expected, dst, opts, &saved_st);
    else
        result = atomic_save(tb, expected, dst, opts, false, &saved_st);
    if (yew_save_committed(result)) {
        refresh_saved_meta(meta, &saved_st, saved_realpath, is_symlink);
        filemeta_snapshot_set(meta, tb);
        saved_realpath = NULL;
    }
    yew_xfree(saved_realpath);
    yew_xfree(dir);
    yew_xfree(resolved);
    return result;
}

YewSaveErr yew_file_save(const TextBuf *tb, FileMeta *meta,
                         const char *path)
{
    return file_save(tb, meta, path, NULL, false);
}

YewSaveErr yew_file_save_opts(const TextBuf *tb, FileMeta *meta,
                              const char *path, const YewSaveOpts *opts)
{
    return file_save(tb, meta, path, opts, false);
}

YewSaveErr yew_file_save_force(const TextBuf *tb, FileMeta *meta,
                               const char *path)
{
    return file_save(tb, meta, path, NULL, true);
}

YewSaveErr yew_file_save_force_opts(const TextBuf *tb, FileMeta *meta,
                                    const char *path,
                                    const YewSaveOpts *opts)
{
    return file_save(tb, meta, path, opts, true);
}
