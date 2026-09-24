/*
 * Sprint 57.27: the persistent shell session.
 *
 * Every test runs a real shell -- a fixture $SHELL, never the user's,
 * under the runner's own empty HOME (so no .zshenv, no history) -- and
 * drives it the way yew_loop_run does: poll, pump, reap, tick, settle,
 * and the session's own settle.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "edit/shell.h"
#include "edit/shsession.h"
#include "text/piece.h"
#include "ui/cmdline.h"
#include "ui/cmdparse.h"
#include "ui/message.h"
#include "ui/layout.h"

typedef struct ShFix {
    Ed ed;
    char root[PATH_MAX];
    char *saved_shell;
    bool had_shell;
} ShFix;

static int sh_rm_one(const char *path, const struct stat *st, int flag,
                     struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return flag == FTW_DP ? rmdir(path) : unlink(path);
}

static void sh_fix_make(ShFix *f, const char *shell)
{
    const char *tmp = getenv("TMPDIR");
    char made[PATH_MAX];
    const char *old = getenv("SHELL");
    int n;

    (void)memset(f, 0, sizeof(*f));
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(made, sizeof(made), "%s/yew-shs-XXXXXX", tmp);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(made));
    YEW_ASSERT_NOT_NULL(mkdtemp(made));
    /* The physical spelling, so every cwd the shell reports compares
     * byte-for-byte with what the test builds. */
    YEW_ASSERT_NOT_NULL(realpath(made, f->root));
    f->had_shell = old != NULL;
    f->saved_shell = old != NULL ? strdup(old) : NULL;
    YEW_ASSERT_EQ_I64(setenv("SHELL", shell, 1), 0);
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_ed_set_workspace_root(&f->ed, f->root));
    yew_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void sh_fix_free(ShFix *f)
{
    yew_ed_free(&f->ed);
    yew_cmd_shutdown();
    if (f->had_shell)
        YEW_ASSERT_EQ_I64(setenv("SHELL", f->saved_shell, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("SHELL"), 0);
    free(f->saved_shell);
    (void)nftw(f->root, sh_rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

static void sh_path(const ShFix *f, const char *rel, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", f->root, rel);

    YEW_ASSERT(n > 0 && (size_t)n < cap);
}

/* One turn of the event loop's job half. */
static void sh_step(Ed *ed, int wait_ms)
{
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    u32 n = 0U;

    yew_job_collect_fds(ed, pfd, &n);
    if (n != 0U)
        (void)poll(pfd, (nfds_t)n, wait_ms);
    else
        (void)poll(NULL, 0U, wait_ms < 5 ? wait_ms : 5);
    ed->now_ms = yew_now_ms();
    yew_job_pump(ed, pfd, n);
    yew_job_reap(ed);
    yew_job_tick(ed, yew_now_ms());
    (void)yew_job_settle(ed);
    yew_shsession_settle(ed);
}

/* Until job `id` is finished and settled.  False after 10 s. */
static bool sh_wait(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    for (;;) {
        YewJob *j = yew_job_find(ed, id);

        if (j == NULL || j->drained)
            return j != NULL;
        sh_step(ed, 20);
        if (yew_now_ms() - start > 10000)
            return false;
    }
}

/* Until the session has no shell at all, live or dying. */
static bool sh_wait_ended(Ed *ed)
{
    i64 start = yew_now_ms();

    for (;;) {
        u32 i;
        bool internal = false;

        for (i = 0U; i < ed->jobs.len; i++)
            internal = internal || ed->jobs.v[i].internal;
        if (yew_shsession_job(ed) == 0U && !internal)
            return true;
        sh_step(ed, 20);
        if (yew_now_ms() - start > 10000)
            return false;
    }
}

/* Runs `cmd` as a plain `:!` without focusing its buffer. */
static u32 sh_start(ShFix *f, const char *cmd)
{
    char err[256] = {0};
    u32 id = yew_shell_run(&f->ed, cmd, false, err, sizeof(err));

    if (id == 0U)
        (void)fprintf(stderr, "sh_start(%s): %s\n", cmd, err);
    YEW_ASSERT(id != 0U);
    return id;
}

/* The job buffer's text, up to (not including) the footer line. */
static char *sh_output(ShFix *f, u32 id)
{
    YewJob *j = yew_job_find(&f->ed, id);
    TextIter it;
    const u8 *chunk;
    u64 len;
    Bytebuf out;
    char *s;
    char *foot;

    YEW_ASSERT_NOT_NULL(j);
    bytebuf_init(&out);
    if (j->buf != NULL && yew_textbuf_len(j->buf->tb) != 0U &&
        yew_textiter_begin(&it, j->buf->tb, BYTEOFF(0U))) {
        do {
            if (!yew_textiter_chunk(&it, j->buf->tb, &chunk, &len))
                break;
            bytebuf_append(&out, chunk, (size_t)len);
        } while (yew_textiter_advance(&it, j->buf->tb));
    }
    bytebuf_push_u8(&out, 0U);
    s = (char *)out.data;
    /* Output then "\n[...]\n": cut at the footer's bracket line. */
    foot = strrchr(s, '[');
    if (foot != NULL && (foot == s || foot[-1] == '\n'))
        *foot = '\0';
    return s;
}

/* Runs `cmd`, waits, and returns its output (caller frees). */
static char *sh_do(ShFix *f, const char *cmd)
{
    u32 id = sh_start(f, cmd);

    YEW_ASSERT(sh_wait(&f->ed, id));
    return sh_output(f, id);
}

static void sh_expect(ShFix *f, const char *cmd, const char *want)
{
    char *got = sh_do(f, cmd);

    YEW_ASSERT_EQ_STR(got, want);
    free(got);
}

static void sh_set(ShFix *f, const char *name, const char *value)
{
    OptVal v;
    const char *err = NULL;

    (void)memset(&v, 0, sizeof(v));
    v.type = YEW_OPT_ENUM;
    v.as.str.s = value;
    v.as.str.len = (u32)strlen(value);
    YEW_ASSERT(yew_opt_set(&f->ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                           &v, &err));
}

static CmdStatus sh_line(ShFix *f, const char *line)
{
    Arena scratch;
    CmdParse parsed;
    YewCmdInvoke invoke;
    CmdStatus status;

    arena_init(&scratch);
    if (!yew_cmd_parse(&f->ed, line, strlen(line), &scratch, &parsed)) {
        arena_free_all(&scratch);
        return YEW_CMD_ERR_ARG;
    }
    invoke = (YewCmdInvoke){parsed.range, parsed.argv, 0, parsed.bang,
                            f->ed.win};
    status = yew_ed_invoke_parsed(&f->ed, parsed.command, &invoke);
    arena_free_all(&scratch);
    return status;
}

static bool sh_env_has(Ed *ed, const char *row)
{
    const char *const *env = yew_shsession_env(ed);
    size_t i;

    for (i = 0U; env != NULL && env[i] != NULL; i++) {
        if (strcmp(env[i], row) == 0)
            return true;
    }
    return false;
}

static bool sh_env_names(Ed *ed, const char *name)
{
    const char *const *env = yew_shsession_env(ed);
    size_t n = strlen(name);
    size_t i;

    for (i = 0U; env != NULL && env[i] != NULL; i++) {
        if (strncmp(env[i], name, n) == 0 && env[i][n] == '=')
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                        */
/* ------------------------------------------------------------------ */

/* DoD 1: `:!cd sub` then `:!pwd` prints sub; the state is exposed. */
void test_shsession_cd_persists(void)
{
    ShFix f;
    char sub[PATH_MAX];
    char want[PATH_MAX + 2];
    u32 shell;

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "sub", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    /* Before the first command: the workspace root and environ. */
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), f.root);
    YEW_ASSERT_NULL(yew_shsession_env(&f.ed));
    YEW_ASSERT(!yew_shsession_busy(&f.ed));

    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd sub")));
    shell = yew_shsession_job(&f.ed);
    YEW_ASSERT(shell != 0U);
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), sub);
    (void)snprintf(want, sizeof(want), "%s\n", sub);
    sh_expect(&f, "pwd", want);
    /* One shell served both. */
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), shell);
    YEW_ASSERT(sh_env_has(&f.ed, "PWD=") == false);
    {
        char row[PATH_MAX + 8];

        (void)snprintf(row, sizeof(row), "PWD=%s", sub);
        YEW_ASSERT(sh_env_has(&f.ed, row));
    }
    sh_fix_free(&f);
}

/* Exports and functions persist; an unexported variable persists inside
 * the session but is not in the environment the other forms inherit. */
void test_shsession_exports_functions_and_locals(void)
{
    ShFix f;

    sh_fix_make(&f, "/bin/sh");
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "export X=1")));
    sh_expect(&f, "echo \"x=$X\"", "x=1\n");
    YEW_ASSERT(sh_env_has(&f.ed, "X=1"));

    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "f() { echo hi; }")));
    sh_expect(&f, "f", "hi\n");

    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "L=local")));
    sh_expect(&f, "echo \"l=$L\"", "l=local\n");
    YEW_ASSERT(!sh_env_names(&f.ed, "L"));
    /* The frame's own variable never leaks into the user's exports, and
     * `_` (the helper's path) is not a row. */
    YEW_ASSERT(!sh_env_names(&f.ed, "_"));
    YEW_ASSERT(!sh_env_names(&f.ed, "__yew_w"));
    sh_fix_free(&f);
}

/* The job layer's per-command rows follow the caret into the session:
 * the shell started with YEW_LINE=1, but each `:!` sees where it is. */
void test_shsession_job_rows_are_per_command(void)
{
    ShFix f;
    EditCtx ec;

    sh_fix_make(&f, "/bin/sh");
    ec = yew_ed_edit_ctx(&f.ed);
    yew_undo_begin(&ec, YEW_TXN_TYPE);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"a\nb\nc\n",
                               6U));
    yew_undo_end(&ec);
    yew_ed_finish_edit(&f.ed, &ec);
    f.ed.win->cs.curs.data[f.ed.win->cs.primary].pos = BYTEOFF(0U);
    sh_expect(&f, "echo \"$YEW_LINE\"", "1\n");
    f.ed.win->cs.curs.data[f.ed.win->cs.primary].pos = BYTEOFF(4U);
    f.ed.win->cs.curs.data[f.ed.win->cs.primary].anchor = BYTEOFF(4U);
    sh_expect(&f, "echo \"$YEW_LINE $YEW_COL\"", "3 1\n");
    sh_fix_free(&f);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* `exit` ends the session with the message; the next `:!` starts a new
 * one in the LAST directory. */
void test_shsession_exit_recovers_last_directory(void)
{
    ShFix f;
    char sub[PATH_MAX];
    char want[PATH_MAX + 2];
    u32 first;
    u32 id;

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "sub", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd sub; export KEEP=yes")));
    first = yew_shsession_job(&f.ed);
    id = sh_start(&f, "exit 3");
    YEW_ASSERT(sh_wait(&f.ed, id));
    YEW_ASSERT(sh_wait_ended(&f.ed));
    YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->exit_code, 3);
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "shell session ended; the next :! starts a new one");
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), 0U);
    /* The state survives the shell. */
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), sub);

    (void)snprintf(want, sizeof(want), "%s yes\n", sub);
    sh_expect(&f, "echo \"$PWD $KEEP\"", want);
    YEW_ASSERT(yew_shsession_job(&f.ed) != 0U);
    YEW_ASSERT(yew_shsession_job(&f.ed) != first);
    sh_fix_free(&f);
}

/* ed.shell.reset (`:shreset`) restarts from the WORKSPACE root and the
 * standard environment. */
void test_shsession_reset_returns_to_workspace_root(void)
{
    ShFix f;
    char sub[PATH_MAX];
    char want[PATH_MAX + 2];

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "sub", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd sub; export GONE=1")));
    YEW_ASSERT_EQ_I64(sh_line(&f, "shreset"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), 0U);
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), f.root);
    YEW_ASSERT_NULL(yew_shsession_env(&f.ed));
    YEW_ASSERT(sh_wait_ended(&f.ed));
    (void)snprintf(want, sizeof(want), "%s -\n", f.root);
    sh_expect(&f, "echo \"$(pwd) ${GONE:--}\"", want);
    sh_fix_free(&f);
}

/* `shell.session = fresh` is Sprint 19's `$SHELL -c` per command: no
 * session, nothing carries, and switching ends a running session. */
void test_shsession_fresh_is_sprint19(void)
{
    ShFix f;
    char sub[PATH_MAX];
    char want[PATH_MAX + 2];
    u32 id;

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "sub", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd sub")));
    YEW_ASSERT(yew_shsession_job(&f.ed) != 0U);
    sh_set(&f, "shell.session", "fresh");
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), 0U);
    YEW_ASSERT(sh_wait_ended(&f.ed));
    /* The session's state is not consulted under `fresh`. */
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), f.root);
    YEW_ASSERT_NULL(yew_shsession_env(&f.ed));

    id = sh_start(&f, "cd sub");
    YEW_ASSERT(yew_job_find(&f.ed, id)->pid > 0);
    YEW_ASSERT(sh_wait(&f.ed, id));
    (void)snprintf(want, sizeof(want), "%s\n", f.root);
    sh_expect(&f, "pwd", want);
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), 0U);
    /* And stderr is its own pipe again, as Sprint 19 counts it. */
    id = sh_start(&f, "echo e >&2");
    YEW_ASSERT(sh_wait(&f.ed, id));
    YEW_ASSERT_EQ_U64(yew_job_find(&f.ed, id)->bytes_err, 2U);
    YEW_ASSERT_EQ_U64(yew_job_find(&f.ed, id)->bytes_out, 0U);

    /* Back to persistent: the next session starts where the last one
     * was (the end path keeps its state). */
    sh_set(&f, "shell.session", "persistent");
    (void)snprintf(want, sizeof(want), "%s\n", sub);
    sh_expect(&f, "pwd", want);
    sh_fix_free(&f);
}

/* A `fish` $SHELL is not sh-family: no session, one message, and every
 * `:!` runs fresh through it (the fixture "fish" is /bin/sh). */
void test_shsession_non_sh_shell_falls_back_to_fresh(void)
{
    ShFix f;
    char fish[PATH_MAX];
    char want[PATH_MAX + 2];
    u32 id;

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "fish", fish, sizeof(fish));
    YEW_ASSERT_EQ_I64(symlink("/bin/sh", fish), 0);
    YEW_ASSERT_EQ_I64(setenv("SHELL", fish, 1), 0);
    YEW_ASSERT(yew_shsession_wanted(&f.ed) == false);
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "shell session needs a POSIX-family $SHELL; running "
                      "each :! fresh");
    yew_msg_clear(&f.ed);
    YEW_ASSERT(!yew_shsession_wanted(&f.ed));
    YEW_ASSERT(!f.ed.msg.active);

    id = sh_start(&f, "cd /");
    YEW_ASSERT(yew_job_find(&f.ed, id)->pid > 0);
    YEW_ASSERT(sh_wait(&f.ed, id));
    (void)snprintf(want, sizeof(want), "%s\n", f.root);
    sh_expect(&f, "pwd", want);
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), 0U);
    sh_fix_free(&f);
}

/* ------------------------------------------------------------------ */
/* Framing robustness                                                 */
/* ------------------------------------------------------------------ */

/* Appends a printf(1) format that prints `bytes` exactly. */
static void sh_printf_bytes(Bytebuf *out, const u8 *bytes, size_t n)
{
    size_t i;

    bytebuf_append(out, "printf '", 8U);
    for (i = 0U; i < n; i++)
        bytebuf_printf(out, "\\%03o", (unsigned)bytes[i]);
    bytebuf_append(out, "'", 1U);
}

/*
 * DoD 3: output shaped exactly like a marker -- 0x1e, 32 hex digits, the
 * frame's sequence, a status -- but with the wrong nonce is output, and
 * the frame ends at the real marker.
 */
void test_shsession_forged_marker_is_output(void)
{
    static const char forged[] =
        "\036" "0123456789abcdef0123456789abcdef-1 0\036"
        "\036" "0123456789abcdef0123456789abcdef-1.\036";
    ShFix f;
    Bytebuf cmd;
    char *got;
    u32 id;

    sh_fix_make(&f, "/bin/sh");
    bytebuf_init(&cmd);
    sh_printf_bytes(&cmd, (const u8 *)forged, sizeof(forged) - 1U);
    bytebuf_append(&cmd, "; echo after; exit_is_not=1; false", 34U);
    bytebuf_push_u8(&cmd, 0U);
    id = sh_start(&f, (const char *)cmd.data);
    YEW_ASSERT(sh_wait(&f.ed, id));
    got = sh_output(&f, id);
    YEW_ASSERT_EQ_U64(strlen(got), sizeof(forged) - 1U + 6U);
    YEW_ASSERT_EQ_MEM(got, forged, sizeof(forged) - 1U);
    YEW_ASSERT_EQ_STR(got + sizeof(forged) - 1U, "after\n");
    YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->state, YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->exit_code, 1);
    free(got);
    bytebuf_free(&cmd);
    sh_expect(&f, "echo next", "next\n");
    sh_fix_free(&f);
}

/*
 * DoD 3: a syntax error is eval's error, not the session's -- without the
 * eval/single-quote frame, `echo "unterminated` makes the shell read every
 * later frame as the rest of the string and the session hangs.
 */
void test_shsession_syntax_error_cannot_swallow_frames(void)
{
    static const char *const bad[] = {"echo \"unterminated", "if true",
                                      "echo 'half", "a)", "echo $(", "fi"};
    ShFix f;
    size_t i;
    u32 shell;

    sh_fix_make(&f, "/bin/sh");
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "true")));
    shell = yew_shsession_job(&f.ed);
    for (i = 0U; i < YEW_ARRAY_LEN(bad); i++) {
        u32 id = sh_start(&f, bad[i]);

        YEW_ASSERT(sh_wait(&f.ed, id));
        YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->state, YEW_JOB_EXITED);
        YEW_ASSERT(yew_job_find(&f.ed, id)->exit_code != 0);
        sh_expect(&f, "echo still", "still\n");
    }
    /* Never a restart: the same shell read every frame. */
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), shell);
    sh_fix_free(&f);
}

/* Output without a trailing newline, and 1 MiB of it, arrive exactly. */
void test_shsession_output_shapes(void)
{
    ShFix f;
    char *got;
    u32 id;
    size_t i;

    sh_fix_make(&f, "/bin/sh");
    sh_expect(&f, "printf abc", "abc\n");
    id = sh_start(&f, "head -c 1048576 /dev/zero | tr '\\0' a");
    YEW_ASSERT(sh_wait(&f.ed, id));
    YEW_ASSERT_EQ_U64(yew_job_find(&f.ed, id)->bytes_out, 1048576U);
    got = sh_output(&f, id);
    YEW_ASSERT_EQ_U64(strlen(got), 1048577U);
    for (i = 0U; i < 1048576U; i++) {
        if (got[i] != 'a')
            break;
    }
    YEW_ASSERT_EQ_U64(i, 1048576U);
    free(got);
    /* Invalid UTF-8 and a lone 0x1e are bytes like any other. */
    sh_expect(&f, "printf 'x\\377\\036y\\n'", "x\377\036y\n");
    sh_fix_free(&f);
}

/*
 * DoD 3 / §1 (b): a directory and a value holding a newline, 0x1e and a
 * byte >= 0x80 come back exactly.  A newline-separated transport would
 * read "a" as the directory and "b\377" as an env row.
 */
void test_shsession_state_survives_any_bytes(void)
{
    ShFix f;
    char path[PATH_MAX];
    char row[64];
    const char *cd = "cd \"$(printf 'a\\nb\\377\\036c')\"";

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "a\nb\377\036c", path, sizeof(path));
    if (mkdir(path, 0700) != 0) {
        /* APFS refuses a name that is not UTF-8; a valid multibyte name
         * still carries bytes >= 0x80 (and the value below keeps \377). */
        YEW_ASSERT_EQ_I64(errno, EILSEQ);
        sh_path(&f, "a\nb\303\251\036c", path, sizeof(path));
        YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
        cd = "cd \"$(printf 'a\\nb\\303\\251\\036c')\"";
    }
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, cd)));
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), path);
    YEW_ASSERT(sh_wait(&f.ed,
                       sh_start(&f, "export Z=\"$(printf 'x\\ny\\377\\036=')\"")));
    (void)snprintf(row, sizeof(row), "Z=x\ny\377\036=");
    YEW_ASSERT(sh_env_has(&f.ed, row));
    YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), path);
    sh_fix_free(&f);
}

/*
 * One turn in which the session shell's output is read HERE and fed to
 * its parser `chunk` bytes at a time -- every marker, status, header and
 * record split at every possible offset.  `raw` collects what was read.
 */
static void sh_step_split(Ed *ed, size_t chunk, Bytebuf *raw)
{
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    u32 n = 0U;
    u32 k;
    YewJob *sj = yew_job_find(ed, yew_shsession_job(ed));

    yew_job_collect_fds(ed, pfd, &n);
    (void)poll(pfd, (nfds_t)n, 20);
    if (sj != NULL && sj->out_fd >= 0) {
        u8 buf[4096];
        ssize_t got = read(sj->out_fd, buf, sizeof(buf));

        if (got > 0 || (got < 0 && (errno == EAGAIN || errno == EINTR))) {
            for (k = 0U; k < n; k++) {
                if (pfd[k].fd == sj->out_fd)
                    pfd[k].revents = 0;
            }
        }
        if (got > 0) {
            size_t off;

            bytebuf_append(raw, buf, (size_t)got);
            for (off = 0U; off < (size_t)got; off += chunk) {
                size_t take = (size_t)got - off < chunk ? (size_t)got - off
                                                        : chunk;

                YEW_ASSERT(sj->framed_ops->feed_stdout(sj->framed_owner,
                                                       buf + off, take));
            }
        }
    }
    yew_job_pump(ed, pfd, n);
    yew_job_reap(ed);
    yew_job_tick(ed, yew_now_ms());
    (void)yew_job_settle(ed);
    yew_shsession_settle(ed);
}

static bool sh_wait_split(Ed *ed, u32 id, size_t chunk, Bytebuf *raw)
{
    i64 start = yew_now_ms();

    for (;;) {
        YewJob *j = yew_job_find(ed, id);

        if (j == NULL || j->drained)
            return j != NULL;
        sh_step_split(ed, chunk, raw);
        if (yew_now_ms() - start > 10000)
            return false;
    }
}

/* The stream's first `\036<nonce>-<seq> ` marker for `seq`, or NULL. */
static const u8 *sh_find_mark(const Bytebuf *raw, unsigned seq, size_t *len)
{
    size_t i;
    char tail[24];
    int n = snprintf(tail, sizeof(tail), "-%u ", seq);

    YEW_ASSERT(n > 0 && (size_t)n < sizeof(tail));
    for (i = 0U; i + 34U + (size_t)n <= raw->len; i++) {
        if (raw->data[i] == 0x1eU &&
            memcmp(raw->data + i + 33U, tail, (size_t)n) == 0) {
            *len = 33U + (size_t)n;
            return raw->data + i;
        }
    }
    return NULL;
}

/*
 * Output split across many reads -- one byte, then seven, at a time --
 * frames exactly as one read does; and a REPLAY of an earlier frame's
 * real marker (right nonce, wrong sequence) is output, not an end.
 */
void test_shsession_split_reads_and_replayed_markers(void)
{
    static const size_t chunks[] = {1U, 7U};
    ShFix f;
    size_t c;

    sh_fix_make(&f, "/bin/sh");
    for (c = 0U; c < YEW_ARRAY_LEN(chunks); c++) {
        Bytebuf raw;
        Bytebuf cmd;
        const u8 *mark;
        size_t mlen = 0U;
        char *got;
        u32 id;
        char sub[PATH_MAX];
        char name[16];
        u8 replay[64];

        bytebuf_init(&raw);
        bytebuf_init(&cmd);
        (void)snprintf(name, sizeof(name), "d%u", (unsigned)c);
        sh_path(&f, name, sub, sizeof(sub));
        YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
        bytebuf_printf(&cmd, "printf 'one\\036two'; cd '%s'; echo; echo x",
                       sub);
        bytebuf_push_u8(&cmd, 0U);
        id = sh_start(&f, (const char *)cmd.data);
        YEW_ASSERT(sh_wait_split(&f.ed, id, chunks[c], &raw));
        got = sh_output(&f, id);
        YEW_ASSERT_EQ_STR(got, "one\036two\nx\n");
        free(got);
        YEW_ASSERT_EQ_STR(yew_shsession_cwd(&f.ed), sub);

        /* Replay this frame's own status marker in the next frame. */
        mark = sh_find_mark(&raw, (unsigned)(2U * c + 1U), &mlen);
        YEW_ASSERT_NOT_NULL(mark);
        YEW_ASSERT(mlen <= sizeof(replay));
        (void)memcpy(replay, mark, mlen);
        mark = replay;
        cmd.len = 0U;
        sh_printf_bytes(&cmd, mark, mlen);
        bytebuf_append(&cmd, "; printf '0\\036'; echo tail", 27U);
        bytebuf_push_u8(&cmd, 0U);
        raw.len = 0U;
        id = sh_start(&f, (const char *)cmd.data);
        YEW_ASSERT(sh_wait_split(&f.ed, id, chunks[c], &raw));
        got = sh_output(&f, id);
        YEW_ASSERT_EQ_U64(strlen(got), mlen + 2U + 5U);
        YEW_ASSERT_EQ_MEM(got, mark, mlen);
        YEW_ASSERT_EQ_STR(got + mlen, "0\036tail\n");
        free(got);
        bytebuf_free(&raw);
        bytebuf_free(&cmd);
    }
    sh_fix_free(&f);
}

/* ------------------------------------------------------------------ */
/* Cancel                                                             */
/* ------------------------------------------------------------------ */

/* Until job `id` has written something: the command is running. */
static void sh_wait_output(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    while (yew_job_find(ed, id) != NULL &&
           yew_job_find(ed, id)->bytes_out == 0U) {
        sh_step(ed, 20);
        YEW_ASSERT(yew_now_ms() - start < 10000);
    }
}

/*
 * Cancel until the frame completes.  One cancel is enough; a second is
 * sent only past the escalation window (never escalating), for the rare
 * SIGINT that lands between `echo started` and the fork of `sleep`.
 */
static void sh_cancel_until_done(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();
    i64 last = 0;

    for (;;) {
        YewJob *j = yew_job_find(ed, id);
        i64 now = yew_now_ms();

        YEW_ASSERT_NOT_NULL(j);
        if (j->drained)
            return;
        if (j->state == YEW_JOB_RUNNING &&
            (last == 0 || now - last > YEW_SHSESSION_ESCALATE_MS + 500)) {
            YEW_ASSERT(yew_job_signal(ed, id, SIGTERM));
            last = now;
        }
        sh_step(ed, 20);
        YEW_ASSERT(now - start < 20000);
    }
}

/*
 * The contract's pitfall, verified per shell: with `trap : INT` in the
 * prologue, a SIGINT to the session's group kills the command and the
 * REST OF THE FRAME LINE still runs -- the status marker arrives with
 * 128+2 and the same shell reads the next frame.  Every sh-family shell
 * installed here is exercised (zsh, bash, dash, ksh, /bin/sh).
 */
void test_shsession_cancel_keeps_the_session_on_every_shell(void)
{
    static const char *const shells[] = {
        "/bin/sh",  "/bin/bash", "/usr/bin/bash", "/opt/homebrew/bin/bash",
        "/bin/zsh", "/usr/bin/zsh", "/bin/dash", "/usr/bin/dash",
        "/bin/ksh", "/usr/bin/ksh"};
    char seen[YEW_ARRAY_LEN(shells)][PATH_MAX];
    size_t n_seen = 0U;
    size_t i;
    u32 tested = 0U;

    for (i = 0U; i < YEW_ARRAY_LEN(shells); i++) {
        char real[PATH_MAX];
        size_t k;
        bool dup = false;
        ShFix f;
        u32 shell;
        u32 id;
        YewJob *j;

        if (access(shells[i], X_OK) != 0 || realpath(shells[i], real) == NULL)
            continue;
        for (k = 0U; k < n_seen; k++)
            dup = dup || strcmp(seen[k], real) == 0;
        /* /bin/sh is often another shell by name; the name decides the
         * shell's mode, so only an identical PATH is a duplicate. */
        if (dup && strcmp(shells[i], "/bin/sh") != 0)
            continue;
        (void)memcpy(seen[n_seen++], real, sizeof(real));
        sh_fix_make(&f, shells[i]);
        YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "true")));
        shell = yew_shsession_job(&f.ed);
        id = sh_start(&f, "echo started; sleep 30; echo not-reached");
        sh_wait_output(&f.ed, id);
        sh_cancel_until_done(&f.ed, id);
        j = yew_job_find(&f.ed, id);
        if (j->state != YEW_JOB_SIGNALED || j->termsig != SIGINT)
            (void)fprintf(stderr, "cancel on %s: state %d code %d sig %d\n",
                          shells[i], (int)j->state, j->exit_code,
                          j->termsig);
        YEW_ASSERT_EQ_I64(j->state, YEW_JOB_SIGNALED);
        YEW_ASSERT_EQ_I64(j->termsig, SIGINT);
        {
            char *got = sh_output(&f, id);

            YEW_ASSERT_EQ_STR(got, "started\n");
            free(got);
        }
        /* The session survived: same shell, next frame runs. */
        YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), shell);
        sh_expect(&f, "echo after", "after\n");
        YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), shell);
        sh_fix_free(&f);
        tested++;
    }
    YEW_ASSERT(tested >= 1U);
}

/* A command that ignores INT cannot wedge the session: the second cancel
 * inside the window SIGKILLs the group and ends it; the next `:!` gets a
 * new shell. */
void test_shsession_double_cancel_ends_the_session(void)
{
    ShFix f;
    u32 shell;
    u32 id;

    sh_fix_make(&f, "/bin/sh");
    id = sh_start(&f, "trap '' INT; echo started; sleep 30");
    sh_wait_output(&f.ed, id);
    shell = yew_shsession_job(&f.ed);
    YEW_ASSERT(yew_job_signal(&f.ed, id, SIGTERM));
    sh_step(&f.ed, 50);
    YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->state, YEW_JOB_RUNNING);
    YEW_ASSERT(yew_job_signal(&f.ed, id, SIGTERM));
    YEW_ASSERT_EQ_U64(yew_shsession_job(&f.ed), 0U);
    YEW_ASSERT(sh_wait(&f.ed, id));
    YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->state, YEW_JOB_CANCELLED);
    YEW_ASSERT(sh_wait_ended(&f.ed));
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "shell session ended; the next :! starts a new one");
    sh_expect(&f, "echo fresh-shell", "fresh-shell\n");
    YEW_ASSERT(yew_shsession_job(&f.ed) != shell);

    /* kill_force is the immediate route. */
    id = sh_start(&f, "sleep 30");
    YEW_ASSERT(yew_job_signal(&f.ed, id, SIGKILL));
    YEW_ASSERT(sh_wait(&f.ed, id));
    YEW_ASSERT_EQ_I64(yew_job_find(&f.ed, id)->state, YEW_JOB_CANCELLED);
    YEW_ASSERT(sh_wait_ended(&f.ed));
    sh_fix_free(&f);
}

/* ------------------------------------------------------------------ */
/* Busy, and the forms that start from the session's state            */
/* ------------------------------------------------------------------ */

/* Goals 3: a `:!` while a session command runs goes alongside, from the
 * session's directory and exports, and its `cd` does not persist. */
void test_shsession_busy_runs_alongside(void)
{
    ShFix f;
    char sub[PATH_MAX];
    char want[PATH_MAX + 16];
    u32 slow;
    u32 id;
    char *got;

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "sub", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd sub; export B=1")));
    slow = sh_start(&f, "echo started; sleep 30");
    sh_wait_output(&f.ed, slow);
    YEW_ASSERT(yew_shsession_busy(&f.ed));

    id = sh_start(&f, "pwd; echo \"b=$B\"; cd /");
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "session busy: ran outside it (a cd here will not "
                      "persist)");
    YEW_ASSERT(yew_job_find(&f.ed, id)->pid > 0);
    YEW_ASSERT(sh_wait(&f.ed, id));
    got = sh_output(&f, id);
    (void)snprintf(want, sizeof(want), "%s\nb=1\n", sub);
    YEW_ASSERT_EQ_STR(got, want);
    free(got);

    sh_cancel_until_done(&f.ed, slow);
    (void)snprintf(want, sizeof(want), "%s\n", sub);
    sh_expect(&f, "pwd", want);
    sh_fix_free(&f);
}

static char *sh_buffer_text(const Buffer *b)
{
    TextIter it;
    const u8 *chunk;
    u64 len;
    Bytebuf out;

    bytebuf_init(&out);
    if (yew_textbuf_len(b->tb) != 0U &&
        yew_textiter_begin(&it, b->tb, BYTEOFF(0U))) {
        do {
            if (!yew_textiter_chunk(&it, b->tb, &chunk, &len))
                break;
            bytebuf_append(&out, chunk, (size_t)len);
        } while (yew_textiter_advance(&it, b->tb));
    }
    bytebuf_push_u8(&out, 0U);
    return (char *)out.data;
}

static bool sh_slurp(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "rb");
    size_t n;

    if (fp == NULL)
        return false;
    n = fread(out, 1U, cap - 1U, fp);
    out[n] = '\0';
    return fclose(fp) == 0;
}

/*
 * §3: `:r !`, `:%!` and `:!!` spawn exactly as before, but in the
 * session's directory with its exports -- and `:!!`, which owns the
 * terminal, keeps the user's pager unless the session exported one.
 */
void test_shsession_other_forms_inherit_state(void)
{
    ShFix f;
    char sub[PATH_MAX];
    char out[PATH_MAX];
    char want[PATH_MAX + 32];
    char got[PATH_MAX + 32];
    char err[256];
    char *text;
    char *saved = getenv("PAGER") != NULL ? strdup(getenv("PAGER")) : NULL;
    YewJobWait wait;
    Arena a;
    char **env;
    size_t i;
    bool y = false;
    u32 id;

    YEW_ASSERT_EQ_I64(setenv("PAGER", "my-pager", 1), 0);
    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "sub", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd sub; export Y=2")));

    /* :r ! */
    id = yew_shell_read(&f.ed, "echo \"$(pwd) $Y\"", err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(sh_wait(&f.ed, id));
    text = sh_buffer_text(f.ed.win->buf);
    (void)snprintf(want, sizeof(want), "%s 2\n", sub);
    YEW_ASSERT_EQ_STR(text, want);
    free(text);

    /* :%! */
    YEW_ASSERT_EQ_I64(
        yew_shell_filter(&f.ed, f.ed.win,
                         (Span){0U, yew_textbuf_len(f.ed.win->buf->tb)},
                         "cat >/dev/null; echo \"f $(pwd) $Y\"", NULL),
        YEW_FILT_OK);
    text = sh_buffer_text(f.ed.win->buf);
    (void)snprintf(want, sizeof(want), "f %s 2\n", sub);
    YEW_ASSERT_EQ_STR(text, want);
    free(text);

    /* :!! (no controlling terminal here, so no handover -- 57.18) */
    YEW_ASSERT(yew_shell_term_run(&f.ed,
                                  "echo \"$(pwd) $Y $PAGER\" > out.txt",
                                  &wait, err, sizeof(err)));
    YEW_ASSERT_EQ_I64(wait.exit_code, 0);
    sh_path(&f, "sub/out.txt", out, sizeof(out));
    YEW_ASSERT(sh_slurp(out, got, sizeof(got)));
    (void)snprintf(want, sizeof(want), "%s 2 my-pager\n", sub);
    YEW_ASSERT_EQ_STR(got, want);
    /* A pager the user exported in the session is theirs. */
    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "export PAGER=less")));
    YEW_ASSERT(yew_shell_term_run(&f.ed, "echo \"$PAGER\" > out.txt", &wait,
                                  err, sizeof(err)));
    YEW_ASSERT(sh_slurp(out, got, sizeof(got)));
    YEW_ASSERT_EQ_STR(got, "less\n");
    /* Inside the session the user's export is the user's; a job started
     * FROM its state that has no terminal still gets PAGER=cat. */
    sh_expect(&f, "echo \"$PAGER\"", "less\n");
    id = yew_shell_read(&f.ed, "echo \"$PAGER\"", err, sizeof(err));
    YEW_ASSERT(sh_wait(&f.ed, id));
    text = sh_buffer_text(f.ed.win->buf);
    YEW_ASSERT_NOT_NULL(strstr(text, "cat\n"));
    free(text);

    /* The environment completion offers is the session's. */
    arena_init(&a);
    env = yew_job_env(&f.ed, &a);
    for (i = 0U; env != NULL && env[i] != NULL; i++)
        y = y || strcmp(env[i], "Y=2") == 0;
    YEW_ASSERT(y);
    arena_free_all(&a);

    sh_fix_free(&f);
    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv("PAGER", saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("PAGER"), 0);
    }
}

/* §5: the `:!` prompt's hint says where the session is. */
void test_shsession_prompt_hint_names_the_directory(void)
{
    static const char line[] = "!ls";
    ShFix f;
    char sub[PATH_MAX];

    sh_fix_make(&f, "/bin/sh");
    sh_path(&f, "ch7", sub, sizeof(sub));
    YEW_ASSERT_EQ_I64(mkdir(sub, 0700), 0);
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)line, strlen(line));
    yew_cmdline_edited(&f.ed);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.hint, "");
    yew_cmdline_close(&f.ed, false);

    YEW_ASSERT(sh_wait(&f.ed, sh_start(&f, "cd ch7")));
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)line, strlen(line));
    yew_cmdline_edited(&f.ed);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.hint, "in ch7/");
    yew_cmdline_close(&f.ed, false);
    /* The spelled-out command says it after what it already says. */
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)"shell.run ls", 12U);
    yew_cmdline_edited(&f.ed);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.hint,
                      "shell.run \xC2\xB7 <text> \xC2\xB7 in ch7/");
    yew_cmdline_close(&f.ed, false);
    sh_fix_free(&f);
}
