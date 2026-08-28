#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "mod/plug/pkg.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/data.h"
#include "mod/plug/manifest.h"
#include "text/file.h"
#include "util/log.h"
#include "util/sort.h"
#include "util/xdg.h"
#include "ws/trust.h"

/*
 * `yew pkg install gh:someone/thing` runs `git clone` and puts someone
 * else's code into your plugin directory. There is no registry, no review,
 * no namespace ownership, no signature, no vetting. The only thing between
 * you and that code is your judgement about the repository you named -- and
 * a URL is not a reputation. yew's capability prompts limit what an enabled
 * plugin can reach (fs, shell, net, clipboard) and nothing else; a plugin
 * holding no capabilities at all can still read every buffer you have open,
 * because it runs in the same VM and the same address space as your editor.
 * The prompts are a blast-radius control, not a security boundary. The tree
 * hash tells you the code changed since you agreed to it. Together that is
 * honest defence in depth, and it is not a substitute for reading a few
 * hundred lines of Fletch.
 */

typedef struct PkgJobResult {
    GitRun *run;
    bool done;
} PkgJobResult;

typedef struct TreeItem {
    char *rel;
    mode_t mode;
    off_t size;
    dev_t dev;
    ino_t ino;
    bool link;
} TreeItem;

typedef struct TreeVec {
    TreeItem *v;
    size_t n;
    size_t cap;
} TreeVec;

typedef struct SpecRow {
    const char *prefix;
    const char *fmt;
} SpecRow;

static const SpecRow spec_rows[] = {
    {"gh:", "https://github.com/%s.git"},
    {"gl:", "https://gitlab.com/%s.git"},
    {"cb:", "https://codeberg.org/%s.git"},
    {"sr:", "https://git.sr.ht/%s"},
};

static void pkg_print_sanitized(FILE *stream, const void *bytes, size_t len);

static void pkg_diag(DiagCtx *dc, const char *msg)
{
    if (dc != NULL)
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U}, "%s",
                     msg);
}

static void pkg_diag_stderr(void *ctx, FlDiagLevel level, FlSpan sp,
                            const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)rendered;
    (void)fputs("yew pkg: ", stderr);
    pkg_print_sanitized(stderr, msg, strlen(msg));
    (void)fputc('\n', stderr);
}

static void pkg_diag_errno(DiagCtx *dc, const char *what)
{
    char msg[512];

    (void)snprintf(msg, sizeof(msg), "%s: %s", what, strerror(errno));
    pkg_diag(dc, msg);
}

static void pkg_print_sanitized(FILE *stream, const void *bytes, size_t len)
{
    const u8 *p = bytes;
    size_t i;

    for (i = 0U; i < len; i++) {
        u8 c = p[i];

        if (c == '\n' || c == '\t' ||
            (c >= 0x20U && c != 0x7fU && !(c >= 0x80U && c <= 0x9fU)))
            (void)fputc((int)c, stream);
        else
            (void)fprintf(stream, "\\x%02x", (unsigned)c);
    }
}

static void pkg_print_field(FILE *stream, const char *text)
{
    const u8 *p = (const u8 *)text;

    while (*p != 0U) {
        u8 c = *p++;

        if (c >= 0x20U && c != 0x7fU &&
            !(c >= 0x80U && c <= 0x9fU))
            (void)fputc((int)c, stream);
        else
            (void)fprintf(stream, "\\x%02x", (unsigned)c);
    }
}

static void pkg_diag_arg(const char *prefix, const char *arg,
                         const char *suffix)
{
    (void)fputs(prefix, stderr);
    pkg_print_field(stderr, arg);
    (void)fputs(suffix, stderr);
}

static char *pkg_join(const char *a, const char *b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    bool slash = na != 0U && a[na - 1U] != '/';
    char *out = yew_xmalloc(na + (slash ? 1U : 0U) + nb + 1U);
    size_t at = 0U;

    (void)memcpy(out + at, a, na);
    at += na;
    if (slash)
        out[at++] = '/';
    (void)memcpy(out + at, b, nb + 1U);
    return out;
}

static char *pkg_strdup(const char *text)
{
    size_t len = strlen(text);
    char *out = yew_xmalloc(len + 1U);

    (void)memcpy(out, text, len + 1U);
    return out;
}

static bool pkg_fsync_dir(const char *path, DiagCtx *dc)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok;

    if (fd < 0) {
        pkg_diag_errno(dc, "cannot open package directory for fsync");
        return false;
    }
    do {
        ok = fsync(fd) == 0;
    } while (!ok && errno == EINTR);
    if (!ok)
        pkg_diag_errno(dc, "cannot fsync package directory");
    if (close(fd) != 0 && ok) {
        pkg_diag_errno(dc, "cannot close package directory");
        ok = false;
    }
    return ok;
}

static bool pkg_fsync_parent(const char *path, DiagCtx *dc)
{
    char *parent = pkg_strdup(path);
    char *slash = strrchr(parent, '/');
    bool ok = false;

    if (slash != NULL) {
        *slash = '\0';
        ok = pkg_fsync_dir(parent, dc);
    }
    yew_xfree(parent);
    return ok;
}

static bool pkg_fsync_tree_fd(int dirfd, bool top, DiagCtx *dc)
{
    int scanfd = dup(dirfd);
    DIR *dir;
    struct dirent *de;
    bool ok = true;

    if (scanfd < 0 || (dir = fdopendir(scanfd)) == NULL) {
        if (scanfd >= 0)
            (void)close(scanfd);
        pkg_diag_errno(dc, "cannot scan package tree for fsync");
        return false;
    }
    while ((de = readdir(dir)) != NULL && ok) {
        struct stat st;
        int fd;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
            (top && strcmp(de->d_name, ".git") == 0))
            continue;
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
        } else if (S_ISDIR(st.st_mode)) {
            fd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY |
                                             O_NOFOLLOW | O_CLOEXEC);
            ok = fd >= 0 && pkg_fsync_tree_fd(fd, false, dc);
            if (fd >= 0)
                (void)close(fd);
        } else if (S_ISREG(st.st_mode)) {
            fd = openat(dirfd, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            ok = fd >= 0 && fsync(fd) == 0;
            if (fd >= 0)
                (void)close(fd);
        }
    }
    if (ok)
        ok = fsync(dirfd) == 0;
    if (!ok)
        pkg_diag_errno(dc, "cannot durably sync package tree");
    (void)closedir(dir);
    return ok;
}

static bool pkg_fsync_tree(const char *path, DiagCtx *dc)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool ok;

    if (fd < 0) {
        pkg_diag_errno(dc, "cannot open package tree for fsync");
        return false;
    }
    ok = pkg_fsync_tree_fd(fd, true, dc);

    if (fd >= 0)
        (void)close(fd);
    return ok;
}

static bool pkg_read_file(const char *path, Bytebuf *out, bool *missing)
{
    u8 block[16384];
    int fd;

    *missing = false;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        *missing = errno == ENOENT;
        return false;
    }
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        if (n > 0) {
            bytebuf_append(out, block, (size_t)n);
            if (out->len > FL_DATA_MAX_BYTES) {
                errno = EFBIG;
                (void)close(fd);
                return false;
            }
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        (void)close(fd);
        return false;
    }
    return close(fd) == 0;
}

static bool pkg_map_get(const FlMap *map, const char *name, FlValue *out)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;
    size_t len = strlen(name);

    while (fl_map_iter(map, &cursor, &key, &value)) {
        const FlStr *s;

        if (key.t != (u8)FL_STR)
            continue;
        s = (const FlStr *)key.as.o;
        if (s->len == len && memcmp(s->b, name, len) == 0) {
            if (out != NULL)
                *out = value;
            return true;
        }
    }
    return false;
}

static bool pkg_key_eq(FlValue key, const char *name)
{
    const FlStr *s;
    size_t len;

    if (key.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)key.as.o;
    len = strlen(name);
    return s->len == len && memcmp(s->b, name, len) == 0;
}

static void pkg_map_set(PkgLock *lock, FlMap *map, const char *name,
                        FlValue value)
{
    FlStr *key = fl_str_new(&lock->vm, name, (u32)strlen(name));

    (void)fl_map_set(&lock->vm, map, FL_OBJ_V(FL_STR, key), value);
}

static FlValue pkg_unknown_map(PkgLock *lock, const FlMap *map,
                               const char *const *known, size_t nknown)
{
    FlMap *extra = fl_map_new(&lock->vm);
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    while (fl_map_iter(map, &cursor, &key, &value)) {
        size_t i;
        bool is_known = false;

        for (i = 0U; i < nknown; i++) {
            if (pkg_key_eq(key, known[i])) {
                is_known = true;
                break;
            }
        }
        if (!is_known)
            (void)fl_map_set(&lock->vm, extra, key, value);
    }
    return FL_OBJ_V(FL_MAP, extra);
}

static bool pkg_hex(const char *s, size_t n)
{
    size_t i;

    if (s == NULL || strlen(s) != n)
        return false;
    for (i = 0U; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    }
    return true;
}

static bool pkg_name_valid(const char *name, size_t len)
{
    size_t i;

    if (name == NULL || len == 0U || len > 32U)
        return false;
    for (i = 0U; i < len; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-'))
            return false;
    return true;
}

static bool pkg_field_safe(const char *text, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        u8 c = (u8)text[i];

        if (c < 0x20U || c == 0x7fU || (c >= 0x80U && c <= 0x9fU))
            return false;
    }
    return true;
}

bool yew_pkg_ref_valid(const char *ref)
{
    size_t i;
    size_t len;

    if (ref == NULL || ref[0] == '-' || (len = strlen(ref)) == 0U ||
        len > 255U || strstr(ref, "..") != NULL)
        return false;
    for (i = 0U; i < len; i++) {
        char c = ref[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '/' ||
              c == '-'))
            return false;
    }
    return true;
}

bool yew_pkg_pin_valid(const char *pin)
{
    if (pin == NULL)
        return false;
    if (strcmp(pin, "head") == 0)
        return true;
    if (strncmp(pin, "rev:", 4U) == 0)
        return pkg_hex(pin + 4U, YEW_PKG_REV_HEX);
    if (strncmp(pin, "tag:", 4U) == 0 ||
        strncmp(pin, "branch:", 7U) == 0)
        return yew_pkg_ref_valid(pin + (pin[0] == 't' ? 4U : 7U));
    return false;
}

bool yew_pkg_resolve_spec(const char *spec, Bytebuf *url, DiagCtx *dc)
{
    size_t i;
    static const char *const verbatim[] = {
        "https://", "http://", "ssh://", "git://", "file://"
    };

    if (spec == NULL || spec[0] == '\0' || spec[0] == '-' ||
        !pkg_field_safe(spec, strlen(spec)))
        goto bad;
    for (i = 0U; i < YEW_ARRAY_LEN(spec_rows); i++) {
        size_t n = strlen(spec_rows[i].prefix);

        if (strncmp(spec, spec_rows[i].prefix, n) == 0) {
            const char *tail = spec + n;

            if (*tail == '\0' || strchr(tail, '\n') != NULL ||
                strchr(tail, '\r') != NULL || strstr(tail, "..") != NULL)
                goto bad;
            bytebuf_printf(url, spec_rows[i].fmt, tail);
            return true;
        }
    }
    for (i = 0U; i < YEW_ARRAY_LEN(verbatim); i++) {
        if (strncmp(spec, verbatim[i], strlen(verbatim[i])) == 0) {
            if (strchr(spec, '\n') != NULL || strchr(spec, '\r') != NULL)
                goto bad;
            bytebuf_append(url, spec, strlen(spec));
            return true;
        }
    }
    if (spec[0] == '/' || strncmp(spec, "./", 2U) == 0 ||
        (strncmp(spec, "git@", 4U) == 0 && strchr(spec + 4U, ':') != NULL)) {
        if (strchr(spec, '\n') != NULL || strchr(spec, '\r') != NULL)
            goto bad;
        bytebuf_append(url, spec, strlen(spec));
        return true;
    }
bad:
    pkg_diag(dc, "invalid plugin source; use gh:, gl:, cb:, sr:, an explicit URL, git@host:path, /abs, ./rel, or file://");
    return false;
}

void yew_pkg_lock_init(PkgLock *lock)
{
    if (lock == NULL)
        return;
    (void)memset(lock, 0, sizeof(*lock));
    arena_init(&lock->a);
    interner_init(&lock->in, &lock->a);
    fl_diag_init(&lock->dc, &lock->a);
    (void)fl_vm_init(&lock->vm, &lock->a, &lock->in, &lock->dc);
    lock->schema = 1U;
    lock->extra = FL_OBJ_V(FL_MAP, fl_map_new(&lock->vm));
    lock->initialized = true;
}

void yew_pkg_lock_free(PkgLock *lock)
{
    if (lock == NULL || !lock->initialized)
        return;
    yew_xfree(lock->v.data);
    fl_vm_free(&lock->vm);
    interner_free(&lock->in);
    arena_free_all(&lock->a);
    (void)memset(lock, 0, sizeof(*lock));
}

static void pkg_lock_reset(PkgLock *lock)
{
    if (lock->initialized)
        yew_pkg_lock_free(lock);
    yew_pkg_lock_init(lock);
}

static void pkg_entry_push(PkgLock *lock, PkgEntry entry)
{
    if (lock->v.len == lock->v.cap) {
        size_t cap = lock->v.cap == 0U ? 8U : lock->v.cap * 2U;

        lock->v.data = yew_xreallocarray(lock->v.data, cap,
                                         sizeof(*lock->v.data));
        lock->v.cap = cap;
    }
    lock->v.data[lock->v.len++] = entry;
}

static const char *pkg_value_str(PkgLock *lock, FlValue value)
{
    const FlStr *s;

    if (value.t != (u8)FL_STR)
        return NULL;
    s = (const FlStr *)value.as.o;
    if (memchr(s->b, '\0', s->len) != NULL)
        return NULL;
    return arena_strndup(&lock->a, s->b, s->len);
}

static bool pkg_entry_read(PkgLock *lock, FlValue namev, FlValue value,
                           PkgEntry *out)
{
    static const char *const known[] = {
        "url", "shorthand", "rev", "pin", "tree", "installed_at",
        "updated_at"
    };
    const FlStr *name;
    const FlMap *map;
    FlValue field;

    (void)memset(out, 0, sizeof(*out));
    if (namev.t != (u8)FL_STR || value.t != (u8)FL_MAP)
        return false;
    name = (const FlStr *)namev.as.o;
    if (!pkg_name_valid(name->b, name->len))
        return false;
    out->name = arena_strndup(&lock->a, name->b, name->len);
    map = (const FlMap *)value.as.o;
    if (!pkg_map_get(map, "url", &field) ||
        (out->url = pkg_value_str(lock, field)) == NULL ||
        !pkg_field_safe(out->url, strlen(out->url)) ||
        !pkg_map_get(map, "shorthand", &field) ||
        (out->shorthand = pkg_value_str(lock, field)) == NULL ||
        !pkg_field_safe(out->shorthand, strlen(out->shorthand)) ||
        !pkg_map_get(map, "rev", &field))
        return false;
    {
        const char *s = pkg_value_str(lock, field);
        if (!pkg_hex(s, YEW_PKG_REV_HEX))
            return false;
        (void)memcpy(out->rev, s, sizeof(out->rev));
    }
    if (!pkg_map_get(map, "pin", &field) ||
        (out->pin = pkg_value_str(lock, field)) == NULL ||
        !yew_pkg_pin_valid(out->pin) || !pkg_map_get(map, "tree", &field))
        return false;
    {
        const char *s = pkg_value_str(lock, field);
        if (!pkg_hex(s, YEW_PKG_TREE_HEX))
            return false;
        (void)memcpy(out->tree, s, sizeof(out->tree));
    }
    if (!pkg_map_get(map, "installed_at", &field) ||
        field.t != (u8)FL_INT || field.as.i < 0)
        return false;
    out->installed_at = field.as.i;
    if (!pkg_map_get(map, "updated_at", &field) ||
        field.t != (u8)FL_INT || field.as.i < 0)
        return false;
    out->updated_at = field.as.i;
    out->extra = pkg_unknown_map(lock, map, known, YEW_ARRAY_LEN(known));
    return true;
}

static char *pkg_lock_path(bool ensure)
{
    char *dir = yew_xdg_data_dir();
    char *path;

    if (dir == NULL)
        return NULL;
    if (ensure && !yew_mkdirs(dir, 0700U)) {
        yew_xfree(dir);
        return NULL;
    }
    path = pkg_join(dir, "plugins.lock");
    yew_xfree(dir);
    return path;
}

bool yew_pkg_lock_load(PkgLock *lock, DiagCtx *dc)
{
    static const char *const known[] = {"schema", "plugins"};
    char *path;
    Bytebuf bytes;
    bool missing;
    FlValue root;
    FlValue schema;
    FlValue plugins;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;
    DiagCtx *parse_dc;
    char *source;

    if (lock == NULL)
        return false;
    pkg_lock_reset(lock);
    path = pkg_lock_path(false);
    if (path == NULL) {
        pkg_diag(dc, "cannot resolve yew data directory");
        return false;
    }
    bytebuf_init(&bytes);
    if (!pkg_read_file(path, &bytes, &missing)) {
        yew_xfree(path);
        bytebuf_free(&bytes);
        if (missing)
            return true;
        pkg_diag_errno(dc, "cannot read plugins.lock");
        return false;
    }
    yew_xfree(path);
    parse_dc = dc != NULL ? dc : &lock->dc;
    /* fl_data_read registers its source in the diagnostic context so a
     * later diagnostic can still render a caret against it.  The file
     * Bytebuf is temporary, but the context may outlive both this call and
     * the PkgLock (CLI commands reuse it for transaction diagnostics).
     * Keep the registered bytes in that context's arena. */
    source = arena_strndup(parse_dc->arena, (const char *)bytes.data,
                           bytes.len);
    root = fl_data_read(&lock->vm, source, bytes.len, parse_dc);
    bytebuf_free(&bytes);
    if (root.t != (u8)FL_MAP ||
        !pkg_map_get((const FlMap *)root.as.o, "schema", &schema) ||
        schema.t != (u8)FL_INT || schema.as.i != 1 ||
        !pkg_map_get((const FlMap *)root.as.o, "plugins", &plugins) ||
        plugins.t != (u8)FL_MAP) {
        lock->corrupt = true;
        pkg_diag(dc, "plugins.lock is corrupt or has an unsupported schema");
        return false;
    }
    lock->schema = 1U;
    lock->extra = pkg_unknown_map(lock, (const FlMap *)root.as.o, known,
                                  YEW_ARRAY_LEN(known));
    while (fl_map_iter((const FlMap *)plugins.as.o, &cursor, &key, &value)) {
        PkgEntry entry;

        if (!pkg_entry_read(lock, key, value, &entry)) {
            lock->corrupt = true;
            pkg_diag(dc, "plugins.lock contains an invalid plugin entry");
            return false;
        }
        pkg_entry_push(lock, entry);
    }
    return true;
}

PkgEntry *yew_pkg_lock_find(PkgLock *lock, const char *name, u32 nlen)
{
    size_t i;

    if (lock == NULL || name == NULL)
        return NULL;
    for (i = 0U; i < lock->v.len; i++) {
        if (strlen(lock->v.data[i].name) == nlen &&
            memcmp(lock->v.data[i].name, name, nlen) == 0)
            return &lock->v.data[i];
    }
    return NULL;
}

static int pkg_entry_ptr_cmp(const void *a, const void *b, void *ctx)
{
    const PkgEntry *const *ea = a;
    const PkgEntry *const *eb = b;

    (void)ctx;
    return strcmp((*ea)->name, (*eb)->name);
}

static int pkg_string_ptr_cmp(const void *a, const void *b, void *ctx)
{
    const char *const *left = a;
    const char *const *right = b;

    (void)ctx;
    return strcmp(*left, *right);
}

static void pkg_copy_extra(PkgLock *lock, FlMap *dst, FlValue extra,
                           const char *const *known, size_t nknown)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    if (extra.t != (u8)FL_MAP)
        return;
    while (fl_map_iter((const FlMap *)extra.as.o, &cursor, &key, &value)) {
        size_t i;
        bool skip = false;

        for (i = 0U; i < nknown; i++) {
            if (pkg_key_eq(key, known[i])) {
                skip = true;
                break;
            }
        }
        if (!skip)
            (void)fl_map_set(&lock->vm, dst, key, value);
    }
}

bool yew_pkg_lock_save(const PkgLock *const_lock, DiagCtx *dc)
{
    static const char header[] =
        "# yew plugin lockfile.  Generated by `yew pkg`; hand-editable, but\n"
        "# `yew pkg doctor` is the only thing that will agree with your edits.\n";
    static const char *const top_known[] = {"schema", "plugins"};
    static const char *const entry_known[] = {
        "url", "shorthand", "rev", "pin", "tree", "installed_at",
        "updated_at"
    };
    PkgLock *lock = (PkgLock *)const_lock;
    PkgEntry **order;
    FlMap *root;
    FlMap *plugins;
    Bytebuf out;
    Bytebuf current;
    char *path;
    size_t i;
    bool ok;

    if (lock == NULL || !lock->initialized || lock->corrupt) {
        pkg_diag(dc, "refusing to overwrite a corrupt plugins.lock");
        return false;
    }
    root = fl_map_new(&lock->vm);
    plugins = fl_map_new(&lock->vm);
    pkg_map_set(lock, root, "schema", FL_INT_V(1));
    pkg_map_set(lock, root, "plugins", FL_OBJ_V(FL_MAP, plugins));
    order = yew_xcalloc(lock->v.len == 0U ? 1U : lock->v.len,
                        sizeof(*order));
    for (i = 0U; i < lock->v.len; i++)
        order[i] = &lock->v.data[i];
    yew_sort_stable(order, lock->v.len, sizeof(*order), pkg_entry_ptr_cmp,
                    NULL);
    for (i = 0U; i < lock->v.len; i++) {
        PkgEntry *entry = order[i];
        FlMap *map = fl_map_new(&lock->vm);

        pkg_map_set(lock, map, "url",
                    FL_OBJ_V(FL_STR, fl_str_new(&lock->vm, entry->url,
                                               (u32)strlen(entry->url))));
        pkg_map_set(lock, map, "shorthand",
                    FL_OBJ_V(FL_STR,
                             fl_str_new(&lock->vm, entry->shorthand,
                                        (u32)strlen(entry->shorthand))));
        pkg_map_set(lock, map, "rev",
                    FL_OBJ_V(FL_STR, fl_str_new(&lock->vm, entry->rev, 40U)));
        pkg_map_set(lock, map, "pin",
                    FL_OBJ_V(FL_STR, fl_str_new(&lock->vm, entry->pin,
                                               (u32)strlen(entry->pin))));
        pkg_map_set(lock, map, "tree",
                    FL_OBJ_V(FL_STR, fl_str_new(&lock->vm, entry->tree, 16U)));
        pkg_map_set(lock, map, "installed_at", FL_INT_V(entry->installed_at));
        pkg_map_set(lock, map, "updated_at", FL_INT_V(entry->updated_at));
        pkg_copy_extra(lock, map, entry->extra, entry_known,
                       YEW_ARRAY_LEN(entry_known));
        (void)fl_map_set(&lock->vm, plugins,
                         FL_OBJ_V(FL_STR,
                                  fl_str_new(&lock->vm, entry->name,
                                             (u32)strlen(entry->name))),
                         FL_OBJ_V(FL_MAP, map));
    }
    yew_xfree(order);
    pkg_copy_extra(lock, root, lock->extra, top_known,
                   YEW_ARRAY_LEN(top_known));
    bytebuf_init(&out);
    bytebuf_append(&out, header, sizeof(header) - 1U);
    fl_data_write(&out, FL_OBJ_V(FL_MAP, root), 0U);
    path = pkg_lock_path(true);
    bytebuf_init(&current);
    if (path != NULL) {
        bool missing = false;

        ok = pkg_read_file(path, &current, &missing) &&
             current.len == out.len &&
             (out.len == 0U || memcmp(current.data, out.data, out.len) == 0);
        if (!ok) {
            YewAtomicWriteResult saved =
                yew_file_write_atomic_result(path, out.data, out.len, 0600);

            ok = saved.error == YEW_SAVE_OK;
            if (saved.committed && saved.error != YEW_SAVE_OK)
                (void)fprintf(stderr,
                              "yew pkg: error: plugins.lock was replaced but its directory could not be synchronized; retaining the package transaction for recovery\n");
        }
    } else {
        ok = false;
    }
    if (!ok)
        pkg_diag_errno(dc, "cannot write plugins.lock");
    yew_xfree(path);
    bytebuf_free(&current);
    bytebuf_free(&out);
    return ok;
}

static int tree_item_cmp(const void *a, const void *b, void *ctx)
{
    const TreeItem *ia = a;
    const TreeItem *ib = b;

    (void)ctx;
    return strcmp(ia->rel, ib->rel);
}

static void tree_vec_free(TreeVec *items)
{
    size_t i;

    for (i = 0U; i < items->n; i++)
        yew_xfree(items->v[i].rel);
    yew_xfree(items->v);
    (void)memset(items, 0, sizeof(*items));
}

static void tree_vec_push(TreeVec *items, TreeItem item)
{
    if (items->n == items->cap) {
        size_t cap = items->cap == 0U ? 32U : items->cap * 2U;

        items->v = yew_xreallocarray(items->v, cap, sizeof(*items->v));
        items->cap = cap;
    }
    items->v[items->n++] = item;
}

static bool tree_scan(int dirfd, const char *prefix, TreeVec *items,
                      DiagCtx *dc)
{
    int scanfd = dup(dirfd);
    DIR *dir;
    struct dirent *de;
    bool ok = true;

    if (scanfd < 0) {
        pkg_diag_errno(dc, "cannot duplicate plugin directory");
        return false;
    }
    dir = fdopendir(scanfd);
    if (dir == NULL) {
        (void)close(scanfd);
        pkg_diag_errno(dc, "cannot scan plugin directory");
        return false;
    }
    errno = 0;
    while ((de = readdir(dir)) != NULL) {
        struct stat st;
        char *rel;
        int child;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (prefix[0] == '\0' && strcmp(de->d_name, ".git") == 0)
            continue;
        rel = prefix[0] == '\0' ? pkg_strdup(de->d_name)
                                 : pkg_join(prefix, de->d_name);
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            yew_xfree(rel);
            pkg_diag_errno(dc, "cannot stat plugin tree entry");
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            child = openat(dirfd, de->d_name,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                yew_xfree(rel);
                pkg_diag_errno(dc, "cannot open plugin subdirectory");
                ok = false;
                break;
            }
            ok = tree_scan(child, rel, items, dc);
            (void)close(child);
            yew_xfree(rel);
            if (!ok)
                break;
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            tree_vec_push(items, (TreeItem){rel, st.st_mode, st.st_size,
                                            st.st_dev, st.st_ino,
                                            S_ISLNK(st.st_mode)});
        } else {
            yew_xfree(rel);
            pkg_diag(dc, "plugin tree contains a non-file filesystem entry");
            ok = false;
            break;
        }
        errno = 0;
    }
    if (ok && errno != 0) {
        pkg_diag_errno(dc, "cannot read plugin directory");
        ok = false;
    }
    (void)closedir(dir);
    return ok;
}

static int tree_open_dir_path(int rootfd, const char *path)
{
    char *copy;
    char *part;
    char *save = NULL;
    int fd = dup(rootfd);

    if (fd < 0 || path[0] == '\0')
        return fd;
    copy = pkg_strdup(path);
    for (part = strtok_r(copy, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                   O_CLOEXEC);
        (void)close(fd);
        if (next < 0) {
            yew_xfree(copy);
            return -1;
        }
        fd = next;
    }
    yew_xfree(copy);
    return fd;
}

static int tree_open_parent(int rootfd, const char *rel, char **base)
{
    char *copy = pkg_strdup(rel);
    char *slash = strrchr(copy, '/');
    int fd;

    if (slash == NULL) {
        *base = copy;
        return dup(rootfd);
    }
    *slash = '\0';
    *base = pkg_strdup(slash + 1U);
    fd = tree_open_dir_path(rootfd, copy);
    yew_xfree(copy);
    return fd;
}

static u64 pkg_fnv(u64 h, const void *bytes, size_t len)
{
    const u8 *p = bytes;
    size_t i;

    for (i = 0U; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

bool yew_pkg_tree_hash(const char *dir, char out[17], DiagCtx *dc)
{
    TreeVec items = {0};
    u64 h = UINT64_C(14695981039346656037);
    int rootfd;
    size_t i;
    bool ok = false;

    if (dir == NULL || out == NULL)
        return false;
    rootfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootfd < 0) {
        pkg_diag_errno(dc, "cannot open plugin tree");
        return false;
    }
    if (!tree_scan(rootfd, "", &items, dc))
        goto done;
    yew_sort_stable(items.v, items.n, sizeof(*items.v), tree_item_cmp, NULL);
    for (i = 0U; i < items.n; i++) {
        const TreeItem *item = &items.v[i];
        static const u8 zero = 0U;

        h = pkg_fnv(h, item->rel, strlen(item->rel));
        h = pkg_fnv(h, &zero, 1U);
        if (item->link) {
            size_t cap = item->size > 0 ? (size_t)item->size + 1U : 256U;
            char *target = yew_xmalloc(cap);
            char *base = NULL;
            int parentfd = tree_open_parent(rootfd, item->rel, &base);
            struct stat now;
            ssize_t n = parentfd < 0 ? -1 :
                readlinkat(parentfd, base, target, cap);

            h = pkg_fnv(h, "L\0", 2U);
            if (parentfd < 0 ||
                fstatat(parentfd, base, &now, AT_SYMLINK_NOFOLLOW) != 0 ||
                now.st_dev != item->dev || now.st_ino != item->ino ||
                !S_ISLNK(now.st_mode) || n < 0 || (size_t)n == cap) {
                if (parentfd >= 0)
                    (void)close(parentfd);
                yew_xfree(base);
                yew_xfree(target);
                pkg_diag(dc, "plugin tree changed while hashing symlink");
                goto done;
            }
            (void)close(parentfd);
            yew_xfree(base);
            h = pkg_fnv(h, target, (size_t)n);
            yew_xfree(target);
        } else {
            u8 size_le[8];
            u64 size;
            int fd;
            u8 block[16384];
            size_t j;

            h = pkg_fnv(h, (item->mode & 0111) != 0 ? "X\0" : "F\0", 2U);
            if (item->size < 0) {
                pkg_diag(dc, "plugin file has a negative size");
                goto done;
            }
            size = (u64)item->size;
            for (j = 0U; j < 8U; j++)
                size_le[j] = (u8)(size >> (j * 8U));
            h = pkg_fnv(h, size_le, sizeof(size_le));
            {
                char *base = NULL;
                int parentfd = tree_open_parent(rootfd, item->rel, &base);

                fd = parentfd < 0 ? -1 :
                    openat(parentfd, base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if (parentfd >= 0)
                    (void)close(parentfd);
                yew_xfree(base);
            }
            if (fd < 0) {
                pkg_diag_errno(dc, "cannot open plugin file");
                goto done;
            }
            {
                struct stat now;

                if (fstat(fd, &now) != 0 || now.st_dev != item->dev ||
                    now.st_ino != item->ino || !S_ISREG(now.st_mode) ||
                    now.st_size != item->size ||
                    (now.st_mode & 0111) != (item->mode & 0111)) {
                    (void)close(fd);
                    pkg_diag(dc, "plugin tree changed while hashing file");
                    goto done;
                }
            }
            for (;;) {
                ssize_t n = read(fd, block, sizeof(block));

                if (n > 0) {
                    h = pkg_fnv(h, block, (size_t)n);
                    continue;
                }
                if (n == 0)
                    break;
                if (errno == EINTR)
                    continue;
                pkg_diag_errno(dc, "cannot read plugin file");
                (void)close(fd);
                goto done;
            }
            if (close(fd) != 0) {
                pkg_diag_errno(dc, "cannot close plugin file");
                goto done;
            }
        }
        h = pkg_fnv(h, &zero, 1U);
    }
    (void)snprintf(out, 17U, "%016llx", (unsigned long long)h);
    ok = true;
done:
    tree_vec_free(&items);
    (void)close(rootfd);
    return ok;
}

bool yew_pkg_expected_tree(const char *name, char out[17], bool *managed,
                           DiagCtx *dc)
{
    PkgLock lock;
    PkgEntry *entry;
    bool ok;

    if (name == NULL || out == NULL || managed == NULL)
        return false;
    (void)memset(&lock, 0, sizeof(lock));
    *managed = false;
    out[0] = '\0';
    ok = yew_pkg_lock_load(&lock, dc);
    if (ok) {
        entry = yew_pkg_lock_find(&lock, name, (u32)strlen(name));
        if (entry != NULL) {
            (void)memcpy(out, entry->tree, 17U);
            *managed = true;
        }
    }
    yew_pkg_lock_free(&lock);
    return ok;
}

static bool rmtree_fd(int dirfd, DiagCtx *dc)
{
    int scanfd;
    DIR *dir;
    struct dirent *de;
    bool ok = true;
    struct stat self;

    if (fstat(dirfd, &self) != 0 ||
        (((self.st_mode & 0300U) != 0300U) &&
         fchmod(dirfd, self.st_mode | 0700U) != 0)) {
        pkg_diag_errno(dc, "cannot make plugin directory removable");
        return false;
    }

    scanfd = dup(dirfd);
    if (scanfd < 0)
        return false;
    dir = fdopendir(scanfd);
    if (dir == NULL) {
        (void)close(scanfd);
        return false;
    }
    errno = 0;
    while ((de = readdir(dir)) != NULL) {
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(dirfd, de->d_name,
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                   O_CLOEXEC);

            if (child < 0 || !rmtree_fd(child, dc)) {
                if (child >= 0)
                    (void)close(child);
                ok = false;
                break;
            }
            (void)close(child);
            if (unlinkat(dirfd, de->d_name, AT_REMOVEDIR) != 0) {
                ok = false;
                break;
            }
        } else if (unlinkat(dirfd, de->d_name, 0) != 0) {
            ok = false;
            break;
        }
        errno = 0;
    }
    if (ok && errno != 0)
        ok = false;
    (void)closedir(dir);
    if (!ok)
        pkg_diag_errno(dc, "cannot remove plugin tree");
    return ok;
}

bool yew_rmtree(const char *path, const char *must_contain, DiagCtx *dc)
{
    char root[PATH_MAX];
    char target[PATH_MAX];
    char parent[PATH_MAX];
    const char *base;
    char *slash;
    size_t nroot;
    int parentfd;
    int targetfd;
    bool ok;

    if (path == NULL || must_contain == NULL ||
        realpath(must_contain, root) == NULL || realpath(path, target) == NULL) {
        pkg_diag_errno(dc, "cannot resolve removal path");
        return false;
    }
    nroot = strlen(root);
    if (strncmp(target, root, nroot) != 0 || target[nroot] != '/' ||
        target[nroot + 1U] == '\0') {
        pkg_diag(dc, "refusing to remove a path outside the plugin root");
        return false;
    }
    (void)memcpy(parent, target, strlen(target) + 1U);
    slash = strrchr(parent, '/');
    if (slash == NULL || slash[1] == '\0')
        return false;
    *slash = '\0';
    base = slash + 1U;
    parentfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parentfd < 0) {
        pkg_diag_errno(dc, "cannot open plugin parent directory");
        return false;
    }
    targetfd = openat(parentfd, base,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (targetfd < 0) {
        (void)close(parentfd);
        pkg_diag_errno(dc, "cannot open plugin directory for removal");
        return false;
    }
    ok = rmtree_fd(targetfd, dc);
    (void)close(targetfd);
    if (ok && unlinkat(parentfd, base, AT_REMOVEDIR) != 0) {
        pkg_diag_errno(dc, "cannot remove plugin directory");
        ok = false;
    }
    (void)close(parentfd);
    return ok;
}

void yew_pkg_git_run_init(GitRun *run)
{
    if (run == NULL)
        return;
    (void)memset(run, 0, sizeof(*run));
    bytebuf_init(&run->out);
    bytebuf_init(&run->err);
    run->status = -1;
}

void yew_pkg_git_run_free(GitRun *run)
{
    if (run == NULL)
        return;
    bytebuf_free(&run->out);
    bytebuf_free(&run->err);
    (void)memset(run, 0, sizeof(*run));
}

static void pkg_job_complete(void *owner, Ed *ed, const YewJob *job)
{
    PkgJobResult *result = owner;

    (void)ed;
    bytebuf_append(&result->run->out, job->collect.data, job->collect.len);
    bytebuf_append(&result->run->err, job->collect_err.data,
                   job->collect_err.len);
    result->run->timed_out = job->state == YEW_JOB_TIMEOUT;
    result->run->exec_failed = job->state == YEW_JOB_EXECFAIL;
    if (job->state == YEW_JOB_EXITED)
        result->run->status = job->exit_code;
    else if (job->state == YEW_JOB_EXECFAIL)
        result->run->status = 127;
    else
        result->run->status = 128 + job->termsig;
    result->done = true;
}

static void pkg_job_destroy(void *owner)
{
    (void)owner;
}

bool yew_pkg_git(const char *const *argv, u32 nargv, i64 timeout_ms,
                 bool c_locale, GitRun *out)
{
    static const YewJobCallbackOps ops = {pkg_job_complete, pkg_job_destroy};
    static const char *const env_set_c[] = {
        "GIT_TERMINAL_PROMPT=0", "GIT_ASKPASS=", "SSH_ASKPASS_REQUIRE=never",
        "PAGER=cat", "GIT_PAGER=cat", "LC_ALL=C", NULL
    };
    static const char *const env_set_user[] = {
        "GIT_TERMINAL_PROMPT=0", "GIT_ASKPASS=", "SSH_ASKPASS_REQUIRE=never",
        "PAGER=cat", "GIT_PAGER=cat", NULL
    };
    static const char *const env_unset[] = {
        "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
        "GIT_COMMON_DIR", NULL
    };
    Ed ed;
    YewJobSpec spec = {0};
    PkgJobResult result;
    char **owned_argv;
    char err[256];
    char cwd[PATH_MAX];
    u32 id;
    u32 i;
    i64 started;
    bool ok = false;

    if (argv == NULL || nargv == 0U || out == NULL)
        return false;
    yew_pkg_git_run_init(out);
    owned_argv = yew_xcalloc((size_t)nargv + 1U, sizeof(*owned_argv));
    for (i = 0U; i < nargv; i++)
        owned_argv[i] = (char *)argv[i];
    (void)memset(&result, 0, sizeof(result));
    result.run = out;
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed))
        goto done;
    spec.argv = owned_argv;
    spec.cwd = getcwd(cwd, sizeof(cwd)) != NULL ? cwd : "/";
    spec.sink = YEW_SINK_CALLBACK;
    spec.timeout_ms = timeout_ms;
    spec.internal = true;
    spec.env_set = c_locale ? env_set_c : env_set_user;
    spec.env_unset = env_unset;
    spec.callback_owner = &result;
    spec.callback_ops = &ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    if (id == 0U)
        goto done;
    started = yew_now_ms();
    while (!result.done) {
        struct pollfd fds[YEW_JOB_MAX * 4U];
        u32 nfds = 0U;

        yew_job_collect_fds(&ed, fds, &nfds);
        if (nfds != 0U)
            (void)poll(fds, (nfds_t)nfds, 20);
        else
            (void)poll(NULL, 0U, 5);
        yew_job_pump(&ed, fds, nfds);
        yew_job_reap(&ed);
        yew_job_tick(&ed, yew_now_ms());
        (void)yew_job_settle(&ed);
        if (yew_now_ms() - started > timeout_ms + 5000 && timeout_ms > 0)
            break;
    }
    ok = result.done;
done:
    yew_ed_free(&ed);
    yew_xfree(owned_argv);
    return ok;
}

static char *pkg_buf_string(const Bytebuf *buf)
{
    char *text = yew_xmalloc(buf->len + 1U);

    if (buf->len != 0U)
        (void)memcpy(text, buf->data, buf->len);
    text[buf->len] = '\0';
    return text;
}

static bool pkg_run_ok(const char *const *argv, u32 nargv, i64 timeout,
                       bool c_locale, GitRun *run, const char *op)
{
    if (!yew_pkg_git(argv, nargv, timeout, c_locale, run)) {
        (void)fprintf(stderr, "yew pkg: error: cannot start git %s\n", op);
        return false;
    }
    if (run->timed_out) {
        (void)fprintf(stderr, "yew pkg: error: git %s timed out after %llds "
                              "(--timeout SECONDS)\n",
                      op, (long long)(timeout / 1000));
        return false;
    }
    if (run->exec_failed) {
        (void)fprintf(stderr,
                      "yew pkg: error: git not found in PATH\n"
                      "yew pkg installs plugins over git; you can also copy "
                      "a plugin directory into <plugins> by hand\n");
        return false;
    }
    if (run->status != 0) {
        (void)fprintf(stderr, "yew pkg: error: git %s failed (exit %d)\n",
                      op, run->status);
        if (run->err.len != 0U)
        {
            (void)fprintf(stderr, "git said:\n  ");
            pkg_print_sanitized(stderr, run->err.data, run->err.len);
            if (run->err.data[run->err.len - 1U] != '\n')
                (void)fputc('\n', stderr);
        }
        return false;
    }
    return true;
}

static bool pkg_git_probe(void)
{
    const char *const argv[] = {"git", "--version"};
    GitRun run;
    char *text;
    unsigned int major = 0U;
    unsigned int minor = 0U;
    bool ok;

    if (!pkg_run_ok(argv, YEW_ARRAY_LEN(argv), 5000, true, &run, "probe")) {
        yew_pkg_git_run_free(&run);
        return false;
    }
    text = pkg_buf_string(&run.out);
    ok = sscanf(text, "git version %u.%u", &major, &minor) == 2 &&
         (major > 2U || (major == 2U && minor >= 24U));
    if (!ok)
        (void)fprintf(stderr,
                      "yew pkg: error: git %u.%u is too old; git 2.24 or "
                      "newer is required\n",
                      major, minor);
    yew_xfree(text);
    yew_pkg_git_run_free(&run);
    return ok;
}

static char *pkg_plugins_root(bool ensure)
{
    char *data = yew_xdg_data_dir();
    char *root;

    if (data == NULL)
        return NULL;
    if (ensure && !yew_mkdirs(data, 0700U)) {
        yew_xfree(data);
        return NULL;
    }
    root = pkg_join(data, "plugins");
    yew_xfree(data);
    if (ensure && !yew_mkdirs(root, 0700U)) {
        yew_xfree(root);
        return NULL;
    }
    return root;
}

static bool pkg_resolve_rev(const char *dir, const char *expr, char rev[41])
{
    const char *const argv[] = {"git", "-C", dir, "rev-parse", "--verify",
                                "--end-of-options", expr};
    GitRun run;
    char *text;
    size_t len;
    bool ok;

    ok = pkg_run_ok(argv, YEW_ARRAY_LEN(argv), 10000, true, &run,
                    "rev-parse");
    if (!ok) {
        yew_pkg_git_run_free(&run);
        return false;
    }
    text = pkg_buf_string(&run.out);
    len = strcspn(text, "\r\n");
    text[len] = '\0';
    ok = pkg_hex(text, 40U);
    if (ok)
        (void)memcpy(rev, text, 41U);
    else
        (void)fprintf(stderr, "yew pkg: error: git returned an invalid rev\n");
    yew_xfree(text);
    yew_pkg_git_run_free(&run);
    return ok;
}

static const char *pkg_pin_expr(const char *pin, char *buf, size_t cap,
                                bool remote)
{
    int n;

    if (strncmp(pin, "rev:", 4U) == 0) {
        n = snprintf(buf, cap, "%s^{commit}", pin + 4U);
    } else if (strncmp(pin, "tag:", 4U) == 0) {
        n = snprintf(buf, cap, "refs/tags/%s^{commit}", pin + 4U);
    } else if (strncmp(pin, "branch:", 7U) == 0) {
        n = snprintf(buf, cap, "%s%s%s^{commit}",
                     remote ? "refs/remotes/origin/" : "refs/heads/",
                     pin + 7U, "");
    } else {
        n = snprintf(buf, cap, "%s", remote ?
                     "refs/remotes/origin/HEAD^{commit}" : "HEAD^{commit}");
    }
    return n > 0 && (size_t)n < cap ? buf : NULL;
}

static bool pkg_checkout(const char *dir, const char *rev, bool force)
{
    const char *argv[10];
    u32 n = 0U;
    GitRun run;
    bool ok;

    argv[n++] = "git";
    argv[n++] = "-C";
    argv[n++] = dir;
    argv[n++] = "checkout";
    argv[n++] = "--quiet";
    if (force)
        argv[n++] = "--force";
    argv[n++] = "--detach";
    argv[n++] = "--end-of-options";
    argv[n++] = rev;
    ok = pkg_run_ok(argv, n, 30000, false, &run, "checkout");
    yew_pkg_git_run_free(&run);
    return ok;
}

static void pkg_warn_gitmodules(const char *dir)
{
    char *path = pkg_join(dir, ".gitmodules");
    struct stat st;

    if (lstat(path, &st) == 0)
        (void)fprintf(stderr,
                      "yew pkg: warning: repository contains .gitmodules; submodules are not initialized\n");
    yew_xfree(path);
}

static char *pkg_manifest_name(PkgLock *lock, const char *dir, DiagCtx *dc)
{
    char *path = pkg_join(dir, "plugin.fl");
    Bytebuf bytes;
    bool missing;
    FlValue root;
    FlValue name;
    char *copy = NULL;
    DiagCtx *parse_dc;
    char *source;

    bytebuf_init(&bytes);
    if (!pkg_read_file(path, &bytes, &missing)) {
        pkg_diag(dc, missing ? "plugin repository has no plugin.fl"
                             : "cannot read plugin.fl");
        goto done;
    }
    parse_dc = dc != NULL ? dc : &lock->dc;
    source = arena_strndup(parse_dc->arena, (const char *)bytes.data,
                           bytes.len);
    root = fl_data_read(&lock->vm, source, bytes.len, parse_dc);
    if (root.t == (u8)FL_MAP &&
        pkg_map_get((const FlMap *)root.as.o, "name", &name) &&
        name.t == (u8)FL_STR) {
        const FlStr *text = (const FlStr *)name.as.o;

        copy = yew_xmalloc((size_t)text->len + 1U);
        if (text->len != 0U)
            (void)memcpy(copy, text->b, text->len);
        copy[text->len] = '\0';
    }
    if (copy == NULL)
        pkg_diag(dc, "plugin.fl has no string name");
done:
    bytebuf_free(&bytes);
    yew_xfree(path);
    return copy;
}

static YewTrustWriteResult pkg_trust_write_durable(YewTrustDb *db,
                                                   const char *what)
{
    YewTrustWriteResult result =
        yew_trust_db_write_result(db, time(NULL),
                                  YEW_TRUST_PRUNE_DAYS_DEFAULT);

    if (!result.ok)
        (void)fprintf(stderr,
                      result.committed ?
                      "yew pkg: error: %s trust policy was committed but not durably synchronized\n" :
                      "yew pkg: error: %s trust policy was not committed\n",
                      what);
    return result;
}

static void pkg_lock_remove_at(PkgLock *lock, size_t at)
{
    if (at + 1U < lock->v.len)
        (void)memmove(&lock->v.data[at], &lock->v.data[at + 1U],
                      (lock->v.len - at - 1U) * sizeof(*lock->v.data));
    lock->v.len--;
}

typedef struct PkgTxnPaths {
    char *state;
    char *intent;
    char *trust_before;
    char *lock_before;
    char *trust;
    char *lock;
} PkgTxnPaths;

static void pkg_txn_paths_free(PkgTxnPaths *paths)
{
    yew_xfree(paths->lock);
    yew_xfree(paths->trust);
    yew_xfree(paths->lock_before);
    yew_xfree(paths->trust_before);
    yew_xfree(paths->intent);
    yew_xfree(paths->state);
    (void)memset(paths, 0, sizeof(*paths));
}

static bool pkg_txn_paths(PkgTxnPaths *paths, bool ensure)
{
    (void)memset(paths, 0, sizeof(*paths));
    paths->state = yew_xdg_state_dir();
    if (paths->state == NULL ||
        (ensure && !yew_mkdirs(paths->state, 0700U)))
        goto fail;
    paths->intent = pkg_join(paths->state, "pkg.intent");
    paths->trust_before = pkg_join(paths->state, "pkg.trust.before");
    paths->lock_before = pkg_join(paths->state, "pkg.lock.before");
    paths->trust = pkg_join(paths->state, "trust.fl");
    paths->lock = pkg_lock_path(false);
    if (paths->lock == NULL)
        goto fail;
    return true;
fail:
    pkg_txn_paths_free(paths);
    return false;
}

static bool pkg_txn_write_exact(const char *path, const u8 *bytes, size_t len)
{
    YewAtomicWriteResult result =
        yew_file_write_atomic_result(path, bytes, len, 0600);

    return result.error == YEW_SAVE_OK;
}

static bool pkg_txn_decimal_pair_valid(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    if (!isdigit(*p))
        return false;
    while (isdigit(*p))
        p++;
    if (*p++ != '-' || !isdigit(*p))
        return false;
    while (isdigit(*p))
        p++;
    return *p == '\0';
}

static bool pkg_txn_aux_valid(char op, const char *name, const char *aux)
{
    static const char install_prefix[] = ".pkg-tmp-";
    char remove_prefix[80];
    int n;

    if (op == 'I') {
        return strncmp(aux, install_prefix, sizeof(install_prefix) - 1U) ==
                   0 &&
               pkg_txn_decimal_pair_valid(aux + sizeof(install_prefix) -
                                                 1U);
    }
    n = snprintf(remove_prefix, sizeof(remove_prefix), ".pkg-trash-%s-",
                 name);
    return n > 0 && (size_t)n < sizeof(remove_prefix) &&
           strncmp(aux, remove_prefix, (size_t)n) == 0 &&
           pkg_txn_decimal_pair_valid(aux + (size_t)n);
}

static bool pkg_remove_path_sync(const char *path, const char *parent,
                                 DiagCtx *dc)
{
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno != ENOENT) {
            pkg_diag_errno(dc, "cannot inspect package transaction path");
            return false;
        }
        return pkg_fsync_dir(parent, dc);
    }
    if (!S_ISDIR(st.st_mode)) {
        pkg_diag(dc,
                 "refusing to remove a non-directory package transaction path");
        return false;
    }
    return yew_rmtree(path, parent, dc) && pkg_fsync_dir(parent, dc);
}

static bool pkg_txn_begin(char op, const char *name, bool enable,
                          const char *aux, DiagCtx *dc)
{
    PkgTxnPaths paths;
    Bytebuf trust;
    Bytebuf lock;
    bool trust_missing = false;
    bool lock_missing = false;
    char intent[384];
    int n;
    bool ok = false;

    bytebuf_init(&trust);
    bytebuf_init(&lock);
    if (!pkg_txn_paths(&paths, true) ||
        (!pkg_read_file(paths.trust, &trust, &trust_missing) &&
         !trust_missing) ||
        (!pkg_read_file(paths.lock, &lock, &lock_missing) && !lock_missing))
        goto done;
    if (!pkg_txn_write_exact(paths.trust_before, trust.data, trust.len) ||
        !pkg_txn_write_exact(paths.lock_before, lock.data, lock.len))
        goto done;
    n = snprintf(intent, sizeof(intent), "2 %c %u %u %u %s %s\n", op,
                 enable ? 1U : 0U, trust_missing ? 1U : 0U,
                 lock_missing ? 1U : 0U, name,
                 aux != NULL ? aux : "-");
    if (n < 0 || (size_t)n >= sizeof(intent) ||
        !pkg_txn_write_exact(paths.intent, (const u8 *)intent, (size_t)n))
        goto done;
    ok = true;
done:
    if (!ok)
        pkg_diag_errno(dc, "cannot create durable package transaction");
    bytebuf_free(&lock);
    bytebuf_free(&trust);
    pkg_txn_paths_free(&paths);
    return ok;
}

static bool pkg_txn_clear(DiagCtx *dc)
{
    PkgTxnPaths paths;
    bool ok = false;

    if (!pkg_txn_paths(&paths, false))
        return false;
    if ((unlink(paths.intent) != 0 && errno != ENOENT) ||
        !pkg_fsync_dir(paths.state, dc))
        goto done;
    (void)unlink(paths.trust_before);
    (void)unlink(paths.lock_before);
    ok = true;
done:
    pkg_txn_paths_free(&paths);
    return ok;
}

static bool pkg_txn_restore_one(const char *path, const char *backup,
                                bool missing, DiagCtx *dc)
{
    char *parent = pkg_strdup(path);
    char *slash = strrchr(parent, '/');
    Bytebuf bytes;
    bool backup_missing = false;
    bool ok = false;

    if (slash == NULL)
        goto done_parent;
    *slash = '\0';
    if (missing) {
        if (unlink(path) != 0 && errno != ENOENT)
            goto done_parent;
        ok = pkg_fsync_dir(parent, dc);
        goto done_parent;
    }
    bytebuf_init(&bytes);
    if (pkg_read_file(backup, &bytes, &backup_missing) && !backup_missing)
        ok = pkg_txn_write_exact(path, bytes.data, bytes.len);
    bytebuf_free(&bytes);
done_parent:
    yew_xfree(parent);
    return ok;
}

static bool pkg_txn_recover(void)
{
    PkgTxnPaths paths;
    Bytebuf intent;
    bool missing = false;
    unsigned version;
    char op;
    unsigned enable;
    unsigned trust_missing;
    unsigned lock_missing;
    char name[33];
    char auxname[128];
    PkgLock lock;
    Arena a;
    DiagCtx dc;
    PkgEntry *entry;
    char *root = NULL;
    char *dir = NULL;
    char *data = NULL;
    char *trash = NULL;
    char *staging = NULL;
    bool dir_exists;
    bool ok = false;

    bytebuf_init(&intent);
    if (!pkg_txn_paths(&paths, false))
        return false;
    if (!pkg_read_file(paths.intent, &intent, &missing) && !missing)
        goto done_paths;
    if (missing) {
        ok = true;
        goto done_paths;
    }
    bytebuf_append(&intent, "\0", 1U);
    if (sscanf((const char *)intent.data, "%u %c %u %u %u %32s %127s",
               &version, &op, &enable, &trust_missing, &lock_missing,
               name, auxname) != 7 || version != 2U ||
        (op != 'I' && op != 'R') || enable > 1U || trust_missing > 1U ||
        lock_missing > 1U || !pkg_name_valid(name, strlen(name)) ||
        !pkg_txn_aux_valid(op, name, auxname)) {
        (void)fprintf(stderr,
                      "yew pkg: pending package transaction is corrupt; refusing to continue\n");
        goto done_paths;
    }
    (void)memset(&lock, 0, sizeof(lock));
    arena_init(&a);
    fl_diag_init(&dc, &a);
    fl_diag_set_sink(&dc, pkg_diag_stderr, NULL);
    if (!yew_pkg_lock_load(&lock, &dc))
        goto done_lock;
    entry = yew_pkg_lock_find(&lock, name, (u32)strlen(name));
    root = pkg_plugins_root(false);
    dir = root != NULL ? pkg_join(root, name) : NULL;
    dir_exists = dir != NULL && access(dir, F_OK) == 0;
    if (op == 'I' && root != NULL)
        staging = pkg_join(root, auxname);
    if (op == 'R' && root != NULL) {
        char *slash;

        data = pkg_strdup(root);
        slash = strrchr(data, '/');
        if (slash == NULL)
            goto done_lock;
        *slash = '\0';
        trash = pkg_join(data, auxname);
    }
    if (op == 'I' && entry != NULL && dir_exists) {
        ok = pkg_fsync_parent(paths.lock, &dc);
        if (!ok)
            goto recovery_decided;
        if (enable != 0U) {
            YewTrustDb trust;
            YewTrustWriteResult write;

            yew_trust_db_init(&trust);
            ok = yew_trust_db_load(&trust) &&
                 yew_trust_plugin_set_desired(
                     &trust, name, YEW_PLUGIN_DESIRED_ENABLED);
            if (ok) {
                write = pkg_trust_write_durable(&trust,
                                                "install recovery");
                ok = write.ok;
            }
            yew_trust_db_free(&trust);
        } else {
            ok = true;
        }
    } else if (op == 'R' && entry == NULL && !dir_exists) {
        YewTrustDb trust;
        YewTrustWriteResult write;

        ok = pkg_fsync_parent(paths.lock, &dc);
        if (ok)
            ok = trash == NULL || pkg_remove_path_sync(trash, data, &dc);
        if (!ok || enable != 0U)
            goto recovery_decided;
        yew_trust_db_init(&trust);
        ok = yew_trust_db_load(&trust) &&
             yew_trust_plugin_drop_policy(&trust, name);
        if (ok) {
            write = pkg_trust_write_durable(&trust, "remove recovery");
            ok = write.ok;
        }
        yew_trust_db_free(&trust);
    } else {
        if (op == 'I' && dir_exists) {
            ok = yew_rmtree(dir, root, &dc) && pkg_fsync_dir(root, &dc);
        } else if (op == 'R' && !dir_exists && trash != NULL) {
            ok = access(trash, F_OK) == 0 && rename(trash, dir) == 0 &&
                 pkg_fsync_dir(root, &dc) && pkg_fsync_dir(data, &dc);
        } else {
            ok = true;
        }
        if (ok)
            ok = pkg_txn_restore_one(paths.lock, paths.lock_before,
                                     lock_missing != 0U, &dc) &&
                 pkg_txn_restore_one(paths.trust, paths.trust_before,
                                     trust_missing != 0U, &dc);
    }
recovery_decided:
    if (ok && staging != NULL)
        ok = pkg_remove_path_sync(staging, root, &dc);
    if (ok)
        ok = pkg_txn_clear(&dc);
    if (ok)
        (void)fprintf(stderr,
                      "yew pkg: recovered interrupted %s of %s\n",
                      op == 'I' ? "install" : "remove", name);
done_lock:
    yew_xfree(staging);
    yew_xfree(trash);
    yew_xfree(data);
    yew_xfree(dir);
    yew_xfree(root);
    yew_pkg_lock_free(&lock);
    arena_free_all(&a);
done_paths:
    bytebuf_free(&intent);
    pkg_txn_paths_free(&paths);
    return ok;
}

static bool pkg_timeout_arg(const char *text, i64 *timeout_ms)
{
    char *end;
    long seconds;

    if (text == NULL || text[0] == '\0' || text[0] == '-')
        return false;
    errno = 0;
    seconds = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || seconds < 1L ||
        seconds > (long)(INT_MAX / 1000))
        return false;
    *timeout_ms = (i64)seconds * 1000;
    return true;
}

/* Integration determinism needs a stable timestamp while exercising the
 * production serializer through the real CLI.  This test-only seam changes
 * lockfile metadata, never package selection or trust decisions. */
static i64 pkg_now(void)
{
    const char *fixed = getenv("YEW_TEST_PKG_NOW");
    char *end;
    long long value;

    if (fixed == NULL || fixed[0] == '\0')
        return (i64)time(NULL);
    errno = 0;
    value = strtoll(fixed, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0)
        return (i64)time(NULL);
    return (i64)value;
}

static int pkg_install(int argc, char **argv)
{
    PkgLock lock;
    Arena diag_arena;
    DiagCtx dc;
    Bytebuf resolved;
    const char *spec;
    const char *pin = "head";
    bool enable = false;
    bool force_relock = false;
    i64 net_timeout = YEW_PKG_NET_TIMEOUT_MS;
    char pinbuf[300];
    char expr[600];
    char rev[41];
    char tree[17];
    char tmpname[96];
    char *root = NULL;
    char *tmp = NULL;
    char *repo = NULL;
    char *named = NULL;
    char *dest = NULL;
    char *name = NULL;
    char *url = NULL;
    PlugManifest mf;
    Arena manifest_arena;
    GitRun run;
    PkgEntry entry;
    YewTrustDb trust_before;
    YewTrustDb trust_next;
    bool trust_initialized = false;
    bool trust_prepared = false;
    bool txn_started = false;
    bool package_committed = false;
    YewTrustWriteResult trust_write = {false, false};
    int i;
    u32 pin_options = 0U;
    int rc = 3;

    if (argc < 2) {
        (void)fprintf(stderr, "usage: yew pkg install <spec> [--rev R | "
                              "--tag T | --branch B] [--enable]\n");
        return 1;
    }
    spec = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--enable") == 0) {
            enable = true;
        } else if (strcmp(argv[i], "--force-relock") == 0) {
            force_relock = true;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            if (!pkg_timeout_arg(argv[++i], &net_timeout)) {
                (void)fprintf(stderr,
                              "yew pkg: --timeout requires positive seconds\n");
                return 1;
            }
        } else if ((strcmp(argv[i], "--rev") == 0 ||
                    strcmp(argv[i], "--tag") == 0 ||
                    strcmp(argv[i], "--branch") == 0) && i + 1 < argc) {
            const char *kind = argv[i] + 2U;
            const char *ref = argv[++i];

            if (++pin_options > 1U) {
                (void)fprintf(stderr,
                              "yew pkg: choose exactly one of --rev, --tag, or --branch\n");
                return 1;
            }
            if (!yew_pkg_ref_valid(ref)) {
                pkg_diag_arg("yew pkg: invalid ref ", ref, "\n");
                return 1;
            }
            (void)snprintf(pinbuf, sizeof(pinbuf), "%s:%s", kind, ref);
            pin = pinbuf;
        } else if (strcmp(argv[i], "--recurse-submodules") == 0) {
            (void)fprintf(stderr,
                          "yew pkg: submodules are not recursed: they multiply "
                          "the trust surface across unnamed repositories\n");
            return 1;
        } else {
            pkg_diag_arg("yew pkg: unknown install option: ", argv[i], "\n");
            return 1;
        }
    }
    (void)memset(&lock, 0, sizeof(lock));
    arena_init(&diag_arena);
    fl_diag_init(&dc, &diag_arena);
    fl_diag_set_sink(&dc, pkg_diag_stderr, NULL);
    bytebuf_init(&resolved);
    if (!yew_pkg_resolve_spec(spec, &resolved, &dc)) {
        pkg_diag_arg("yew pkg: invalid source spec ", spec,
                     "; bare user/repo is ambiguous\naccepted forms: "
                     "gh:user/repo, gl:user/repo, cb:user/repo, "
                     "sr:~user/repo, URL, git@host:path, /abs, ./rel, "
                     "file://...\n");
        rc = 1;
        goto done;
    }
    url = pkg_buf_string(&resolved);
    if (!pkg_git_probe())
        goto done;
    if (!yew_pkg_lock_load(&lock, &dc)) {
        if (force_relock && lock.corrupt) {
            yew_pkg_lock_free(&lock);
            yew_pkg_lock_init(&lock);
            (void)fprintf(stderr,
                          "yew pkg: warning: --force-relock discards the "
                          "corrupt lockfile's recorded revisions and tree hashes\n");
        } else {
        (void)fprintf(stderr,
                      "yew pkg: plugins.lock is corrupt or unreadable; "
                      "refusing to overwrite the lock (use --force-relock "
                      "to discard it)\n");
        rc = 1;
        goto done;
        }
    }
    root = pkg_plugins_root(true);
    if (root == NULL)
        goto done;
    (void)snprintf(tmpname, sizeof(tmpname), ".pkg-tmp-%ld-%lld",
                   (long)getpid(), (long long)yew_now_ms());
    tmp = pkg_join(root, tmpname);
    if (mkdir(tmp, 0700) != 0)
        goto done;
    repo = pkg_join(tmp, "repo");
    {
        const char *const clone_argv[] = {
            "git", "-c", "advice.detachedHead=false", "clone", "--quiet",
            "--", url, repo
        };

        if (!pkg_run_ok(clone_argv, YEW_ARRAY_LEN(clone_argv),
                        net_timeout, false, &run, "clone")) {
            yew_pkg_git_run_free(&run);
            goto done;
        }
        yew_pkg_git_run_free(&run);
    }
    if (pkg_pin_expr(pin, expr, sizeof(expr), true) == NULL ||
        !pkg_resolve_rev(repo, expr, rev) || !pkg_checkout(repo, rev, false))
        goto done;
    pkg_warn_gitmodules(repo);
    name = pkg_manifest_name(&lock, repo, &dc);
    if (name == NULL || !pkg_name_valid(name, strlen(name))) {
        rc = 1;
        goto done;
    }
    dest = pkg_join(root, name);
    if (access(dest, F_OK) == 0) {
        (void)fprintf(stderr,
                      "yew pkg: %s already exists; use update or remove\n",
                      name);
        rc = 1;
        goto done;
    }
    named = pkg_join(tmp, name);
    if (rename(repo, named) != 0)
        goto done;
    yew_xfree(repo);
    repo = NULL;
    arena_init(&manifest_arena);
    (void)memset(&mf, 0, sizeof(mf));
    if (!yew_plug_manifest_read(&manifest_arena, named, &mf, &dc)) {
        arena_free_all(&manifest_arena);
        rc = 1;
        goto done;
    }
    if (!yew_pkg_tree_hash(named, tree, &dc)) {
        arena_free_all(&manifest_arena);
        goto done;
    }
    if (!pkg_txn_begin('I', name, enable, tmpname, &dc)) {
        arena_free_all(&manifest_arena);
        goto done;
    }
    txn_started = true;
    yew_trust_db_init(&trust_before);
    yew_trust_db_init(&trust_next);
    trust_initialized = true;
    if (!yew_trust_db_load(&trust_before) || !yew_trust_db_load(&trust_next) ||
        !yew_trust_plugin_set_desired(&trust_next, name,
                                      YEW_PLUGIN_DESIRED_DISABLED)) {
        arena_free_all(&manifest_arena);
        goto done;
    }
    trust_write = pkg_trust_write_durable(&trust_next, "install");
    if (!trust_write.ok) {
        if (trust_write.committed) {
            YewTrustWriteResult restore =
                yew_trust_db_write_result(&trust_before, time(NULL),
                                          YEW_TRUST_PRUNE_DAYS_DEFAULT);

            if (!restore.ok)
                (void)fprintf(stderr,
                              "yew pkg: error: could not durably restore the pre-install trust policy\n");
        }
        arena_free_all(&manifest_arena);
        goto done;
    }
    trust_prepared = true;
    if (rename(named, dest) != 0) {
        goto done_manifest;
    }
    yew_xfree(named);
    named = NULL;
    if (!pkg_remove_path_sync(tmp, root, &dc)) {
        bool restored = yew_rmtree(dest, root, &dc);

        restored = pkg_fsync_dir(root, &dc) && restored;
        if (!restored)
            (void)fprintf(stderr,
                          "yew pkg: error: install rollback could not remove and synchronize the published plugin directory\n");
        goto done_manifest;
    }
    (void)memset(&entry, 0, sizeof(entry));
    entry.name = arena_strdup(&lock.a, name);
    entry.url = arena_strdup(&lock.a, url);
    entry.shorthand = arena_strdup(&lock.a, spec);
    entry.pin = arena_strdup(&lock.a, pin);
    (void)memcpy(entry.rev, rev, sizeof(entry.rev));
    (void)memcpy(entry.tree, tree, sizeof(entry.tree));
    entry.installed_at = pkg_now();
    entry.updated_at = entry.installed_at;
    entry.extra = FL_OBJ_V(FL_MAP, fl_map_new(&lock.vm));
    pkg_entry_push(&lock, entry);
    if (!yew_pkg_lock_save(&lock, &dc)) {
        pkg_lock_remove_at(&lock, lock.v.len - 1U);
        if (!yew_rmtree(dest, root, &dc) || !pkg_fsync_dir(root, &dc))
            (void)fprintf(stderr,
                          "yew pkg: error: install rollback could not be durably restored\n");
        goto done_manifest;
    }
    if (enable) {
        YewTrustWriteResult enabled_write;

        if (!yew_trust_plugin_set_desired(&trust_next, name,
                                          YEW_PLUGIN_DESIRED_ENABLED))
            goto install_publish_rollback;
        enabled_write = pkg_trust_write_durable(&trust_next, "install enable");
        if (!enabled_write.ok)
            goto install_publish_rollback;
    }
    package_committed = true;
    if (!pkg_txn_clear(&dc))
        goto done_manifest;
    txn_started = false;
    (void)printf("installed %s %.12s %s capabilities:", name, rev, pin);
    for (i = 0; i < (int)YEW_CAP__N; i++) {
        if ((mf.caps_wanted & (1U << (u32)i)) != 0U)
            (void)printf(" %s", yew_cap_name((YewCap)i));
    }
    (void)printf("\n");
    if (!enable)
        (void)printf("run: yew plug enable %s\n", name);
    rc = 0;
    goto done_manifest;
install_publish_rollback:
    pkg_lock_remove_at(&lock, lock.v.len - 1U);
    if (!yew_pkg_lock_save(&lock, &dc))
        (void)fprintf(stderr,
                      "yew pkg: error: install rollback could not restore plugins.lock\n");
    if (!yew_rmtree(dest, root, &dc) || !pkg_fsync_dir(root, &dc))
        (void)fprintf(stderr,
                      "yew pkg: error: install rollback could not remove and synchronize the published plugin directory\n");
done_manifest:
    if (trust_prepared && rc != 0 && !package_committed) {
        YewTrustWriteResult restored =
            yew_trust_db_write_result(&trust_before, time(NULL),
                                      YEW_TRUST_PRUNE_DAYS_DEFAULT);

        if (!restored.ok)
            (void)fprintf(stderr,
                          "yew pkg: error: install rollback could not durably restore the previous trust policy\n");
    }
    arena_free_all(&manifest_arena);
done:
    if (tmp != NULL && access(tmp, F_OK) == 0)
        (void)pkg_remove_path_sync(tmp, root, &dc);
    yew_xfree(name);
    yew_xfree(dest);
    yew_xfree(named);
    yew_xfree(repo);
    yew_xfree(tmp);
    yew_xfree(root);
    yew_xfree(url);
    bytebuf_free(&resolved);
    if (trust_initialized) {
        yew_trust_db_free(&trust_next);
        yew_trust_db_free(&trust_before);
    }
    yew_pkg_lock_free(&lock);
    arena_free_all(&diag_arena);
    if (txn_started && !pkg_txn_recover())
        rc = 3;
    return rc;
}

static bool pkg_reinstall_locked(const PkgEntry *entry, const char *root,
                                 DiagCtx *dc)
{
    char tmpname[96];
    char *tmp;
    char *repo;
    char *dest;
    char tree[17];
    GitRun run;
    bool ok = false;
    bool installed = false;
    const char *clone_argv[9];

    (void)snprintf(tmpname, sizeof(tmpname), ".pkg-fix-%ld-%lld",
                   (long)getpid(), (long long)yew_now_ms());
    tmp = pkg_join(root, tmpname);
    repo = pkg_join(tmp, "repo");
    dest = pkg_join(root, entry->name);
    if (mkdir(tmp, 0700) != 0)
        goto done;
    clone_argv[0] = "git"; clone_argv[1] = "-c";
    clone_argv[2] = "advice.detachedHead=false"; clone_argv[3] = "clone";
    clone_argv[4] = "--quiet"; clone_argv[5] = "--";
    clone_argv[6] = entry->url; clone_argv[7] = repo;
    if (!pkg_run_ok(clone_argv, 8U, YEW_PKG_NET_TIMEOUT_MS, false, &run,
                    "clone")) {
        yew_pkg_git_run_free(&run);
        goto done;
    }
    yew_pkg_git_run_free(&run);
    if (!pkg_checkout(repo, entry->rev, false) ||
        !yew_pkg_tree_hash(repo, tree, dc) || strcmp(tree, entry->tree) != 0) {
        pkg_diag(dc, "locked revision does not reproduce the locked tree");
        goto done;
    }
    pkg_warn_gitmodules(repo);
    if (rename(repo, dest) != 0)
        goto done;
    installed = true;
    if (!pkg_fsync_dir(root, dc))
        goto done;
    ok = true;
done:
    if (!ok && installed) {
        bool restored = yew_rmtree(dest, root, dc);

        restored = pkg_fsync_dir(root, dc) && restored;
        if (!restored)
            (void)fprintf(stderr,
                          "yew pkg: error: doctor --fix rollback could not remove and synchronize the published plugin directory\n");
    }
    if (access(tmp, F_OK) == 0)
        (void)yew_rmtree(tmp, root, dc);
    yew_xfree(dest);
    yew_xfree(repo);
    yew_xfree(tmp);
    return ok;
}

static void pkg_doctor_paths(const PkgEntry *entry, const char *dir)
{
    const char *const diff_argv[] = {
        "git", "-C", dir, "diff", "--name-status", "--no-renames",
        "--end-of-options", entry->rev
    };
    const char *const other_argv[] = {
        "git", "-C", dir, "ls-files", "--others"
    };
    GitRun run;
    char *text;
    char *line;
    char *save;
    u32 shown = 0U;
    bool truncated = false;

    if (yew_pkg_git(diff_argv, YEW_ARRAY_LEN(diff_argv), 10000, true, &run) &&
        run.status == 0) {
        text = pkg_buf_string(&run.out);
        save = NULL;
        for (line = strtok_r(text, "\n", &save); line != NULL;
             line = strtok_r(NULL, "\n", &save)) {
            const char *kind = line[0] == 'D' ? "removed" : "changed";
            const char *path = strchr(line, '\t');
            if (path != NULL && shown < 100U) {
                (void)printf("%s\tpath\t%s\t", entry->name, kind);
                pkg_print_sanitized(stdout, path + 1U, strlen(path + 1U));
                (void)fputc('\n', stdout);
                shown++;
            } else if (path != NULL)
                truncated = true;
        }
        yew_xfree(text);
    }
    yew_pkg_git_run_free(&run);
    if (yew_pkg_git(other_argv, YEW_ARRAY_LEN(other_argv), 10000, true,
                    &run) && run.status == 0) {
        text = pkg_buf_string(&run.out);
        save = NULL;
        for (line = strtok_r(text, "\n", &save); line != NULL;
             line = strtok_r(NULL, "\n", &save)) {
            if (shown < 100U) {
                (void)printf("%s\tpath\tadded\t", entry->name);
                pkg_print_sanitized(stdout, line, strlen(line));
                (void)fputc('\n', stdout);
                shown++;
            } else
                truncated = true;
        }
        yew_xfree(text);
    }
    yew_pkg_git_run_free(&run);
    if (truncated)
        (void)printf("%s\tpath\t...\treport capped at 100 paths\n",
                     entry->name);
}

static bool pkg_name_arg(int argc, char **argv, int at, bool has_timeout)
{
    bool after_dashdash = false;
    int i;

    for (i = 1; i <= at && i < argc; i++) {
        if (!after_dashdash && strcmp(argv[i], "--") == 0) {
            after_dashdash = true;
            if (i == at)
                return false;
            continue;
        }
        if (i == at) {
            if (after_dashdash)
                return true;
            if (has_timeout && i > 1 && strcmp(argv[i - 1], "--timeout") == 0)
                return false;
            return argv[i][0] != '-';
        }
    }
    return false;
}

static int pkg_list_or_doctor(int argc, char **argv, bool doctor)
{
    PkgLock lock;
    Arena a;
    DiagCtx dc;
    char *root;
    bool accept = false;
    bool fix = false;
    bool long_form = false;
    bool all_ok = true;
    bool changed = false;
    bool has_names = false;
    YewTrustDb trust;
    bool have_trust;
    size_t i;
    int ai;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--") == 0)
            continue;
        if (pkg_name_arg(argc, argv, ai, false)) {
            has_names = true;
            continue;
        }
        if (doctor && strcmp(argv[ai], "--accept") == 0)
            accept = true;
        else if (doctor && strcmp(argv[ai], "--fix") == 0)
            fix = true;
        else if (!doctor && strcmp(argv[ai], "--long") == 0)
            long_form = true;
        else if (argv[ai][0] == '-') {
            pkg_diag_arg("yew pkg: unknown option: ", argv[ai], "\n");
            return 1;
        }
    }
    if (accept && fix) {
        (void)fprintf(stderr, "yew pkg doctor: choose --fix or --accept\n");
        return 1;
    }
    (void)memset(&lock, 0, sizeof(lock));
    arena_init(&a);
    fl_diag_init(&dc, &a);
    fl_diag_set_sink(&dc, pkg_diag_stderr, NULL);
    if (!yew_pkg_lock_load(&lock, &dc)) {
        arena_free_all(&a);
        return 1;
    }
    for (ai = 1; ai < argc; ai++) {
        if (pkg_name_arg(argc, argv, ai, false) &&
            yew_pkg_lock_find(&lock, argv[ai], (u32)strlen(argv[ai])) == NULL) {
            pkg_diag_arg("yew pkg: unknown package name: ", argv[ai], "\n");
            yew_pkg_lock_free(&lock);
            arena_free_all(&a);
            return 1;
        }
    }
    root = pkg_plugins_root(false);
    yew_trust_db_init(&trust);
    have_trust = yew_trust_db_load(&trust);
    for (i = 0U; i < lock.v.len; i++) {
        PkgEntry *entry = &lock.v.data[i];
        char *dir;
        char actual[17];
        const char *state;
        bool selected = !has_names;
        Arena manifest_arena;
        PlugManifest mf;

        for (ai = 1; ai < argc; ai++) {
            if (pkg_name_arg(argc, argv, ai, false) &&
                strcmp(argv[ai], entry->name) == 0)
                selected = true;
        }
        if (!selected)
            continue;
        arena_init(&manifest_arena);
        (void)memset(&mf, 0, sizeof(mf));
        dir = root == NULL ? NULL : pkg_join(root, entry->name);
        if (dir == NULL || access(dir, F_OK) != 0) {
            state = "missing";
            if (doctor && fix && root != NULL && pkg_git_probe() &&
                pkg_reinstall_locked(entry, root, &dc))
                state = "ok";
            else if (doctor)
                all_ok = false;
        } else if (doctor &&
                   !yew_plug_manifest_read(&manifest_arena, dir, &mf, &dc)) {
            state = "error";
            all_ok = false;
        } else if (doctor && strcmp(mf.name_text, entry->name) != 0) {
            (void)fprintf(stderr,
                          "yew pkg: manifest name %s does not match directory %s\n",
                          mf.name_text, entry->name);
            state = "error";
            all_ok = false;
        } else if (!yew_pkg_tree_hash(dir, actual, &dc)) {
            state = "error";
            if (doctor)
                all_ok = false;
        } else if (strcmp(actual, entry->tree) != 0) {
            state = "drift";
            if (doctor)
                pkg_doctor_paths(entry, dir);
            if (accept) {
                char head[41];

                if (!pkg_resolve_rev(dir, "HEAD^{commit}", head)) {
                    state = "error";
                    all_ok = false;
                } else {
                    (void)memcpy(entry->tree, actual, sizeof(entry->tree));
                    (void)memcpy(entry->rev, head, sizeof(entry->rev));
                    entry->updated_at = pkg_now();
                    state = "ok";
                    changed = true;
                }
            } else if (fix) {
                if (!pkg_git_probe() || !pkg_checkout(dir, entry->rev, true) ||
                    !yew_pkg_tree_hash(dir, actual, &dc) ||
                    strcmp(actual, entry->tree) != 0) {
                    state = "error";
                    all_ok = false;
                } else {
                    state = "ok";
                }
            } else if (doctor)
                all_ok = false;
        } else {
            state = "ok";
            if (doctor) {
                char *gitdir = pkg_join(dir, ".git");
                char head[41];

                if (access(gitdir, F_OK) != 0) {
                    state = "untracked-tree";
                    all_ok = false;
                } else if (!pkg_resolve_rev(dir, "HEAD^{commit}", head)) {
                    state = "untracked-tree";
                    all_ok = false;
                } else if (strcmp(head, entry->rev) != 0) {
                    state = "rev-mismatch";
                    if (accept) {
                        (void)memcpy(entry->rev, head, sizeof(entry->rev));
                        entry->updated_at = pkg_now();
                        state = "ok";
                        changed = true;
                    } else if (fix && pkg_checkout(dir, entry->rev, true)) {
                        state = "ok";
                    } else {
                        all_ok = false;
                    }
                }
                yew_xfree(gitdir);
            }
        }
        (void)printf("%s\t%s\t%.12s\t%s\t%s%s%s\n", entry->name,
                     entry->pin, entry->rev, state, entry->url,
                     long_form ? "\t" : "", long_form ? entry->tree : "");
        if (doctor && mf.name_text != NULL) {
            int cap;
            static const char *const grants[] = {"unset", "allow", "deny"};

            for (cap = 0; cap < (int)YEW_CAP__N; cap++) {
                YewPluginGrant grant = have_trust ?
                    yew_trust_plugin_capability(
                        &trust, entry->name, (YewPluginCapability)cap) :
                    YEW_PLUGIN_GRANT_UNSET;
                (void)printf("%s\tcapability\t%s\t%s\t%s\n", entry->name,
                             yew_cap_name((YewCap)cap),
                             (mf.caps_wanted & (1U << (u32)cap)) != 0U ?
                                 "declared" : "undeclared",
                             grants[(u32)grant]);
            }
        }
        yew_xfree(dir);
        arena_free_all(&manifest_arena);
    }
    if (root != NULL) {
        DIR *scan = opendir(root);
        char **names = NULL;
        size_t nnames = 0U;
        size_t cap = 0U;
        struct dirent *de;

        while (scan != NULL && (de = readdir(scan)) != NULL) {
            struct stat st;
            bool selected = !has_names;

            if (de->d_name[0] == '.' ||
                yew_pkg_lock_find(&lock, de->d_name,
                                  (u32)strlen(de->d_name)) != NULL ||
                fstatat(dirfd(scan), de->d_name, &st,
                        AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
                continue;
            for (ai = 1; ai < argc; ai++)
                if (pkg_name_arg(argc, argv, ai, false) &&
                    strcmp(argv[ai], de->d_name) == 0)
                    selected = true;
            if (!selected)
                continue;
            if (nnames == cap) {
                cap = cap == 0U ? 8U : cap * 2U;
                names = yew_xreallocarray(names, cap, sizeof(*names));
            }
            names[nnames++] = pkg_strdup(de->d_name);
        }
        if (scan != NULL)
            (void)closedir(scan);
        if (nnames > 1U) {
            yew_sort_stable(names, nnames, sizeof(*names),
                            pkg_string_ptr_cmp, NULL);
        }
        for (i = 0U; i < nnames; i++) {
            pkg_print_field(stdout, names[i]);
            (void)fputs("\t-\t-\tunmanaged\t-\n", stdout);
            yew_xfree(names[i]);
        }
        yew_xfree(names);
    }
    if (changed && !yew_pkg_lock_save(&lock, &dc))
        all_ok = false;
    yew_xfree(root);
    yew_trust_db_free(&trust);
    yew_pkg_lock_free(&lock);
    arena_free_all(&a);
    return all_ok ? 0 : 1;
}

static int pkg_remove(int argc, char **argv)
{
    PkgLock lock;
    Arena a;
    DiagCtx dc;
    char *root = NULL;
    char *dir = NULL;
    char *data = NULL;
    char *trash = NULL;
    char trashname[128];
    bool keep = false;
    bool quarantined = false;
    size_t at;
    PkgEntry *entry = NULL;
    PkgEntry removed;
    YewTrustDb trust_before;
    YewTrustDb trust_next;
    bool trust_initialized = false;
    bool trust_prepared = false;
    bool removal_committed = false;
    bool txn_started = false;

    if (argc < 2 || argc > 3 ||
        (argc == 3 && strcmp(argv[2], "--keep-grants") != 0)) {
        (void)fprintf(stderr,
                      "usage: yew pkg remove <name> [--keep-grants]\n");
        return 1;
    }
    keep = argc == 3;
    (void)memset(&lock, 0, sizeof(lock));
    arena_init(&a);
    fl_diag_init(&dc, &a);
    fl_diag_set_sink(&dc, pkg_diag_stderr, NULL);
    if (!yew_pkg_lock_load(&lock, &dc)) {
        arena_free_all(&a);
        return 1;
    }
    for (at = 0U; at < lock.v.len; at++) {
        if (strcmp(lock.v.data[at].name, argv[1]) == 0) {
            entry = &lock.v.data[at];
            break;
        }
    }
    if (entry == NULL) {
        pkg_diag_arg("yew pkg: ", argv[1],
                     " is not managed by yew pkg\n");
        yew_pkg_lock_free(&lock);
        arena_free_all(&a);
        return 1;
    }
    (void)snprintf(trashname, sizeof(trashname),
                   ".pkg-trash-%s-%ld-%lld", entry->name,
                   (long)getpid(), (long long)yew_now_ms());
    if (!pkg_txn_begin('R', entry->name, keep, trashname, &dc))
        goto remove_io_fail;
    txn_started = true;
    if (!keep) {
        YewTrustWriteResult write;
        int cap;

        yew_trust_db_init(&trust_before);
        yew_trust_db_init(&trust_next);
        trust_initialized = true;
        if (!yew_trust_db_load(&trust_before) ||
            !yew_trust_db_load(&trust_next) ||
            !yew_trust_plugin_set_desired(
                &trust_next, entry->name, YEW_PLUGIN_DESIRED_DISABLED))
            goto remove_io_fail;
        for (cap = 0; cap < (int)YEW_CAP__N; cap++)
            if (!yew_trust_plugin_set_capability(
                    &trust_next, entry->name, (YewPluginCapability)cap,
                    YEW_PLUGIN_GRANT_UNSET))
                goto remove_io_fail;
        write = pkg_trust_write_durable(&trust_next, "remove");
        if (!write.ok) {
            if (write.committed) {
                YewTrustWriteResult restored =
                    yew_trust_db_write_result(&trust_before, time(NULL),
                                              YEW_TRUST_PRUNE_DAYS_DEFAULT);

                if (!restored.ok)
                    (void)fprintf(stderr,
                                  "yew pkg: error: remove rollback could not durably restore the previous trust policy\n");
            }
            goto remove_io_fail;
        }
        trust_prepared = true;
    }
    root = pkg_plugins_root(false);
    dir = root == NULL ? NULL : pkg_join(root, entry->name);
    if (dir != NULL && access(dir, F_OK) == 0) {
        char *slash;

        data = pkg_strdup(root);
        slash = strrchr(data, '/');
        if (slash == NULL) {
            yew_xfree(data);
            data = NULL;
            goto remove_io_fail;
        }
        *slash = '\0';
        trash = pkg_join(data, trashname);
        if (rename(dir, trash) != 0)
            goto remove_io_fail;
        quarantined = true;
        if (!pkg_fsync_dir(root, &dc) || !pkg_fsync_dir(data, &dc)) {
            bool renamed = rename(trash, dir) == 0;
            bool synced = renamed && pkg_fsync_dir(root, &dc) &&
                          pkg_fsync_dir(data, &dc);

            if (renamed)
                quarantined = false;
            if (!renamed)
                (void)fprintf(stderr,
                              "yew pkg: error: rollback could not restore plugin; recover it from %s\n",
                              trash);
            else if (!synced)
                (void)fprintf(stderr,
                              "yew pkg: error: plugin was restored at %s but rollback fsync failed\n",
                              dir);
            goto remove_io_fail;
        }
    }
    removed = *entry;
    pkg_lock_remove_at(&lock, at);
    if (!yew_pkg_lock_save(&lock, &dc)) {
        bool lock_restored;
        bool dir_restored = true;
        bool parents_synced = true;

        pkg_entry_push(&lock, removed);
        lock_restored = yew_pkg_lock_save(&lock, &dc);
        if (quarantined) {
            dir_restored = rename(trash, dir) == 0;
            if (dir_restored) {
                quarantined = false;
                parents_synced = pkg_fsync_dir(root, &dc) &&
                                 pkg_fsync_dir(data, &dc);
            }
        }
        if (!lock_restored)
            (void)fprintf(stderr,
                          "yew pkg: error: rollback could not restore plugins.lock\n");
        if (!dir_restored)
            (void)fprintf(stderr,
                          "yew pkg: error: rollback could not restore plugin; recover it from %s\n",
                          trash);
        else if (!parents_synced)
            (void)fprintf(stderr,
                          "yew pkg: error: plugin was restored at %s but rollback fsync failed\n",
                          dir);
        goto remove_io_fail;
    }
    removal_committed = true;
    if (!keep) {
        YewTrustWriteResult write;

        if (!yew_trust_plugin_drop_policy(&trust_next, removed.name))
            goto remove_io_fail;
        write = pkg_trust_write_durable(&trust_next, "remove finalize");
        if (!write.ok)
            goto remove_io_fail;
    }
    if (quarantined) {
        if (!pkg_remove_path_sync(trash, data, &dc)) {
            (void)fprintf(stderr,
                          "yew pkg: removal committed; could not durably remove residual trash at %s\n",
                          trash);
            goto remove_io_fail;
        }
        quarantined = false;
    }
    if (!pkg_txn_clear(&dc))
        goto remove_io_fail;
    txn_started = false;
    (void)printf("removed %s\n", argv[1]);
    yew_xfree(trash);
    yew_xfree(data);
    yew_xfree(dir);
    yew_xfree(root);
    if (trust_initialized) {
        yew_trust_db_free(&trust_next);
        yew_trust_db_free(&trust_before);
    }
    yew_pkg_lock_free(&lock);
    arena_free_all(&a);
    return 0;
remove_io_fail:
    if (trust_prepared && !removal_committed) {
        YewTrustWriteResult restored =
            yew_trust_db_write_result(&trust_before, time(NULL),
                                      YEW_TRUST_PRUNE_DAYS_DEFAULT);

        if (!restored.ok)
            (void)fprintf(stderr,
                          "yew pkg: error: remove rollback could not durably restore the previous trust policy\n");
    }
    yew_xfree(trash);
    yew_xfree(data);
    yew_xfree(dir);
    yew_xfree(root);
    if (trust_initialized) {
        yew_trust_db_free(&trust_next);
        yew_trust_db_free(&trust_before);
    }
    yew_pkg_lock_free(&lock);
    arena_free_all(&a);
    if (txn_started && !pkg_txn_recover())
        return 3;
    return 3;
}

static bool pkg_git_simple(const char *const *argv, u32 nargv, i64 timeout,
                           bool locale, const char *op, GitRun *retain)
{
    GitRun local;
    GitRun *run = retain != NULL ? retain : &local;
    bool ok = pkg_run_ok(argv, nargv, timeout, locale, run, op);

    if (retain == NULL)
        yew_pkg_git_run_free(run);
    return ok;
}

static void pkg_print_not_ff(const PkgEntry *entry, const char *dir,
                             const char *target)
{
    (void)fprintf(stderr,
                  "yew pkg: refusing to update \"%s\": %.12s is not an "
                  "ancestor\n  of %.12s -- the branch was force-pushed or "
                  "history was rewritten.\n  Inspect it (git -C %s log "
                  "--oneline --all), then either\n  `yew pkg install --rev "
                  "<sha>` deliberately, or pin a rev you trust.\n",
                  entry->name, entry->rev, target, dir);
}

static int pkg_update(int argc, char **argv)
{
    PkgLock lock;
    Arena a;
    DiagCtx dc;
    char *root;
    bool dry = false;
    bool discard = false;
    bool failed = false;
    bool refused = false;
    bool has_names = false;
    i64 net_timeout = YEW_PKG_NET_TIMEOUT_MS;
    u32 nupdated = 0U;
    u32 nuptodate = 0U;
    u32 nunreachable = 0U;
    size_t i;
    int ai;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--") == 0)
            continue;
        if (pkg_name_arg(argc, argv, ai, true)) {
            has_names = true;
            continue;
        }
        if (strcmp(argv[ai], "--dry-run") == 0)
            dry = true;
        else if (strcmp(argv[ai], "--discard-local") == 0)
            discard = true;
        else if (strcmp(argv[ai], "--timeout") == 0 && ai + 1 < argc) {
            if (!pkg_timeout_arg(argv[++ai], &net_timeout)) {
                (void)fprintf(stderr,
                              "yew pkg: --timeout requires positive seconds\n");
                return 1;
            }
        }
        else if (argv[ai][0] == '-') {
            pkg_diag_arg("yew pkg: unknown update option: ", argv[ai], "\n");
            return 1;
        }
    }
    if (!pkg_git_probe())
        return 3;
    (void)memset(&lock, 0, sizeof(lock));
    arena_init(&a);
    fl_diag_init(&dc, &a);
    fl_diag_set_sink(&dc, pkg_diag_stderr, NULL);
    if (!yew_pkg_lock_load(&lock, &dc)) {
        arena_free_all(&a);
        return 1;
    }
    for (ai = 1; ai < argc; ai++) {
        if (pkg_name_arg(argc, argv, ai, true) &&
            yew_pkg_lock_find(&lock, argv[ai], (u32)strlen(argv[ai])) == NULL) {
            pkg_diag_arg("yew pkg: unknown package name: ", argv[ai], "\n");
            yew_pkg_lock_free(&lock);
            arena_free_all(&a);
            return 1;
        }
    }
    root = pkg_plugins_root(false);
    for (i = 0U; i < lock.v.len; i++) {
        PkgEntry *entry = &lock.v.data[i];
        bool selected = !has_names;
        char *dir;
        char expr[600];
        char target[41];
        char old_rev[41];
        char old_tree[17];
        i64 old_updated_at;
        GitRun run;

        for (ai = 1; ai < argc; ai++) {
            if (pkg_name_arg(argc, argv, ai, true) &&
                strcmp(argv[ai], entry->name) == 0)
                selected = true;
        }
        if (!selected)
            continue;
        dir = root == NULL ? NULL : pkg_join(root, entry->name);
        if (dir == NULL || access(dir, F_OK) != 0) {
            (void)fprintf(stderr, "yew pkg: %s is missing\n", entry->name);
            yew_xfree(dir);
            failed = true;
            nunreachable++;
            continue;
        }
        if (strncmp(entry->pin, "rev:", 4U) == 0) {
            (void)printf("%s pinned at %.12s\n", entry->name, entry->rev);
            nuptodate++;
            yew_xfree(dir);
            continue;
        }
        if (strncmp(entry->pin, "tag:", 4U) == 0) {
            char refspec[600];
            const char *const fetch_tag[] = {
                "git", "-C", dir, "fetch", "--quiet", "--prune",
                "--no-tags", "--", "origin", refspec
            };

            (void)snprintf(refspec, sizeof(refspec),
                           "+refs/tags/%s:refs/yew-pkg/tags/%s",
                           entry->pin + 4U, entry->pin + 4U);
            if (!pkg_git_simple(fetch_tag, YEW_ARRAY_LEN(fetch_tag),
                                net_timeout, false, "fetch", NULL)) {
                yew_xfree(dir);
                failed = true;
                nunreachable++;
                continue;
            }
        } else {
            const char *const fetch[] = {"git", "-C", dir, "fetch", "--quiet",
                                         "--prune", "--tags", "--",
                                         "origin"};
            if (!pkg_git_simple(fetch, YEW_ARRAY_LEN(fetch),
                                net_timeout, false, "fetch", NULL)) {
                yew_xfree(dir);
                failed = true;
                nunreachable++;
                continue;
            }
        }
        if (strcmp(entry->pin, "head") == 0) {
            const char *const set_head[] = {"git", "-C", dir, "remote",
                                            "set-head", "origin", "--auto"};
            if (!pkg_git_simple(set_head, YEW_ARRAY_LEN(set_head),
                                net_timeout, false,
                                "remote set-head", NULL)) {
                yew_xfree(dir);
                failed = true;
                nunreachable++;
                continue;
            }
        }
        if (strncmp(entry->pin, "tag:", 4U) == 0) {
            (void)snprintf(expr, sizeof(expr),
                           "refs/yew-pkg/tags/%s^{commit}", entry->pin + 4U);
        } else if (pkg_pin_expr(entry->pin, expr, sizeof(expr), true) == NULL) {
            yew_xfree(dir);
            failed = true;
            nunreachable++;
            continue;
        }
        if (!pkg_resolve_rev(dir, expr, target)) {
            yew_xfree(dir);
            failed = true;
            nunreachable++;
            continue;
        }
        if (strcmp(target, entry->rev) == 0) {
            (void)printf("%s up to date\n", entry->name);
            nuptodate++;
            yew_xfree(dir);
            continue;
        }
        if (strncmp(entry->pin, "tag:", 4U) == 0) {
            pkg_print_not_ff(entry, dir, target);
            yew_xfree(dir);
            refused = true;
            continue;
        }
        {
            const char *const ff[] = {"git", "-C", dir, "merge-base",
                                      "--is-ancestor", "--end-of-options",
                                      entry->rev, target};
            if (!yew_pkg_git(ff, YEW_ARRAY_LEN(ff), 10000, true, &run)) {
                yew_xfree(dir);
                failed = true;
                continue;
            }
            if (run.status != 0) {
                bool not_ff = run.status == 1;

                if (not_ff)
                    pkg_print_not_ff(entry, dir, target);
                else
                    (void)fprintf(stderr, "yew pkg: ancestry check failed\n");
                yew_pkg_git_run_free(&run);
                yew_xfree(dir);
                if (not_ff)
                    refused = true;
                else
                    failed = true;
                continue;
            }
            yew_pkg_git_run_free(&run);
        }
        {
            char range[84];
            const char *log_argv[11];
            const char *count_argv[9];

            (void)snprintf(range, sizeof(range), "%s..%s", entry->rev,
                           target);
            log_argv[0] = "git";
            log_argv[1] = "-C";
            log_argv[2] = dir;
            log_argv[3] = "log";
            log_argv[4] = "--no-merges";
            log_argv[5] = "--no-color";
            log_argv[6] = "--format=%h%x09%s";
            log_argv[7] = "--max-count=50";
            log_argv[8] = "--end-of-options";
            log_argv[9] = range;
            if (!pkg_git_simple(log_argv, 10U, 10000, true, "log", &run)) {
                yew_pkg_git_run_free(&run);
                yew_xfree(dir);
                failed = true;
                nunreachable++;
                continue;
            }
            if (run.out.len != 0U)
                pkg_print_sanitized(stdout, run.out.data, run.out.len);
            yew_pkg_git_run_free(&run);
            count_argv[0] = "git";
            count_argv[1] = "-C";
            count_argv[2] = dir;
            count_argv[3] = "rev-list";
            count_argv[4] = "--count";
            count_argv[5] = "--no-merges";
            count_argv[6] = "--end-of-options";
            count_argv[7] = range;
            if (!pkg_git_simple(count_argv, 8U, 10000, true,
                                "rev-list count", &run)) {
                yew_pkg_git_run_free(&run);
                yew_xfree(dir);
                failed = true;
                nunreachable++;
                continue;
            }
            {
                char *count_text = pkg_buf_string(&run.out);
                char *end = NULL;
                unsigned long count;

                errno = 0;
                count = strtoul(count_text, &end, 10);
                if (errno == 0 && end != count_text &&
                    (*end == '\n' || *end == '\r' || *end == '\0') &&
                    count > 50UL)
                    (void)printf("... and %lu more\n", count - 50UL);
                yew_xfree(count_text);
            }
            yew_pkg_git_run_free(&run);
        }
        if (dry) {
            (void)printf("%s would update %.12s -> %.12s\n", entry->name,
                         entry->rev, target);
            yew_xfree(dir);
            continue;
        }
        {
            const char *const status_argv[] = {"git", "-C", dir, "status",
                                               "--porcelain",
                                               "--untracked-files=no"};
            if (!pkg_git_simple(status_argv, YEW_ARRAY_LEN(status_argv),
                                10000, true, "status", &run)) {
                yew_pkg_git_run_free(&run);
                yew_xfree(dir);
                failed = true;
                continue;
            }
            if (run.out.len != 0U && !discard) {
                (void)fprintf(stderr,
                              "yew pkg: refusing to update %s: working tree "
                              "is dirty (use --discard-local)\n",
                              entry->name);
                pkg_print_sanitized(stderr, run.out.data, run.out.len);
                yew_pkg_git_run_free(&run);
                yew_xfree(dir);
                refused = true;
                continue;
            }
            yew_pkg_git_run_free(&run);
        }
        (void)memcpy(old_rev, entry->rev, sizeof(old_rev));
        (void)memcpy(old_tree, entry->tree, sizeof(old_tree));
        old_updated_at = entry->updated_at;
        if (!pkg_checkout(dir, target, discard) ||
            !yew_pkg_tree_hash(dir, entry->tree, &dc) ||
            !pkg_fsync_tree(dir, &dc)) {
            if (!pkg_checkout(dir, old_rev, true) ||
                !pkg_fsync_tree(dir, &dc))
                (void)fprintf(stderr,
                              "yew pkg: error: update rollback failed; recover with git -C %s checkout --force --detach %s\n",
                              dir, old_rev);
            (void)memcpy(entry->tree, old_tree, sizeof(entry->tree));
            yew_xfree(dir);
            failed = true;
            continue;
        }
        pkg_warn_gitmodules(dir);
        (void)memcpy(entry->rev, target, sizeof(entry->rev));
        entry->updated_at = pkg_now();
        if (!yew_pkg_lock_save(&lock, &dc)) {
            if (!pkg_checkout(dir, old_rev, true) ||
                !pkg_fsync_tree(dir, &dc))
                (void)fprintf(stderr,
                              "yew pkg: error: update rollback failed; recover with git -C %s checkout --force --detach %s\n",
                              dir, old_rev);
            (void)memcpy(entry->rev, old_rev, sizeof(entry->rev));
            (void)memcpy(entry->tree, old_tree, sizeof(entry->tree));
            entry->updated_at = old_updated_at;
            yew_xfree(dir);
            failed = true;
            continue;
        }
        (void)printf("updated %s to %.12s\n", entry->name, entry->rev);
        nupdated++;
        yew_xfree(dir);
    }
    yew_xfree(root);
    yew_pkg_lock_free(&lock);
    arena_free_all(&a);
    (void)printf("%u updated, %u up to date, %u unreachable\n",
                 nupdated, nuptodate, nunreachable);
    return failed ? 3 : refused ? 1 : 0;
}

int yew_pkg_main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) {
        (void)fprintf(stderr,
                      "usage: yew pkg <install|update|remove|list|doctor> "
                      "...\n");
        return 1;
    }
    if (!pkg_txn_recover()) {
        (void)fprintf(stderr,
                      "yew pkg: could not recover an interrupted package transaction\n");
        return 3;
    }
    cmd = argv[1];
    if (strcmp(cmd, "install") == 0)
        return pkg_install(argc - 1, argv + 1);
    if (strcmp(cmd, "update") == 0)
        return pkg_update(argc - 1, argv + 1);
    if (strcmp(cmd, "remove") == 0)
        return pkg_remove(argc - 1, argv + 1);
    if (strcmp(cmd, "list") == 0)
        return pkg_list_or_doctor(argc - 1, argv + 1, false);
    if (strcmp(cmd, "doctor") == 0 || strcmp(cmd, "verify") == 0)
        return pkg_list_or_doctor(argc - 1, argv + 1, true);
    if (strcmp(cmd, "publish") == 0 || strcmp(cmd, "search") == 0) {
        (void)fprintf(stderr,
                      "yew pkg: no central registry: a registry implies "
                      "curation, and yew would not do the curation\n");
        return 1;
    }
    pkg_diag_arg("yew pkg: unknown subcommand: ", cmd, "\n");
    return 1;
}
