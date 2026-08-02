#define _XOPEN_SOURCE 700

#include "text/journal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/log.h"
#include "text/edit.h"

#define SAG_JOURNAL_VERSION 1U
#define SAG_JOURNAL_HEADER_FIXED 36U
#define SAG_JOURNAL_RECORD_FIXED 21U

struct Journal {
    int fd;
    char *path;
    char *dir;
    bool failed;
    struct Journal *next;
};

static u32 crc32_table[256];
static bool crc32_ready;
static Journal *open_journals;

static int adopt_existing_journal(const char *path, const char *realpath,
                                  const FileMeta *meta);

static bool journal_path_owned(const char *path)
{
    const Journal *journal;

    for (journal = open_journals; journal != NULL; journal = journal->next) {
        if (strcmp(journal->path, path) == 0)
            return true;
    }
    return false;
}

static void journal_unregister(Journal *journal)
{
    Journal **at = &open_journals;

    while (*at != NULL && *at != journal)
        at = &(*at)->next;
    if (*at == journal)
        *at = journal->next;
}

static bool size_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static void put_u32_le(u8 out[4], u32 value)
{
    out[0] = (u8)value;
    out[1] = (u8)(value >> 8U);
    out[2] = (u8)(value >> 16U);
    out[3] = (u8)(value >> 24U);
}

static void put_u64_le(u8 out[8], u64 value)
{
    unsigned int i;

    for (i = 0U; i < 8U; i++) {
        out[i] = (u8)(value >> (i * 8U));
    }
}

static u32 get_u32_le(const u8 *in)
{
    return (u32)in[0] | ((u32)in[1] << 8U) |
           ((u32)in[2] << 16U) | ((u32)in[3] << 24U);
}

static u64 get_u64_le(const u8 *in)
{
    u64 value = 0U;
    unsigned int i;

    for (i = 0U; i < 8U; i++) {
        value |= (u64)in[i] << (i * 8U);
    }
    return value;
}

static void crc32_init(void)
{
    u32 i;

    if (crc32_ready) {
        return;
    }
    for (i = 0U; i < 256U; i++) {
        u32 value = i;
        unsigned int bit;

        for (bit = 0U; bit < 8U; bit++) {
            value = (value >> 1U) ^
                    (0xedb88320U & (u32)-(i32)(value & 1U));
        }
        crc32_table[i] = value;
    }
    crc32_ready = true;
}

u32 sag_crc32_begin(void)
{
    crc32_init();
    return UINT32_MAX;
}

u32 sag_crc32_add(u32 crc, const u8 *bytes, size_t len)
{
    size_t i;

    if (bytes == NULL && len != 0U)
        SAG_BUG("sag_crc32_add: NULL bytes");
    for (i = 0U; i < len; i++) {
        crc = crc32_table[(crc ^ bytes[i]) & 0xffU] ^ (crc >> 8U);
    }
    return crc;
}

u32 sag_crc32_end(u32 crc)
{
    return crc ^ UINT32_MAX;
}

u32 sag_crc32(const u8 *bytes, size_t len)
{
    return sag_crc32_end(sag_crc32_add(sag_crc32_begin(), bytes, len));
}

static u64 fnv64(const char *path)
{
    const u8 *p = (const u8 *)path;
    u64 hash = UINT64_C(14695981039346656037);

    while (*p != 0U) {
        hash ^= *p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool make_dir(const char *path)
{
    struct stat st;

    if (mkdir(path, 0700) == 0) {
        return true;
    }
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool make_dirs(char *path)
{
    char *at;

    for (at = path + 1; *at != '\0'; at++) {
        if (*at == '/') {
            *at = '\0';
            if (!make_dir(path)) {
                *at = '/';
                return false;
            }
            *at = '/';
        }
    }
    return make_dir(path);
}

static char *journal_dir(void)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *suffix;
    size_t len;
    char *dir;

    if (state != NULL && state[0] != '\0') {
        suffix = "/sagitta/journal";
    } else {
        state = getenv("HOME");
        if (state == NULL || state[0] == '\0') {
            return NULL;
        }
        suffix = "/.local/state/sagitta/journal";
    }
    if (!size_add(strlen(state), strlen(suffix) + 1U, &len)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    dir = sag_xmalloc(len);
    (void)snprintf(dir, len, "%s%s", state, suffix);
    return dir;
}

static char *journal_path(const char *dir, const char *realpath)
{
    size_t len;
    char *path;

    if (!size_add(strlen(dir), 1U + 16U + sizeof(".sagj"), &len)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    path = sag_xmalloc(len);
    (void)snprintf(path, len, "%s/%016" PRIx64 ".sagj", dir,
                   fnv64(realpath));
    return path;
}

static bool write_all(int fd, const u8 *bytes, size_t len)
{
    while (len != 0U) {
        ssize_t written = write(fd, bytes, len);

        if (written > 0) {
            bytes += (size_t)written;
            len -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool read_all(int fd, u8 *bytes, size_t len)
{
    while (len != 0U) {
        ssize_t count = read(fd, bytes, len);

        if (count > 0) {
            bytes += (size_t)count;
            len -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool fsync_retry(int fd)
{
    int result;

    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

static bool lock_journal(int fd)
{
    struct flock lock;

    (void)memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLK, &lock) == 0;
}

static bool fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);
    bool ok = false;

    if (fd >= 0) {
        ok = fsync_retry(fd);
        if (close(fd) != 0)
            ok = false;
    }
    return ok;
}

static bool write_header(int fd, const char *realpath, const FileMeta *meta)
{
    size_t path_len = strlen(realpath);
    u8 fixed[SAG_JOURNAL_HEADER_FIXED];

    if ((u64)path_len != path_len) {
        errno = EOVERFLOW;
        return false;
    }
    (void)memcpy(fixed, "SAGJ", 4U);
    put_u32_le(fixed + 4U, SAG_JOURNAL_VERSION);
    put_u64_le(fixed + 8U, (u64)path_len);
    put_u64_le(fixed + 16U, meta->size_on_disk);
    put_u64_le(fixed + 24U, (u64)(i64)meta->mtime.tv_sec);
    put_u32_le(fixed + 32U, (u32)meta->mtime.tv_nsec);
    return write_all(fd, fixed, 16U) &&
           write_all(fd, (const u8 *)realpath, path_len) &&
           write_all(fd, fixed + 16U, SAG_JOURNAL_HEADER_FIXED - 16U);
}

Journal *sag_journal_open(const char *realpath, const FileMeta *m)
{
    Journal *journal;
    char *dir;
    char *path;
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_APPEND;
    int fd;
    bool created;

    if (realpath == NULL || realpath[0] == '\0' || m == NULL) {
        errno = EINVAL;
        return NULL;
    }
    dir = journal_dir();
    if (dir == NULL || !make_dirs(dir)) {
        sag_log(SAG_LOG_ERROR, "cannot create crash journal directory: %s",
                strerror(errno));
        free(dir);
        return NULL;
    }
    path = journal_path(dir, realpath);
    if (path == NULL) {
        free(dir);
        return NULL;
    }
    if (journal_path_owned(path)) {
        free(path);
        free(dir);
        errno = EBUSY;
        return NULL;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags, 0600);
    created = fd >= 0;
    if (!created && errno == EEXIST)
        fd = adopt_existing_journal(path, realpath, m);
    if (fd < 0) {
        sag_log(SAG_LOG_ERROR, "cannot open crash journal %s: %s", path,
                strerror(errno));
        free(path);
        free(dir);
        return NULL;
    }
    if (created && !lock_journal(fd)) {
        int saved_errno = errno;

        (void)close(fd);
        (void)unlink(path);
        (void)fsync_dir(dir);
        free(path);
        free(dir);
        errno = saved_errno;
        return NULL;
    }
    if (created && (!write_header(fd, realpath, m) || !fsync_retry(fd) ||
                    !fsync_dir(dir))) {
        int saved_errno = errno;

        (void)close(fd);
        (void)unlink(path);
        free(path);
        free(dir);
        errno = saved_errno;
        return NULL;
    }
    journal = sag_xcalloc(1U, sizeof(*journal));
    journal->fd = fd;
    journal->path = path;
    journal->dir = dir;
    journal->next = open_journals;
    open_journals = journal;
    return journal;
}

bool sag_journal_record(Journal *j, u8 op, u64 off, const u8 *b, u64 n)
{
    u8 fixed[17];
    u8 encoded_crc[4];
    u32 crc;

    if (j == NULL || j->failed) {
        errno = EIO;
        return false;
    }
    if ((op != SAG_JOURNAL_INS && op != SAG_JOURNAL_DEL) ||
        (n != 0U && b == NULL) || n > SIZE_MAX) {
        j->failed = true;
        errno = EINVAL;
        sag_log(SAG_LOG_ERROR, "invalid crash journal record");
        return false;
    }
    fixed[0] = op;
    put_u64_le(fixed + 1U, off);
    put_u64_le(fixed + 9U, n);
    crc = sag_crc32_begin();
    crc = sag_crc32_add(crc, fixed, sizeof(fixed));
    crc = sag_crc32_add(crc, b, (size_t)n);
    put_u32_le(encoded_crc, sag_crc32_end(crc));
    if (!write_all(j->fd, fixed, sizeof(fixed)) ||
        !write_all(j->fd, b, (size_t)n) ||
        !write_all(j->fd, encoded_crc, sizeof(encoded_crc))) {
        j->failed = true;
        sag_log(SAG_LOG_ERROR, "cannot append crash journal %s: %s", j->path,
                strerror(errno));
        return false;
    }
    return true;
}

bool sag_journal_sync(Journal *j)
{
    if (j == NULL || j->failed) {
        errno = EIO;
        return false;
    }
    if (!fsync_retry(j->fd)) {
        j->failed = true;
        sag_log(SAG_LOG_ERROR, "cannot sync crash journal %s: %s", j->path,
                strerror(errno));
        return false;
    }
    return true;
}

bool sag_journal_ok(const Journal *j)
{
    return j != NULL && !j->failed;
}

void sag_journal_close(Journal *j)
{
    if (j == NULL)
        return;
    journal_unregister(j);
    if (close(j->fd) != 0)
        sag_log(SAG_LOG_ERROR, "cannot close crash journal %s: %s", j->path,
                strerror(errno));
    free(j->path);
    free(j->dir);
    free(j);
}

void sag_journal_discard(Journal *j)
{
    int saved_errno = 0;

    if (j == NULL) {
        return;
    }
    journal_unregister(j);
    if (unlink(j->path) < 0 && errno != ENOENT) {
        saved_errno = errno;
    } else {
        (void)fsync_dir(j->dir);
    }
    if (close(j->fd) < 0 && saved_errno == 0)
        saved_errno = errno;
    if (saved_errno != 0) {
        sag_log(SAG_LOG_ERROR, "cannot discard crash journal %s: %s", j->path,
                strerror(saved_errno));
    }
    free(j->path);
    free(j->dir);
    free(j);
}

static bool buffer_matches(const TextBuf *tb, u64 off, const u8 *bytes,
                           u64 len)
{
    TextIter it;
    u64 left = len;

    if (!sag_textiter_begin(&it, tb, (ByteOff){off})) {
        return len == 0U;
    }
    while (left != 0U) {
        const u8 *chunk;
        u64 chunk_len;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &chunk, &chunk_len)) {
            return false;
        }
        take = chunk_len < left ? chunk_len : left;
        if (memcmp(chunk, bytes, (size_t)take) != 0) {
            return false;
        }
        bytes += (size_t)take;
        left -= take;
        if (left != 0U && !sag_textiter_advance(&it, tb)) {
            return false;
        }
    }
    return true;
}

static bool apply_record(EditCtx *ec, u8 op, u64 off, const u8 *bytes,
                         u64 len)
{
    TextBuf *tb = ec->tb;
    u64 total = sag_textbuf_len(tb);

    if (off > total) {
        return false;
    }
    if (op == SAG_JOURNAL_INS && len <= UINT64_MAX - total) {
        return sag_edit_insert(ec, (ByteOff){off}, bytes, len);
    }
    if (op == SAG_JOURNAL_DEL && len <= total - off &&
        buffer_matches(tb, off, bytes, len)) {
        return sag_edit_delete(ec, (Span){off, off + len});
    }
    return false;
}

static void stale_journal(int fd, const char *path, const char *dir)
{
    struct stat fd_st;
    struct stat path_st;
    size_t len;
    char *stale;
    unsigned int suffix = 0U;

    if (fstat(fd, &fd_st) != 0 || lstat(path, &path_st) != 0 ||
        !S_ISREG(path_st.st_mode) || fd_st.st_dev != path_st.st_dev ||
        fd_st.st_ino != path_st.st_ino ||
        !size_add(strlen(path), sizeof(".stale") + 12U, &len)) {
        return;
    }
    stale = sag_xmalloc(len);
    for (;;) {
        if (suffix == 0U) {
            (void)snprintf(stale, len, "%s.stale", path);
        } else {
            (void)snprintf(stale, len, "%s.stale.%u", path, suffix);
        }
        suffix++;
        if (link(path, stale) == 0)
            break;
        if (errno != EEXIST || suffix == 0U) {
            sag_log(SAG_LOG_ERROR,
                    "cannot preserve stale crash journal %s: %s", path,
                    strerror(errno));
            free(stale);
            return;
        }
    }
    if (lstat(path, &path_st) != 0 || path_st.st_dev != fd_st.st_dev ||
        path_st.st_ino != fd_st.st_ino) {
        sag_log(SAG_LOG_ERROR,
                "crash journal identity changed while preserving %s", path);
        (void)unlink(stale);
    } else if (unlink(path) == 0) {
        fsync_dir(dir);
        sag_log(SAG_LOG_WARN, "renamed stale crash journal to %s", stale);
    } else {
        sag_log(SAG_LOG_ERROR, "cannot preserve stale crash journal %s: %s",
                path, strerror(errno));
    }
    free(stale);
}

static bool header_matches(const u8 *data, size_t size, const char *realpath,
                           const FileMeta *meta, size_t *records_at)
{
    u64 path_len;
    size_t path_size;
    size_t tail_at;

    if (size < 16U || memcmp(data, "SAGJ", 4U) != 0 ||
        get_u32_le(data + 4U) != SAG_JOURNAL_VERSION) {
        return false;
    }
    path_len = get_u64_le(data + 8U);
    if (path_len > SIZE_MAX) {
        return false;
    }
    path_size = (size_t)path_len;
    if (!size_add(16U, path_size, &tail_at) ||
        tail_at > size || size - tail_at < 20U) {
        return false;
    }
    if (strlen(realpath) != path_size ||
        memcmp(data + 16U, realpath, path_size) != 0 ||
        get_u64_le(data + tail_at) != meta->size_on_disk ||
        get_u64_le(data + tail_at + 8U) !=
            (u64)(i64)meta->mtime.tv_sec ||
        get_u32_le(data + tail_at + 16U) != (u32)meta->mtime.tv_nsec) {
        return false;
    }
    *records_at = tail_at + 20U;
    return true;
}

static bool journal_fd_valid(int fd, struct stat *st)
{
    if (fstat(fd, st) != 0)
        return false;
    if (!S_ISREG(st->st_mode) || st->st_uid != geteuid() ||
        st->st_nlink != 1 || (st->st_mode & 0077U) != 0U ||
        st->st_size < 0 || (u64)st->st_size > SIZE_MAX) {
        errno = EPERM;
        return false;
    }
    return true;
}

static size_t valid_record_prefix(const u8 *data, size_t size, size_t at)
{
    while (size - at >= SAG_JOURNAL_RECORD_FIXED) {
        const u8 *record = data + at;
        u64 payload_len = get_u64_le(record + 9U);
        size_t payload_size;
        size_t record_size;
        u32 expected;
        u32 actual;

        if ((record[0] != SAG_JOURNAL_INS &&
             record[0] != SAG_JOURNAL_DEL) ||
            payload_len > SIZE_MAX)
            break;
        payload_size = (size_t)payload_len;
        if (!size_add(17U, payload_size, &record_size) ||
            !size_add(record_size, 4U, &record_size) ||
            record_size > size - at)
            break;
        expected = get_u32_le(record + 17U + payload_size);
        actual = sag_crc32(record, 17U + payload_size);
        if (actual != expected)
            break;
        at += record_size;
    }
    return at;
}

static int adopt_existing_journal(const char *path, const char *realpath,
                                  const FileMeta *meta)
{
    struct stat st;
    u8 *data = NULL;
    size_t size;
    size_t records_at;
    size_t prefix;
    int flags = O_RDWR | O_APPEND;
    int fd;
    int saved_errno;

#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0)
        return -1;
    if (!lock_journal(fd))
        goto fail;
    if (!journal_fd_valid(fd, &st))
        goto fail;
    size = (size_t)st.st_size;
    data = sag_xmalloc(size == 0U ? 1U : size);
    if (!read_all(fd, data, size))
        goto fail;
    if (!header_matches(data, size, realpath, meta, &records_at)) {
        errno = ESTALE;
        goto fail;
    }
    prefix = valid_record_prefix(data, size, records_at);
    if (prefix != size &&
        (ftruncate(fd, (off_t)prefix) != 0 || !fsync_retry(fd)))
        goto fail;
    if (lseek(fd, 0, SEEK_END) < 0)
        goto fail;
    if (prefix != size)
        sag_log(SAG_LOG_WARN,
                "discarded incomplete crash journal tail while adopting %s",
                path);
    free(data);
    return fd;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    free(data);
    (void)close(fd);
    errno = saved_errno;
    return -1;
}

static const char *replay_realpath(const char *path, const FileMeta *meta,
                                   char **owned)
{
    if (meta->realpath != NULL && meta->realpath[0] != '\0') {
        return meta->realpath;
    }
    *owned = realpath(path, NULL);
    return *owned != NULL ? *owned : path;
}

bool sag_journal_probe(const char *path, const FileMeta *m)
{
    const char *canonical;
    char *owned = NULL;
    char *dir = NULL;
    char *jpath = NULL;
    struct stat st;
    u8 *data = NULL;
    size_t size;
    size_t records_at;
    bool matched = false;
    int fd = -1;

    if (path == NULL || m == NULL) {
        errno = EINVAL;
        return false;
    }
    canonical = replay_realpath(path, m, &owned);
    dir = journal_dir();
    if (dir == NULL)
        goto done;
    jpath = journal_path(dir, canonical);
    if (jpath == NULL)
        goto done;
    if (journal_path_owned(jpath)) {
        errno = EBUSY;
        goto done;
    }
    fd = open(jpath, O_RDONLY
#ifdef O_CLOEXEC
              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0 || !journal_fd_valid(fd, &st))
        goto done;
    size = (size_t)st.st_size;
    data = sag_xmalloc(size == 0U ? 1U : size);
    if (read_all(fd, data, size))
        matched = header_matches(data, size, canonical, m, &records_at);

done:
    if (fd >= 0)
        (void)close(fd);
    free(data);
    free(jpath);
    free(dir);
    free(owned);
    return matched;
}

bool sag_journal_discard_path(const char *path, const FileMeta *m)
{
    const char *canonical;
    char *owned = NULL;
    char *dir = NULL;
    char *jpath = NULL;
    struct stat st;
    struct stat path_st;
    u8 *data = NULL;
    size_t size;
    size_t records_at;
    bool discarded = false;
    int fd = -1;

    if (path == NULL || m == NULL) {
        errno = EINVAL;
        return false;
    }
    canonical = replay_realpath(path, m, &owned);
    dir = journal_dir();
    if (dir == NULL)
        goto done;
    jpath = journal_path(dir, canonical);
    if (jpath == NULL)
        goto done;
    if (journal_path_owned(jpath)) {
        errno = EBUSY;
        goto done;
    }
    fd = open(jpath, O_RDWR
#ifdef O_CLOEXEC
              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0 || !lock_journal(fd) || !journal_fd_valid(fd, &st))
        goto done;
    size = (size_t)st.st_size;
    data = sag_xmalloc(size == 0U ? 1U : size);
    if (!read_all(fd, data, size) ||
        !header_matches(data, size, canonical, m, &records_at))
        goto done;
    if (lstat(jpath, &path_st) != 0 || path_st.st_dev != st.st_dev ||
        path_st.st_ino != st.st_ino) {
        errno = ESTALE;
        goto done;
    }
    if (unlink(jpath) == 0 && fsync_dir(dir))
        discarded = true;

done:
    if (fd >= 0)
        (void)close(fd);
    free(data);
    free(jpath);
    free(dir);
    free(owned);
    return discarded;
}

static bool journal_replay_edit(const char *path, EditCtx *ec, FileMeta *m,
                                bool external_transaction)
{
    const char *canonical;
    char *owned = NULL;
    char *dir = NULL;
    char *jpath = NULL;
    struct stat st;
    u8 *data = NULL;
    size_t size;
    size_t at;
    bool matched = false;
    int fd = -1;

    if (path == NULL || ec == NULL || ec->tb == NULL || m == NULL) {
        errno = EINVAL;
        return false;
    }
    canonical = replay_realpath(path, m, &owned);
    dir = journal_dir();
    if (dir == NULL) {
        goto done;
    }
    jpath = journal_path(dir, canonical);
    if (jpath == NULL) {
        goto done;
    }
    if (journal_path_owned(jpath)) {
        errno = EBUSY;
        goto done;
    }
    fd = open(jpath, O_RDWR
#ifdef O_CLOEXEC
              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0) {
        goto done;
    }
    if (!lock_journal(fd)) {
        goto done;
    }
    if (!journal_fd_valid(fd, &st)) {
        goto done;
    }
    size = (size_t)st.st_size;
    data = sag_xmalloc(size == 0U ? 1U : size);
    if (!read_all(fd, data, size)) {
        goto done;
    }
    if (!header_matches(data, size, canonical, m, &at)) {
        stale_journal(fd, jpath, dir);
        goto done;
    }
    matched = true;
    if (external_transaction && ec->undo != NULL)
        sag_undo_begin(ec, SAG_TXN_EXTERNAL);
    while (size - at >= SAG_JOURNAL_RECORD_FIXED) {
        const u8 *record = data + at;
        u8 op = record[0];
        u64 off = get_u64_le(record + 1U);
        u64 payload_len = get_u64_le(record + 9U);
        size_t payload_size;
        size_t record_size;
        u32 expected;
        u32 actual;

        if (payload_len > SIZE_MAX) {
            break;
        }
        payload_size = (size_t)payload_len;
        if (!size_add(17U, payload_size, &record_size) ||
            !size_add(record_size, 4U, &record_size) ||
            record_size > size - at) {
            break;
        }
        expected = get_u32_le(record + 17U + payload_size);
        actual = sag_crc32(record, 17U + payload_size);
        if (actual != expected ||
            !apply_record(ec, op, off, record + 17U, payload_len)) {
            break;
        }
        at += record_size;
    }
    if (external_transaction && ec->undo != NULL)
        sag_undo_end(ec);
    if (at != size) {
        sag_log(SAG_LOG_WARN,
                "ignored incomplete or corrupt crash journal tail in %s",
                jpath);
        if (ftruncate(fd, (off_t)at) != 0 || !fsync_retry(fd))
            sag_log(SAG_LOG_ERROR,
                    "cannot discard crash journal tail in %s: %s", jpath,
                    strerror(errno));
    }

done:
    if (fd >= 0) {
        (void)close(fd);
    }
    free(data);
    free(jpath);
    free(dir);
    free(owned);
    return matched;
}

bool sag_journal_replay(const char *path, TextBuf *tb, FileMeta *m)
{
    EditCtx ec;

    if (tb == NULL) {
        errno = EINVAL;
        return false;
    }
    (void)memset(&ec, 0, sizeof(ec));
    ec.tb = tb;
    return journal_replay_edit(path, &ec, m, false);
}

bool sag_journal_replay_edit(const char *path, EditCtx *ec, FileMeta *m)
{
    EditCtx replay;

    if (ec == NULL) {
        errno = EINVAL;
        return false;
    }
    replay = *ec;
    /* Recovery replays an existing durable stream; writing it back would
     * duplicate every operation and could grow the journal without bound. */
    replay.jrnl = NULL;
    replay.meta = NULL;
    return journal_replay_edit(path, &replay, m, true);
}
