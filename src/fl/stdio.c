/*
 * Sprint 31 deliverable 7: the `io` module.
 *
 * EVERY FILESYSTEM ENTRY POINT CHECKS A CAPABILITY FIRST, and checks it
 * through fl_cap_check, which reads the grant from the DEFINING MODULE
 * of the calling function (spec §13).  Not from the stack top, not from
 * a VM-global: `list.map(f, io.read)` must check f's grants rather than
 * list's, or every stdlib function launders authority for whatever
 * called it.
 *
 * io.print and io.eprint need no capability.  The host already chose to
 * run this script, and denying it output produces silent failures
 * rather than safety.
 *
 * PATHS ARE BYTES.  A path is not validated as UTF-8 anywhere here --
 * invariant 2 exists because a file whose name is not valid UTF-8 is
 * still a file.  The one thing refused is an embedded NUL, because the
 * syscall would stop there and act on a DIFFERENT path than the one the
 * script named, which is the byte-confusion this module must not have.
 *
 * ERRORS CARRY THE ERRNO NAME AND THE PATH, and the kind supplies the
 * "io" -- rendered, they read `io: ENOENT: /home/u/x.fl`.  The name
 * rather than the number because a user can search for ENOENT, and our
 * own table rather than strerror because strerror is locale-dependent
 * and a message that changes with LANG breaks a golden.
 */
#define _POSIX_C_SOURCE 200809L

#include "fl/std.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/gc.h"
#include "text/file.h"
#include "util/intern.h"
#include "util/sort.h"

enum {
    /* The same cap str and fmt use.  A config that reads a 4 GiB file
     * into a Fletch string has made a mistake we should name. */
    IO_MAX = 64U * 1024U * 1024U,
    IO_GLOB_MAX_DEPTH = 64,
    IO_GLOB_MAX_RESULTS = 100000
};

/* ---------------------------------------------------------------- */
/* errno names and raising                                          */
/* ---------------------------------------------------------------- */

static const char *errno_name(int e)
{
    switch (e) {
    case EACCES:       return "EACCES";
    case EAGAIN:       return "EAGAIN";
    case EBADF:        return "EBADF";
    case EBUSY:        return "EBUSY";
    case EEXIST:       return "EEXIST";
    case EFAULT:       return "EFAULT";
    case EFBIG:        return "EFBIG";
    case EINTR:        return "EINTR";
    case EINVAL:       return "EINVAL";
    case EIO:          return "EIO";
    case EISDIR:       return "EISDIR";
    case ELOOP:        return "ELOOP";
    case EMFILE:       return "EMFILE";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case ENFILE:       return "ENFILE";
    case ENODEV:       return "ENODEV";
    case ENOENT:       return "ENOENT";
    case ENOMEM:       return "ENOMEM";
    case ENOSPC:       return "ENOSPC";
    case ENOTDIR:      return "ENOTDIR";
    case ENOTEMPTY:    return "ENOTEMPTY";
    case EOVERFLOW:    return "EOVERFLOW";
    case EPERM:        return "EPERM";
    case EROFS:        return "EROFS";
    case ESPIPE:       return "ESPIPE";
    case EXDEV:        return "EXDEV";
    default:           return NULL;
    }
}

static bool io_err(FlVm *vm, int e, const char *path)
{
    const char *nm = errno_name(e);

    if (nm != NULL)
        return fl_raise(vm, "io", "%s: %s", nm, path);
    /* An errno we have no name for still names its number rather than
     * printing a locale-dependent sentence. */
    return fl_raise(vm, "io", "errno %d: %s", e, path);
}

/* ---------------------------------------------------------------- */
/* Paths                                                            */
/* ---------------------------------------------------------------- */

/*
 * A NUL-terminated copy for the syscall, refusing an embedded NUL.
 *
 * Truncating at the NUL would have the kernel act on a path the script
 * did not name -- "/etc/passwd\0/safe" reading /etc/passwd -- which is
 * the difference between a refused call and a confused one.  Returns
 * NULL having raised; the caller frees on success.
 */
static char *path_of(FlVm *vm, const FlStr *s)
{
    char *p;
    u32 i;

    for (i = 0U; i < s->len; i++) {
        if (s->b[i] == '\0') {
            (void)fl_raise(vm, "io",
                           "path contains a NUL byte at offset %u",
                           (unsigned)i);
            return NULL;
        }
    }
    if (s->len == 0U) {
        (void)fl_raise(vm, "io", "the path is empty");
        return NULL;
    }
    p = yew_xmalloc((size_t)s->len + 1U);
    (void)memcpy(p, s->b, (size_t)s->len);
    p[s->len] = '\0';
    return p;
}

/* Argument 0 as a checked path, with the capability taken first: a
 * denied call must not reveal whether the path was well formed. */
static char *arg_path(FlVm *vm, FlValue *a, u32 i, u32 cap)
{
    const FlStr *s;

    if (!fl_cap_check(vm, cap))
        return NULL;
    if (!fl_arg_str(vm, a, i, &s))
        return NULL;
    return path_of(vm, s);
}

/* ---------------------------------------------------------------- */
/* Reading and writing                                              */
/* ---------------------------------------------------------------- */

/* Whole file into `bb`.  Returns false having raised. */
static bool slurp(FlVm *vm, const char *path, Bytebuf *bb)
{
    struct stat st;
    int fd;
    int e;

    bytebuf_init(bb);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return io_err(vm, errno, path);
    if (fstat(fd, &st) != 0) {
        e = errno;
        (void)close(fd);
        return io_err(vm, e, path);
    }
    /* A directory read()s as EISDIR on Linux and succeeds on some other
     * systems; naming it here makes the message the same everywhere. */
    if (S_ISDIR(st.st_mode)) {
        (void)close(fd);
        return io_err(vm, EISDIR, path);
    }
    for (;;) {
        char buf[65536];
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            e = errno;
            (void)close(fd);
            bytebuf_free(bb);
            return io_err(vm, e, path);
        }
        if (n == 0)
            break;
        bytebuf_append(bb, buf, (size_t)n);
        if (bb->len > (size_t)IO_MAX) {
            (void)close(fd);
            bytebuf_free(bb);
            return fl_raise(vm, "limit", "io.read: %s exceeds 64 MiB", path);
        }
    }
    (void)close(fd);
    return true;
}

static bool io_read(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_READ);
    Bytebuf bb;

    (void)n;
    if (path == NULL)
        return false;
    if (!slurp(vm, path, &bb)) {
        yew_xfree(path);
        return false;
    }
    yew_xfree(path);
    *out = FL_OBJ_V(FL_STR, fl_str_take(vm, &bb));
    bytebuf_free(&bb);
    return true;
}

static bool io_read_lines(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_READ);
    Bytebuf bb;

    (void)n;
    if (path == NULL)
        return false;
    if (!slurp(vm, path, &bb)) {
        yew_xfree(path);
        return false;
    }
    yew_xfree(path);
    /* str.split_lines of the content, and literally so -- see
     * fl_split_lines. */
    *out = fl_split_lines(vm, (const char *)bb.data, (u32)bb.len);
    bytebuf_free(&bb);
    return true;
}

static bool io_stdin(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Bytebuf bytes;

    (void)a;
    (void)n;
    if (vm->ed != NULL && vm->ed->batch_stdin_claimed)
        return fl_raise(vm, "io",
                        "stdin is already the '-' batch file buffer");
    if (vm->ed != NULL)
        vm->ed->batch_stdin_claimed = true;
    bytebuf_init(&bytes);
    for (;;) {
        u8 chunk[65536];
        ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));

        if (got > 0) {
            if (bytes.len > (size_t)IO_MAX - (size_t)got) {
                bytebuf_free(&bytes);
                return fl_raise(vm, "limit", "io.stdin input exceeds %u bytes",
                                (unsigned)IO_MAX);
            }
            bytebuf_append(&bytes, chunk, (size_t)got);
        } else if (got == 0) {
            *out = FL_OBJ_V(FL_STR, fl_str_take(vm, &bytes));
            bytebuf_free(&bytes);
            return true;
        } else if (errno != EINTR) {
            int saved = errno;

            bytebuf_free(&bytes);
            return io_err(vm, saved, "stdin");
        }
    }
}

static bool io_write(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *body;
    char *path;

    (void)n;
    path = arg_path(vm, a, 0U, FL_CAP_FS_WRITE);
    if (path == NULL)
        return false;
    if (!fl_arg_str(vm, a, 1U, &body)) {
        yew_xfree(path);
        return false;
    }
    /*
     * ATOMIC, through Sprint 8's own primitive: temp file in the same
     * directory, fsync, rename, fsync the directory.  Invariant 1 says
     * an interrupted write must not lose the old contents, and there is
     * exactly one implementation of that in the tree.
     */
    if (yew_file_write_atomic(path, (const u8 *)body->b, (size_t)body->len,
                              0666) != YEW_SAVE_OK) {
        bool r = io_err(vm, errno == 0 ? EIO : errno, path);

        yew_xfree(path);
        return r;
    }
    yew_xfree(path);
    *out = FL_NIL_V;
    return true;
}

static bool io_append(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *body;
    char *path;
    size_t off = 0U;
    int fd;

    (void)n;
    path = arg_path(vm, a, 0U, FL_CAP_FS_WRITE);
    if (path == NULL)
        return false;
    if (!fl_arg_str(vm, a, 1U, &body)) {
        yew_xfree(path);
        return false;
    }
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) {
        bool r = io_err(vm, errno, path);

        yew_xfree(path);
        return r;
    }
    while (off < (size_t)body->len) {
        ssize_t w = write(fd, body->b + off, (size_t)body->len - off);

        if (w < 0) {
            int e = errno;

            if (e == EINTR)
                continue;
            bool r;

            (void)close(fd);
            r = io_err(vm, e, path);
            yew_xfree(path);
            return r;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) {
        bool r = io_err(vm, errno, path);

        yew_xfree(path);
        return r;
    }
    yew_xfree(path);
    *out = FL_NIL_V;
    return true;
}

/* ---------------------------------------------------------------- */
/* Interrogation                                                    */
/* ---------------------------------------------------------------- */

static bool io_exists(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_READ);
    struct stat st;

    (void)n;
    if (path == NULL)
        return false;
    /* False on ANY error, per the table: `exists` answers a question,
     * and a permission problem on the parent directory is still "I
     * cannot see it". */
    *out = FL_BOOL_V(stat(path, &st) == 0);
    yew_xfree(path);
    return true;
}

static bool io_is_dir(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_READ);
    struct stat st;

    (void)n;
    if (path == NULL)
        return false;
    *out = FL_BOOL_V(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    yew_xfree(path);
    return true;
}

static bool io_size(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_READ);
    struct stat st;

    (void)n;
    if (path == NULL)
        return false;
    if (stat(path, &st) != 0) {
        bool r = io_err(vm, errno, path);

        yew_xfree(path);
        return r;
    }
    yew_xfree(path);
    *out = FL_INT_V((i64)st.st_size);
    return true;
}

static bool io_remove(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_WRITE);
    struct stat st;
    int rc;

    (void)n;
    if (path == NULL)
        return false;
    /* lstat, not stat: removing a symlink removes the LINK, and asking
     * about its target would send a dangling link down the rmdir path. */
    if (lstat(path, &st) != 0) {
        bool r = io_err(vm, errno, path);

        yew_xfree(path);
        return r;
    }
    rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (rc != 0) {
        bool r = io_err(vm, errno, path);

        yew_xfree(path);
        return r;
    }
    yew_xfree(path);
    *out = FL_NIL_V;
    return true;
}

/* One level; `parents` walks the prefixes.  Existing directories are
 * not an error under `parents`, because "make sure this path exists" is
 * what the flag means. */
static bool mkdir_parents(FlVm *vm, char *path)
{
    size_t i;

    for (i = 1U; path[i] != '\0'; i++) {
        if (path[i] != '/')
            continue;
        path[i] = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) {
            int e = errno;
            /* Reported while still truncated: the level that failed is
             * the actionable half of the path. */
            bool r = io_err(vm, e, path);

            path[i] = '/';
            return r;
        }
        path[i] = '/';
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        return io_err(vm, errno, path);
    return true;
}

static bool io_mkdir(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    char *path = arg_path(vm, a, 0U, FL_CAP_FS_WRITE);
    bool parents = n >= 2U && fl_truthy(a[1]);
    bool ok;

    if (path == NULL)
        return false;
    if (parents) {
        ok = mkdir_parents(vm, path);
    } else if (mkdir(path, 0777) != 0) {
        ok = io_err(vm, errno, path);
    } else {
        ok = true;
    }
    yew_xfree(path);
    if (!ok)
        return false;
    *out = FL_NIL_V;
    return true;
}

static bool io_env(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    char *name;
    const char *v;

    (void)n;
    if (!fl_cap_check(vm, FL_CAP_FS_READ))
        return false;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    name = path_of(vm, s);
    if (name == NULL)
        return false;
    /* The one getenv A SCRIPT can reach.  Environment reads are ambient
     * authority by another name, so they go through a capability and
     * through one function a reviewer can find; the VM's own two --
     * FL_GC_STRESS and YEW_FL_DUMP_BAD_CHUNK -- are developer switches
     * no Fletch program can name. */
    v = getenv(name);
    yew_xfree(name);
    *out = v == NULL ? FL_NIL_V
                     : FL_OBJ_V(FL_STR, fl_str_new(vm, v, (u32)strlen(v)));
    return true;
}

/* ---------------------------------------------------------------- */
/* Output                                                           */
/* ---------------------------------------------------------------- */

/*
 * Space-joined display forms and a newline.
 *
 * From Sprint 34 the host redirects both streams to the message line
 * and yew_log while the TUI owns the terminal; nothing in src/fl/ ever
 * writes an escape sequence or touches the tty directly.
 */
static bool print_to(FlVm *vm, FlValue *a, u32 n, FILE *f, FlValue *out)
{
    Bytebuf bb;
    u32 i;

    bytebuf_init(&bb);
    for (i = 0U; i < n; i++) {
        if (i != 0U)
            bytebuf_push_u8(&bb, (u8)' ');
        if (!fl_fmt_display(vm, &bb, a[i])) {
            bytebuf_free(&bb);
            return false;
        }
    }
    bytebuf_push_u8(&bb, (u8)'\n');
    (void)fwrite(bb.data, 1U, bb.len, f);
    bytebuf_free(&bb);
    *out = FL_NIL_V;
    return true;
}

static bool io_print(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    return print_to(vm, a, n, stdout, out);
}

static bool io_eprint(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    return print_to(vm, a, n, stderr, out);
}

/* ---------------------------------------------------------------- */
/* glob                                                             */
/* ---------------------------------------------------------------- */

/*
 * Bespoke (invariant 7), and deliberately smaller than the libc one:
 * `*` `?` `[abc]` `[a-z]` `[!...]` `\` within a path component, and
 * `**` as a WHOLE component to cross directories.
 *
 * Matching is recursive with a `*` that backtracks, which is quadratic
 * on a pathological component and bounded by the component length --
 * a directory entry is at most NAME_MAX bytes, so there is no runaway
 * here to guard against.
 */
static bool class_match(const char *p, size_t pn, size_t *pi, u8 c)
{
    size_t i = *pi + 1U;
    bool neg = false;
    bool hit = false;

    if (i < pn && (p[i] == '!' || p[i] == '^')) {
        neg = true;
        i++;
    }
    /* A `]` first is a literal `]`, the traditional escape-free way to
     * put one in a class. */
    if (i < pn && p[i] == ']') {
        if (c == (u8)']')
            hit = true;
        i++;
    }
    while (i < pn && p[i] != ']') {
        u8 lo;

        if (p[i] == '\\' && i + 1U < pn)
            i++;
        lo = (u8)p[i];
        if (i + 2U < pn && p[i + 1U] == '-' && p[i + 2U] != ']') {
            u8 hi;

            i += 2U;
            if (p[i] == '\\' && i + 1U < pn)
                i++;
            hi = (u8)p[i];
            if (c >= lo && c <= hi)
                hit = true;
        } else if (c == lo) {
            hit = true;
        }
        i++;
    }
    /* An unterminated class consumes the rest of the pattern; it cannot
     * match, and reporting it as a syntax error would make a literal
     * `[` in a filename impossible to glob for without an escape. */
    *pi = i < pn ? i + 1U : pn;
    return neg ? !hit : hit;
}

static bool comp_match(const char *p, size_t pn, const char *s, size_t sn)
{
    size_t pi = 0U;
    size_t si = 0U;
    size_t star = (size_t)-1;
    size_t star_s = 0U;

    while (si < sn) {
        if (pi < pn && p[pi] == '*') {
            star = pi++;
            star_s = si;
            continue;
        }
        if (pi < pn && p[pi] == '?') {
            pi++;
            si++;
            continue;
        }
        if (pi < pn && p[pi] == '[') {
            size_t next = pi;

            if (class_match(p, pn, &next, (u8)s[si])) {
                pi = next;
                si++;
                continue;
            }
        } else if (pi < pn) {
            size_t lit = pi;

            if (p[lit] == '\\' && lit + 1U < pn)
                lit++;
            if (p[lit] == s[si]) {
                pi = lit + 1U;
                si++;
                continue;
            }
        }
        if (star == (size_t)-1)
            return false;
        pi = star + 1U;
        si = ++star_s;
    }
    while (pi < pn && p[pi] == '*')
        pi++;
    return pi == pn;
}

typedef struct GlobCtx {
    FlVm *vm;
    FlList *out;
    const char **comp;      /* pattern components, pointers into `pat` */
    size_t *complen;
    u32 ncomp;
    bool failed;            /* a raise is in flight                    */
} GlobCtx;

/* Entries of one directory, sorted bytewise: readdir order differs
 * between filesystems and between two checkouts, and invariant 5 says
 * the same tree must produce the same list. */
typedef struct Names {
    char **v;
    u32 n;
    u32 cap;
} Names;

static int name_cmp(const void *a, const void *b, void *ctx)
{
    const char *const *x = a;
    const char *const *y = b;

    (void)ctx;
    return strcmp(*x, *y);
}

static void names_free(Names *ns)
{
    u32 i;

    for (i = 0U; i < ns->n; i++)
        yew_xfree(ns->v[i]);
    yew_xfree(ns->v);
    ns->v = NULL;
    ns->n = 0U;
    ns->cap = 0U;
}

static bool names_read(Names *ns, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    ns->v = NULL;
    ns->n = 0U;
    ns->cap = 0U;
    if (d == NULL)
        return false;
    while ((e = readdir(d)) != NULL) {
        char *copy;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (ns->n == ns->cap) {
            ns->cap = ns->cap == 0U ? 16U : ns->cap * 2U;
            ns->v = yew_xreallocarray(ns->v, ns->cap, sizeof(*ns->v));
        }
        copy = yew_xmalloc(strlen(e->d_name) + 1U);
        (void)memcpy(copy, e->d_name, strlen(e->d_name) + 1U);
        ns->v[ns->n++] = copy;
    }
    (void)closedir(d);
    yew_sort_stable(ns->v, ns->n, sizeof(*ns->v), name_cmp, NULL);
    return true;
}

static bool is_dir_nofollow(const char *path)
{
    struct stat st;

    /* lstat: a symlinked directory is NOT descended.  A loop then
     * cannot happen without a visited set, and a glob that follows
     * links is a glob that eventually walks /proc. */
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool glob_add(GlobCtx *g, const char *path)
{
    if (g->out->n >= (u32)IO_GLOB_MAX_RESULTS) {
        g->failed = true;
        return fl_raise(g->vm, "limit", "io.glob: more than %d matches",
                        IO_GLOB_MAX_RESULTS);
    }
    (void)fl_list_push(g->vm, g->out,
                       FL_OBJ_V(FL_STR,
                                fl_str_new(g->vm, path,
                                           (u32)strlen(path))));
    return true;
}

/* `here` is the directory being scanned, already joined onto the root;
 * `ci` is the pattern component it must be matched against. */
static void glob_walk(GlobCtx *g, Bytebuf *here, u32 ci, u32 depth)
{
    Names ns;
    u32 i;
    size_t mark = here->len;

    if (g->failed)
        return;
    if (depth > (u32)IO_GLOB_MAX_DEPTH) {
        g->failed = true;
        (void)fl_raise(g->vm, "limit", "io.glob: deeper than %d directories",
                       IO_GLOB_MAX_DEPTH);
        return;
    }
    bytebuf_push_u8(here, 0U);
    if (!names_read(&ns, (const char *)here->data)) {
        /* An unreadable directory is skipped, not fatal: a glob over a
         * home directory that contains one root-owned subdirectory
         * should still return the other matches. */
        here->len = mark;
        return;
    }
    here->len = mark;
    for (i = 0U; i < ns.n && !g->failed; i++) {
        const char *name = ns.v[i];
        size_t namelen = strlen(name);
        bool leaf = ci + 1U == g->ncomp;
        bool dbl = g->complen[ci] == 2U && g->comp[ci][0] == '*' &&
                   g->comp[ci][1] == '*';

        /* Dot-files only when the pattern component asks for them.  `*`
         * not matching `.bashrc` is the behaviour every shell has and
         * every user expects. */
        if (name[0] == '.' && !(g->complen[ci] > 0U && g->comp[ci][0] == '.'))
            continue;
        here->len = mark;
        bytebuf_push_u8(here, (u8)'/');
        bytebuf_append(here, name, namelen);
        bytebuf_push_u8(here, 0U);
        here->len--;
        if (dbl) {
            /* `**` matches this level too, so the remaining components
             * are tried both here and one level down. */
            if (leaf) {
                if (!glob_add(g, (const char *)here->data))
                    break;
            } else if (comp_match(g->comp[ci + 1U], g->complen[ci + 1U],
                                  name, namelen)) {
                if (ci + 2U == g->ncomp) {
                    if (!glob_add(g, (const char *)here->data))
                        break;
                } else if (is_dir_nofollow((const char *)here->data)) {
                    glob_walk(g, here, ci + 2U, depth + 1U);
                }
            }
            if (is_dir_nofollow((const char *)here->data))
                glob_walk(g, here, ci, depth + 1U);
            continue;
        }
        if (!comp_match(g->comp[ci], g->complen[ci], name, namelen))
            continue;
        if (leaf) {
            if (!glob_add(g, (const char *)here->data))
                break;
        } else if (is_dir_nofollow((const char *)here->data)) {
            glob_walk(g, here, ci + 1U, depth + 1U);
        }
    }
    here->len = mark;
    names_free(&ns);
}

static int path_cmp(const void *a, const void *b, void *ctx)
{
    const FlValue *x = a;
    const FlValue *y = b;
    const FlStr *p = (const FlStr *)x->as.o;
    const FlStr *q = (const FlStr *)y->as.o;
    u32 n = p->len < q->len ? p->len : q->len;
    int c = n == 0U ? 0 : memcmp(p->b, q->b, (size_t)n);

    (void)ctx;
    if (c != 0)
        return c;
    return p->len < q->len ? -1 : (p->len > q->len ? 1 : 0);
}

/*
 * Sorted BYTEWISE over the whole result, not per directory.
 *
 * Per-directory sorting is not enough: a depth-first walk emits "a/z"
 * before "a.txt", and bytewise '.' sorts before '/'.  Invariant 5 wants
 * one order, and it is this one.
 *
 * The dedupe rides along because a crossing component can reach the
 * same path by more than one route -- two of them in one pattern names
 * a file twice otherwise, and a list with a repeat in it makes a config
 * that iterates it do the work twice.
 */
static void sort_unique(FlList *l)
{
    u32 i;
    u32 keep = 0U;

    if (l->n < 2U) {
        return;
    }
    yew_sort_stable(l->v, l->n, sizeof(*l->v), path_cmp, NULL);
    for (i = 0U; i < l->n; i++) {
        if (i == 0U || path_cmp(&l->v[keep - 1U], &l->v[i], NULL) != 0)
            l->v[keep++] = l->v[i];
    }
    l->n = keep;
}

/*
 * The default root is the DIRECTORY OF THE IMPORTING MODULE, never the
 * process cwd.
 *
 * Pinned because it is a real bug class: a config that globbed relative
 * to the cwd would find its own snippets when yew was launched from
 * the config directory and find nothing anywhere else, and the user
 * would report it as "plugins load sometimes".  When the caller has no
 * file -- the CLI and the REPL -- the cwd IS the caller's context, and
 * that is the only case where it is used.
 */
static void default_root(FlVm *vm, Bytebuf *out)
{
    FlOrigin o = fl_cap_origin(vm);
    const char *p = o.path_id == 0U ? NULL : yew_intern_str(vm->in, o.path_id);
    const char *slash;

    if (p != NULL) {
        slash = strrchr(p, '/');
        if (slash != NULL && slash != p) {
            bytebuf_append(out, p, (size_t)(slash - p));
            return;
        }
        if (slash == p) {
            bytebuf_push_u8(out, (u8)'/');
            return;
        }
    }
    bytebuf_push_u8(out, (u8)'.');
}

static bool io_glob(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *pat;
    const FlStr *rootarg = NULL;
    GlobCtx g;
    Bytebuf here;
    const char *comp[IO_GLOB_MAX_DEPTH];
    size_t complen[IO_GLOB_MAX_DEPTH];
    size_t i = 0U;
    u32 ncomp = 0U;

    if (!fl_cap_check(vm, FL_CAP_FS_READ))
        return false;
    if (!fl_arg_str(vm, a, 0U, &pat))
        return false;
    if (n >= 2U && !fl_arg_str(vm, a, 1U, &rootarg))
        return false;
    if (pat->len == 0U)
        return fl_raise(vm, "type", "io.glob: the pattern is empty");
    for (i = 0U; i < (size_t)pat->len; i++) {
        if (pat->b[i] == '\0')
            return fl_raise(vm, "type",
                            "io.glob: the pattern contains a NUL byte");
    }
    /* Split on '/'.  An empty component (a doubled slash, or a leading
     * one on an absolute pattern) is dropped: the root carries the
     * anchor. */
    i = 0U;
    while (i < (size_t)pat->len) {
        size_t start = i;

        while (i < (size_t)pat->len && pat->b[i] != '/')
            i++;
        if (i > start) {
            if (ncomp == (u32)IO_GLOB_MAX_DEPTH)
                return fl_raise(vm, "limit",
                                "io.glob: more than %d path components",
                                IO_GLOB_MAX_DEPTH);
            comp[ncomp] = pat->b + start;
            complen[ncomp] = i - start;
            ncomp++;
        }
        if (i < (size_t)pat->len)
            i++;
    }
    if (ncomp == 0U)
        return fl_raise(vm, "type", "io.glob: the pattern has no components");
    bytebuf_init(&here);
    if (pat->b[0] == '/') {
        /* An absolute pattern anchors itself; the root is ignored
         * rather than prepended, so a pattern under /etc stays under
         * /etc instead of being reparented below the root. */
        ;
    } else if (rootarg != NULL) {
        if (rootarg->len == 0U) {
            bytebuf_free(&here);
            return fl_raise(vm, "type", "io.glob: the root is empty");
        }
        bytebuf_append(&here, rootarg->b, (size_t)rootarg->len);
        while (here.len > 1U && here.data[here.len - 1U] == (u8)'/')
            here.len--;
        /* A root of "/" becomes the empty prefix, so the join below
         * produces "/etc" rather than "//etc". */
        if (here.len == 1U && here.data[0] == (u8)'/')
            here.len = 0U;
    } else {
        default_root(vm, &here);
    }
    g.vm = vm;
    g.comp = comp;
    g.complen = complen;
    g.ncomp = ncomp;
    g.failed = false;
    g.out = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, g.out));
    glob_walk(&g, &here, 0U, 0U);
    sort_unique(g.out);
    fl_gc_release(vm, 1U);
    bytebuf_free(&here);
    if (g.failed)
        return false;
    *out = FL_OBJ_V(FL_LIST, g.out);
    return true;
}

/* ---------------------------------------------------------------- */

static const FlNativeDef IO_DEFS[] = {
    {"stdin",      io_stdin,      0U, 0U, 0U,                "() -> str"},
    {"read",       io_read,       1U, 1U, FL_CAP_FS_READ,  "(path) -> str"},
    {"read_lines", io_read_lines, 1U, 1U, FL_CAP_FS_READ,  "(path) -> list"},
    {"write",      io_write,      2U, 2U, FL_CAP_FS_WRITE, "(path, s) -> nil"},
    {"append",     io_append,     2U, 2U, FL_CAP_FS_WRITE, "(path, s) -> nil"},
    {"exists",     io_exists,     1U, 1U, FL_CAP_FS_READ,  "(path) -> bool"},
    {"is_dir",     io_is_dir,     1U, 1U, FL_CAP_FS_READ,  "(path) -> bool"},
    {"size",       io_size,       1U, 1U, FL_CAP_FS_READ,  "(path) -> int"},
    {"remove",     io_remove,     1U, 1U, FL_CAP_FS_WRITE, "(path) -> nil"},
    {"mkdir",      io_mkdir,      1U, 2U, FL_CAP_FS_WRITE,
     "(path, [parents]) -> nil"},
    {"glob",       io_glob,       1U, 2U, FL_CAP_FS_READ,
     "(pattern, [root]) -> list"},
    {"env",        io_env,        1U, 1U, FL_CAP_FS_READ,  "(name) -> str|nil"},
    {"print",      io_print,      0U, FL_VARIADIC, 0U, "(...) -> nil"},
    {"eprint",     io_eprint,     0U, FL_VARIADIC, 0U, "(...) -> nil"}
};

const FlModuleDef fl_mod_io = {
    "io", IO_DEFS, (u32)YEW_ARRAY_LEN(IO_DEFS), NULL, 0U
};
