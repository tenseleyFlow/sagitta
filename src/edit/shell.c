/* Sprint 19: the three consumption modes and the *jobs* table. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "edit/shell.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "term/input.h"
#include "term/tty.h"
#include "text/piece.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "util/log.h"
#include "ws/workspace.h"

#define YEW_JOBS_TABLE_NAME "*jobs*"

/* Filter poll tick: bounds how long the restricted loop sleeps before
 * checking for child exit.  See filter_drive. */
#define YEW_FILTER_TICK_MS 20

/* Our own table rather than strsignal(): its text is locale-dependent and
 * would leak into pty goldens, breaking deterministic rendering. */
static const char *yew_signame(int sig)
{
    switch (sig) {
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    default:      break;
    }
    return "SIG?";
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Elapsed time is the one nondeterministic thing a job puts on screen, and
 * invariant 5 requires the pty harness to byte-compare grids exactly.
 * YEW_JOB_ELAPSED_MS pins the displayed value so goldens stay exact
 * instead of fuzzy-matched — the same test-determinism hook pattern as
 * YEW_CHORD_TIMEOUT_MS.  Unset in normal use, where real timings show.
 */
static i64 elapsed_override(void)
{
    static bool looked_up;
    static i64 fixed = -1;

    if (!looked_up) {
        const char *value = getenv("YEW_JOB_ELAPSED_MS");

        looked_up = true;
        if (value != NULL && value[0] != '\0') {
            char *end = NULL;
            long parsed = strtol(value, &end, 10);

            if (end != NULL && *end == '\0' && parsed >= 0)
                fixed = (i64)parsed;
        }
    }
    return fixed;
}

static void fmt_elapsed(char *out, size_t n, i64 ms)
{
    i64 fixed = elapsed_override();

    if (fixed >= 0)
        ms = fixed;
    (void)snprintf(out, n, "%.2fs", (double)ms / 1000.0);
}

/* Appends straight through the edit chokepoint so marks and cursors track,
 * but outside any undo transaction: a job buffer is YEW_BUF_NOUNDO. */
static EditCtx buf_raw_edit_ctx(Ed *ed, Buffer *b)
{
    EditCtx ec = {0};
    Win *w = ed->win != NULL && ed->win->buf == b ? ed->win : NULL;

    ec.tb = b->tb;
    ec.marks = b->marks;
    ec.cset = w != NULL ? &w->cs : NULL;
    ec.undo = NULL; /* YEW_BUF_NOUNDO: no ops recorded, nothing journaled */
    ec.jrnl = NULL;
    /* meta stays NULL: the edit chokepoint treats a non-NULL meta as
     * file-backed and demands a journal path, which a scratch buffer has
     * no business owning — there is no file here to protect. */
    ec.meta = NULL;
    ec.now_ms = ed->now_ms;
    ec.ed = ed;
    ec.buffer = b;
    return ec;
}

static void buf_append_raw(Ed *ed, Buffer *b, const u8 *bytes, u64 len)
{
    EditCtx ec;

    if (b == NULL || b->tb == NULL || len == 0U)
        return;
    ec = buf_raw_edit_ctx(ed, b);
    (void)yew_edit_insert(&ec, BYTEOFF(yew_textbuf_len(b->tb)), bytes, len);
}

static void buf_append_str(Ed *ed, Buffer *b, const char *s)
{
    buf_append_raw(ed, b, (const u8 *)s, (u64)strlen(s));
}

/* ------------------------------------------------------------------ */
/* Mode (a): streaming into a job buffer (§4)                         */
/* ------------------------------------------------------------------ */

void yew_job_buffer_append(Ed *ed, YewJob *j, const u8 *bytes, u64 len,
                           bool is_err)
{
    bool at_end = false;
    Win *w;

    (void)is_err; /* stderr attr spans arrive with the theme in Sprint 41 */
    if (ed == NULL || j == NULL || j->buf == NULL || len == 0U)
        return;
    w = ed->win != NULL && ed->win->buf == j->buf ? ed->win : NULL;
    if (w != NULL && w->cs.curs.len != 0U) {
        const Cursor *cur = &w->cs.curs.data[w->cs.primary];

        /* Follow-tail moves the cursor only when it was already at the end
         * and nothing is selected.  Yanking the view to the bottom while
         * someone reads the middle is the classic complaint. */
        at_end = cur->pos.v >= yew_textbuf_len(j->buf->tb) &&
                 w->cs.active == YEW_MC_ACTIVE_NONE;
    }
    buf_append_raw(ed, j->buf, bytes, len);
    if (at_end && j->follow_tail && w != NULL) {
        Cursor *cur = &w->cs.curs.data[w->cs.primary];

        cur->pos = BYTEOFF(yew_textbuf_len(j->buf->tb));
        cur->anchor = cur->pos;
        ed->cursor_follow_pending = true;
    }
    if (w != NULL)
        yew_ed_damage_document(ed);
}

static void job_footer(Ed *ed, YewJob *j, char *out, size_t n)
{
    char elapsed[32];

    (void)ed;

    fmt_elapsed(elapsed, sizeof(elapsed), j->end_ms - j->start_ms);
    switch (j->state) {
    case YEW_JOB_EXECFAIL:
        (void)snprintf(out, n, "[cannot run %s: %s]", j->label,
                       strerror(j->exec_errno));
        break;
    case YEW_JOB_SIGNALED:
        (void)snprintf(out, n, "[killed by %s after %s]",
                       yew_signame(j->termsig), elapsed);
        break;
    case YEW_JOB_TIMEOUT:
        (void)snprintf(out, n, "[timed out after %s]", elapsed);
        break;
    case YEW_JOB_CANCELLED:
        (void)snprintf(out, n, "[cancelled after %s]", elapsed);
        break;
    case YEW_JOB_EXITED:
    default:
        (void)snprintf(out, n, "[exit %d in %s]", j->exit_code, elapsed);
        break;
    }
}

/* Mode (c): the collected output lands in one transaction at the mark. */
static void job_insert_collected(Ed *ed, YewJob *j)
{
    EditCtx ec;
    Buffer *b;
    ByteOff at;
    bool needs_nl;

    if (ed->win == NULL || ed->win->buf == NULL)
        return;
    b = ed->win->buf;
    if (j->state != YEW_JOB_EXITED || j->exit_code != 0) {
        char foot[256];

        job_footer(ed, j, foot, sizeof(foot));
        yew_msg(ed, YEW_MSG_WARN, "%s %s", j->label, foot);
        return;
    }
    if (j->collect.len == 0U) {
        yew_msg(ed, YEW_MSG_INFO, "%s — no output", j->label);
        return;
    }
    at = BYTEOFF(yew_mark_pos(b->marks, j->at).v);
    ec = yew_ed_edit_ctx(ed);
    yew_undo_begin(&ec, YEW_TXN_EXTERNAL);
    (void)yew_edit_insert(&ec, at, j->collect.data, (u64)j->collect.len);
    /* A trailing newline iff we inserted at a line start and the output
     * lacks one, so `:r !date` on an empty line does not glue the next
     * line onto it. */
    needs_nl = j->collect.data[j->collect.len - 1U] != (u8)'\n' &&
               (at.v == 0U || at.v == yew_textbuf_line_start(
                                          b->tb,
                                          yew_textbuf_line_of(b->tb, at)).v);
    if (needs_nl) {
        ByteOff end = BYTEOFF(at.v + (u64)j->collect.len);

        (void)yew_edit_insert(&ec, end, (const u8 *)"\n", 1U);
    }
    yew_undo_end(&ec);
    yew_ed_finish_edit(ed, &ec);
    yew_mark_del(b->marks, j->at);
    j->has_mark = false;
    yew_ed_damage_document(ed);
    yew_msg(ed, YEW_MSG_INFO, "%s — %llu bytes read", j->label,
            (unsigned long long)j->collect.len);
}

void yew_job_finish(Ed *ed, YewJob *j)
{
    char foot[256];

    if (ed == NULL || j == NULL)
        return;
    /* The filter drives its own job and owns the outcome, rollback
     * included; touching buffers here would fight it. */
    if (j->synchronous)
        return;
    if (j->sink == YEW_SINK_COLLECT && j->has_mark) {
        job_insert_collected(ed, j);
        return;
    }
    if (j->buf == NULL)
        return;
    job_footer(ed, j, foot, sizeof(foot));
    /* "Command produced no output": a full-screen empty buffer after every
     * :!make is the most-hated behavior of every editor that skips this. */
    if (j->bytes_out + j->bytes_err == 0U) {
        if (ed->win != NULL && ed->win->buf == j->buf) {
            Buffer *origin = yew_ws_buf_by_id(ed, j->origin_buf_id);

            (void)yew_ed_show_buffer(ed, origin != NULL ? origin :
                                                         &ed->buffer);
        }
        yew_msg(ed, YEW_MSG_INFO, ":!%s — no output (exit %d)",
                j->label, j->exit_code);
        yew_ws_scratch_drop(ed, j->buf);
        j->buf = NULL;
        return;
    }
    if (yew_textbuf_len(j->buf->tb) != 0U) {
        u64 end = yew_textbuf_len(j->buf->tb);
        u8 last = 0U;
        TextIter it;
        const u8 *chunk = NULL;
        u64 chunk_len = 0U;

        if (yew_textiter_begin(&it, j->buf->tb, BYTEOFF(end - 1U)) &&
            yew_textiter_chunk(&it, j->buf->tb, &chunk, &chunk_len) &&
            chunk_len != 0U)
            last = chunk[0];
        if (last != (u8)'\n')
            buf_append_str(ed, j->buf, "\n");
    }
    buf_append_str(ed, j->buf, foot);
    buf_append_str(ed, j->buf, "\n");
    yew_msg(ed, j->state == YEW_JOB_EXITED && j->exit_code == 0 ?
                    YEW_MSG_INFO : YEW_MSG_WARN,
            ":!%s %s", j->label, foot);
    yew_ed_damage_document(ed);
}

typedef enum {
    SELF_WORD_END,
    SELF_WORD_OK,
    SELF_WORD_AMBIGUOUS,
    SELF_WORD_BAD
} SelfWordResult;

static bool self_hspace(char c)
{
    return c == ' ' || c == '\t';
}

static bool self_unquoted_special(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '$' || c == '`' || c == '*' ||
           c == '?' || c == '[' || c == ']' || c == '{' || c == '}';
}

/* Deliberately incomplete: accept only words whose value is knowable without
 * asking the configured shell to expand or interpret anything. */
static SelfWordResult self_word(const char **cursor, Bytebuf *out)
{
    const char *p = *cursor;

    while (self_hspace(*p))
        p++;
    if (*p == '\0') {
        *cursor = p;
        return SELF_WORD_END;
    }
    out->len = 0U;
    while (*p != '\0' && !self_hspace(*p)) {
        char c = *p++;

        if (c == '\n' || c == '\r')
            return SELF_WORD_AMBIGUOUS;
        if (c == '\'') {
            while (*p != '\0' && *p != '\'') {
                if (*p == '\n' || *p == '\r')
                    return SELF_WORD_AMBIGUOUS;
                bytebuf_push_u8(out, (u8)*p++);
            }
            if (*p != '\'')
                return SELF_WORD_BAD;
            p++;
            continue;
        }
        if (c == '"') {
            while (*p != '\0' && *p != '"') {
                c = *p++;
                if (c == '\n' || c == '\r' || c == '$' || c == '`')
                    return SELF_WORD_AMBIGUOUS;
                if (c == '\\') {
                    char next = *p++;

                    if (next == '\0')
                        return SELF_WORD_BAD;
                    if (next == '\n' || next == '\r')
                        return SELF_WORD_AMBIGUOUS;
                    if (next != '"' && next != '\\' && next != '$' &&
                        next != '`')
                        return SELF_WORD_AMBIGUOUS;
                    c = next;
                }
                bytebuf_push_u8(out, (u8)c);
            }
            if (*p != '"')
                return SELF_WORD_BAD;
            p++;
            continue;
        }
        if (c == '\\') {
            if (*p == '\0')
                return SELF_WORD_BAD;
            c = *p++;
            if (c == '\n' || c == '\r')
                return SELF_WORD_AMBIGUOUS;
            bytebuf_push_u8(out, (u8)c);
            continue;
        }
        if (self_unquoted_special(c) || c == '#' ||
            ((c == '~' || c == '=') && out->len == 0U))
            return SELF_WORD_AMBIGUOUS;
        bytebuf_push_u8(out, (u8)c);
    }
    *cursor = p;
    return SELF_WORD_OK;
}

/* Collapse dot components in an absolute spelling without changing cwd. */
static bool self_lexical_absolute(const char *path, char out[PATH_MAX])
{
    const char *p = path;
    size_t len = 1U;

    if (path == NULL || path[0] != '/')
        return false;
    out[0] = '/';
    out[1] = '\0';
    while (*p != '\0') {
        const char *start;
        size_t n;

        while (*p == '/')
            p++;
        start = p;
        while (*p != '\0' && *p != '/')
            p++;
        n = (size_t)(p - start);
        if (n == 0U)
            break;
        if (n == 1U && start[0] == '.')
            continue;
        if (n == 2U && start[0] == '.' && start[1] == '.') {
            while (len > 1U && out[len - 1U] != '/')
                len--;
            if (len > 1U)
                len--;
            out[len] = '\0';
            continue;
        }
        if (len > 1U) {
            if (len + 1U >= PATH_MAX)
                return false;
            out[len++] = '/';
        }
        if (n >= PATH_MAX - len)
            return false;
        (void)memcpy(out + len, start, n);
        len += n;
        out[len] = '\0';
    }
    return true;
}

/* Existing files resolve in one realpath call.  For a new file, resolve the
 * deepest existing ancestor first, then normalize only the unresolved tail.
 * This preserves symlink semantics in the parent without requiring chdir. */
static bool self_canonical_absolute(const char *path, char out[PATH_MAX])
{
    char probe[PATH_MAX];
    char resolved[PATH_MAX];
    char joined[PATH_MAX];
    size_t path_len;

    if (path == NULL || path[0] != '/')
        return false;
    path_len = strlen(path);
    if (path_len >= sizeof(probe))
        return false;
    if (realpath(path, resolved) != NULL) {
        (void)snprintf(out, PATH_MAX, "%s", resolved);
        return true;
    }
    (void)memcpy(probe, path, path_len + 1U);
    for (;;) {
        char *slash = strrchr(probe, '/');
        size_t prefix_len;
        int n;

        if (slash == NULL)
            return false;
        if (slash == probe) {
            probe[1] = '\0';
            prefix_len = 1U;
        } else {
            prefix_len = (size_t)(slash - probe);
            *slash = '\0';
        }
        if (realpath(probe, resolved) != NULL) {
            const char *tail = path + prefix_len;

            if (strcmp(resolved, "/") == 0 && tail[0] == '/')
                tail++;
            if (strcmp(resolved, "/") == 0)
                n = snprintf(joined, sizeof(joined), "/%s", tail);
            else
                n = snprintf(joined, sizeof(joined), "%s%s", resolved,
                             tail);
            if (n < 0 || (size_t)n >= sizeof(joined))
                return false;
            return self_lexical_absolute(joined, out);
        }
        if (slash == probe)
            return false;
    }
}

static bool self_path_in_workspace(const Ed *ed, const char *path)
{
    const char *root = yew_ws_root(ed);
    size_t n;

    if (root == NULL || path == NULL || root[0] != '/' || path[0] != '/')
        return false;
    n = strlen(root);
    while (n > 1U && root[n - 1U] == '/')
        n--;
    if (strncmp(path, root, n) != 0)
        return false;
    if (n == 1U)
        return path[1] != '\0';
    return path[n] == '/' && path[n + 1U] != '\0';
}

static bool self_open_path(const Ed *ed, const char *operand,
                           char out[PATH_MAX], char *err, size_t errsz)
{
    char joined[PATH_MAX];
    const char *root;
    int n;

    if (operand[0] == '/') {
        n = snprintf(joined, sizeof(joined), "%s", operand);
    } else {
        root = yew_ws_root(ed);
        if (strcmp(root, "/") == 0)
            n = snprintf(joined, sizeof(joined), "/%s", operand);
        else
            n = snprintf(joined, sizeof(joined), "%s/%s", root, operand);
    }
    if (n < 0 || (size_t)n >= sizeof(joined) ||
        !self_canonical_absolute(joined, out)) {
        (void)snprintf(err, errsz, "self-open path is too long");
        return false;
    }
    return true;
}

YewShellSelfResult yew_shell_try_self_open(Ed *ed, const char *cmdline,
                                            char *err, size_t errsz)
{
    const char *at = cmdline;
    Bytebuf word;
    Bytebuf operand;
    SelfWordResult result;
    char path[PATH_MAX];
    struct stat st;
    bool separated = false;
    int existing;
    int idx;
    Buffer *created_buf;
    u32 buffer_count_before;
    u32 origin_tab_id;
    u32 gid;

    if (ed == NULL || cmdline == NULL)
        return YEW_SHELL_SELF_NOT_HANDLED;
    bytebuf_init(&word);
    bytebuf_init(&operand);
    result = self_word(&at, &word);
    if (result != SELF_WORD_OK) {
        bytebuf_free(&word);
        bytebuf_free(&operand);
        return YEW_SHELL_SELF_NOT_HANDLED;
    }
    bytebuf_push_u8(&word, 0U);
    if (strcmp((const char *)word.data, "yew") != 0)
        goto not_handled;
    result = self_word(&at, &operand);
    if (result == SELF_WORD_END) {
        (void)snprintf(err, errsz, ":!yew needs a file");
        bytebuf_free(&word);
        bytebuf_free(&operand);
        return YEW_SHELL_SELF_ERROR;
    }
    if (result != SELF_WORD_OK)
        goto not_handled;
    bytebuf_push_u8(&operand, 0U);
    if (strcmp((const char *)operand.data, "--") == 0) {
        operand.len = 0U;
        separated = true;
        result = self_word(&at, &operand);
        if (result == SELF_WORD_END) {
            (void)snprintf(err, errsz, ":!yew needs a file after --");
            bytebuf_free(&word);
            bytebuf_free(&operand);
            return YEW_SHELL_SELF_ERROR;
        }
        if (result != SELF_WORD_OK)
            goto not_handled;
        bytebuf_push_u8(&operand, 0U);
    }
    while (self_hspace(*at))
        at++;
    if (*at != '\0' || (!separated && operand.data[0] == (u8)'-'))
        goto not_handled;
    if (operand.len == 1U) {
        (void)snprintf(err, errsz, ":!yew needs a non-empty file");
        bytebuf_free(&word);
        bytebuf_free(&operand);
        return YEW_SHELL_SELF_ERROR;
    }
    if (!self_open_path(ed, (const char *)operand.data, path, err, errsz)) {
        bytebuf_free(&word);
        bytebuf_free(&operand);
        return YEW_SHELL_SELF_ERROR;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        goto not_handled;
    existing = yew_tab_find_by_path(ed, path);
    origin_tab_id = ed->tabs.active >= 0
                        ? ed->tabs.v.data[ed->tabs.active].tab_id
                        : 0U;
    gid = existing < 0 && self_path_in_workspace(ed, path)
              ? yew_group_for_path(ed, path)
              : 0U;
    buffer_count_before = ed->ws.nbufs;
    idx = yew_tab_open(ed, path);
    if (idx < 0) {
        int origin = yew_tab_index_of_id(ed, origin_tab_id);

        if (origin >= 0)
            yew_tab_switch(ed, origin);
        (void)snprintf(err, errsz, "could not open %s", path);
        bytebuf_free(&word);
        bytebuf_free(&operand);
        return YEW_SHELL_SELF_ERROR;
    }
    created_buf = existing < 0 ? yew_tab_buffer(ed, idx) : NULL;
    if (existing < 0 && yew_tab_hydrate(ed, idx) != 0) {
        int origin;

        (void)yew_tab_close(ed, idx);
        if (ed->ws.nbufs > buffer_count_before)
            yew_ws_scratch_drop(ed, created_buf);
        origin = yew_tab_index_of_id(ed, origin_tab_id);
        if (origin >= 0)
            yew_tab_switch(ed, origin);
        (void)snprintf(err, errsz, "could not open %s", path);
        bytebuf_free(&word);
        bytebuf_free(&operand);
        return YEW_SHELL_SELF_ERROR;
    }
    if (existing < 0 && gid != 0U)
        yew_group_add_member(ed, gid, idx);
    yew_tab_switch(ed, idx);
    bytebuf_free(&word);
    bytebuf_free(&operand);
    return YEW_SHELL_SELF_OPENED;

not_handled:
    bytebuf_free(&word);
    bytebuf_free(&operand);
    return YEW_SHELL_SELF_NOT_HANDLED;
}

u32 yew_shell_run(Ed *ed, const char *cmdline, bool focus, char *err,
                  size_t errsz)
{
    YewJobSpec spec = {0};
    char name[192];
    u32 id;
    YewJob *j;
    Buffer *buf;

    if (ed == NULL || cmdline == NULL)
        return 0U;
    spec.cmdline = cmdline;
    spec.sink = YEW_SINK_BUFFER;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    j = yew_job_find(ed, id);
    j->origin_buf_id = ed->win != NULL && ed->win->buf != NULL
                           ? ed->win->buf->id
                           : 0U;
    (void)snprintf(name, sizeof(name), "*job:%u %s*", (unsigned)id,
                   cmdline);
    buf = yew_ws_scratch_new(ed, name, YEW_BUF_NOUNDO);
    if (buf == NULL) {
        (void)yew_job_signal(ed, id, SIGTERM);
        (void)snprintf(err, errsz, "cannot create job buffer");
        return 0U;
    }
    j->buf = buf;
    if (focus)
        (void)yew_ed_show_buffer(ed, buf);
    return id;
}

/* ------------------------------------------------------------------ */
/* Mode (c): read output at the cursor (§6)                           */
/* ------------------------------------------------------------------ */

u32 yew_shell_read(Ed *ed, const char *cmdline, char *err, size_t errsz)
{
    YewJobSpec spec = {0};
    u32 id;
    YewJob *j;
    Buffer *b;
    const Cursor *cur;

    if (ed == NULL || cmdline == NULL || ed->win == NULL ||
        ed->win->buf == NULL || ed->win->cs.curs.len == 0U)
        return 0U;
    b = ed->win->buf;
    spec.cmdline = cmdline;
    spec.sink = YEW_SINK_COLLECT;
    id = yew_job_spawn(ed, &spec, err, errsz);
    if (id == 0U)
        return 0U;
    j = yew_job_find(ed, id);
    cur = &ed->win->cs.curs.data[ed->win->cs.primary];
    /* RIGHT bias so text typed at the insertion point stays before the
     * arriving output, and so editing elsewhere keeps the point correct. */
    j->at = yew_mark_add(b->marks, cur->pos, YEW_BIAS_RIGHT);
    j->has_mark = true;
    return id;
}

/* ------------------------------------------------------------------ */
/* Mode (b): the synchronous region filter (§5)                       */
/* ------------------------------------------------------------------ */

/* The filter's restricted loop.  NOT a nested event loop: terminal input
 * is buffered as typeahead and replayed afterwards, never dispatched —
 * dispatching would re-enter the editor with a half-applied transaction. */
static YewFilterResult filter_drive(Ed *ed, YewJob *j, Bytebuf *typeahead)
{
    for (;;) {
        /* The synchronous filter shares the process-wide job table with
         * background Git, LSP, AI, and index work.  Each job can contribute
         * four descriptors; the tty and signal pipe need two more. */
        struct pollfd pfd[YEW_JOB_MAX * 4U + 2U];
        u32 n = 0U;
        int tty_slot;
        int sig_slot;
        i64 now = yew_now_ms();
        i64 deadline = yew_job_deadline(ed, now);
        int rc;
        bool winch = false;
        bool cont = false;
        bool chld = false;

        /*
         * Exit only when the child is reaped AND its output has reached
         * EOF.  Breaking on reap alone silently truncated large filter
         * output — `cat` over 3.7 MB lost a few hundred KB, and the
         * filter still reported success, which is data loss wearing a
         * success message.  The timeout path still forces termination,
         * so a stuck writer cannot hang this loop.
         */
        if (!yew_job_pending(j))
            break;
        yew_job_collect_fds(ed, pfd, &n);
        tty_slot = -1;
        if (ed->tty_ready) {
            pfd[n].fd = ed->tty.rfd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            tty_slot = (int)n++;
        }
        sig_slot = -1;
        if (ed->tty_ready) {
            pfd[n].fd = yew_tty_signal_fd(&ed->tty);
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            sig_slot = (int)n++;
        }
        /* Cap the wait: once the child's pipes hit EOF there is nothing
         * left in the poll set, and without a tty there is no SIGCHLD
         * pipe here either — sleeping the full timeout would report a
         * finished `sort` as YEW_FILT_TIMEOUT.  A short tick keeps exit
         * detection prompt; this loop is synchronous, so it costs
         * nothing the user can perceive. */
        if (deadline < 0 || deadline > YEW_FILTER_TICK_MS)
            deadline = YEW_FILTER_TICK_MS;
        rc = poll(pfd, (nfds_t)n, (int)deadline);
        if (rc < 0 && errno != EINTR)
            return YEW_FILT_SPAWN;
        now = yew_now_ms();
        yew_job_pump(ed, pfd, n);
        if (sig_slot >= 0 && (pfd[sig_slot].revents & POLLIN) != 0) {
            yew_tty_drain_signals(&ed->tty, &winch, &cont, &chld);
            if (chld)
                yew_job_reap(ed);
            /* SIGWINCH is honored — resize and repaint — and the filter
             * keeps running.  A resize is not a reason to lose the run. */
            if (winch || cont)
                yew_ed_resize(ed, cont);
        }
        if (tty_slot >= 0 && (pfd[tty_slot].revents & POLLIN) != 0) {
            u8 bytes[1024];
            ssize_t got = read(ed->tty.rfd, bytes, sizeof(bytes));
            ssize_t i;

            for (i = 0; i < got; i++) {
                /* The only keys inspected are cancel keys, and they are
                 * consumed rather than replayed. */
                if (bytes[i] == 0x03U || bytes[i] == 0x1BU) {
                    (void)yew_job_signal(ed, j->id, SIGTERM);
                    j->state = YEW_JOB_CANCELLED;
                    continue;
                }
                bytebuf_push_u8(typeahead, bytes[i]);
            }
        }
        /* Reap before tick: a child that exits right as the deadline
         * lands must be recorded as EXITED, not lose the race and be
         * reported as a timeout it actually beat. */
        yew_job_reap(ed);
        yew_job_tick(ed, now);
    }
    switch (j->state) {
    case YEW_JOB_TIMEOUT:
        return YEW_FILT_TIMEOUT;
    case YEW_JOB_CANCELLED:
        return YEW_FILT_CANCELLED;
    case YEW_JOB_EXECFAIL:
        return YEW_FILT_SPAWN;
    case YEW_JOB_SIGNALED:
        return YEW_FILT_CANCELLED;
    case YEW_JOB_EXITED:
    default:
        break;
    }
    return j->exit_code == 0 ? YEW_FILT_OK : YEW_FILT_NONZERO;
}

YewFilterResult yew_shell_filter(Ed *ed, Win *w, Span region,
                                 const char *cmdline, Bytebuf *stderr_out)
{
    YewJobSpec spec = {0};
    Bytebuf typeahead;
    YewFilterResult result;
    u32 id;
    YewJob *j;
    EditCtx ec;
    u64 before_lines;
    u64 after_lines;
    char err[256];
    bool own_transaction;

    if (ed == NULL || w == NULL || w->buf == NULL || cmdline == NULL)
        return YEW_FILT_SPAWN;
    spec.cmdline = cmdline;
    spec.sink = YEW_SINK_COLLECT;
    spec.in_buf = w->buf->tb;
    spec.in_span = region;
    spec.timeout_ms = YEW_FILTER_TIMEOUT_MS;
    id = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (id == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        return YEW_FILT_SPAWN;
    }
    j = yew_job_find(ed, id);
    j->synchronous = true;
    bytebuf_init(&typeahead);
    result = filter_drive(ed, j, &typeahead);

    if (result == YEW_FILT_SPAWN && j->state == YEW_JOB_EXECFAIL)
        yew_msg(ed, YEW_MSG_ERROR, "cannot run %s: %s", cmdline,
                strerror(j->exec_errno));
    if (stderr_out != NULL && j->bytes_err != 0U)
        bytebuf_append(stderr_out, j->collect.data, j->collect.len);

    before_lines = yew_textbuf_line_count(w->buf->tb);
    if (result == YEW_FILT_OK) {
        ec = yew_ed_edit_ctx_for(ed, w);
        /* One transaction wrapping delete+insert, so a single undo
         * restores the original text exactly and the journal sees one
         * commit: a crash mid-filter recovers to pre- or post-, never
         * half. */
        own_transaction = ec.undo->depth == 0U;
        if (own_transaction)
            yew_undo_begin(&ec, YEW_TXN_FILTER);
        (void)yew_edit_delete(&ec, region);
        if (j->collect.len != 0U)
            (void)yew_edit_insert(&ec, BYTEOFF(region.lo), j->collect.data,
                                  (u64)j->collect.len);
        if (own_transaction)
            yew_undo_end(&ec);
        yew_ed_finish_edit(ed, &ec);
        after_lines = yew_textbuf_line_count(w->buf->tb);
        if (j->collect.len == 0U)
            /* Not rolled back — grep legitimately matches nothing — but
             * announced, because a silently vanished selection reads as
             * data loss. */
            yew_msg(ed, YEW_MSG_WARN,
                    "filter produced no output; region deleted (undo restores)");
        else
            yew_msg(ed, YEW_MSG_INFO, "filter: %llu → %llu lines",
                    (unsigned long long)before_lines,
                    (unsigned long long)after_lines);
        yew_ed_damage_document(ed);
    } else if (result == YEW_FILT_NONZERO) {
        yew_msg(ed, YEW_MSG_WARN, "filter: exit %d; buffer unchanged",
                j->exit_code);
    } else if (result == YEW_FILT_TIMEOUT) {
        yew_msg(ed, YEW_MSG_WARN, "filter timed out after %.1fs",
                (double)YEW_FILTER_TIMEOUT_MS / 1000.0);
    } else if (result == YEW_FILT_CANCELLED) {
        yew_msg(ed, YEW_MSG_WARN, "filter cancelled; buffer unchanged");
    }

    /* Typed-ahead bytes were never dispatched; replay them now. */
    if (typeahead.len != 0U && ed->probe_seeded)
        yew_input_feed(&ed->in, typeahead.data, typeahead.len);
    bytebuf_free(&typeahead);
    return result;
}

/* ------------------------------------------------------------------ */
/* The *jobs* table (§8)                                              */
/* ------------------------------------------------------------------ */

static void jobs_table_render(Ed *ed, Buffer *b)
{
    Bytebuf out;
    EditCtx ec;
    u64 old_len;
    u32 i;

    bytebuf_init(&out);
    bytebuf_printf(&out, " id  state     code  elapsed    bytes  command\n");
    /* Newest first: the job you just started is the one you are looking
     * for. */
    for (i = ed->jobs.len; i-- > 0U;) {
        const YewJob *j = &ed->jobs.v[i];
        char elapsed[32];
        char code[16];
        i64 end = j->state == YEW_JOB_RUNNING ? yew_now_ms() : j->end_ms;

        if (j->internal)
            continue;

        fmt_elapsed(elapsed, sizeof(elapsed), end - j->start_ms);
        if (j->state == YEW_JOB_RUNNING)
            (void)snprintf(code, sizeof(code), "-");
        else if (j->state == YEW_JOB_SIGNALED)
            (void)snprintf(code, sizeof(code), "%s",
                           yew_signame(j->termsig));
        else
            (void)snprintf(code, sizeof(code), "%d", j->exit_code);
        bytebuf_printf(&out, "%3u  %-9s %4s  %7s  %7llu  %s\n",
                       (unsigned)j->id, yew_job_state_name(j->state), code,
                       elapsed,
                       (unsigned long long)(j->bytes_out + j->bytes_err),
                       j->label);
    }
    /* Replace wholesale through the edit chokepoint.  The table is derived
     * state, but its syntax cache, symbol index, marks, and visible cursor
     * still have to observe the replacement. */
    ec = buf_raw_edit_ctx(ed, b);
    old_len = yew_textbuf_len(b->tb);
    if (old_len != 0U)
        (void)yew_edit_delete(&ec, (Span){0U, old_len});
    if (out.len != 0U)
        (void)yew_edit_insert(&ec, BYTEOFF(0U), out.data, (u64)out.len);
    bytebuf_free(&out);
}

void yew_jobs_table_refresh(Ed *ed)
{
    Buffer *b;

    if (ed == NULL)
        return;
    b = yew_ws_scratch_find(ed, YEW_JOBS_TABLE_NAME);
    if (b == NULL)
        return;
    jobs_table_render(ed, b);
    if (ed->win != NULL && ed->win->buf == b) {
        yew_vp_clamp(ed->win);
        yew_ed_damage_document(ed);
    }
}

Buffer *yew_jobs_table_open(Ed *ed)
{
    Buffer *b;

    if (ed == NULL)
        return NULL;
    b = yew_ws_scratch_find(ed, YEW_JOBS_TABLE_NAME);
    if (b == NULL)
        b = yew_ws_scratch_new(ed, YEW_JOBS_TABLE_NAME, YEW_BUF_NOUNDO);
    if (b == NULL)
        return NULL;
    jobs_table_render(ed, b);
    (void)yew_ed_show_buffer(ed, b);
    return b;
}

u32 yew_jobs_table_row_id(Ed *ed)
{
    Buffer *b;
    LineNo line;
    Span span;
    TextIter it;
    const u8 *chunk = NULL;
    u64 len = 0U;
    unsigned id = 0U;
    char row[32];
    size_t n = 0U;

    if (ed == NULL || ed->win == NULL)
        return 0U;
    b = ed->win->buf;
    if (b == NULL || b->name == NULL ||
        strcmp(b->name, YEW_JOBS_TABLE_NAME) != 0 ||
        ed->win->cs.curs.len == 0U)
        return 0U;
    line = yew_textbuf_line_of(b->tb,
                               ed->win->cs.curs.data[ed->win->cs.primary].pos);
    if (line.v == 0U)
        return 0U; /* the header row addresses no job */
    span = yew_textbuf_line_span(b->tb, line);
    if (!yew_textiter_begin(&it, b->tb, BYTEOFF(span.lo)))
        return 0U;
    while (n + 1U < sizeof(row) &&
           yew_textiter_chunk(&it, b->tb, &chunk, &len) && len != 0U) {
        size_t room = sizeof(row) - 1U - n;
        size_t take = len > (u64)room ? room : (size_t)len;
        size_t k;

        for (k = 0U; k < take; k++)
            row[n++] = (char)chunk[k];
        break;
    }
    row[n] = '\0';
    if (sscanf(row, "%u", &id) != 1)
        return 0U;
    return (u32)id;
}
