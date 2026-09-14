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
#include <sys/uio.h>
#include <unistd.h>

#include "util/log.h"
#include "text/edit.h"

#define YEW_JOURNAL_VERSION 2U
#define YEW_JOURNAL_HEADER_FIXED 60U
#define YEW_JOURNAL_RECORD_FIXED 21U

enum {
    YEW_JOURNAL_BASE_NONE = 0U,
    YEW_JOURNAL_BASE_LINK = 1U,
    YEW_JOURNAL_BASE_COPY = 2U
};

typedef struct {
    size_t records_at;
    u64 size;
    i64 mtime_sec;
    u32 mtime_nsec;
    u64 dev;
    u64 ino;
    u32 base_kind;
    u32 base_crc;
} JournalHeader;

typedef struct {
    u32 kind;
    u32 crc;
} JournalBase;

struct Journal {
    int fd;
    char *path;
    char *base_path;
    char *dir;
    /* Borrowed for the handle lifetime so discard can release its topology
     * pin.  Buffers own both this handle and the pointed-to FileMeta. */
    FileMeta *meta;
    bool base_link;
    bool failed;
    struct Journal *next;
};

static u32 crc32_table[256];
static bool crc32_ready;
static Journal *open_journals;

/* adopt_existing_journal returns this when the file on disk belongs to
 * another version of the document: not an error, a replace. */
#define YEW_JOURNAL_OBSOLETE (-2)

static int adopt_existing_journal(const char *path, const char *realpath,
                                  const char *base_path, const char *dir,
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

u32 yew_crc32_begin(void)
{
    crc32_init();
    return UINT32_MAX;
}

u32 yew_crc32_add(u32 crc, const u8 *bytes, size_t len)
{
    size_t i;

    if (bytes == NULL && len != 0U)
        YEW_BUG("yew_crc32_add: NULL bytes");
    for (i = 0U; i < len; i++) {
        crc = crc32_table[(crc ^ bytes[i]) & 0xffU] ^ (crc >> 8U);
    }
    return crc;
}

u32 yew_crc32_end(u32 crc)
{
    return crc ^ UINT32_MAX;
}

u32 yew_crc32(const u8 *bytes, size_t len)
{
    return yew_crc32_end(yew_crc32_add(yew_crc32_begin(), bytes, len));
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
        suffix = "/yew/journal";
    } else {
        state = getenv("HOME");
        if (state == NULL || state[0] == '\0') {
            return NULL;
        }
        suffix = "/.local/state/yew/journal";
    }
    if (!size_add(strlen(state), strlen(suffix) + 1U, &len)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    dir = yew_xmalloc(len);
    (void)snprintf(dir, len, "%s%s", state, suffix);
    return dir;
}

static char *journal_path(const char *dir, const char *realpath)
{
    size_t len;
    char *path;

    if (!size_add(strlen(dir), 1U + 16U + sizeof(".yewj"), &len)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    path = yew_xmalloc(len);
    (void)snprintf(path, len, "%s/%016" PRIx64 ".yewj", dir,
                   fnv64(realpath));
    return path;
}

static char *journal_base_path(const char *path)
{
    size_t len;
    char *base;

    if (!size_add(strlen(path), sizeof(".base"), &len)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    base = yew_xmalloc(len);
    (void)snprintf(base, len, "%s.base", path);
    return base;
}

static struct timespec journal_stat_mtime(const struct stat *st)
{
#if defined(__APPLE__)
    return st->st_mtimespec;
#else
    return st->st_mtim;
#endif
}

static bool stat_matches_meta(const struct stat *st, const FileMeta *meta)
{
    struct timespec mtime = journal_stat_mtime(st);

    return S_ISREG(st->st_mode) && st->st_size >= 0 &&
           (u64)st->st_size == meta->size_on_disk &&
           (u64)st->st_dev == (u64)meta->dev &&
           (u64)st->st_ino == (u64)meta->ino &&
           mtime.tv_sec == meta->mtime.tv_sec &&
           mtime.tv_nsec == meta->mtime.tv_nsec;
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

static bool writev_all(int fd, struct iovec *iov, int count)
{
    int first = 0;

    while (first < count) {
        ssize_t written = writev(fd, iov + first, count - first);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        while (first < count && (size_t)written >= iov[first].iov_len) {
            written -= (ssize_t)iov[first].iov_len;
            first++;
        }
        if (first < count && written != 0) {
            iov[first].iov_base = (u8 *)iov[first].iov_base + written;
            iov[first].iov_len -= (size_t)written;
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

static bool create_empty_base(const char *base_path, const char *dir,
                              JournalBase *base)
{
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    int fd;
    bool ok;
    int saved_errno = 0;

#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(base_path, flags, 0600);
    if (fd < 0)
        return false;
    ok = fsync_retry(fd);
    if (!ok)
        saved_errno = errno == 0 ? EIO : errno;
    if (close(fd) != 0) {
        if (saved_errno == 0)
            saved_errno = errno;
        ok = false;
    }
    if (ok && !fsync_dir(dir)) {
        saved_errno = errno == 0 ? EIO : errno;
        ok = false;
    }
    if (!ok) {
        (void)unlink(base_path);
        errno = saved_errno;
        return false;
    }
    base->kind = YEW_JOURNAL_BASE_COPY;
    base->crc = yew_crc32(NULL, 0U);
    return true;
}

static bool copy_base(const char *realpath, const char *base_path,
                      const char *dir, const FileMeta *meta,
                      JournalBase *base)
{
    u8 block[64U * 1024U];
    struct stat before;
    struct stat after;
    u64 left;
    u32 crc = yew_crc32_begin();
    int in_flags = O_RDONLY;
    int out_flags = O_WRONLY | O_CREAT | O_EXCL;
    int in_fd = -1;
    int out_fd = -1;
    bool ok = false;
    int saved_errno;

#ifdef O_CLOEXEC
    in_flags |= O_CLOEXEC;
    out_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    in_flags |= O_NOFOLLOW;
    out_flags |= O_NOFOLLOW;
#endif
    in_fd = open(realpath, in_flags);
    if (in_fd < 0 || fstat(in_fd, &before) != 0 ||
        !stat_matches_meta(&before, meta))
        goto done;
    out_fd = open(base_path, out_flags, 0600);
    if (out_fd < 0)
        goto done;
    left = meta->size_on_disk;
    while (left != 0U) {
        size_t ask = left < (u64)sizeof(block) ? (size_t)left
                                               : sizeof(block);
        ssize_t got = read(in_fd, block, ask);

        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            goto done;
        if (!write_all(out_fd, block, (size_t)got))
            goto done;
        crc = yew_crc32_add(crc, block, (size_t)got);
        left -= (u64)got;
    }
    if (fstat(in_fd, &after) != 0 || !stat_matches_meta(&after, meta) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        !fsync_retry(out_fd))
        goto done;
    ok = true;

done:
    saved_errno = ok ? 0 : (errno == 0 ? EIO : errno);
    if (out_fd >= 0 && close(out_fd) != 0) {
        if (saved_errno == 0)
            saved_errno = errno;
        ok = false;
    }
    if (in_fd >= 0 && close(in_fd) != 0) {
        if (saved_errno == 0)
            saved_errno = errno;
        ok = false;
    }
    if (!ok) {
        (void)unlink(base_path);
        errno = saved_errno;
        return false;
    }
    if (!fsync_dir(dir)) {
        saved_errno = errno == 0 ? EIO : errno;
        (void)unlink(base_path);
        errno = saved_errno;
        return false;
    }
    base->kind = YEW_JOURNAL_BASE_COPY;
    base->crc = yew_crc32_end(crc);
    return true;
}

static bool create_base(const char *realpath, const char *base_path,
                        const char *dir, const FileMeta *meta,
                        JournalBase *base)
{
    struct stat st;

    base->kind = YEW_JOURNAL_BASE_NONE;
    base->crc = 0U;
    if (!meta->exists)
        return create_empty_base(base_path, dir, base);
    if (link(realpath, base_path) == 0) {
        if (stat(base_path, &st) == 0 && stat_matches_meta(&st, meta) &&
            fsync_dir(dir)) {
            base->kind = YEW_JOURNAL_BASE_LINK;
            return true;
        }
        (void)unlink(base_path);
        errno = ESTALE;
        return false;
    }
    if (errno == EEXIST && stat(base_path, &st) == 0 &&
        stat_matches_meta(&st, meta)) {
        base->kind = YEW_JOURNAL_BASE_LINK;
        return true;
    }
    if (copy_base(realpath, base_path, dir, meta, base))
        return true;
    /* Unit-only synthetic metadata predates a real source file.  It can
     * exercise the wire format but cannot provide replacement recovery. */
    if (meta->dev == 0 && meta->ino == 0 && !meta->disk_snapshot_valid) {
        errno = 0;
        return true;
    }
    return false;
}

static bool preserve_stale_path(const char *path, const char *dir,
                                const struct stat *expected,
                                const char *description)
{
    struct stat before;
    struct stat after;
    struct stat stale_st;
    size_t len;
    char *stale;
    unsigned int suffix = 0U;

    if (lstat(path, &before) != 0)
        return expected == NULL && errno == ENOENT;
    if (!S_ISREG(before.st_mode) ||
        (expected != NULL &&
         (before.st_dev != expected->st_dev ||
          before.st_ino != expected->st_ino)) ||
        !size_add(strlen(path), sizeof(".stale") + 12U, &len)) {
        errno = EPERM;
        return false;
    }
    stale = yew_xmalloc(len);
    for (;;) {
        if (suffix == 0U)
            (void)snprintf(stale, len, "%s.stale", path);
        else
            (void)snprintf(stale, len, "%s.stale.%u", path, suffix);
        suffix++;
        if (link(path, stale) == 0)
            break;
        if (errno != EEXIST || suffix == 0U) {
            yew_xfree(stale);
            return false;
        }
    }
    if (lstat(path, &after) != 0 || lstat(stale, &stale_st) != 0 ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        stale_st.st_dev != before.st_dev || stale_st.st_ino != before.st_ino ||
        unlink(path) != 0) {
        int saved_errno = errno == 0 ? ESTALE : errno;

        (void)unlink(stale);
        yew_xfree(stale);
        errno = saved_errno;
        return false;
    }
    if (!fsync_dir(dir)) {
        int saved_errno = errno == 0 ? EIO : errno;

        /* The stale name is now the only name.  Keep it even if the
         * directory barrier failed: deleting it here would lose recovery
         * data while reporting the preservation failure. */
        yew_xfree(stale);
        errno = saved_errno;
        return false;
    }
    yew_log(YEW_LOG_WARN, "preserved stale %s as %s", description, stale);
    yew_xfree(stale);
    return true;
}

static bool write_header(int fd, const char *realpath, const FileMeta *meta,
                         const JournalBase *base)
{
    size_t path_len = strlen(realpath);
    u8 fixed[YEW_JOURNAL_HEADER_FIXED];

    if ((u64)path_len != path_len) {
        errno = EOVERFLOW;
        return false;
    }
    /* v2 extends the v1 path/size/mtime header with source identity and the
     * authenticated companion kind.  Unsupported versions are preserved. */
    (void)memcpy(fixed, "YEWJ", 4U);
    put_u32_le(fixed + 4U, YEW_JOURNAL_VERSION);
    put_u64_le(fixed + 8U, (u64)path_len);
    put_u64_le(fixed + 16U, meta->size_on_disk);
    put_u64_le(fixed + 24U, (u64)(i64)meta->mtime.tv_sec);
    put_u32_le(fixed + 32U, (u32)meta->mtime.tv_nsec);
    put_u64_le(fixed + 36U, (u64)meta->dev);
    put_u64_le(fixed + 44U, (u64)meta->ino);
    put_u32_le(fixed + 52U, base->kind);
    put_u32_le(fixed + 56U, base->crc);
    return write_all(fd, fixed, 16U) &&
           write_all(fd, (const u8 *)realpath, path_len) &&
           write_all(fd, fixed + 16U, YEW_JOURNAL_HEADER_FIXED - 16U);
}

Journal *yew_journal_open(const char *realpath, FileMeta *m)
{
    Journal *journal;
    JournalBase base;
    char *dir;
    char *path;
    char *base_path;
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_APPEND;
    int fd;
    bool created;

    if (realpath == NULL || realpath[0] == '\0' || m == NULL) {
        errno = EINVAL;
        return NULL;
    }
    base.kind = YEW_JOURNAL_BASE_NONE;
    base.crc = 0U;
    dir = journal_dir();
    if (dir == NULL || !make_dirs(dir)) {
        yew_log(YEW_LOG_ERROR, "cannot create crash journal directory: %s",
                strerror(errno));
        yew_xfree(dir);
        return NULL;
    }
    path = journal_path(dir, realpath);
    if (path == NULL) {
        yew_xfree(dir);
        return NULL;
    }
    base_path = journal_base_path(path);
    if (base_path == NULL) {
        yew_xfree(path);
        yew_xfree(dir);
        return NULL;
    }
    if (journal_path_owned(path)) {
        yew_xfree(base_path);
        yew_xfree(path);
        yew_xfree(dir);
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
    if (!created && errno == EEXIST) {
        fd = adopt_existing_journal(path, realpath, base_path, dir, m);
        if (fd == YEW_JOURNAL_OBSOLETE) {
            /* The old pair has been preserved under .stale names.  Create
             * a fresh pair; never truncate recovery data in place. */
            fd = open(path, flags, 0600);
            created = fd >= 0;
            if (created)
                yew_log(YEW_LOG_WARN,
                        "replaced an obsolete crash journal for %s",
                        realpath);
        }
    }
    if (fd < 0) {
        yew_log(YEW_LOG_ERROR, "cannot open crash journal %s: %s", path,
                strerror(errno));
        yew_xfree(base_path);
        yew_xfree(path);
        yew_xfree(dir);
        return NULL;
    }
    if (created && !lock_journal(fd)) {
        int saved_errno = errno;

        (void)close(fd);
        (void)unlink(path);
        (void)fsync_dir(dir);
        yew_xfree(base_path);
        yew_xfree(path);
        yew_xfree(dir);
        errno = saved_errno;
        return NULL;
    }
    if (created &&
        (!preserve_stale_path(base_path, dir, NULL,
                              "orphaned journal base") ||
         !create_base(realpath, base_path, dir, m, &base) ||
         !write_header(fd, realpath, m, &base) ||
         !fsync_retry(fd) || !fsync_dir(dir))) {
        int saved_errno = errno;

        (void)close(fd);
        (void)unlink(path);
        if (base.kind != YEW_JOURNAL_BASE_NONE)
            (void)unlink(base_path);
        yew_xfree(base_path);
        yew_xfree(path);
        yew_xfree(dir);
        errno = saved_errno;
        return NULL;
    }
    journal = yew_xcalloc(1U, sizeof(*journal));
    journal->fd = fd;
    journal->path = path;
    journal->base_path = base_path;
    journal->dir = dir;
    journal->meta = m;
    if (created) {
        journal->base_link = base.kind == YEW_JOURNAL_BASE_LINK;
    } else {
        struct stat base_st;

        journal->base_link = stat(base_path, &base_st) == 0 &&
                             (u64)base_st.st_dev == (u64)m->dev &&
                             (u64)base_st.st_ino == (u64)m->ino;
    }
    if (journal->base_link)
        m->journal_pinned = true;
    journal->next = open_journals;
    open_journals = journal;
    return journal;
}

bool yew_journal_record(Journal *j, u8 op, u64 off, const u8 *b, u64 n)
{
    u8 fixed[17];
    u8 encoded_crc[4];
    struct iovec record[3];
    u32 crc;

    if (j == NULL || j->failed) {
        errno = EIO;
        return false;
    }
    if ((op != YEW_JOURNAL_INS && op != YEW_JOURNAL_DEL) ||
        (n != 0U && b == NULL) || n > SIZE_MAX) {
        j->failed = true;
        errno = EINVAL;
        yew_log(YEW_LOG_ERROR, "invalid crash journal record");
        return false;
    }
    fixed[0] = op;
    put_u64_le(fixed + 1U, off);
    put_u64_le(fixed + 9U, n);
    crc = yew_crc32_begin();
    crc = yew_crc32_add(crc, fixed, sizeof(fixed));
    crc = yew_crc32_add(crc, b, (size_t)n);
    put_u32_le(encoded_crc, yew_crc32_end(crc));
    record[0].iov_base = fixed;
    record[0].iov_len = sizeof(fixed);
    record[1].iov_base = (void *)b;
    record[1].iov_len = (size_t)n;
    record[2].iov_base = encoded_crc;
    record[2].iov_len = sizeof(encoded_crc);
    if (!writev_all(j->fd, record, (int)YEW_ARRAY_LEN(record))) {
        j->failed = true;
        yew_log(YEW_LOG_ERROR, "cannot append crash journal %s: %s", j->path,
                strerror(errno));
        return false;
    }
    return true;
}

bool yew_journal_sync(Journal *j)
{
    if (j == NULL || j->failed) {
        errno = EIO;
        return false;
    }
    if (!fsync_retry(j->fd)) {
        j->failed = true;
        yew_log(YEW_LOG_ERROR, "cannot sync crash journal %s: %s", j->path,
                strerror(errno));
        return false;
    }
    return true;
}

bool yew_journal_ok(const Journal *j)
{
    return j != NULL && !j->failed;
}

void yew_journal_close(Journal *j)
{
    if (j == NULL)
        return;
    journal_unregister(j);
    if (close(j->fd) != 0)
        yew_log(YEW_LOG_ERROR, "cannot close crash journal %s: %s", j->path,
                strerror(errno));
    yew_xfree(j->base_path);
    yew_xfree(j->path);
    yew_xfree(j->dir);
    yew_xfree(j);
}

void yew_journal_discard(Journal *j)
{
    int saved_errno = 0;
    bool base_absent = false;

    if (j == NULL) {
        return;
    }
    journal_unregister(j);
    if (unlink(j->base_path) == 0 || errno == ENOENT) {
        base_absent = true;
    } else {
        saved_errno = errno;
    }
    if (base_absent) {
        if (unlink(j->path) < 0 && errno != ENOENT)
            saved_errno = errno;
        else
            (void)fsync_dir(j->dir);
    }
    if (close(j->fd) < 0 && saved_errno == 0)
        saved_errno = errno;
    if (saved_errno != 0) {
        yew_log(YEW_LOG_ERROR, "cannot discard crash journal %s: %s", j->path,
                strerror(saved_errno));
    }
    if (j->base_link && base_absent && j->meta != NULL)
        j->meta->journal_pinned = false;
    yew_xfree(j->base_path);
    yew_xfree(j->path);
    yew_xfree(j->dir);
    yew_xfree(j);
}

static bool buffer_matches(const TextBuf *tb, u64 off, const u8 *bytes,
                           u64 len)
{
    TextIter it;
    u64 left = len;

    if (!yew_textiter_begin(&it, tb, (ByteOff){off})) {
        return len == 0U;
    }
    while (left != 0U) {
        const u8 *chunk;
        u64 chunk_len;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &chunk, &chunk_len)) {
            return false;
        }
        take = chunk_len < left ? chunk_len : left;
        if (memcmp(chunk, bytes, (size_t)take) != 0) {
            return false;
        }
        bytes += (size_t)take;
        left -= take;
        if (left != 0U && !yew_textiter_advance(&it, tb)) {
            return false;
        }
    }
    return true;
}

static bool apply_record(EditCtx *ec, u8 op, u64 off, const u8 *bytes,
                         u64 len)
{
    TextBuf *tb = ec->tb;
    u64 total = yew_textbuf_len(tb);

    if (off > total) {
        return false;
    }
    if (op == YEW_JOURNAL_INS && len <= UINT64_MAX - total) {
        return yew_edit_insert(ec, (ByteOff){off}, bytes, len);
    }
    if (op == YEW_JOURNAL_DEL && len <= total - off &&
        buffer_matches(tb, off, bytes, len)) {
        return yew_edit_delete(ec, (Span){off, off + len});
    }
    return false;
}

static bool stale_journal(int fd, const char *path, const char *base_path,
                          const char *dir)
{
    struct stat fd_st;

    if (fstat(fd, &fd_st) != 0 ||
        !preserve_stale_path(base_path, dir, NULL, "journal base") ||
        !preserve_stale_path(path, dir, &fd_st, "crash journal")) {
        yew_log(YEW_LOG_ERROR, "cannot preserve stale crash journal %s: %s",
                path, strerror(errno));
        return false;
    }
    return true;
}

static bool header_decode(const u8 *data, size_t size, const char *realpath,
                          JournalHeader *header)
{
    u64 path_len;
    size_t path_size;
    size_t tail_at;

    if (size < 16U || memcmp(data, "YEWJ", 4U) != 0 ||
        get_u32_le(data + 4U) != YEW_JOURNAL_VERSION) {
        return false;
    }
    path_len = get_u64_le(data + 8U);
    if (path_len > SIZE_MAX) {
        return false;
    }
    path_size = (size_t)path_len;
    if (!size_add(16U, path_size, &tail_at) ||
        tail_at > size || size - tail_at < 44U) {
        return false;
    }
    if (strlen(realpath) != path_size ||
        memcmp(data + 16U, realpath, path_size) != 0) {
        return false;
    }
    header->records_at = tail_at + 44U;
    header->size = get_u64_le(data + tail_at);
    header->mtime_sec = (i64)get_u64_le(data + tail_at + 8U);
    header->mtime_nsec = get_u32_le(data + tail_at + 16U);
    header->dev = get_u64_le(data + tail_at + 20U);
    header->ino = get_u64_le(data + tail_at + 28U);
    header->base_kind = get_u32_le(data + tail_at + 36U);
    header->base_crc = get_u32_le(data + tail_at + 40U);
    if (header->mtime_nsec >= UINT32_C(1000000000) ||
        header->base_kind > YEW_JOURNAL_BASE_COPY)
        return false;
    return true;
}

static bool header_matches_meta(const JournalHeader *header,
                                const FileMeta *meta)
{
    /* YEW-F-078: size and mtime do not identify a file after replacement;
     * bind normal replay to the loaded device/inode as well. */
    return header->size == meta->size_on_disk &&
           header->mtime_sec == (i64)meta->mtime.tv_sec &&
           header->mtime_nsec == (u32)meta->mtime.tv_nsec &&
           header->dev == (u64)meta->dev &&
           header->ino == (u64)meta->ino;
}

static bool base_matches_header(const char *base_path,
                                const JournalHeader *header)
{
    u8 block[64U * 1024U];
    struct stat before;
    struct stat after;
    struct timespec mtime;
    u64 left;
    u32 crc = yew_crc32_begin();
    int flags = O_RDONLY;
    int fd;
    bool matched = false;

    if (header->base_kind == YEW_JOURNAL_BASE_NONE)
        return false;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(base_path, flags);
    if (fd < 0 || fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || (u64)before.st_size != header->size)
        goto done;
    mtime = journal_stat_mtime(&before);
    if (header->base_kind == YEW_JOURNAL_BASE_LINK) {
        matched = (u64)before.st_dev == header->dev &&
                  (u64)before.st_ino == header->ino &&
                  (i64)mtime.tv_sec == header->mtime_sec &&
                  (u32)mtime.tv_nsec == header->mtime_nsec;
        goto done;
    }
    if (before.st_uid != geteuid() || before.st_nlink != 1 ||
        (before.st_mode & 0077U) != 0U)
        goto done;
    left = header->size;
    while (left != 0U) {
        size_t ask = left < (u64)sizeof(block) ? (size_t)left
                                               : sizeof(block);
        ssize_t got = read(fd, block, ask);

        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            goto done;
        crc = yew_crc32_add(crc, block, (size_t)got);
        left -= (u64)got;
    }
    if (fstat(fd, &after) != 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        journal_stat_mtime(&before).tv_sec !=
            journal_stat_mtime(&after).tv_sec ||
        journal_stat_mtime(&before).tv_nsec !=
            journal_stat_mtime(&after).tv_nsec)
        goto done;
    matched = yew_crc32_end(crc) == header->base_crc;

done:
    if (fd >= 0)
        (void)close(fd);
    return matched;
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
    while (size - at >= YEW_JOURNAL_RECORD_FIXED) {
        const u8 *record = data + at;
        u64 payload_len = get_u64_le(record + 9U);
        size_t payload_size;
        size_t record_size;
        u32 expected;
        u32 actual;

        if ((record[0] != YEW_JOURNAL_INS &&
             record[0] != YEW_JOURNAL_DEL) ||
            payload_len > SIZE_MAX)
            break;
        payload_size = (size_t)payload_len;
        if (!size_add(17U, payload_size, &record_size) ||
            !size_add(record_size, 4U, &record_size) ||
            record_size > size - at)
            break;
        expected = get_u32_le(record + 17U + payload_size);
        actual = yew_crc32(record, 17U + payload_size);
        if (actual != expected)
            break;
        at += record_size;
    }
    return at;
}

static int adopt_existing_journal(const char *path, const char *realpath,
                                  const char *base_path, const char *dir,
                                  const FileMeta *meta)
{
    struct stat st;
    u8 *data = NULL;
    size_t size;
    size_t prefix;
    JournalHeader header;
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
    data = yew_xmalloc(size == 0U ? 1U : size);
    if (!read_all(fd, data, size))
        goto fail;
    if (!header_decode(data, size, realpath, &header) ||
        !header_matches_meta(&header, meta)) {
        /*
         * The leftover journal describes a DIFFERENT version of this
         * file (or a different file that hashed to the same name).
         * yew_journal_probe applies this same predicate at open time,
         * so the editor has already decided there is nothing here to
         * recover and has told the user nothing.  Reporting a failure
         * now would block every future edit of this path until someone
         * deleted the file by hand — which is what it did, under the
         * nonsense message "Stale file handle", because ESTALE was
         * being used as an internal sentinel and then printed with
         * strerror.  An obsolete journal is replaced, not obeyed.
         */
        yew_xfree(data);
        data = NULL;
        if (!stale_journal(fd, path, base_path, dir))
            goto fail;
        (void)close(fd);
        return YEW_JOURNAL_OBSOLETE;
    }
    prefix = valid_record_prefix(data, size, header.records_at);
    if (prefix != size &&
        (ftruncate(fd, (off_t)prefix) != 0 || !fsync_retry(fd)))
        goto fail;
    if (lseek(fd, 0, SEEK_END) < 0)
        goto fail;
    if (prefix != size)
        yew_log(YEW_LOG_WARN,
                "discarded incomplete crash journal tail while adopting %s",
                path);
    yew_xfree(data);
    return fd;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    yew_xfree(data);
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
    *owned = yew_xrealpath(path);
    return *owned != NULL ? *owned : path;
}

bool yew_journal_probe(const char *path, const FileMeta *m)
{
    const char *canonical;
    char *owned = NULL;
    char *dir = NULL;
    char *jpath = NULL;
    char *base_path = NULL;
    struct stat st;
    u8 *data = NULL;
    size_t size;
    JournalHeader header;
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
    base_path = journal_base_path(jpath);
    if (base_path == NULL)
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
    data = yew_xmalloc(size == 0U ? 1U : size);
    if (read_all(fd, data, size) &&
        header_decode(data, size, canonical, &header)) {
        matched = header_matches_meta(&header, m) ||
                  base_matches_header(base_path, &header);
    }

done:
    if (fd >= 0)
        (void)close(fd);
    yew_xfree(data);
    yew_xfree(base_path);
    yew_xfree(jpath);
    yew_xfree(dir);
    yew_xfree(owned);
    return matched;
}

bool yew_journal_discard_path(const char *path, const FileMeta *m)
{
    const char *canonical;
    char *owned = NULL;
    char *dir = NULL;
    char *jpath = NULL;
    char *base_path = NULL;
    struct stat st;
    struct stat path_st;
    u8 *data = NULL;
    size_t size;
    JournalHeader header;
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
    base_path = journal_base_path(jpath);
    if (base_path == NULL)
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
    data = yew_xmalloc(size == 0U ? 1U : size);
    if (!read_all(fd, data, size) ||
        !header_decode(data, size, canonical, &header) ||
        (!header_matches_meta(&header, m) &&
         !base_matches_header(base_path, &header)))
        goto done;
    if (lstat(jpath, &path_st) != 0 || path_st.st_dev != st.st_dev ||
        path_st.st_ino != st.st_ino) {
        errno = ESTALE;
        goto done;
    }
    if (unlink(base_path) == 0 || errno == ENOENT) {
        if (unlink(jpath) == 0 && fsync_dir(dir))
            discarded = true;
    }

done:
    if (fd >= 0)
        (void)close(fd);
    yew_xfree(data);
    yew_xfree(base_path);
    yew_xfree(jpath);
    yew_xfree(dir);
    yew_xfree(owned);
    return discarded;
}

static bool replace_text_from_base(EditCtx *ec, FileMeta *meta,
                                   const char *base_path,
                                   const JournalHeader *header)
{
    TextBuf *base_tb = NULL;
    FileMeta base_meta;
    TextIter it;
    char *canonical;
    bool via_symlink;
    u64 inserted = 0U;
    bool ok = false;

    yew_filemeta_init(&base_meta);
    if (yew_file_load(base_path, &base_tb, &base_meta) != YEW_LOAD_OK)
        goto done;
    if (yew_textbuf_len(ec->tb) != 0U &&
        !yew_edit_delete(ec, (Span){0U, yew_textbuf_len(ec->tb)}))
        goto done;
    if (yew_textiter_begin(&it, base_tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            if (!yew_textiter_chunk(&it, base_tb, &bytes, &len) ||
                !yew_edit_insert(ec, BYTEOFF(inserted), bytes, len))
                goto done;
            inserted += len;
        } while (yew_textiter_advance(&it, base_tb));
    }

    canonical = meta->realpath;
    via_symlink = meta->via_symlink;
    yew_filemeta_content_forget(meta);
    yew_xfree(base_meta.realpath);
    base_meta.realpath = canonical;
    base_meta.via_symlink = via_symlink;
    base_meta.size_on_disk = header->size;
    base_meta.mtime.tv_sec = (time_t)header->mtime_sec;
    base_meta.mtime.tv_nsec = (long)header->mtime_nsec;
    base_meta.dev = (dev_t)header->dev;
    base_meta.ino = (ino_t)header->ino;
    base_meta.journal_pinned =
        header->base_kind == YEW_JOURNAL_BASE_LINK;
    *meta = base_meta;
    (void)memset(&base_meta, 0, sizeof(base_meta));
    ok = true;

done:
    yew_filemeta_dispose(&base_meta);
    yew_textbuf_free(base_tb);
    return ok;
}

static bool journal_replay_edit(const char *path, EditCtx *ec, FileMeta *m,
                                bool external_transaction)
{
    const char *canonical;
    char *owned = NULL;
    char *dir = NULL;
    char *jpath = NULL;
    char *base_path = NULL;
    struct stat st;
    u8 *data = NULL;
    size_t size;
    size_t at;
    JournalHeader header;
    bool matched = false;
    bool current_base;
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
    base_path = journal_base_path(jpath);
    if (base_path == NULL)
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
    data = yew_xmalloc(size == 0U ? 1U : size);
    if (!read_all(fd, data, size)) {
        goto done;
    }
    if (!header_decode(data, size, canonical, &header)) {
        (void)stale_journal(fd, jpath, base_path, dir);
        goto done;
    }
    current_base = header_matches_meta(&header, m);
    if (!current_base && !base_matches_header(base_path, &header)) {
        (void)stale_journal(fd, jpath, base_path, dir);
        goto done;
    }
    matched = true;
    if (external_transaction && ec->undo != NULL)
        yew_undo_begin(ec, YEW_TXN_EXTERNAL);
    if (!current_base &&
        !replace_text_from_base(ec, m, base_path, &header)) {
        if (external_transaction && ec->undo != NULL)
            yew_undo_abort(ec);
        matched = false;
        goto done;
    }
    at = header.records_at;
    while (size - at >= YEW_JOURNAL_RECORD_FIXED) {
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
        actual = yew_crc32(record, 17U + payload_size);
        if (actual != expected ||
            !apply_record(ec, op, off, record + 17U, payload_len)) {
            break;
        }
        at += record_size;
    }
    if (external_transaction && ec->undo != NULL)
        yew_undo_end(ec);
    if (at != size) {
        yew_log(YEW_LOG_WARN,
                "ignored incomplete or corrupt crash journal tail in %s",
                jpath);
        if (ftruncate(fd, (off_t)at) != 0 || !fsync_retry(fd))
            yew_log(YEW_LOG_ERROR,
                    "cannot discard crash journal tail in %s: %s", jpath,
                    strerror(errno));
    }

done:
    if (fd >= 0) {
        (void)close(fd);
    }
    yew_xfree(data);
    yew_xfree(base_path);
    yew_xfree(jpath);
    yew_xfree(dir);
    yew_xfree(owned);
    return matched;
}

bool yew_journal_replay(const char *path, TextBuf *tb, FileMeta *m)
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

bool yew_journal_replay_edit(const char *path, EditCtx *ec, FileMeta *m)
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
