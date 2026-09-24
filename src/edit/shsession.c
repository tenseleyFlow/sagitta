/* Sprint 57.27: the persistent shell session.  See shsession.h. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "edit/shsession.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/log.h"
#include "ws/workspace.h"

extern char **environ;

#define SH_MARK 0x1e
/* "YEW0 " + a u64 in decimal + ':' */
#define SH_HEAD_MAX 32U
/* "\036" + nonce + "-" + seq + ".\036" */
#define SH_MARK_MAX 64U

typedef enum {
    RX_OUT,    /* command output until this frame's status marker   */
    RX_STATUS, /* the status digits up to the marker's closing 0x1e  */
    RX_HEAD,   /* "YEW0 <len>:"                                      */
    RX_BODY,   /* exactly <len> env bytes                            */
    RX_TAIL,   /* the end marker, immediately                        */
    RX_SKIP    /* no usable env record: discard up to the end marker */
} RxState;

typedef struct ShLink ShLink;

struct YewShSession {
    Ed *ed;
    ShLink *link;  /* the live shell's transport, or NULL             */
    u32 job_id;    /* the live shell's internal job, or 0             */
    /* Shells already abandoned (killed), released once reaped. */
    u32 dead[YEW_JOB_MAX];
    u32 n_dead;
    /* Last reported state; NULL = workspace root / standard env. */
    char *cwd;
    char **env;
    u32 proxy_id;  /* the command running inside now, or 0            */
    i64 cancel_ms; /* when it was last cancelled, 0 = never           */
    /* Said once an abandoned shell is reaped, after the cancel's own
     * message and its command's footer. */
    const char *end_why;
    bool warned;   /* the unsupported-$SHELL message was said         */
};

/*
 * The framed owner of one shell process.  Owned by the job layer from
 * a successful spawn until `destroy`; the session only borrows it, and
 * detaching (`s = NULL`) makes every later byte a no-op.
 */
struct ShLink {
    YewShSession *s;
    Bytebuf tx;
    u64 tx_off;
    char nonce[33];
    char word[16];    /* "command ", "builtin " or "": the probe's answer */
    char *pending;    /* the first command, waiting for the probe        */
    char *pending_env; /* its YEW_FILE/LINE/COL words (frame_rows)       */
    const char *self; /* yew for --yew-env0, or NULL                     */
    u32 seq;          /* the frame being read                            */
    bool open;        /* its line was sent and its end not yet seen      */
    RxState st;
    Bytebuf hold;     /* a partial marker from the previous read          */
    u8 mark[SH_MARK_MAX];
    size_t mark_len;
    u8 end[SH_MARK_MAX];
    size_t end_len;
    char num[SH_HEAD_MAX + 1U];
    size_t num_len;
    int status;
    bool have_status;
    Bytebuf envb;
    u64 env_want;
};

/* ------------------------------------------------------------------ */
/* Session state                                                      */
/* ------------------------------------------------------------------ */

static void env_free(char **env)
{
    size_t i;

    if (env == NULL)
        return;
    for (i = 0U; env[i] != NULL; i++)
        yew_xfree(env[i]);
    yew_xfree(env);
}

static YewShSession *session_get(Ed *ed)
{
    if (ed->shsession == NULL) {
        ed->shsession = yew_xcalloc(1U, sizeof(*ed->shsession));
        ed->shsession->ed = ed;
    }
    return ed->shsession;
}

static bool session_persistent(Ed *ed)
{
    OptVal v;

    if (!yew_opt_get(ed, NULL, NULL, "shell.session", 13U, &v) ||
        v.type != YEW_OPT_ENUM)
        return true;
    return !(v.as.str.len == 5U && memcmp(v.as.str.s, "fresh", 5U) == 0);
}

static bool shell_supported(void)
{
    static const char *const other[] = {"fish", "nu", "xonsh", "elvish",
                                        "pwsh"};
    const char *sh = yew_job_shell();
    const char *base = strrchr(sh, '/');
    size_t i;

    base = base == NULL ? sh : base + 1;
    for (i = 0U; i < YEW_ARRAY_LEN(other); i++) {
        if (strcmp(base, other[i]) == 0)
            return false;
    }
    return true;
}

bool yew_shsession_wanted(Ed *ed)
{
    YewShSession *s;

    if (ed == NULL || !session_persistent(ed))
        return false;
    if (shell_supported())
        return true;
    s = session_get(ed);
    if (!s->warned) {
        s->warned = true;
        yew_msg(ed, YEW_MSG_WARN,
                "shell session needs a POSIX-family $SHELL; running each "
                ":! fresh");
    }
    return false;
}

static YewJob *session_proxy(YewShSession *s)
{
    YewJob *j = s->proxy_id != 0U ? yew_job_find(s->ed, s->proxy_id) : NULL;

    if (j == NULL || j->state != YEW_JOB_RUNNING || j->reaped) {
        s->proxy_id = 0U;
        return NULL;
    }
    return j;
}

bool yew_shsession_busy(Ed *ed)
{
    return ed != NULL && ed->shsession != NULL &&
           session_proxy(ed->shsession) != NULL;
}

const char *yew_shsession_cwd(Ed *ed)
{
    if (ed == NULL)
        return NULL;
    if (ed->shsession != NULL && ed->shsession->cwd != NULL &&
        session_persistent(ed))
        return ed->shsession->cwd;
    return yew_ws_root(ed);
}

const char *const *yew_shsession_env(Ed *ed)
{
    if (ed == NULL || ed->shsession == NULL ||
        ed->shsession->env == NULL || !session_persistent(ed))
        return NULL;
    return (const char *const *)ed->shsession->env;
}

u32 yew_shsession_job(Ed *ed)
{
    return ed != NULL && ed->shsession != NULL ? ed->shsession->job_id
                                               : 0U;
}

/* environ's SHLVL row, or NULL. */
static const char *parent_shlvl(void)
{
    size_t i;

    for (i = 0U; environ[i] != NULL; i++) {
        if (strncmp(environ[i], "SHLVL=", 6U) == 0)
            return environ[i];
    }
    return NULL;
}

/*
 * Adopt one env record: cwd, then NAME=value rows.  The session's own
 * SHLVL is one deeper than yew's, and a child started FROM the session's
 * state is not started inside it, so it gets yew's.  Returns false (state
 * untouched) for a record that is not NUL-terminated rows.
 */
static bool session_adopt(YewShSession *s, const u8 *rec, u64 len)
{
    const u8 *p = rec;
    const u8 *end = rec + len;
    const char *shlvl = parent_shlvl();
    char **env;
    size_t rows = 0U;
    size_t n = 0U;
    const u8 *q;

    if (len == 0U || rec[len - 1U] != 0U)
        return false;
    for (q = rec; q < end; q++) {
        if (*q == 0U)
            rows++;
    }
    env = yew_xcalloc(rows + 1U, sizeof(*env));
    q = memchr(p, 0, (size_t)(end - p));
    if (q != p && p[0] == (u8)'/') {
        yew_xfree(s->cwd);
        s->cwd = yew_xstrdup((const char *)p);
    }
    p = q + 1;
    while (p < end) {
        const char *row = (const char *)p;
        const char *eq = strchr(row, '=');

        q = memchr(p, 0, (size_t)(end - p));
        p = q + 1;
        if (eq == NULL || eq == row || strncmp(row, "_=", 2U) == 0)
            continue;
        if (strncmp(row, "SHLVL=", 6U) == 0) {
            if (shlvl != NULL)
                env[n++] = yew_xstrdup(shlvl);
            continue;
        }
        env[n++] = yew_xstrdup(row);
    }
    env[n] = NULL;
    env_free(s->env);
    s->env = env;
    return true;
}

/* ------------------------------------------------------------------ */
/* The link: transmit side                                            */
/* ------------------------------------------------------------------ */

static void link_put(ShLink *l, const char *s)
{
    bytebuf_append(&l->tx, s, strlen(s));
}

static void link_arm(ShLink *l)
{
    int n;

    n = snprintf((char *)l->mark, sizeof(l->mark), "%c%s-%u ", SH_MARK,
                 l->nonce, (unsigned)l->seq);
    l->mark_len = n > 0 && (size_t)n < sizeof(l->mark) ? (size_t)n : 0U;
    n = snprintf((char *)l->end, sizeof(l->end), "%c%s-%u.%c", SH_MARK,
                 l->nonce, (unsigned)l->seq, SH_MARK);
    l->end_len = n > 0 && (size_t)n < sizeof(l->end) ? (size_t)n : 0U;
    l->open = true;
    l->st = RX_OUT;
    l->hold.len = 0U;
    l->have_status = false;
    l->num_len = 0U;
}

/*
 * The status marker, the env record and the end marker for frame `seq`.
 * `between` runs after the status is taken and before the record (the
 * command frame puts the idle INT trap back there).
 */
static void link_put_tail(ShLink *l, const char *status_expr,
                          const char *between)
{
    char tag[64];
    int n;

    n = snprintf(tag, sizeof(tag), "'%s-%u'", l->nonce, (unsigned)l->seq);
    if (n < 0 || (size_t)n >= sizeof(tag))
        tag[0] = '\0';
    link_put(l, l->word);
    link_put(l, "printf '\\036%s %d\\036' ");
    link_put(l, tag);
    link_put(l, " ");
    link_put(l, status_expr);
    link_put(l, " >&9; ");
    link_put(l, between);
    if (l->self != NULL) {
        yew_shell_quote(&l->tx, (const u8 *)l->self, strlen(l->self));
        link_put(l, " --yew-env0 </dev/null >&9; ");
    }
    link_put(l, l->word);
    link_put(l, "printf '\\036%s.\\036' ");
    link_put(l, tag);
    link_put(l, " >&9\n");
}

/*
 * One command's line.  The command runs inside a function whose INT trap
 * RETURNS: a trapped SIGINT only runs the trap once the killed child is
 * reaped, and at top level every shell then carries on with the rest of
 * the list (`sleep 30; make install` would install).  `return` from the
 * trap ends the whole command in zsh, bash, dash and ksh alike; the idle
 * `trap : INT` is back before the env record.  The function is defined
 * again on every line, so a command cannot break the next frame by
 * unsetting it, and it takes no arguments: `$#` is 0 and `$1` empty, as
 * under `$SHELL -c`.
 */
static void link_send(ShLink *l, const char *cmdline, const char *rows)
{
    char between[64];
    int n;

    l->seq++;
    link_arm(l);
    if (rows != NULL && rows[0] != '\0') {
        link_put(l, l->word);
        link_put(l, "export");
        link_put(l, rows);
        link_put(l, "; ");
    }
    link_put(l, "__yew_run() { trap 'return 130' INT; ");
    link_put(l, l->word);
    link_put(l, "eval \"$__yew_c\"; }; __yew_c=");
    yew_shell_quote(&l->tx, (const u8 *)cmdline, strlen(cmdline));
    link_put(l, "; __yew_run </dev/null 9>&-; ");
    n = snprintf(between, sizeof(between), "%strap : INT; %sunset __yew_c; ",
                 l->word, l->word);
    if (n < 0 || (size_t)n >= sizeof(between))
        between[0] = '\0';
    link_put_tail(l, "\"$?\"", between);
}

/*
 * Written once, first thing on stdin.  Frame 0 is the probe: it answers
 * which prefix makes `eval` both exit-safe and function-proof here (0:
 * `command`, 1: `builtin`, 2: neither) and reports the starting state.
 * Its output -- and anything a startup file printed before it -- is
 * discarded: the prologue is not a command.
 */
static void link_prologue(ShLink *l)
{
    link_put(l, "exec 2>&1\n");
    /* A trapped signal is reset to default in children; an IGNORED one
     * is not -- `''` here would make every command immune to cancel. */
    link_put(l, "trap : INT\n");
    link_put(l, "set +m\n");
    link_put(l, "exec 9>&1\n");
    l->seq = 0U;
    link_arm(l);
    link_put(l, "if command eval : 2>/dev/null; then __yew_w=0; "
                "elif builtin eval : 2>/dev/null; then __yew_w=1; "
                "else __yew_w=2; fi </dev/null 9>&-; ");
    link_put_tail(l, "\"$__yew_w\"", "unset __yew_w; ");
}

static u64 link_tx_view(void *owner, const u8 **bytes)
{
    ShLink *l = owner;

    if (l->s == NULL || l->tx_off >= (u64)l->tx.len) {
        *bytes = NULL;
        return 0U;
    }
    *bytes = l->tx.data + (size_t)l->tx_off;
    return (u64)l->tx.len - l->tx_off;
}

static void link_tx_consume(void *owner, u64 len)
{
    ShLink *l = owner;
    u64 remain = (u64)l->tx.len - l->tx_off;

    l->tx_off += len < remain ? len : remain;
    if (l->tx_off == (u64)l->tx.len) {
        l->tx.len = 0U;
        l->tx_off = 0U;
    }
}

/* ------------------------------------------------------------------ */
/* The link: receive side                                             */
/* ------------------------------------------------------------------ */

static void session_abandon(YewShSession *s, const char *why);

/* Output belongs to the running command; between frames, and in the
 * probe frame, it belongs to nobody. */
static void link_output(ShLink *l, const u8 *bytes, size_t len)
{
    YewJob *j;

    if (len == 0U || l->s == NULL || l->seq == 0U || !l->open)
        return;
    j = session_proxy(l->s);
    if (j != NULL)
        yew_job_proxy_output(l->s->ed, j, bytes, (u64)len);
}

static YewJobWait frame_wait(const YewShSession *s, int status)
{
    YewJobWait w;

    (void)memset(&w, 0, sizeof(w));
    /* ksh93 reports a signal death as 256 + N. */
    if (status > 255)
        status = 128 + (status - 256);
    if (s->cancel_ms != 0 && status > 128 && status < 128 + 64) {
        w.state = YEW_JOB_SIGNALED;
        w.termsig = status - 128;
    } else {
        w.state = YEW_JOB_EXITED;
        w.exit_code = status;
    }
    return w;
}

static void link_frame_done(ShLink *l, bool env_ok)
{
    YewShSession *s = l->s;

    l->open = false;
    l->st = RX_OUT;
    l->hold.len = 0U;
    if (s == NULL)
        return;
    if (env_ok && !session_adopt(s, l->envb.data, (u64)l->envb.len))
        yew_log(YEW_LOG_WARN, "shell session: malformed env record");
    l->envb.len = 0U;
    if (l->seq == 0U) {
        const char *word = l->status == 0 ? "command " :
                           l->status == 1 ? "builtin " : "";
        size_t wn = strlen(word);

        _Static_assert(sizeof(((ShLink *)0)->word) > sizeof("builtin "),
                       "the probe's word fits");
        (void)memcpy(l->word, word, wn + 1U);
        if (l->pending != NULL) {
            char *cmd = l->pending;
            char *rows = l->pending_env;

            l->pending = NULL;
            l->pending_env = NULL;
            link_send(l, cmd, rows);
            yew_xfree(cmd);
            yew_xfree(rows);
        }
        return;
    }
    {
        YewJob *j = session_proxy(s);
        YewJobWait w = frame_wait(s, l->status);

        if (j != NULL)
            yew_job_proxy_end(s->ed, j, &w);
        s->proxy_id = 0U;
        s->cancel_ms = 0;
    }
}

/*
 * How much of `b` matches the start of `pat`: -1 no, 0 a proper prefix
 * (wait for more), 1 all of `pat`.
 */
static int prefix_match(const u8 *b, size_t n, const u8 *pat, size_t plen)
{
    size_t k = n < plen ? n : plen;

    if (plen == 0U || memcmp(b, pat, k) != 0)
        return -1;
    return n >= plen ? 1 : 0;
}

/* Finds `pat` in b[0..n); returns its offset, or the offset of a partial
 * match at the tail (*partial set), or n. */
static size_t find_marker(const u8 *b, size_t n, const u8 *pat, size_t plen,
                          bool *partial)
{
    size_t at = 0U;

    *partial = false;
    while (at < n) {
        const u8 *p = memchr(b + at, SH_MARK, n - at);
        int m;

        if (p == NULL)
            return n;
        at = (size_t)(p - b);
        m = prefix_match(b + at, n - at, pat, plen);
        if (m == 1)
            return at;
        if (m == 0) {
            *partial = true;
            return at;
        }
        at++;
    }
    return n;
}

static bool head_parse(const char *s, size_t n, u64 *len)
{
    u64 v = 0U;
    size_t i;

    if (n < 6U || memcmp(s, "YEW0 ", 5U) != 0)
        return false;
    for (i = 5U; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' || v > (UINT64_MAX - 9U) / 10U)
            return false;
        v = v * 10U + (u64)(s[i] - '0');
    }
    *len = v;
    return true;
}

/* One pass over `b`; returns how many bytes were consumed.  A partial
 * marker at the end is left unconsumed for the caller to hold. */
static size_t link_scan(ShLink *l, const u8 *b, size_t n)
{
    size_t i = 0U;

    while (i < n) {
        if (!l->open) {
            /* Between frames: a background job's bytes, nobody's. */
            return n;
        }
        switch (l->st) {
        case RX_OUT: {
            bool partial;
            size_t at = find_marker(b + i, n - i, l->mark, l->mark_len,
                                    &partial);

            link_output(l, b + i, at);
            i += at;
            if (partial)
                return i;
            if (i < n) {
                i += l->mark_len;
                l->st = RX_STATUS;
                l->num_len = 0U;
            }
            break;
        }
        case RX_STATUS: {
            u8 c = b[i++];

            if (c == SH_MARK) {
                char *endp = NULL;
                long v;

                l->num[l->num_len] = '\0';
                v = strtol(l->num, &endp, 10);
                if (l->num_len == 0U || endp == NULL || *endp != '\0' ||
                    v < 0 || v > 65535) {
                    session_abandon(l->s, "shell session lost its frame");
                    return n;
                }
                l->status = (int)v;
                l->have_status = true;
                l->st = RX_HEAD;
                l->num_len = 0U;
            } else if (l->num_len < SH_HEAD_MAX) {
                l->num[l->num_len++] = (char)c;
            } else {
                session_abandon(l->s, "shell session lost its frame");
                return n;
            }
            break;
        }
        case RX_HEAD: {
            u8 c = b[i];

            if (c == (u8)':') {
                i++;
                if (!head_parse(l->num, l->num_len, &l->env_want) ||
                    l->env_want > YEW_SHSESSION_ENV_MAX) {
                    l->st = RX_SKIP;
                } else {
                    l->envb.len = 0U;
                    l->st = l->env_want == 0U ? RX_TAIL : RX_BODY;
                }
            } else if (c == SH_MARK || c < 0x20U || c > 0x7eU ||
                       l->num_len >= SH_HEAD_MAX) {
                /* Not a record (no helper, or it failed): do not consume
                 * the byte -- it may open the end marker. */
                l->st = RX_SKIP;
            } else {
                l->num[l->num_len++] = (char)c;
                i++;
            }
            break;
        }
        case RX_BODY: {
            u64 left = l->env_want - (u64)l->envb.len;
            size_t take = (u64)(n - i) < left ? n - i : (size_t)left;

            bytebuf_append(&l->envb, b + i, take);
            i += take;
            if ((u64)l->envb.len == l->env_want)
                l->st = RX_TAIL;
            break;
        }
        case RX_TAIL: {
            int m = prefix_match(b + i, n - i, l->end, l->end_len);

            if (m == 0)
                return i;
            if (m == 1) {
                i += l->end_len;
                link_frame_done(l, true);
            } else {
                l->envb.len = 0U;
                l->st = RX_SKIP;
            }
            break;
        }
        case RX_SKIP:
        default: {
            bool partial;
            size_t at = find_marker(b + i, n - i, l->end, l->end_len,
                                    &partial);

            i += at;
            if (partial || i >= n)
                return i;
            i += l->end_len;
            link_frame_done(l, false);
            break;
        }
        }
    }
    return i;
}

static bool link_feed(void *owner, const u8 *bytes, u64 len)
{
    ShLink *l = owner;
    size_t used;

    if (l->s == NULL || len == 0U)
        return true;
    if (l->hold.len != 0U) {
        /* A marker split across reads: rescan it with the new bytes. */
        Bytebuf work;

        bytebuf_init(&work);
        bytebuf_append(&work, l->hold.data, l->hold.len);
        bytebuf_append(&work, bytes, (size_t)len);
        l->hold.len = 0U;
        used = link_scan(l, work.data, work.len);
        if (used < work.len && l->s != NULL)
            bytebuf_append(&l->hold, work.data + used, work.len - used);
        bytebuf_free(&work);
        return true;
    }
    used = link_scan(l, bytes, (size_t)len);
    if (used < (size_t)len && l->s != NULL)
        bytebuf_append(&l->hold, bytes + used, (size_t)len - used);
    return true;
}

static bool link_finish(void *owner)
{
    ShLink *l = owner;

    /* A held partial marker was output after all. */
    if (l->s != NULL && l->st == RX_OUT && l->hold.len != 0U)
        link_output(l, l->hold.data, l->hold.len);
    l->hold.len = 0U;
    return true;
}

static void link_destroy(void *owner)
{
    ShLink *l = owner;

    if (l->s != NULL && l->s->link == l)
        l->s->link = NULL;
    bytebuf_free(&l->tx);
    bytebuf_free(&l->hold);
    bytebuf_free(&l->envb);
    yew_xfree(l->pending);
    yew_xfree(l->pending_env);
    yew_xfree(l);
}

static const YewJobFramedOps link_ops = {
    link_feed, link_finish, link_tx_view, link_tx_consume,
    NULL,      NULL,        link_destroy
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void make_nonce(char out[33])
{
    static u64 salt;
    u8 raw[16];
    size_t got = 0U;
    size_t i;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        while (got < sizeof(raw)) {
            ssize_t r = read(fd, raw + got, sizeof(raw) - got);

            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        (void)close(fd);
    }
    if (got < sizeof(raw)) {
        /* No /dev/urandom: still distinct per session and per process.
         * splitmix64 over the clock, pid and a counter. */
        u64 x = (u64)yew_now_ms() ^ ((u64)getpid() << 32) ^ ++salt;

        for (i = 0U; i < sizeof(raw); i++) {
            u64 z;

            x += 0x9E3779B97F4A7C15ULL;
            z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            raw[i] = (u8)(z ^ (z >> 31));
        }
    }
    for (i = 0U; i < sizeof(raw); i++) {
        static const char hex[] = "0123456789abcdef";

        out[i * 2U] = hex[raw[i] >> 4];
        out[i * 2U + 1U] = hex[raw[i] & 0x0FU];
    }
    out[32] = '\0';
}

/* Detach the live shell: kill its group, keep its job for reaping. */
static void session_detach(YewShSession *s, bool kill_it)
{
    YewJob *j = s->job_id != 0U ? yew_job_find(s->ed, s->job_id) : NULL;

    if (s->link != NULL) {
        s->link->s = NULL;
        s->link = NULL;
    }
    if (j != NULL) {
        if (kill_it && j->pgid > 0 && yew_job_pending(j))
            (void)kill(-j->pgid, SIGKILL);
        if (s->n_dead < YEW_ARRAY_LEN(s->dead))
            s->dead[s->n_dead++] = s->job_id;
    }
    s->job_id = 0U;
}

/* End the running command (if any) with `w`. */
static void session_end_proxy(YewShSession *s, const YewJobWait *w)
{
    YewJob *j = session_proxy(s);

    if (j != NULL)
        yew_job_proxy_end(s->ed, j, w);
    s->proxy_id = 0U;
    s->cancel_ms = 0;
}

/* Kill the shell now; its running command is CANCELLED.  Safe inside a
 * feed: nothing here releases or compacts the job table. */
static void session_abandon(YewShSession *s, const char *why)
{
    YewJobWait w;

    if (s == NULL)
        return;
    (void)memset(&w, 0, sizeof(w));
    w.state = YEW_JOB_CANCELLED;
    session_end_proxy(s, &w);
    session_detach(s, true);
    s->end_why = why;
}

static bool proxy_signal(void *owner, Ed *ed, u32 id, int sig)
{
    YewShSession *s = owner;
    YewJob *shell;
    i64 now = yew_now_ms();

    (void)ed;
    if (s == NULL || id != s->proxy_id || s->job_id == 0U)
        return false;
    shell = yew_job_find(s->ed, s->job_id);
    if (shell == NULL || shell->pgid <= 0 ||
        shell->state != YEW_JOB_RUNNING)
        return false;
    if (sig == SIGKILL ||
        (s->cancel_ms != 0 && now - s->cancel_ms < YEW_SHSESSION_ESCALATE_MS)) {
        /* A command that traps INT cannot wedge the session. */
        session_abandon(s, "shell session ended");
        return true;
    }
    if (kill(-shell->pgid, SIGINT) != 0)
        return false;
    s->cancel_ms = now;
    return true;
}

static const YewJobProxyOps proxy_ops = {proxy_signal};

static bool dir_usable(const char *path)
{
    struct stat st;

    return path != NULL && path[0] == '/' && stat(path, &st) == 0 &&
           S_ISDIR(st.st_mode);
}

static bool session_start(YewShSession *s, char *err, size_t errsz)
{
    Ed *ed = s->ed;
    YewJobSpec spec;
    ShLink *l;
    char *argv[3];
    const char *env_set[2];
    char *pwd;
    const char *cwd;
    size_t n;
    u32 id;

    /* A directory removed since the shell left: start at the root. */
    if (s->cwd != NULL && !dir_usable(s->cwd)) {
        yew_xfree(s->cwd);
        s->cwd = NULL;
    }
    cwd = s->cwd != NULL ? s->cwd : yew_ws_root(ed);
    l = yew_xcalloc(1U, sizeof(*l));
    l->s = s;
    bytebuf_init(&l->tx);
    bytebuf_init(&l->hold);
    bytebuf_init(&l->envb);
    make_nonce(l->nonce);
    l->self = yew_job_self_exe();
    if (l->self == NULL)
        yew_log(YEW_LOG_WARN, "shell session: cannot find yew for "
                              "--yew-env0; cd and export will not carry");
    link_prologue(l);
    /* The shell takes $PWD as given when it names its directory, so the
     * workspace's own spelling survives a symlink in its path. */
    n = strlen(cwd);
    pwd = yew_xmalloc(n + 5U);
    (void)memcpy(pwd, "PWD=", 4U);
    (void)memcpy(pwd + 4U, cwd, n + 1U);
    env_set[0] = pwd;
    env_set[1] = NULL;
    argv[0] = (char *)yew_job_shell();
    argv[1] = (char *)"-s";
    argv[2] = NULL;
    (void)memset(&spec, 0, sizeof(spec));
    spec.argv = argv;
    spec.cwd = cwd;
    spec.sink = YEW_SINK_FRAMED;
    spec.display = "shell session";
    spec.internal = true;
    spec.env_base = (const char *const *)s->env;
    spec.env_set = env_set;
    spec.framed_owner = l;
    spec.framed_ops = &link_ops;
    id = yew_job_spawn(ed, &spec, err, errsz);
    yew_xfree(pwd);
    if (id == 0U) {
        link_destroy(l);
        return false;
    }
    s->link = l;
    s->job_id = id;
    return true;
}

/*
 * The rows the job layer sets per command -- YEW_FILE, YEW_LINE, YEW_COL:
 * where the caret is NOW, not where it was when the shell started -- as
 * ` 'NAME=value'` words for the frame's `export`.  Heap string.
 */
static char *frame_rows(Ed *ed)
{
    static const char *const names[] = {"YEW_FILE=", "YEW_LINE=",
                                        "YEW_COL="};
    Arena a;
    Bytebuf out;
    char **env;
    size_t i;
    size_t k;

    arena_init(&a);
    bytebuf_init(&out);
    env = yew_job_env(ed, &a);
    for (i = 0U; env != NULL && env[i] != NULL; i++) {
        for (k = 0U; k < YEW_ARRAY_LEN(names); k++) {
            if (strncmp(env[i], names[k], strlen(names[k])) == 0) {
                bytebuf_push_u8(&out, (u8)' ');
                yew_shell_quote(&out, (const u8 *)env[i], strlen(env[i]));
            }
        }
    }
    bytebuf_push_u8(&out, 0U);
    arena_free_all(&a);
    return (char *)out.data;
}

u32 yew_shsession_run(Ed *ed, const char *cmdline, char *err, size_t errsz)
{
    YewShSession *s;
    YewJobSpec spec;
    char *rows;
    u32 id;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (ed == NULL || cmdline == NULL)
        return 0U;
    /* A shell that left since the last loop turn is finished first. */
    yew_shsession_settle(ed);
    s = session_get(ed);
    if (session_proxy(s) != NULL) {
        (void)snprintf(err, errsz, "shell session is busy");
        return 0U;
    }
    if (s->link == NULL) {
        /* A shell still in the table without a link is on its way out. */
        if (s->job_id != 0U)
            session_detach(s, true);
        if (!session_start(s, err, errsz))
            return 0U;
    }
    (void)memset(&spec, 0, sizeof(spec));
    spec.sink = YEW_SINK_BUFFER;
    spec.display = cmdline;
    spec.proxy_owner = s;
    spec.proxy_ops = &proxy_ops;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    s->proxy_id = id;
    s->cancel_ms = 0;
    rows = frame_rows(ed);
    if (s->link->seq == 0U && s->link->open) {
        /* Still probing: the command goes the moment the probe ends. */
        yew_xfree(s->link->pending);
        yew_xfree(s->link->pending_env);
        s->link->pending = yew_xstrdup(cmdline);
        s->link->pending_env = rows;
    } else {
        link_send(s->link, cmdline, rows);
        yew_xfree(rows);
    }
    return id;
}

/* The shell's own verdict, for a command it never finished. */
static YewJobWait shell_wait(const YewJob *shell)
{
    YewJobWait w;

    (void)memset(&w, 0, sizeof(w));
    if (shell == NULL) {
        w.state = YEW_JOB_CANCELLED;
        return w;
    }
    w.state = shell->state == YEW_JOB_RUNNING ? YEW_JOB_CANCELLED
                                              : shell->state;
    w.exit_code = shell->exit_code;
    w.termsig = shell->termsig;
    w.exec_errno = shell->exec_errno;
    return w;
}

/* The shell is gone but its pipe may still hold what it wrote first:
 * readable (or hung up) means the pump has more to deliver. */
static bool shell_output_pending(const YewJob *shell)
{
    struct pollfd p;

    if (shell == NULL || shell->out_fd < 0)
        return false;
    p.fd = shell->out_fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1U, 0) > 0 && p.revents != 0;
}

void yew_shsession_settle(Ed *ed)
{
    YewShSession *s;
    YewJob *shell;
    u32 i = 0U;

    if (ed == NULL || ed->shsession == NULL)
        return;
    s = ed->shsession;
    while (i < s->n_dead) {
        YewJob *j = yew_job_find(ed, s->dead[i]);

        if (j != NULL && !j->reaped && j->state == YEW_JOB_RUNNING) {
            i++;
            continue;
        }
        /* Released with its pipes: a background child still holding
         * one gets EPIPE, not a reader that never comes. */
        if (j != NULL)
            yew_job_release(ed, j);
        s->dead[i] = s->dead[--s->n_dead];
        if (s->end_why != NULL) {
            yew_msg(ed, YEW_MSG_WARN, "%s; the next :! starts a new one",
                    s->end_why);
            s->end_why = NULL;
        }
    }
    if (s->job_id == 0U)
        return;
    shell = yew_job_find(ed, s->job_id);
    if (shell != NULL &&
        ((!shell->reaped && shell->state == YEW_JOB_RUNNING) ||
         shell_output_pending(shell)))
        return;
    /*
     * The shell is gone (`exit`, `exec`, a death) and everything it
     * wrote has been read.  Its running command ends with what is known:
     * the frame's own status if it got that far, else the shell's.
     */
    {
        YewJobWait w = shell_wait(shell);

        if (s->link != NULL && s->link->have_status && s->link->seq != 0U &&
            s->link->open)
            w = frame_wait(s, s->link->status);
        session_end_proxy(s, &w);
        session_detach(s, false);
        /* Finish the command first so its line does not cover this one. */
        (void)yew_job_settle(ed);
        if (w.state == YEW_JOB_EXECFAIL)
            yew_msg(ed, YEW_MSG_ERROR, "cannot start shell session: %s: %s",
                    yew_job_shell(), strerror(w.exec_errno));
        else
            yew_msg(ed, YEW_MSG_INFO,
                    "shell session ended; the next :! starts a new one");
    }
    /* Release the shell's job now that it is reaped. */
    yew_shsession_settle(ed);
}

void yew_shsession_end(Ed *ed)
{
    if (ed == NULL || ed->shsession == NULL)
        return;
    session_abandon(ed->shsession, NULL);
}

void yew_shsession_reset(Ed *ed)
{
    YewShSession *s;

    if (ed == NULL)
        return;
    s = session_get(ed);
    session_abandon(s, NULL);
    yew_xfree(s->cwd);
    s->cwd = NULL;
    env_free(s->env);
    s->env = NULL;
}

void yew_shsession_free(Ed *ed)
{
    YewShSession *s;

    if (ed == NULL || ed->shsession == NULL)
        return;
    s = ed->shsession;
    /* The job table outlives this: it disposes the shell (and destroys
     * the detached link) and every proxy, none of which calls back. */
    session_detach(s, true);
    yew_xfree(s->cwd);
    env_free(s->env);
    yew_xfree(s);
    ed->shsession = NULL;
}

/* ------------------------------------------------------------------ */
/* yew --yew-env0                                                      */
/* ------------------------------------------------------------------ */

static bool write_all(int fd, const void *data, size_t len)
{
    const u8 *p = data;

    while (len != 0U) {
        ssize_t w = write(fd, p, len);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        len -= (size_t)w;
    }
    return true;
}

/* The logical $PWD when it names this directory, else getcwd(). */
static void env0_cwd(Bytebuf *out)
{
    const char *pwd = getenv("PWD");
    struct stat a;
    struct stat d;
    size_t cap;

    if (pwd != NULL && pwd[0] == '/' && stat(pwd, &a) == 0 &&
        stat(".", &d) == 0 && a.st_dev == d.st_dev && a.st_ino == d.st_ino) {
        bytebuf_append(out, pwd, strlen(pwd));
        return;
    }
    for (cap = 256U; cap <= (size_t)1 << 20; cap *= 2U) {
        char *buf = yew_xmalloc(cap);

        if (getcwd(buf, cap) != NULL) {
            bytebuf_append(out, buf, strlen(buf));
            yew_xfree(buf);
            return;
        }
        yew_xfree(buf);
        if (errno != ERANGE)
            return; /* empty: the parser keeps the last directory */
    }
}

int yew_shsession_env0_main(void)
{
    Bytebuf rec;
    char head[SH_HEAD_MAX];
    size_t i;
    int n;
    bool ok;

    bytebuf_init(&rec);
    env0_cwd(&rec);
    bytebuf_push_u8(&rec, 0U);
    for (i = 0U; environ[i] != NULL; i++) {
        /* `_` is the shell's note of this very command's path. */
        if (strncmp(environ[i], "_=", 2U) == 0)
            continue;
        bytebuf_append(&rec, environ[i], strlen(environ[i]) + 1U);
    }
    n = snprintf(head, sizeof(head), "YEW0 %llu:",
                 (unsigned long long)rec.len);
    ok = n > 0 && (size_t)n < sizeof(head) &&
         write_all(STDOUT_FILENO, head, (size_t)n) &&
         write_all(STDOUT_FILENO, rec.data, rec.len);
    bytebuf_free(&rec);
    return ok ? 0 : 1;
}
