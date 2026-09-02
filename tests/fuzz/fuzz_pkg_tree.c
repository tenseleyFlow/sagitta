#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/* Sprint 55: bounded arbitrary filesystem trees for yew_pkg_tree_hash. */

#include "fuzzlib.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mod/plug/pkg.h"

enum {
    PKG_FUZZ_MAX_DEPTH = 40,
    PKG_FUZZ_MAX_FILES = 64,
    PKG_FUZZ_FILE_BYTES = 512,
    PKG_FUZZ_NAME_CAP = 256
};

static bool write_fd(int fd, const u8 *data, size_t len)
{
    size_t off = 0U;

    while (off < len) {
        ssize_t wrote = write(fd, data + off, len - off);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        off += (size_t)wrote;
    }
    return true;
}

static int create_open_at(int dirfd, const char *name)
{
    static const char hex[] = "0123456789abcdef";
    char escaped[PKG_FUZZ_NAME_CAP * 3U];
    size_t len;
    size_t i;
    size_t at = 0U;
    int fd = openat(dirfd, name,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);

    if (fd >= 0 || errno != EILSEQ)
        return fd;

    /* Darwin rejects malformed UTF-8 path components at the syscall
     * boundary.  Keep feeding the original arbitrary bytes to filesystems
     * that accept them; on stricter filesystems, give each byte a distinct
     * ASCII spelling so the same corpus still drives name shape and order. */
    len = strlen(name);
    if (len >= PKG_FUZZ_NAME_CAP) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (i = 0U; i < len; i++) {
        u8 b = (u8)name[i];

        escaped[at++] = '~';
        escaped[at++] = hex[b >> 4U];
        escaped[at++] = hex[b & 0x0fU];
    }
    escaped[at] = '\0';
    return openat(dirfd, escaped,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
}

static bool create_file_at(int dirfd, const char *name, const u8 *data,
                           size_t len, bool executable)
{
    int fd = create_open_at(dirfd, name);
    bool ok;

    if (fd < 0)
        return false;
    ok = write_fd(fd, data, len);
    if (ok && executable)
        ok = fchmod(fd, 0700) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static void make_name(char out[PKG_FUZZ_NAME_CAP], size_t index,
                      const u8 *data, size_t len)
{
    static const char awkward[] = "aA09._-\n\"'[]{}() ";
    size_t at;
    size_t count;
    int n = snprintf(out, PKG_FUZZ_NAME_CAP, "f%03zu-", index);

    if (n <= 0 || n >= PKG_FUZZ_NAME_CAP) {
        out[0] = 'x';
        out[1] = '\0';
        return;
    }
    at = (size_t)n;
    count = len == 0U ? 1U : 1U + data[index % len] % 24U;
    while (count-- > 0U && at + 1U < PKG_FUZZ_NAME_CAP) {
        u8 b = len == 0U ? (u8)'x' : data[(index + at) % len];

        if (b == 0U || b == (u8)'/')
            b = (u8)awkward[(index + at) % (sizeof(awkward) - 1U)];
        out[at++] = (char)b;
    }
    out[at] = '\0';
}

static bool create_long_component(int rootfd, u8 seed)
{
    char name[PKG_FUZZ_NAME_CAP];
    size_t i;

    (void)memcpy(name, "long-", 5U);
    for (i = 5U; i < sizeof(name) - 1U; i++)
        name[i] = (char)('a' + (seed + i) % 26U);
    name[sizeof(name) - 1U] = '\0';
    return create_file_at(rootfd, name, &seed, 1U, false);
}

static bool create_tree(const char *plugin, const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    int rootfd = -1;
    int dirfd = -1;
    unsigned depth = len == 0U ? 0U : data[0] % (PKG_FUZZ_MAX_DEPTH + 1U);
    unsigned files = len < 2U ? 1U : 1U + data[1] % PKG_FUZZ_MAX_FILES;
    unsigned i;
    bool ok = false;

    rootfd = open(plugin, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (rootfd < 0)
        goto done;
    dirfd = dup(rootfd);
    if (dirfd < 0)
        goto done;
    for (i = 0U; i < depth; i++) {
        char name[32];
        int child;
        int n = snprintf(name, sizeof(name), "d%02u-%02x", i,
                         len == 0U ? 0U : data[(i + 2U) % len]);

        if (n <= 0 || (size_t)n >= sizeof(name) ||
            (mkdirat(dirfd, name, 0700) != 0 && errno != EEXIST))
            goto done;
        child = openat(dirfd, name,
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0)
            goto done;
        (void)close(dirfd);
        dirfd = child;
    }
    for (i = 0U; i < files; i++) {
        char name[PKG_FUZZ_NAME_CAP];
        u8 payload[PKG_FUZZ_FILE_BYTES];
        size_t payload_len;
        size_t k;

        make_name(name, i, data, len);
        payload_len = len == 0U ? 0U :
            data[(i * 3U + 2U) % len] % (sizeof(payload) + 1U);
        for (k = 0U; k < payload_len; k++)
            payload[k] = data[(i + k) % len];
        if (!create_file_at(dirfd, name, payload, payload_len,
                            len != 0U && (data[(i + 3U) % len] & 1U) != 0U))
            goto done;
    }
    if (len != 0U && (data[0] & 1U) != 0U &&
        !create_long_component(rootfd, data[len - 1U]))
        goto done;
    if (symlinkat((len != 0U && (data[0] & 2U) != 0U) ? "/" : "anchor",
                  rootfd, "outside-link") != 0)
        goto done;
    if (mkdirat(rootfd, ".git", 0700) != 0)
        goto done;
    {
        int gitfd = openat(rootfd, ".git",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        static const u8 ignored[] = "git metadata must not contribute";

        if (gitfd < 0 ||
            !create_file_at(gitfd, "churn", ignored, sizeof(ignored) - 1U,
                            false)) {
            if (gitfd >= 0)
                (void)close(gitfd);
            goto done;
        }
        (void)close(gitfd);
    }
    ok = true;
done:
    if (dirfd >= 0)
        (void)close(dirfd);
    if (rootfd >= 0)
        (void)close(rootfd);
    if (!ok)
        (void)snprintf(why, why_cap, "could not construct bounded tree: %s",
                       strerror(errno));
    return ok;
}

static bool mutate_file(const char *path, const u8 *data, size_t len)
{
    static const u8 fallback = 0x5aU;
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    bool ok;

    if (fd < 0)
        return false;
    ok = write_fd(fd, len == 0U ? &fallback : data, len == 0U ? 1U : 1U);
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool check_pkg_tree(const u8 *data, size_t len, char *why,
                           size_t why_cap)
{
    char root[] = "/tmp/yew-fuzz-pkg-tree-XXXXXX";
    char plugin[320];
    char anchor[352];
    char churn[384];
    char hash_a[YEW_PKG_TREE_HEX + 1U];
    char hash_b[YEW_PKG_TREE_HEX + 1U];
    char hash_changed[YEW_PKG_TREE_HEX + 1U];
    char hash_git[YEW_PKG_TREE_HEX + 1U];
    int rootfd = -1;
    int n;
    bool ok = false;

    if (mkdtemp(root) == NULL) {
        (void)snprintf(why, why_cap, "mkdtemp failed: %s", strerror(errno));
        return false;
    }
    n = snprintf(plugin, sizeof(plugin), "%s/plugin", root);
    if (n <= 0 || (size_t)n >= sizeof(plugin) || mkdir(plugin, 0700) != 0)
        goto done;
    n = snprintf(anchor, sizeof(anchor), "%s/anchor", plugin);
    rootfd = open(plugin,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (n <= 0 || (size_t)n >= sizeof(anchor) || rootfd < 0 ||
        !create_file_at(rootfd, "anchor", data, len < 64U ? len : 64U,
                        false))
        goto done;
    (void)close(rootfd);
    rootfd = -1;
    if (!create_tree(plugin, data, len, why, why_cap))
        goto done;
    if (!yew_pkg_tree_hash(plugin, hash_a, NULL) ||
        !yew_pkg_tree_hash(plugin, hash_b, NULL)) {
        (void)snprintf(why, why_cap, "tree hash rejected constructed tree");
        goto done;
    }
    if (strcmp(hash_a, hash_b) != 0) {
        (void)snprintf(why, why_cap, "identical tree hashed differently");
        goto done;
    }
    if (!mutate_file(anchor, data, len) ||
        !yew_pkg_tree_hash(plugin, hash_changed, NULL)) {
        (void)snprintf(why, why_cap, "content mutation could not be hashed");
        goto done;
    }
    if (strcmp(hash_a, hash_changed) == 0) {
        (void)snprintf(why, why_cap, "content mutation did not change hash");
        goto done;
    }
    n = snprintf(churn, sizeof(churn), "%s/.git/churn", plugin);
    if (n <= 0 || (size_t)n >= sizeof(churn) ||
        !mutate_file(churn, data, len) ||
        !yew_pkg_tree_hash(plugin, hash_git, NULL)) {
        (void)snprintf(why, why_cap, ".git mutation could not be hashed");
        goto done;
    }
    if (strcmp(hash_changed, hash_git) != 0) {
        (void)snprintf(why, why_cap, ".git metadata changed tree hash");
        goto done;
    }
    ok = true;
done:
    if (rootfd >= 0)
        (void)close(rootfd);
    {
        struct stat st;
        bool cleanup_ok = true;

        if (lstat(plugin, &st) == 0)
            cleanup_ok = yew_rmtree(plugin, root, NULL);
        if (rmdir(root) != 0)
            cleanup_ok = false;
        if (!cleanup_ok) {
            if (ok)
                (void)snprintf(why, why_cap, "fixture cleanup failed: %s",
                               strerror(errno));
            ok = false;
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_pkg_tree", NULL, check_pkg_tree);
}
