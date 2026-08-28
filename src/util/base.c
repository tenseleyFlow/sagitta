/* _GNU_SOURCE exposes pipe2() on glibc; the macOS path below does not
 * need it, and the guard keeps the declaration from leaking elsewhere. */
#define _GNU_SOURCE
#define YEW_ALLOC_IMPLEMENTATION

#include "util/base.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/buf.h"
#include "util/log.h"

bool yew_pipe_cloexec(int fds[2])
{
#if defined(__linux__) || defined(__FreeBSD__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    /* See base.h: safe without pipe2() because the core is single-threaded
     * and never forks from a signal handler. */
    if (pipe(fds) != 0)
        return false;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
    }
    return true;
#endif
}

#if YEW_ALLOC_DEBUG

/*
 * This build answers who allocates, not how long an operation takes.  The
 * fixed header changes allocation size, alignment pressure, and cache
 * behaviour, so timing results from YEW_ALLOC_DEBUG are invalid by design.
 */
enum { ALLOC_SITE_CAP = 4096U, ALLOC_REPORT_CAP = ALLOC_SITE_CAP + 1U };

typedef struct {
    u32 site;
    u32 generation;
    size_t size;
    u64 magic;
} AllocMeta;

typedef struct {
    max_align_t storage[2];
} AllocHeader;

typedef struct {
    AllocSite site;
} AllocReportRow;

#define ALLOC_MAGIC UINT64_C(0x796577616c6c6f63)
#define ALLOC_OVERFLOW_SITE UINT32_MAX

_Static_assert(sizeof(AllocHeader) == 2U * sizeof(max_align_t),
               "allocation header size is part of the debug ABI");
_Static_assert(sizeof(AllocMeta) <= sizeof(AllocHeader),
               "allocation metadata must fit the fixed header");

static AllocSite alloc_sites[ALLOC_SITE_CAP];
static AllocSite alloc_overflow = {"<overflow>", 0, 0U, 0U, 0U, 0U};
static AllocReportRow alloc_report_rows[ALLOC_REPORT_CAP];
static u64 alloc_calls;
static u32 alloc_generation = 1U;
static bool alloc_exit_armed;

static void alloc_report_exit(void);

static void alloc_arm_exit(void)
{
    if (alloc_exit_armed)
        return;
    if (atexit(alloc_report_exit) != 0)
        YEW_BUG("cannot register allocation report handler");
    alloc_exit_armed = true;
}

static size_t alloc_hash(const char *file, int line)
{
    uintptr_t p = (uintptr_t)(const void *)file;
    u64 mixed = (u64)(p >> 4U) ^ (u64)(u32)line * UINT64_C(0x9e3779b1);

    mixed ^= mixed >> 17U;
    return (size_t)mixed & (ALLOC_SITE_CAP - 1U);
}

static u32 alloc_site_find(const char *file, int line)
{
    size_t start = alloc_hash(file, line);
    size_t probe;

    for (probe = 0U; probe < ALLOC_SITE_CAP; probe++) {
        size_t index = (start + probe) & (ALLOC_SITE_CAP - 1U);
        AllocSite *site = &alloc_sites[index];

        if (site->file == NULL) {
            site->file = file;
            site->line = line;
            return (u32)index;
        }
        if (site->line == line &&
            (site->file == file || strcmp(site->file, file) == 0))
            return (u32)index;
    }
    return ALLOC_OVERFLOW_SITE;
}

static AllocSite *alloc_site_at(u32 index)
{
    return index == ALLOC_OVERFLOW_SITE ? &alloc_overflow
                                        : &alloc_sites[index];
}

static void alloc_account(u32 index, size_t size)
{
    AllocSite *site = alloc_site_at(index);
    u64 amount = (u64)size;

    site->calls++;
    site->bytes += amount;
    site->live += amount;
    if (site->live > site->peak_live)
        site->peak_live = site->live;
    alloc_calls++;
}

static void alloc_unaccount(const AllocMeta *meta)
{
    AllocSite *site;

    if (meta->generation != alloc_generation)
        return;
    site = alloc_site_at(meta->site);
    if (site->live < (u64)meta->size)
        YEW_BUG("allocation live-byte counter underflow");
    site->live -= (u64)meta->size;
}

static AllocMeta alloc_meta_read(const AllocHeader *header)
{
    AllocMeta meta;

    (void)memcpy(&meta, header, sizeof(meta));
    if (meta.magic != ALLOC_MAGIC)
        YEW_BUG("invalid pointer passed to debug allocator");
    return meta;
}

static void alloc_meta_write(AllocHeader *header, u32 site, size_t size)
{
    AllocMeta meta;

    (void)memset(header, 0, sizeof(*header));
    meta.site = site;
    meta.generation = alloc_generation;
    meta.size = size;
    meta.magic = ALLOC_MAGIC;
    (void)memcpy(header, &meta, sizeof(meta));
}

static size_t alloc_total_size(size_t size)
{
    size_t payload = size == 0U ? 1U : size;

    if (payload > SIZE_MAX - sizeof(AllocHeader))
        YEW_BUG("allocation size overflow: %zu + %zu", payload,
                sizeof(AllocHeader));
    return sizeof(AllocHeader) + payload;
}

void *yew_xmalloc_at(size_t size, const char *file, int line)
{
    AllocHeader *header;
    u32 site;

    alloc_arm_exit();
    header = malloc(alloc_total_size(size));
    if (header == NULL)
        YEW_BUG("out of memory allocating %zu bytes", size);
    site = alloc_site_find(file, line);
    alloc_meta_write(header, site, size);
    alloc_account(site, size);
    return header + 1;
}

void *yew_xcalloc_at(size_t count, size_t size, const char *file, int line)
{
    AllocHeader *header;
    size_t bytes;
    u32 site;

    if (size != 0U && count > SIZE_MAX / size)
        YEW_BUG("allocation size overflow: %zu * %zu", count, size);
    bytes = count * size;
    alloc_arm_exit();
    header = calloc(1U, alloc_total_size(bytes));
    if (header == NULL)
        YEW_BUG("out of memory allocating %zu elements of %zu bytes", count,
                size);
    site = alloc_site_find(file, line);
    alloc_meta_write(header, site, bytes);
    alloc_account(site, bytes);
    return header + 1;
}

void *yew_xrealloc_at(void *ptr, size_t size, const char *file, int line)
{
    AllocHeader *header;
    AllocMeta old;
    u32 site;

    if (ptr == NULL)
        return yew_xmalloc_at(size, file, line);
    header = (AllocHeader *)ptr - 1;
    old = alloc_meta_read(header);
    header = realloc(header, alloc_total_size(size));
    if (header == NULL)
        YEW_BUG("out of memory reallocating to %zu bytes", size);
    alloc_unaccount(&old);
    /* Reallocation grows or shrinks the original allocation site.  Moving
     * it to the call site of a shared growth helper would hide the owner the
     * report exists to name.  An allocation from an older reset generation
     * is outside the current epoch, so only that case starts a new site. */
    site = old.generation == alloc_generation
               ? old.site
               : alloc_site_find(file, line);
    alloc_meta_write(header, site, size);
    alloc_account(site, size);
    return header + 1;
}

void *yew_xreallocarray_at(void *ptr, size_t count, size_t size,
                           const char *file, int line)
{
    if (size != 0U && count > SIZE_MAX / size)
        YEW_BUG("allocation size overflow: %zu * %zu", count, size);
    return yew_xrealloc_at(ptr, count * size, file, line);
}

char *yew_xstrdup_at(const char *str, const char *file, int line)
{
    size_t len = strlen(str) + 1U;
    char *copy = yew_xmalloc_at(len, file, line);

    (void)memcpy(copy, str, len);
    return copy;
}

char *yew_xrealpath_at(const char *path, const char *file, int line)
{
    char *resolved = realpath(path, NULL);
    char *copy;

    if (resolved == NULL)
        return NULL;
    copy = yew_xstrdup_at(resolved, file, line);
    free(resolved);
    return copy;
}

char *yew_xgetcwd_at(const char *file, int line)
{
    size_t cap = 256U;
    char *cwd = yew_xmalloc_at(cap, file, line);

    for (;;) {
        int saved;

        if (getcwd(cwd, cap) != NULL)
            return cwd;
        saved = errno;
        if (saved != ERANGE || cap > SIZE_MAX / 2U) {
            yew_xfree_at(cwd, file, line);
            errno = saved;
            return NULL;
        }
        cap *= 2U;
        cwd = yew_xrealloc_at(cwd, cap, file, line);
    }
}

void yew_xfree_at(void *ptr, const char *file, int line)
{
    AllocHeader *header;
    AllocMeta meta;

    (void)file;
    (void)line;
    if (ptr == NULL)
        return;
    header = (AllocHeader *)ptr - 1;
    meta = alloc_meta_read(header);
    alloc_unaccount(&meta);
    (void)memset(header, 0, sizeof(*header));
    free(header);
}

void yew_alloc_reset(void)
{
    alloc_arm_exit();
    (void)memset(alloc_sites, 0, sizeof(alloc_sites));
    alloc_overflow.file = "<overflow>";
    alloc_overflow.line = 0;
    alloc_overflow.calls = 0U;
    alloc_overflow.bytes = 0U;
    alloc_overflow.live = 0U;
    alloc_overflow.peak_live = 0U;
    alloc_calls = 0U;
    alloc_generation++;
    if (alloc_generation == 0U)
        alloc_generation = 1U;
}

u64 yew_alloc_calls(void)
{
    return alloc_calls;
}

static size_t alloc_snapshot(void)
{
    size_t len = 0U;
    size_t i;

    for (i = 0U; i < ALLOC_SITE_CAP; i++) {
        if (alloc_sites[i].file != NULL) {
            alloc_report_rows[len].site = alloc_sites[i];
            len++;
        }
    }
    if (alloc_overflow.calls != 0U || alloc_overflow.live != 0U) {
        alloc_report_rows[len].site = alloc_overflow;
        len++;
    }
    for (i = 1U; i < len; i++) {
        AllocReportRow row = alloc_report_rows[i];
        size_t j = i;

        while (j != 0U) {
            const AllocSite *left = &alloc_report_rows[j - 1U].site;
            const AllocSite *right = &row.site;
            int file_order;
            bool before;

            file_order = strcmp(left->file, right->file);
            before = left->calls < right->calls ||
                     (left->calls == right->calls && file_order > 0) ||
                     (left->calls == right->calls && file_order == 0 &&
                      left->line > right->line);
            if (!before)
                break;
            alloc_report_rows[j] = alloc_report_rows[j - 1U];
            j--;
        }
        alloc_report_rows[j] = row;
    }
    return len;
}

static int alloc_format_row(char *line, size_t cap, const AllocSite *site)
{
    return snprintf(line, cap, "%llu %llu %llu %llu %s:%d\n",
                    (unsigned long long)site->calls,
                    (unsigned long long)site->bytes,
                    (unsigned long long)site->live,
                    (unsigned long long)site->peak_live, site->file,
                    site->line);
}

void yew_alloc_report(Bytebuf *out)
{
    size_t len = alloc_snapshot();
    size_t i;
    char line[512];
    int n;

    bytebuf_append(out, "# yew allocation report v1\n", 27U);
    bytebuf_append(out, "# calls bytes live peak_live site\n", 34U);
    for (i = 0U; i < len; i++) {
        n = alloc_format_row(line, sizeof(line), &alloc_report_rows[i].site);
        if (n < 0 || (size_t)n >= sizeof(line))
            YEW_BUG("allocation report row is too long");
        bytebuf_append(out, line, (size_t)n);
    }
}

static bool alloc_write_all(int fd, const void *data, size_t len)
{
    const u8 *bytes = data;

    while (len != 0U) {
        ssize_t n = write(fd, bytes, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        bytes += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static void alloc_report_exit(void)
{
    const char *path = getenv("YEW_ALLOC_OUT");
    size_t len;
    size_t i;
    int fd;
    char line[512];
    int n;

    if (path == NULL || path[0] == '\0')
        return;
    len = alloc_snapshot();
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    if (!alloc_write_all(fd, "# yew allocation report v1\n", 27U) ||
        !alloc_write_all(fd, "# calls bytes live peak_live site\n", 34U)) {
        (void)close(fd);
        return;
    }
    for (i = 0U; i < len; i++) {
        n = alloc_format_row(line, sizeof(line), &alloc_report_rows[i].site);
        if (n < 0 || (size_t)n >= sizeof(line) ||
            !alloc_write_all(fd, line, (size_t)n))
            break;
    }
    (void)close(fd);
}

#else

void *yew_xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);

    if (!ptr)
        YEW_BUG("out of memory allocating %zu bytes", size);
    return ptr;
}

void *yew_xrealloc(void *ptr, size_t size)
{
    void *resized = realloc(ptr, size ? size : 1);

    if (!resized)
        YEW_BUG("out of memory reallocating to %zu bytes", size);
    return resized;
}

void *yew_xcalloc(size_t count, size_t size)
{
    void *ptr;

    if (count == 0 || size == 0) {
        ptr = calloc(1, 1);
        if (!ptr)
            YEW_BUG("out of memory allocating zero bytes");
        return ptr;
    }
    if (size != 0 && count > SIZE_MAX / size)
        YEW_BUG("allocation size overflow: %zu * %zu", count, size);
    ptr = calloc(count, size);
    if (!ptr)
        YEW_BUG("out of memory allocating %zu elements of %zu bytes", count,
                size);
    return ptr;
}

void *yew_xreallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
        YEW_BUG("allocation size overflow: %zu * %zu", count, size);
    return yew_xrealloc(ptr, count * size);
}

char *yew_xstrdup(const char *str)
{
    size_t len = strlen(str) + 1U;
    char *copy = yew_xmalloc(len);

    (void)memcpy(copy, str, len);
    return copy;
}

char *yew_xrealpath(const char *path)
{
    char *resolved = realpath(path, NULL);
    char *copy;

    if (resolved == NULL)
        return NULL;
    copy = yew_xstrdup(resolved);
    free(resolved);
    return copy;
}

char *yew_xgetcwd(void)
{
    size_t cap = 256U;
    char *cwd = yew_xmalloc(cap);

    for (;;) {
        int saved;

        if (getcwd(cwd, cap) != NULL)
            return cwd;
        saved = errno;
        if (saved != ERANGE || cap > SIZE_MAX / 2U) {
            yew_xfree(cwd);
            errno = saved;
            return NULL;
        }
        cap *= 2U;
        cwd = yew_xrealloc(cwd, cap);
    }
}

void yew_xfree(void *ptr)
{
    free(ptr);
}

void yew_alloc_reset(void)
{
}

u64 yew_alloc_calls(void)
{
    return 0U;
}

void yew_alloc_report(Bytebuf *out)
{
    bytebuf_append(out, "# yew allocation report disabled\n", 33U);
}

#endif

void yew_memzero(void *bytes, size_t len)
{
    volatile u8 *p = bytes;

    while (len != 0U) {
        *p++ = 0U;
        len--;
    }
}
