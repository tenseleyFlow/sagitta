/*
 * Sprint 33 §1-§3: the Fletch conformance runner.
 *
 * Walks tests/fletch/ in LC_ALL=C order, runs each unit through
 * `yew fl`, and applies the directives written in its comments.  Also
 * generates ledger.txt (--ledger), which the coverage gate diffs.
 *
 * THE DIRECTIVES COME FROM THE LEXER'S COMMENT TOKENS, NOT FROM RAW
 * LINES.  `let s = "# CHECK: nope"` is a string, and a line-based
 * scanner fires on it -- the test then passes for the wrong reason
 * forever, which is the failure mode a conformance suite exists to
 * prevent.  s33 added FlLexer.keep_comments for exactly this.
 *
 * A typo'd directive is a CONFIGURATION ERROR, never an ordinary
 * comment, for the same reason: `# CHEK: x` that degrades into prose
 * is a test that silently stops asserting anything.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fl/lex.h"
#include "fl/opcodes.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/sort.h"

enum {
    MAX_UNITS      = 256,
    MAX_DIRECTIVES = 256,
    MAX_COVERS     = 512,
    MAX_SPECS      = 8,
    DEFAULT_TIMEOUT_SECS = 10,
    /* §11 of the sprint: the escape hatch may not become the norm. */
    MAX_GC_OPTOUTS = 2
};

/* ---------------------------------------------------------------- */
/* Small helpers                                                    */
/* ---------------------------------------------------------------- */

static const char *g_yew = "build/yew";
static const char *g_root = "tests/fletch";
static const char *g_spec = ".docs/fletch-spec.md";
static char *g_spec_src;

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)fputs("fletch: ", stderr);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    exit(2);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    Bytebuf bb;
    char *out;

    if (f == NULL)
        return NULL;
    bytebuf_init(&bb);
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1U, sizeof(buf), f);

        if (n != 0U)
            bytebuf_append(&bb, buf, n);
        if (n != sizeof(buf))
            break;
    }
    (void)fclose(f);
    out = malloc(bb.len + 1U);
    if (out == NULL)
        die("out of memory reading %s", path);
    if (bb.len != 0U)
        (void)memcpy(out, bb.data, bb.len);
    out[bb.len] = '\0';
    if (len != NULL)
        *len = bb.len;
    bytebuf_free(&bb);
    return out;
}

static bool is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Trailing whitespace off; leading too.  Directive values are compared
 * literally, so `# EXIT: 3 ` must not carry its space into the parse.
 *
 * SHIFTS IN PLACE rather than returning an advanced pointer: the value
 * is a malloc'd block, and handing back an interior pointer loses the
 * one free() could use.  Under the valgrind lane that is a definite
 * leak per directive.
 */
static char *trim(char *s)
{
    size_t lead = 0U;
    size_t n;

    while (s[lead] == ' ' || s[lead] == '\t')
        lead++;
    n = strlen(s + lead);
    if (lead != 0U)
        (void)memmove(s, s + lead, n + 1U);
    while (n != 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' ||
                       s[n - 1U] == '\r'))
        s[--n] = '\0';
    return s;
}

/* memmem is a GNU extension and this file is -std=c11 -pedantic. */
static const char *find_bytes(const char *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    size_t i;

    if (nn == 0U)
        return hay;
    if (hn < nn)
        return NULL;
    for (i = 0U; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0)
            return hay + i;
    }
    return NULL;
}

static char *dupe(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *c = malloc(n);

    if (c == NULL)
        die("out of memory");
    (void)memcpy(c, s, n);
    return c;
}

static int cmp_str(const void *a, const void *b, void *ctx)
{
    (void)ctx;
    /* LC_ALL=C order, stated rather than inherited from the locale:
     * the ledger is byte-compared in CI and must not depend on it. */
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ---------------------------------------------------------------- */
/* Directives                                                       */
/* ---------------------------------------------------------------- */

typedef enum DKind {
    D_SPEC, D_COVERS, D_CHECK, D_CHECK_NEXT, D_CHECK_NOT, D_OUT,
    D_ERROR, D_ERROR_KIND, D_ERROR_LINE, D_EXIT, D_CAPS, D_ORIGIN,
    D_ARGS, D_TIMEOUT, D_GC_STRESS, D_XFAIL, D_SKIP,
    D_KIND__N
} DKind;

static const char *const d_names[D_KIND__N] = {
    "SPEC", "COVERS", "CHECK", "CHECK_NEXT", "CHECK_NOT", "OUT",
    "ERROR", "ERROR_KIND", "ERROR_LINE", "EXIT", "CAPS", "ORIGIN",
    "ARGS", "TIMEOUT", "GC_STRESS", "XFAIL", "SKIP"
};

typedef struct Directive {
    DKind kind;
    char *value;      /* owned */
    u32 line;         /* source line, for the failure message */
} Directive;

typedef struct Unit {
    char *name;                    /* path relative to tests/fletch      */
    char *entry;                   /* file actually handed to yew fl     */
    Directive dv[MAX_DIRECTIVES];
    size_t ndv;
    /* Flattened for convenience; the ordered CHECK stream still walks
     * dv[] so CHECK/CHECK_NEXT/CHECK_NOT keep their written order. */
    char *specs[MAX_SPECS];
    size_t nspecs;
    char *covers[MAX_COVERS];
    size_t ncovers;
    const char *caps;
    const char *origin;
    const char *args;
    const char *skip;
    bool cfg_error;          /* reported at parse time; never run */
    const char *xfail_id;
    const char *xfail_why;
    const char *error_kind;
    long error_line;
    int exit_code;
    bool has_exit;
    bool wants_error;
    bool gc_optout;
    int timeout;
} Unit;

static DKind d_lookup(const char *name, size_t n, bool *found)
{
    size_t i;

    for (i = 0U; i < (size_t)D_KIND__N; i++) {
        if (strlen(d_names[i]) == n && strncmp(name, d_names[i], n) == 0) {
            *found = true;
            return (DKind)i;
        }
    }
    *found = false;
    return D_SPEC;
}

/*
 * One comment's text -> a directive, or nothing.
 *
 * `# ALL_CAPS:` that is not in the table is a configuration error; a
 * comment that does not look like a directive at all is prose.
 */
static bool comment_to_directive(const char *text, size_t len, u32 line,
                                 Directive *out, char **cfg_err)
{
    size_t i = 0U;
    size_t start;
    size_t nlen;
    bool found;
    DKind k;

    if (len == 0U || text[0] != '#')
        return false;
    i = 1U;
    while (i < len && (text[i] == ' ' || text[i] == '\t'))
        i++;
    start = i;
    while (i < len && ((text[i] >= 'A' && text[i] <= 'Z') ||
                       text[i] == '_' ||
                       (text[i] >= '0' && text[i] <= '9')))
        i++;
    nlen = i - start;
    /* A single capital is prose ("# A note"), not a mistyped directive. */
    if (nlen < 2U)
        return false;
    while (i < len && (text[i] == ' ' || text[i] == '\t'))
        i++;
    if (i >= len || text[i] != ':')
        return false;
    i++;
    k = d_lookup(text + start, nlen, &found);
    if (!found) {
        char *msg = malloc(nlen + 128U);

        if (msg == NULL)
            die("out of memory");
        (void)snprintf(msg, nlen + 128U,
                       "line %lu: unknown directive '%.*s' -- an ALL_CAPS "
                       "comment must be a directive or it is a test that "
                       "asserts nothing",
                       (unsigned long)line, (int)nlen, text + start);
        *cfg_err = msg;
        return false;
    }
    {
        size_t vlen = len - i;
        char *v = malloc(vlen + 1U);

        if (v == NULL)
            die("out of memory");
        if (vlen != 0U)
            (void)memcpy(v, text + i, vlen);
        v[vlen] = '\0';
        out->kind = k;
        out->value = trim(v);
        out->line = line;
        /* trim() may have moved the pointer forward; the original block
         * is still reachable through it because trim only advances. */
    }
    return true;
}

/*
 * Resolve a comment token's bytes from its span.
 *
 * FlSpan carries {line, col, len} and NOT byte offsets, and col is a
 * 1-based BYTE column, so walking to the line and adding col-1 is
 * exact rather than an approximation.
 */
static const char *span_text(const char *src, FlSpan sp, size_t *len)
{
    u32 line = 1U;
    const char *p = src;

    while (line < sp.line && *p != '\0') {
        if (*p == '\n')
            line++;
        p++;
    }
    p += sp.col - 1U;
    *len = sp.len;
    return p;
}

/* Collects every comment token, ignoring diagnostics: a file that does
 * not lex is still allowed to carry `# ERROR:` describing exactly that. */
static bool scan_directives(Unit *u, const char *src, size_t len,
                            char **cfg_err)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlLexer lx;
    bool ok = true;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_diag_add_file(&dc, u->entry, src, len);
    fl_lex_init(&lx, &arena, &dc, &in, src, len, 0U);
    lx.keep_comments = true;
    for (;;) {
        FlTok t = fl_lex_next(&lx);
        size_t tlen;
        const char *text;
        Directive d;

        if (t.kind == FL_T_EOF)
            break;
        if (t.kind != FL_T_COMMENT)
            continue;
        text = span_text(src, t.sp, &tlen);
        if (comment_to_directive(text, tlen, t.sp.line, &d, cfg_err)) {
            if (u->ndv == MAX_DIRECTIVES)
                die("%s: more than %d directives", u->name, MAX_DIRECTIVES);
            u->dv[u->ndv++] = d;
        }
        if (*cfg_err != NULL) {
            ok = false;
            break;
        }
    }
    interner_free(&in);
    arena_free_all(&arena);
    return ok;
}

/* Splits a COVERS value on whitespace. */
static void add_covers(Unit *u, const char *v)
{
    while (*v != '\0') {
        const char *s;
        size_t n;
        char *tok;

        while (*v == ' ' || *v == '\t')
            v++;
        if (*v == '\0')
            break;
        s = v;
        while (*v != '\0' && *v != ' ' && *v != '\t')
            v++;
        n = (size_t)(v - s);
        if (u->ncovers == MAX_COVERS)
            die("%s: more than %d COVERS tokens", u->name, MAX_COVERS);
        tok = malloc(n + 1U);
        if (tok == NULL)
            die("out of memory");
        (void)memcpy(tok, s, n);
        tok[n] = '\0';
        u->covers[u->ncovers++] = tok;
    }
}

/* Splits a SPEC value on commas, normalising "§4.2" / "4.2" to "4.2". */
static void add_specs(Unit *u, const char *v)
{
    while (*v != '\0') {
        char buf[32];
        size_t n = 0U;

        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;
        /* The section marker is optional in the file and absent in the
         * ledger, so both spellings key the same section. */
        if ((unsigned char)v[0] == 0xC2U && (unsigned char)v[1] == 0xA7U)
            v += 2;
        while (*v != '\0' && *v != ',' && *v != ' ' && *v != '\t') {
            if (n + 1U < sizeof(buf))
                buf[n++] = *v;
            v++;
        }
        buf[n] = '\0';
        if (n == 0U)
            continue;
        if (u->nspecs == MAX_SPECS)
            die("%s: more than %d SPEC sections", u->name, MAX_SPECS);
        u->specs[u->nspecs] = malloc(n + 1U);
        if (u->specs[u->nspecs] == NULL)
            die("out of memory");
        (void)memcpy(u->specs[u->nspecs], buf, n + 1U);
        u->nspecs++;
    }
}

static bool finish_unit(Unit *u, char **cfg_err)
{
    size_t i;

    u->timeout = DEFAULT_TIMEOUT_SECS;
    u->error_line = -1;
    for (i = 0U; i < u->ndv; i++) {
        const Directive *d = &u->dv[i];

        switch (d->kind) {
        case D_SPEC:       add_specs(u, d->value); break;
        case D_COVERS:     add_covers(u, d->value); break;
        case D_CAPS:       u->caps = d->value; break;
        case D_ORIGIN:     u->origin = d->value; break;
        case D_ARGS:       u->args = d->value; break;
        case D_SKIP:       u->skip = d->value; break;
        case D_ERROR_KIND: u->error_kind = d->value; u->wants_error = true;
                           break;
        case D_ERROR:      u->wants_error = true; break;
        case D_ERROR_LINE: u->error_line = strtol(d->value, NULL, 10);
                           u->wants_error = true; break;
        case D_EXIT:       u->exit_code = (int)strtol(d->value, NULL, 10);
                           u->has_exit = true; break;
        case D_TIMEOUT:    u->timeout = (int)strtol(d->value, NULL, 10);
                           break;
        case D_GC_STRESS:
            /* "requires a trailing reason" -- an opt-out with no stated
             * reason is how the hatch becomes the norm. */
            if (strncmp(d->value, "0", 1U) != 0) {
                *cfg_err = dupe("GC_STRESS takes only 0, with a reason");
                return false;
            }
            if (strlen(trim(d->value + 1)) == 0U) {
                *cfg_err = dupe("GC_STRESS: 0 requires a trailing reason");
                return false;
            }
            u->gc_optout = true;
            break;
        case D_XFAIL:
            /*
             * `# XFAIL: <XF-id> <reason>` -- the ID is the first token
             * and the rest is prose for a human.  Matching the whole
             * line against the debt list makes every id "unknown" the
             * moment anyone writes the reason the directive asks for.
             */
            {
                char *sp = strchr(d->value, ' ');

                if (sp != NULL)
                    *sp = '\0';
                if (d->value[0] == '\0') {
                    *cfg_err = dupe("XFAIL needs an id and a reason");
                    return false;
                }
                u->xfail_id = d->value;
                u->xfail_why = sp == NULL ? "" : trim(sp + 1);
            }
            break;
        default: break;
        }
    }
    if (u->nspecs == 0U) {
        *cfg_err = dupe("no '# SPEC:' directive -- every conformance file "
                          "must name the section it covers");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* XFAIL ledger                                                     */
/* ---------------------------------------------------------------- */

/*
 * TRACKED, unlike the .docs/audits/xfail-debt.md the sprint names:
 * .docs/ is gitignored by project decision, so a gate that reads an ID
 * list from there passes locally and fails in CI on the first XFAIL.
 * Recorded as correction C1 in the sprint file.
 */
static char *g_xfail_src;

static void xfail_load(void)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/xfail-debt.txt", g_root);
    g_xfail_src = slurp(path, NULL);
}

static bool xfail_known(const char *id)
{
    const char *p = g_xfail_src;
    size_t n = strlen(id);

    if (p == NULL)
        return false;
    while ((p = strstr(p, id)) != NULL) {
        bool left = p == g_xfail_src || p[-1] == '\n';
        char r = p[n];

        if (left && (r == ' ' || r == '\t' || r == '\n' || r == '\0'))
            return true;
        p += n;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* Running a unit                                                   */
/* ---------------------------------------------------------------- */

typedef struct RunOut {
    Bytebuf out;
    Bytebuf err;
    int status;        /* exit code, or -1 */
    bool timed_out;
    bool signalled;
    int sig;
} RunOut;

static i64 now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Splits ARGS on spaces into argv slots.  Quoting is deliberately not
 * supported: a conformance flag with a space in it would be a sign the
 * case belongs in its own file. */
static size_t split_args(char *buf, char **out, size_t cap)
{
    size_t n = 0U;

    while (*buf != '\0') {
        while (*buf == ' ' || *buf == '\t')
            *buf++ = '\0';
        if (*buf == '\0')
            break;
        if (n == cap)
            die("too many ARGS tokens");
        out[n++] = buf;
        while (*buf != '\0' && *buf != ' ' && *buf != '\t')
            buf++;
    }
    return n;
}

static void run_unit(const Unit *u, RunOut *r)
{
    int op[2];
    int ep[2];
    pid_t pid;
    char *argv[32];
    size_t na = 0U;
    char argbuf[512];
    i64 deadline;

    bytebuf_init(&r->out);
    bytebuf_init(&r->err);
    r->status = -1;
    r->timed_out = false;
    r->signalled = false;
    r->sig = 0;

    if (pipe(op) != 0 || pipe(ep) != 0)
        die("pipe: %s", strerror(errno));

    argv[na++] = dupe(g_yew);
    argv[na++] = dupe("fl");
    if (u->caps != NULL) {
        argv[na++] = dupe("--caps");
        argv[na++] = dupe(u->caps);
    }
    if (u->origin != NULL) {
        argv[na++] = dupe("--origin");
        argv[na++] = dupe(u->origin);
    }
    if (u->args != NULL) {
        /*
         * COPIED, like every other argv element the parent owns.
         *
         * split_args points into `argbuf`, which is on this frame --
         * handing those pointers to the free loop below is a free() of
         * stack memory, which is exactly the "double free or
         * corruption" the first run of the only `# ARGS:` case
         * produced.  Every slot before u->entry is now heap and owned.
         */
        char *tok[8];
        size_t nt;
        size_t k;

        (void)snprintf(argbuf, sizeof(argbuf), "%s", u->args);
        nt = split_args(argbuf, tok, YEW_ARRAY_LEN(tok));
        for (k = 0U; k < nt; k++)
            argv[na++] = dupe(tok[k]);
    }
    if (u->entry != NULL)
        argv[na++] = u->entry;
    argv[na] = NULL;

    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        (void)close(op[0]);
        (void)close(ep[0]);
        (void)dup2(op[1], 1);
        (void)dup2(ep[1], 2);
        (void)close(op[1]);
        (void)close(ep[1]);
        /* stdin is /dev/null: `yew fl FILE` must not read it, and a
         * runner that leaves the terminal attached would hand the child
         * a tty and start a REPL. */
        {
            int devnull = open("/dev/null", O_RDONLY);

            if (devnull >= 0) {
                (void)dup2(devnull, 0);
                (void)close(devnull);
            }
        }
        (void)execv(g_yew, argv);
        _exit(127);
    }
    (void)close(op[1]);
    (void)close(ep[1]);
    deadline = now_ms() + (i64)u->timeout * 1000;
    {
        struct pollfd pfd[2];
        bool done[2] = { false, false };

        pfd[0].fd = op[0];
        pfd[1].fd = ep[0];
        while (!done[0] || !done[1]) {
            i64 left = deadline - now_ms();
            int nready;
            int i;

            if (left <= 0) {
                r->timed_out = true;
                (void)kill(pid, SIGKILL);
                break;
            }
            pfd[0].events = done[0] ? 0 : POLLIN;
            pfd[1].events = done[1] ? 0 : POLLIN;
            pfd[0].revents = 0;
            pfd[1].revents = 0;
            nready = poll(pfd, 2U, (int)(left > 1000 ? 1000 : left));
            if (nready < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            for (i = 0; i < 2; i++) {
                char buf[8192];
                ssize_t got;

                if (done[i] || pfd[i].revents == 0)
                    continue;
                got = read(pfd[i].fd, buf, sizeof(buf));
                if (got > 0)
                    bytebuf_append(i == 0 ? &r->out : &r->err, buf,
                                   (size_t)got);
                else
                    done[i] = true;
            }
        }
    }
    (void)close(op[0]);
    (void)close(ep[0]);
    /*
     * The argv strings, freed on the PARENT side only.
     *
     * `u->entry` is the unit's and is not ours; everything before it
     * came from dupe() for execv's sake, and 36 units x half a dozen
     * arguments is what LeakSanitizer reported on the first sanitize
     * run after this file landed.
     */
    {
        size_t k;

        /*
         * `k < na`, not `k + 1U < na`.  The shorter bound assumed the
         * last slot is always u->entry, which is not ours -- but the
         * --list-natives probe passes entry == NULL, so its last slot
         * is an owned flag and leaked.  Comparing against u->entry is
         * the actual rule; the index is not.
         */
        for (k = 0U; k < na; k++) {
            if (argv[k] != u->entry)
                free(argv[k]);
        }
    }
    {
        int st = 0;

        (void)waitpid(pid, &st, 0);
        if (WIFEXITED(st)) {
            r->status = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
            r->signalled = true;
            r->sig = WTERMSIG(st);
        }
    }
}

/* ---------------------------------------------------------------- */
/* Checking                                                         */
/* ---------------------------------------------------------------- */

typedef struct Lines {
    char **v;
    size_t n;
    char *buf;
} Lines;

static void lines_split(Lines *l, const Bytebuf *bb)
{
    size_t cap = 16U;
    size_t i;
    size_t start = 0U;

    l->buf = malloc(bb->len + 1U);
    if (l->buf == NULL)
        die("out of memory");
    if (bb->len != 0U)
        (void)memcpy(l->buf, bb->data, bb->len);
    l->buf[bb->len] = '\0';
    l->v = malloc(cap * sizeof(*l->v));
    if (l->v == NULL)
        die("out of memory");
    l->n = 0U;
    for (i = 0U; i <= bb->len; i++) {
        if (i == bb->len || l->buf[i] == '\n') {
            if (i == bb->len && i == start)
                break;               /* no phantom line after a final \n */
            l->buf[i] = '\0';
            if (l->n == cap) {
                cap *= 2U;
                l->v = realloc(l->v, cap * sizeof(*l->v));
                if (l->v == NULL)
                    die("out of memory");
            }
            l->v[l->n++] = l->buf + start;
            start = i + 1U;
        }
    }
}

static void lines_free(Lines *l)
{
    free(l->v);
    free(l->buf);
}

/* The failure text for one unit; empty means it passed. */
static void check_unit(const Unit *u, const RunOut *r, Bytebuf *why)
{
    Lines out;
    size_t cursor = 0U;
    size_t i;
    /* CHECK_NOTs seen since the last positive match, applied to the
     * window that ends at the next one (or at EOF). */
    const Directive *nots[MAX_DIRECTIVES];
    size_t nnots = 0U;
    int want_exit;

    lines_split(&out, &r->out);

    if (r->timed_out) {
        bytebuf_printf(why, "timed out after %ds\n", u->timeout);
        lines_free(&out);
        return;
    }
    if (r->signalled) {
        bytebuf_printf(why, "killed by signal %d\n", r->sig);
        lines_free(&out);
        return;
    }

    for (i = 0U; i < u->ndv; i++) {
        const Directive *d = &u->dv[i];
        size_t at;
        bool hit = false;

        if (d->kind == D_CHECK_NOT) {
            nots[nnots++] = d;
            continue;
        }
        if (d->kind != D_CHECK && d->kind != D_CHECK_NEXT &&
            d->kind != D_OUT)
            continue;

        if (d->kind == D_CHECK_NEXT) {
            if (cursor < out.n && strstr(out.v[cursor], d->value) != NULL) {
                at = cursor;
                hit = true;
            }
        } else {
            for (at = cursor; at < out.n; at++) {
                bool m = d->kind == D_OUT
                             ? strcmp(out.v[at], d->value) == 0
                             : strstr(out.v[at], d->value) != NULL;

                if (m) {
                    hit = true;
                    break;
                }
            }
        }
        if (!hit) {
            bytebuf_printf(why, "line %lu: %s not satisfied: |%s|\n",
                           (unsigned long)d->line, d_names[d->kind],
                           d->value);
            if (d->kind == D_CHECK_NEXT)
                bytebuf_printf(why, "  the line after the previous match "
                                    "was |%s|\n",
                               cursor < out.n ? out.v[cursor] : "<eof>");
            break;
        }
        /* The window a pending CHECK_NOT owns closes at this match. */
        {
            size_t k;
            size_t j;
            bool bad = false;

            for (k = 0U; k < nnots && !bad; k++) {
                for (j = cursor; j < at; j++) {
                    if (strstr(out.v[j], nots[k]->value) != NULL) {
                        bytebuf_printf(why,
                                       "line %lu: CHECK_NOT fired: |%s| "
                                       "appeared on |%s|\n",
                                       (unsigned long)nots[k]->line,
                                       nots[k]->value, out.v[j]);
                        bad = true;
                        break;
                    }
                }
            }
            nnots = 0U;
            if (bad)
                break;
        }
        cursor = at + 1U;
    }
    /* Trailing CHECK_NOTs run to end of output. */
    if (why->len == 0U && nnots != 0U) {
        size_t k;
        size_t j;

        for (k = 0U; k < nnots; k++) {
            for (j = cursor; j < out.n; j++) {
                if (strstr(out.v[j], nots[k]->value) != NULL) {
                    bytebuf_printf(why,
                                   "line %lu: CHECK_NOT fired: |%s| "
                                   "appeared on |%s|\n",
                                   (unsigned long)nots[k]->line,
                                   nots[k]->value, out.v[j]);
                    break;
                }
            }
        }
    }

    /* stderr assertions. */
    for (i = 0U; i < u->ndv && why->len == 0U; i++) {
        const Directive *d = &u->dv[i];

        if (d->kind != D_ERROR)
            continue;
        if (r->err.len == 0U ||
            find_bytes((const char *)r->err.data, r->err.len,
                       d->value) == NULL)
            bytebuf_printf(why, "line %lu: ERROR not found on stderr: |%s|\n",
                           (unsigned long)d->line, d->value);
    }
    if (why->len == 0U && u->error_kind != NULL) {
        /* `error: KIND: msg` -- the first line of fl_trace_render. */
        char want[128];
        size_t n;

        n = (size_t)snprintf(want, sizeof(want), "error: %s: ", u->error_kind);
        if (r->err.len < n ||
            find_bytes((const char *)r->err.data, r->err.len,
                       want) == NULL) {
            Lines el;

            lines_split(&el, &r->err);
            bytebuf_printf(why, "ERROR_KIND: wanted |%s|, stderr said |%s|\n",
                           u->error_kind, el.n != 0U ? el.v[0] : "<nothing>");
            lines_free(&el);
        }
    }
    if (why->len == 0U && u->error_line >= 0) {
        /* The innermost frame is the FIRST `  at ` line. */
        Lines el;
        long got = -1;

        lines_split(&el, &r->err);
        for (i = 0U; i < el.n; i++) {
            const char *p = strstr(el.v[i], "  at ");
            const char *open;

            if (p != el.v[i])
                continue;
            open = strrchr(el.v[i], '(');
            if (open == NULL)
                continue;
            {
                const char *colon = strchr(open, ':');

                if (colon != NULL)
                    got = strtol(colon + 1, NULL, 10);
            }
            break;
        }
        if (got != u->error_line)
            bytebuf_printf(why, "ERROR_LINE: wanted %ld, innermost frame "
                                "was %ld\n", u->error_line, got);
        lines_free(&el);
    }

    /* Exit code last: a wrong code with satisfied CHECKs is a different
     * bug from output that never appeared, and reporting the output
     * failure first is what points at the cause. */
    want_exit = u->has_exit ? u->exit_code : (u->wants_error ? -1 : 0);
    if (why->len == 0U) {
        if (want_exit >= 0) {
            if (r->status != want_exit)
                bytebuf_printf(why, "exit %d, wanted %d\n", r->status,
                               want_exit);
        } else if (r->status == 0) {
            bytebuf_printf(why, "exit 0, but the file asserts an error\n");
        }
    }
    lines_free(&out);
}

/* ---------------------------------------------------------------- */
/* Discovery                                                        */
/* ---------------------------------------------------------------- */

static Unit g_units[MAX_UNITS];
static size_t g_nunits;

static char *joinp(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2U;
    char *s = malloc(n);

    if (s == NULL)
        die("out of memory");
    (void)snprintf(s, n, "%s/%s", a, b);
    return s;
}

/*
 * A directory's ENTRY POINTS: `main.fl`, or every `*_main.fl`.
 *
 * The sprint says "a directory is one test, entered at main.fl (or the
 * single *_main.fl)".  Several `*_main.fl` are allowed here because
 * 13-capability/ needs two scenarios -- a config origin and a plugin
 * origin -- over one set of helpers, and splitting them into sibling
 * directories would duplicate helper.fl to no purpose.
 *
 * The other half of the rule matters more: when a directory HAS entry
 * points, nothing else in it is a unit.  Without that, helper.fl and
 * lib/util.fl are collected as tests of their own, and a support file
 * with no `# SPEC:` is reported as a configuration error -- which is
 * the right answer for a conformance file and the wrong one for a
 * module that exists to be imported.
 */
static u32 dir_entries(const char *dir, char **out, u32 cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[512];
    u32 nn = 0U;
    u32 n = 0U;
    u32 i;
    bool has_main = false;

    if (d == NULL)
        return 0U;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (e->d_name[0] == '.' || len < 4U ||
            strcmp(e->d_name + len - 3U, ".fl") != 0)
            continue;
        if (nn == (u32)YEW_ARRAY_LEN(names))
            die("%s: too many .fl files", dir);
        names[nn++] = dupe(e->d_name);
    }
    (void)closedir(d);
    yew_sort_stable(names, nn, sizeof(names[0]), cmp_str, NULL);

    for (i = 0U; i < nn; i++) {
        if (strcmp(names[i], "main.fl") == 0)
            has_main = true;
    }
    for (i = 0U; i < nn; i++) {
        size_t len = strlen(names[i]);
        bool is_entry = has_main
                            ? strcmp(names[i], "main.fl") == 0
                            : (len > 8U &&
                               strcmp(names[i] + len - 8U, "_main.fl") == 0);

        if (is_entry && n < cap)
            out[n++] = joinp(dir, names[i]);
        free(names[i]);
    }
    return n;
}

static void collect(const char *dir, const char *relbase)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[512];
    size_t nn = 0U;
    size_t i;

    if (d == NULL)
        die("cannot read %s: %s", dir, strerror(errno));
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (nn == YEW_ARRAY_LEN(names))
            die("%s: too many entries", dir);
        names[nn++] = dupe(e->d_name);
    }
    (void)closedir(d);
    yew_sort_stable(names, nn, sizeof(names[0]), cmp_str, NULL);

    for (i = 0U; i < nn; i++) {
        char *full;
        char *rel;

        /*
         * meta/ holds cases that are SUPPOSED to fail -- they are the
         * runner's own tests -- and roundtrip/ is Sprint 35's empty
         * placeholder.  Both are reached with an explicit --root, and
         * collecting them here would make the real suite red by
         * design.
         */
        if (relbase[0] == '\0' && (strcmp(names[i], "meta") == 0 ||
                                   strcmp(names[i], "roundtrip") == 0)) {
            names[i][0] = '\0';      /* freed with the rest, below */
            continue;
        }
        full = joinp(dir, names[i]);
        rel = relbase[0] == '\0' ? dupe(names[i])
                                  : joinp(relbase, names[i]);

        if (is_dir(full)) {
            char *entries[16];
            u32 ne = dir_entries(full, entries, (u32)YEW_ARRAY_LEN(entries));
            u32 e;

            if (ne == 1U) {
                /* One entry point: the DIRECTORY is the test's name. */
                if (g_nunits == MAX_UNITS)
                    die("more than %d units", MAX_UNITS);
                g_units[g_nunits].name = rel;
                g_units[g_nunits].entry = entries[0];
                g_nunits++;
                free(full);
                continue;
            }
            if (ne > 1U) {
                /* Several scenarios share the directory's helpers, so
                 * each is named for its own entry file. */
                for (e = 0U; e < ne; e++) {
                    const char *base = strrchr(entries[e], '/');

                    if (g_nunits == MAX_UNITS)
                        die("more than %d units", MAX_UNITS);
                    g_units[g_nunits].name =
                        joinp(rel, base == NULL ? entries[e] : base + 1);
                    g_units[g_nunits].entry = entries[e];
                    g_nunits++;
                }
                free(rel);
                free(full);
                continue;
            }
            /* No entry point: a plain subtree of cases (errors/). */
            collect(full, rel);
            free(rel);
            free(full);
            continue;
        }
        {
            size_t n = strlen(names[i]);

            if (n > 3U && strcmp(names[i] + n - 3U, ".fl") == 0) {
                if (g_nunits == MAX_UNITS)
                    die("more than %d units", MAX_UNITS);
                g_units[g_nunits].name = rel;
                g_units[g_nunits].entry = full;
                g_nunits++;
                continue;
            }
        }
        free(rel);
        free(full);
    }
    /* The sorted directory listing is ours; the units took copies. */
    for (i = 0U; i < nn; i++)
        free(names[i]);
}

/* ---------------------------------------------------------------- */
/* Ledger                                                           */
/* ---------------------------------------------------------------- */

typedef struct Section {
    char num[8];
    char title[64];
} Section;

static Section g_sections[32];
static size_t g_nsections;

static void spec_sections(void)
{
    char *p = g_spec_src;

    while ((p = strstr(p, "\n## ")) != NULL) {
        char *line = p + 4;
        char *nl = strchr(line, '\n');
        size_t i = 0U;
        size_t t = 0U;

        p = line;
        if (nl == NULL)
            break;
        /* "## §4 Values and types" */
        if ((unsigned char)line[0] != 0xC2U || (unsigned char)line[1] != 0xA7U)
            continue;
        line += 2;
        while (line < nl && *line != ' ' && i + 1U < sizeof(g_sections[0].num))
            g_sections[g_nsections].num[i++] = *line++;
        g_sections[g_nsections].num[i] = '\0';
        while (line < nl && *line == ' ')
            line++;
        while (line < nl && t + 1U < sizeof(g_sections[0].title))
            g_sections[g_nsections].title[t++] = *line++;
        g_sections[g_nsections].title[t] = '\0';
        if (g_nsections + 1U == YEW_ARRAY_LEN(g_sections))
            die("too many spec sections");
        g_nsections++;
    }
}

/*
 * A sorted, deduplicated set of strings.
 *
 * The gate's whole job is set difference -- "every native appears in
 * some COVERS" -- and doing that with nested scans over 117 natives
 * and every unit's tokens was both O(n^2) and the kind of code that
 * quietly gets a bound wrong.
 */
typedef struct StrSet {
    char **v;
    size_t n;
    size_t cap;
    bool sorted;
} StrSet;

static void set_init(StrSet *s)
{
    (void)memset(s, 0, sizeof(*s));
}

static void set_add(StrSet *s, const char *tok)
{
    if (s->n == s->cap) {
        size_t cap = s->cap == 0U ? 32U : s->cap * 2U;
        char **v = realloc(s->v, cap * sizeof(*v));

        if (v == NULL)
            die("out of memory");
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = dupe(tok);
    s->sorted = false;
}

static void set_seal(StrSet *s)
{
    size_t i;
    size_t w = 0U;

    if (s->sorted)
        return;
    yew_sort_stable(s->v, s->n, sizeof(s->v[0]), cmp_str, NULL);
    for (i = 0U; i < s->n; i++) {
        if (i != 0U && strcmp(s->v[i], s->v[w - 1U]) == 0) {
            free(s->v[i]);
            continue;
        }
        s->v[w++] = s->v[i];
    }
    s->n = w;
    s->sorted = true;
}

static bool set_has(StrSet *s, const char *tok)
{
    size_t lo = 0U;
    size_t hi;

    set_seal(s);
    hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        int c = strcmp(s->v[mid], tok);

        if (c == 0)
            return true;
        if (c < 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return false;
}

static void set_free(StrSet *s)
{
    size_t i;

    for (i = 0U; i < s->n; i++)
        free(s->v[i]);
    free(s->v);
    set_init(s);
}

/*
 * Every COVERS token in the suite, split by shape.
 *
 * The shapes are the sprint's: `kind:` and `op:` are prefixed, and the
 * two bare shapes are told apart by the DOT.  Every one of the 117
 * natives is `module.fn` and every EBNF nonterminal is [a-z_]+, so the
 * dot is exact here rather than a heuristic -- and check 3 would catch
 * it immediately if that ever stopped being true.
 */
static StrSet g_cov_prod;
static StrSet g_cov_native;
static StrSet g_cov_kind;
static StrSet g_cov_op;
static StrSet g_seen_kinds;      /* from ERROR_KIND directives */

static void classify_covers(void)
{
    size_t i;
    size_t j;

    set_init(&g_cov_prod);
    set_init(&g_cov_native);
    set_init(&g_cov_kind);
    set_init(&g_cov_op);
    set_init(&g_seen_kinds);
    for (i = 0U; i < g_nunits; i++) {
        for (j = 0U; j < g_units[i].ncovers; j++) {
            const char *t = g_units[i].covers[j];

            if (strncmp(t, "kind:", 5U) == 0)
                set_add(&g_cov_kind, t + 5);
            else if (strncmp(t, "op:", 3U) == 0)
                set_add(&g_cov_op, t + 3);
            else if (strchr(t, '.') != NULL)
                set_add(&g_cov_native, t);
            else
                set_add(&g_cov_prod, t);
        }
        if (g_units[i].error_kind != NULL)
            set_add(&g_seen_kinds, g_units[i].error_kind);
    }
    set_seal(&g_cov_prod);
    set_seal(&g_cov_native);
    set_seal(&g_cov_kind);
    set_seal(&g_cov_op);
    set_seal(&g_seen_kinds);
}

/* ---------------------------------------------------------------- */
/* Sources of truth                                                 */
/* ---------------------------------------------------------------- */

static void spec_load(void)
{
    g_spec_src = slurp(g_spec, NULL);
    if (g_spec_src == NULL)
        die("cannot read %s -- the spec is the gate's source of truth",
            g_spec);
}

static void scan_ebnf_block(const char *p, const char *end, StrSet *out);

/*
 * Every nonterminal the spec DEFINES, from every ```ebnf block:
 * `^name  = ...`.
 *
 * The sprint says "the §2 EBNF block", and §2's is the big one -- but
 * §12 defines five more (`pl_file` and friends) in a block of its
 * own, and they are nonterminals of the same language.  Counting the
 * numerator over all blocks and the denominator over one produced a
 * ledger reading `productions 42/37`, which is the kind of total that
 * makes a reader stop trusting the rest of the line.
 */
static void spec_productions(StrSet *out)
{
    const char *p = g_spec_src;

    set_init(out);
    if (strstr(g_spec_src, "```ebnf") == NULL)
        die("no ```ebnf block in %s", g_spec);
    while ((p = strstr(p, "```ebnf")) != NULL) {
        const char *end;

        p = strchr(p, '\n');
        end = p == NULL ? NULL : strstr(p, "\n```");
        if (p == NULL || end == NULL)
            die("unterminated ```ebnf block in %s", g_spec);
        scan_ebnf_block(p, end, out);
        p = end + 4;
    }
    set_seal(out);
}

static void scan_ebnf_block(const char *p, const char *end, StrSet *out)
{
    while (p < end) {
        const char *nl = strchr(p + 1, '\n');
        const char *q = p + 1;
        char name[64];
        size_t n = 0U;

        if (nl == NULL || nl > end)
            break;
        while (q < nl && ((*q >= 'a' && *q <= 'z') ||
                          (*q >= 'A' && *q <= 'Z') || *q == '_')) {
            if (n + 1U < sizeof(name))
                name[n++] = *q;
            q++;
        }
        name[n] = '\0';
        /* A definition, not a continuation: `name` then spaces then `=`. */
        while (q < nl && *q == ' ')
            q++;
        if (n != 0U && q < nl && *q == '=')
            set_add(out, name);
        p = nl;
    }
}

/* §9.1's closed kind set, read from the backticked list. */
static void spec_kinds(StrSet *out)
{
    const char *p = strstr(g_spec_src, "### 9.1");
    const char *end;

    set_init(out);
    if (p == NULL)
        die("no '### 9.1' heading in %s", g_spec);
    end = strstr(p, "**Count:");
    if (end == NULL)
        die("no '**Count:' line under §9.1 in %s", g_spec);
    while ((p = strchr(p, '"')) != NULL && p < end) {
        const char *q = strchr(p + 1, '"');
        char buf[64];
        size_t n;

        if (q == NULL || q > end)
            break;
        n = (size_t)(q - p - 1);
        if (n != 0U && n + 1U < sizeof(buf)) {
            (void)memcpy(buf, p + 1, n);
            buf[n] = '\0';
            set_add(out, buf);
        }
        p = q + 1;
    }
    set_seal(out);
}

/* Every `**Conformance:** \`path\`` target named by the spec. */
static void spec_conformance(StrSet *out)
{
    const char *p = g_spec_src;

    set_init(out);
    while ((p = strstr(p, "**Conformance:**")) != NULL) {
        const char *tick = strchr(p, '`');
        const char *end2;

        p += 16;
        if (tick == NULL)
            continue;
        end2 = strchr(tick + 1, '`');
        if (end2 == NULL)
            continue;
        {
            char buf[256];
            size_t n = (size_t)(end2 - tick - 1);

            if (n + 1U < sizeof(buf)) {
                (void)memcpy(buf, tick + 1, n);
                buf[n] = '\0';
                set_add(out, buf);
            }
        }
    }
    set_seal(out);
}

/* `yew fl --list-natives`, which is check 3's stated source of truth. */
static void list_natives(StrSet *out)
{
    Unit probe;
    RunOut r;
    Lines l;
    size_t i;

    (void)memset(&probe, 0, sizeof(probe));
    probe.name = (char *)"<natives>";
    probe.entry = NULL;
    probe.args = "--list-natives";
    probe.timeout = DEFAULT_TIMEOUT_SECS;
    set_init(out);
    run_unit(&probe, &r);
    if (r.status != 0)
        die("`yew fl --list-natives` exited %d", r.status);
    lines_split(&l, &r.out);
    for (i = 0U; i < l.n; i++) {
        if (l.v[i][0] != '\0')
            set_add(out, l.v[i]);
    }
    lines_free(&l);
    bytebuf_free(&r.out);
    bytebuf_free(&r.err);
    set_seal(out);
}

/* ---------------------------------------------------------------- */
/* The coverage gate                                                */
/* ---------------------------------------------------------------- */

/* Which files name section `num` in their SPEC. */
static size_t files_for_section(const char *num, Bytebuf *names)
{
    size_t i;
    size_t n = 0U;
    size_t nl = strlen(num);

    for (i = 0U; i < g_nunits; i++) {
        size_t j;

        for (j = 0U; j < g_units[i].nspecs; j++) {
            /* "4" matches §4, and so does "4.2": a file may name a
             * subsection without inventing a ledger row for it. */
            const char *sp = g_units[i].specs[j];

            if (strncmp(sp, num, nl) == 0 &&
                (sp[nl] == '\0' || sp[nl] == '.')) {
                if (names != NULL) {
                    if (n != 0U)
                        bytebuf_append(names, " ", 1U);
                    bytebuf_append(names, g_units[i].name,
                                   strlen(g_units[i].name));
                }
                n++;
                break;
            }
        }
    }
    return n;
}

static size_t covers_for_section(const char *num)
{
    size_t i;
    size_t total = 0U;
    size_t nl = strlen(num);

    for (i = 0U; i < g_nunits; i++) {
        size_t j;

        for (j = 0U; j < g_units[i].nspecs; j++) {
            const char *sp = g_units[i].specs[j];

            if (strncmp(sp, num, nl) == 0 &&
                (sp[nl] == '\0' || sp[nl] == '.')) {
                total += g_units[i].ncovers;
                break;
            }
        }
    }
    return total;
}

static void emit_ledger(FILE *f)
{
    StrSet prods;
    StrSet kinds;
    StrSet natives;
    size_t s;
    size_t covered = 0U;

    spec_productions(&prods);
    spec_kinds(&kinds);
    list_natives(&natives);

    (void)fputs("# GENERATED by tests/fletch/run.c --ledger -- do not edit\n",
                f);
    (void)fprintf(f, "%-3s | %-36s | %-46s | %s\n",
                  "S", "section title", "files", "covers");
    for (s = 0U; s < g_nsections; s++) {
        Bytebuf names;
        size_t n;

        bytebuf_init(&names);
        n = files_for_section(g_sections[s].num, &names);
        bytebuf_push_u8(&names, 0U);
        if (n != 0U)
            covered++;
        (void)fprintf(f, "%-3s | %-36s | %-46s | %lu\n", g_sections[s].num,
                      g_sections[s].title, (const char *)names.data,
                      (unsigned long)covers_for_section(g_sections[s].num));
        bytebuf_free(&names);
    }
    (void)fprintf(f,
                  "-- totals: sections %lu/%lu  productions %lu/%lu  "
                  "natives %lu/%lu  kinds %lu/%lu  opcodes %lu/%d\n",
                  (unsigned long)covered, (unsigned long)g_nsections,
                  (unsigned long)g_cov_prod.n, (unsigned long)prods.n,
                  (unsigned long)g_cov_native.n, (unsigned long)natives.n,
                  (unsigned long)g_cov_kind.n, (unsigned long)kinds.n,
                  (unsigned long)g_cov_op.n, (int)FL_OP__COUNT);
    set_free(&prods);
    set_free(&kinds);
    set_free(&natives);
}

/*
 * DoD 5: 14-example.fl must START WITH the spec's §14 fenced block,
 * byte for byte.
 *
 * A PREFIX rather than the whole file, because the same DoD also
 * requires the file to assert §14.1's normative results and a file
 * that is only the block asserts nothing.  Everything after the block
 * is this suite's; everything up to it belongs to the spec, and the
 * worked example therefore cannot rot into something the spec no
 * longer shows.
 */
static size_t check_example(void)
{
    const char *p = strstr(g_spec_src, "## \302\24714 Worked example");
    const char *open_fence;
    const char *body;
    const char *close_fence;
    char path[512];
    char *file;
    size_t len = 0U;
    size_t bad = 0U;

    if (p == NULL) {
        (void)printf("check 6: no '## \302\24714' heading in %s\n", g_spec);
        return 1U;
    }
    open_fence = strstr(p, "```fletch\n");
    if (open_fence == NULL) {
        (void)printf("check 6: \302\24714 has no ```fletch block in %s\n", g_spec);
        return 1U;
    }
    body = open_fence + strlen("```fletch\n");
    close_fence = strstr(body, "\n```");
    if (close_fence == NULL) {
        (void)printf("check 6: \302\24714's block is unterminated in %s\n", g_spec);
        return 1U;
    }
    (void)snprintf(path, sizeof(path), "%s/14-example.fl", g_root);
    file = slurp(path, &len);
    if (file == NULL) {
        (void)printf("check 6: %s does not exist\n", path);
        return 1U;
    }
    {
        size_t want = (size_t)(close_fence - body) + 1U;   /* keep the \n */

        if (len < want || memcmp(file, body, want) != 0) {
            (void)printf("check 6: %s does not start with the spec's "
                         "\302\24714 "
                         "block byte for byte (regenerate its prefix from "
                         "%s, or amend the spec -- do not edit only one)\n",
                         path, g_spec);
            bad = 1U;
        }
    }
    free(file);
    return bad;
}

/*
 * Checks 1-6.  Check 7 (the committed ledger is current) is the shell
 * wrapper's, because it is a git diff.
 *
 * Every failure names the missing item AND what to do about it: a gate
 * that says only "coverage incomplete" gets suppressed rather than
 * fixed.
 */
static size_t run_checks(void)
{
    StrSet prods;
    StrSet kinds;
    StrSet natives;
    StrSet conf;
    size_t bad = 0U;
    size_t i;

    spec_productions(&prods);
    spec_kinds(&kinds);
    list_natives(&natives);
    spec_conformance(&conf);

    for (i = 0U; i < g_nsections; i++) {
        if (files_for_section(g_sections[i].num, NULL) == 0U) {
            (void)printf("check 1: spec section %s '%s' has no test "
                         "(add '# SPEC: %s' to a file in %s/)\n",
                         g_sections[i].num, g_sections[i].title,
                         g_sections[i].num, g_root);
            bad++;
        }
    }
    for (i = 0U; i < prods.n; i++) {
        if (!set_has(&g_cov_prod, prods.v[i])) {
            (void)printf("check 2: grammar production '%s' has no COVERS "
                         "token (add it to %s/02-grammar.fl or the file "
                         "that exercises it)\n", prods.v[i], g_root);
            bad++;
        }
    }
    for (i = 0U; i < natives.n; i++) {
        if (!set_has(&g_cov_native, natives.v[i])) {
            (void)printf("check 3: native '%s' has no COVERS token "
                         "(add it to the file that calls it)\n",
                         natives.v[i]);
            bad++;
        }
    }
    for (i = 0U; i < kinds.n; i++) {
        if (!set_has(&g_seen_kinds, kinds.v[i])) {
            (void)printf("check 4: error kind '%s' has no '# ERROR_KIND: %s' "
                         "case (add one to %s/errors/)\n",
                         kinds.v[i], kinds.v[i], g_root);
            bad++;
        }
        if (!set_has(&g_cov_kind, kinds.v[i])) {
            (void)printf("check 4: error kind '%s' has no 'kind:%s' COVERS "
                         "token\n", kinds.v[i], kinds.v[i]);
            bad++;
        }
    }
    for (i = 0U; i < (size_t)FL_OP__COUNT; i++) {
        const char *name = fl_op_name((FlOp)i);

        if (!set_has(&g_cov_op, name)) {
            (void)printf("check 5: opcode '%s' has no 'op:%s' COVERS token "
                         "(add it to the file whose program emits it)\n",
                         name, name);
            bad++;
        }
    }
    for (i = 0U; i < conf.n; i++) {
        /* The spec names paths from the repo root. */
        char alt[512];

        if (is_file(conf.v[i]) || is_dir(conf.v[i]))
            continue;
        /* A section covered by several programs gets a DIRECTORY, and
         * the spec's line still names `<n>-<name>.fl`; the directory is
         * the same stem.  Accept it rather than editing a frozen spec. */
        {
            size_t n = strlen(conf.v[i]);

            if (n > 3U && n + 1U < sizeof(alt)) {
                (void)memcpy(alt, conf.v[i], n - 3U);
                alt[n - 3U] = '\0';
                if (is_dir(alt))
                    continue;
            }
        }
        (void)printf("check 6: spec names '%s', which does not exist "
                     "(create it, or the '**Conformance:**' line is a lie)\n",
                     conf.v[i]);
        bad++;
    }

    bad += check_example();

    set_free(&prods);
    set_free(&kinds);
    set_free(&natives);
    set_free(&conf);
    return bad;
}

/* ---------------------------------------------------------------- */
/* main                                                             */
/* ---------------------------------------------------------------- */

static bool env_on(const char *name)
{
    const char *v = getenv(name);

    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

/*
 * Everything main() owns, released so the sanitize lane can say
 * something useful.
 *
 * A one-shot tool could exit and let the kernel do it -- but then
 * LeakSanitizer reports 121 allocations on every run and a REAL leak
 * introduced later arrives in a list nobody reads.  The teardown is
 * cheap and it keeps the signal.
 */
static void release_all(void)
{
    size_t i;
    size_t j;

    for (i = 0U; i < g_nunits; i++) {
        Unit *u = &g_units[i];

        for (j = 0U; j < u->ndv; j++)
            free(u->dv[j].value);
        for (j = 0U; j < u->nspecs; j++)
            free(u->specs[j]);
        for (j = 0U; j < u->ncovers; j++)
            free(u->covers[j]);
        free(u->name);
        free(u->entry);
    }
    set_free(&g_cov_prod);
    set_free(&g_cov_native);
    set_free(&g_cov_kind);
    set_free(&g_cov_op);
    set_free(&g_seen_kinds);
    free(g_spec_src);
    free(g_xfail_src);
}

int main(int argc, char **argv)
{
    bool ledger_only = false;
    bool check_only = false;
    const char *filter = getenv("YEW_FLETCH_FILTER");
    bool gc_stress = env_on("YEW_FL_GC_STRESS") || env_on("FL_GC_STRESS");
    size_t i;
    size_t npass = 0U;
    size_t nfail = 0U;
    size_t nskip = 0U;
    size_t nxfail = 0U;
    size_t ncfg = 0U;
    size_t noptout = 0U;
    int argi;

    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--ledger") == 0) {
            ledger_only = true;
        } else if (strcmp(argv[argi], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[argi], "--yew") == 0 && argi + 1 < argc) {
            g_yew = argv[++argi];
        } else if (strcmp(argv[argi], "--root") == 0 && argi + 1 < argc) {
            g_root = argv[++argi];
        } else if (strcmp(argv[argi], "--spec") == 0 && argi + 1 < argc) {
            g_spec = argv[++argi];
        } else {
            die("usage: run [--ledger|--check] [--yew P] [--root D] "
                "[--spec F]");
        }
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("cannot ignore SIGPIPE: %s", strerror(errno));
    if (!is_dir(g_root))
        die("no such directory: %s", g_root);
    if (!is_file(g_yew))
        die("no such binary: %s (build it first)", g_yew);

    xfail_load();
    spec_load();
    spec_sections();
    collect(g_root, "");

    /* Parse every unit's directives before running any, so a
     * configuration error is reported even in --ledger mode. */
    for (i = 0U; i < g_nunits; i++) {
        Unit *u = &g_units[i];
        char *cfg = NULL;
        size_t len = 0U;
        char *src = slurp(u->entry, &len);

        if (src == NULL)
            die("cannot read %s", u->entry);
        (void)scan_directives(u, src, len, &cfg);
        if (cfg == NULL)
            (void)finish_unit(u, &cfg);
        if (cfg != NULL) {
            (void)printf("CONFIG %s: %s\n", u->name, cfg);
            ncfg++;
            free(cfg);
            u->cfg_error = true;
        }
        if (u->xfail_id != NULL && !xfail_known(u->xfail_id)) {
            /* An unknown id is a CONFIGURATION ERROR, not a skip: the
             * debt list is the register of what is allowed to fail,
             * and an id that is not in it is a typo or a stale
             * entry -- either way nobody reviewed this failure. */
            (void)printf("CONFIG %s: unknown XFAIL id '%s' (add it to "
                         "%s/xfail-debt.txt)\n", u->name, u->xfail_id,
                         g_root);
            ncfg++;
            u->cfg_error = true;
        }
        free(src);
    }

    classify_covers();
    if (ledger_only) {
        emit_ledger(stdout);
        release_all();
        return ncfg == 0U ? 0 : 1;
    }
    if (check_only) {
        size_t bad = run_checks();

        if (bad != 0U)
            (void)printf("fletch: %lu coverage gaps\n", (unsigned long)bad);
        release_all();
        return bad == 0U && ncfg == 0U ? 0 : 1;
    }

    for (i = 0U; i < g_nunits; i++) {
        Unit *u = &g_units[i];
        RunOut r;
        Bytebuf why;

        if (filter != NULL && filter[0] != '\0' &&
            strstr(u->name, filter) == NULL)
            continue;
        if (u->cfg_error)
            continue;              /* already reported, and not a skip */
        if (u->skip != NULL) {
            /* s01's discipline: a skip is printed, never silent. */
            (void)printf("HARNESS_SKIP %s: %s\n", u->name, u->skip);
            nskip++;
            continue;
        }
        if (gc_stress && u->gc_optout) {
            noptout++;
            (void)printf("HARNESS_SKIP %s: GC_STRESS opt-out\n", u->name);
            nskip++;
            continue;
        }
        run_unit(u, &r);
        bytebuf_init(&why);
        check_unit(u, &r, &why);
        if (u->xfail_id != NULL) {
            if (why.len == 0U) {
                (void)printf("XPASS %s: %s no longer fails -- remove the "
                             "XFAIL\n", u->name, u->xfail_id);
                nfail++;
            } else {
                (void)printf("XFAIL %s: %s\n", u->name, u->xfail_id);
                nxfail++;
            }
        } else if (why.len == 0U) {
            (void)printf("PASS %s\n", u->name);
            npass++;
        } else {
            (void)printf("FAIL %s\n", u->name);
            (void)fwrite(why.data, 1U, why.len, stdout);
            if (r.err.len != 0U) {
                (void)fputs("  --- stderr ---\n", stdout);
                (void)fwrite(r.err.data, 1U, r.err.len, stdout);
            }
            nfail++;
        }
        bytebuf_free(&why);
        bytebuf_free(&r.out);
        bytebuf_free(&r.err);
    }

    if (gc_stress && noptout > (size_t)MAX_GC_OPTOUTS) {
        (void)printf("fletch: %lu GC_STRESS opt-outs, at most %d allowed\n",
                     (unsigned long)noptout, MAX_GC_OPTOUTS);
        nfail++;
    }
    /*
     * ONE MACHINE-READABLE LINE, Cgfried-style, because the meta-suite
     * asserts exact totals and grepping five numbers out of prose is
     * how a driver ends up matching four of them.
     */
    (void)printf("fletch: total=%lu pass=%lu fail=%lu skip=%lu "
                 "xfail=%lu config=%lu\n",
                 (unsigned long)g_nunits, (unsigned long)npass,
                 (unsigned long)nfail, (unsigned long)nskip,
                 (unsigned long)nxfail, (unsigned long)ncfg);
    release_all();
    if (npass == 0U && nfail == 0U) {
        (void)printf("fletch: no units ran -- an empty suite is not a "
                     "green one\n");
        return 1;
    }
    return nfail == 0U && ncfg == 0U ? 0 : 1;
}
