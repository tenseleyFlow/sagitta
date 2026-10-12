#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "tutor_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"
#include "util/buf.h"

enum {
    TUTOR_TASKS = 7,
    TUTOR_KEY_MAX = 128,
    TUTOR_META_MAX = 8192
};

typedef struct TutorKeys {
    char key[TUTOR_TASKS][TUTOR_KEY_MAX];
    u32 len;
} TutorKeys;

static bool read_fd_all(int fd, Bytebuf *out)
{
    bytebuf_init(out);
    for (;;) {
        u8 block[512];
        ssize_t n = read(fd, block, sizeof(block));

        if (n > 0) {
            if (out->len > TUTOR_META_MAX - (size_t)n)
                return false;
            bytebuf_append(out, block, (size_t)n);
        } else if (n == 0) {
            return true;
        } else if (errno != EINTR) {
            return false;
        }
    }
}

static bool split_keys(Bytebuf *text, TutorKeys *out)
{
    size_t at = 0U;

    (void)memset(out, 0, sizeof(*out));
    while (at < text->len) {
        size_t end = at;
        size_t len;

        while (end < text->len && text->data[end] != (u8)'\n')
            end++;
        len = end - at;
        if (len != 0U) {
            if (out->len >= TUTOR_TASKS || len >= TUTOR_KEY_MAX)
                return false;
            (void)memcpy(out->key[out->len], text->data + at, len);
            out->key[out->len][len] = '\0';
            out->len++;
        }
        at = end < text->len ? end + 1U : end;
    }
    return out->len == TUTOR_TASKS;
}

/* Read executable task metadata through Fletch itself.  This intentionally
 * does not parse chapter source in C: changing a task record or its exported
 * shape must break the same public language path users run. */
static bool load_keys(PtyCtx *c, const char *module, TutorKeys *out)
{
    static const char expression_fmt[] =
        "import io\n"
        "import \"tutor/%s\" as chapter\n"
        "for task in chapter.tasks { io.print(task.keys) }\n";
    char expression[512];
    char *runtime;
    Bytebuf text;
    int pipes[2];
    int status;
    int n;
    pid_t pid;
    bool ok;

    n = snprintf(expression, sizeof(expression), expression_fmt, module);
    if (n <= 0 || (size_t)n >= sizeof(expression) || pipe(pipes) != 0)
        return false;
    runtime = realpath("runtime", NULL);
    if (runtime == NULL) {
        (void)close(pipes[0]);
        (void)close(pipes[1]);
        return false;
    }
    pid = fork();
    if (pid == 0) {
        (void)close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0 ||
            setenv("YEW_RUNTIME_DIR", runtime, 1) != 0)
            _exit(126);
        (void)close(pipes[1]);
        execl(ptc_yew_bin(c), ptc_yew_bin(c), "fl", "--caps", "none",
              "-e", expression, (char *)NULL);
        _exit(127);
    }
    free(runtime);
    (void)close(pipes[1]);
    if (pid < 0) {
        (void)close(pipes[0]);
        return false;
    }
    ok = read_fd_all(pipes[0], &text);
    if (close(pipes[0]) != 0)
        ok = false;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            ok = false;
            status = 0;
            break;
        }
    }
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         split_keys(&text, out);
    bytebuf_free(&text);
    return ok;
}

static bool file_contains(const char *path, const char *needle)
{
    Bytebuf text;
    int fd = open(path, O_RDONLY);
    bool found;

    if (fd < 0)
        return false;
    if (!read_fd_all(fd, &text)) {
        (void)close(fd);
        bytebuf_free(&text);
        return false;
    }
    (void)close(fd);
    bytebuf_push_u8(&text, 0U);
    found = strstr((const char *)text.data, needle) != NULL;
    bytebuf_free(&text);
    return found;
}

static bool progress_complete(const char *path, u32 chapter,
                              const char *key)
{
    char expected[384];
    size_t at = 0U;
    u32 task;
    int n;

    n = snprintf(expected, sizeof(expected), "%s: { done: [", key);
    if (n <= 0 || (size_t)n >= sizeof(expected))
        return false;
    at = (size_t)n;
    for (task = 1U; task <= TUTOR_TASKS; task++) {
        n = snprintf(expected + at, sizeof(expected) - at,
                     "%s\"ch%02u.t%u\"", task == 1U ? "" : ", ",
                     (unsigned)chapter, (unsigned)task);
        if (n <= 0 || (size_t)n >= sizeof(expected) - at)
            return false;
        at += (size_t)n;
    }
    n = snprintf(expected + at, sizeof(expected) - at,
                 "], complete: true }");
    return n > 0 && (size_t)n < sizeof(expected) - at &&
           file_contains(path, expected);
}

typedef struct TutorProgressWait {
    const char *path;
    char id[24];
} TutorProgressWait;

static bool progress_has_task(const PtyCtx *c, const void *arg)
{
    const TutorProgressWait *wait = arg;

    (void)c;
    return file_contains(wait->path, wait->id);
}

static bool tutor_progress_path(PtyCtx *c, char out[PATH_MAX])
{
    int n = snprintf(out, PATH_MAX, "%s/yew/tutor/tutor.fl",
                     c->state_dir);

    ptc_check(c, n > 0 && n < PATH_MAX, "tutor progress path overflow");
    return !c->failed;
}

static void replay_tasks(PtyCtx *c, const TutorKeys *keys, u32 chapter,
                         const char *progress)
{
    TutorProgressWait wait;
    u32 i;

    wait.path = progress;
    for (i = 0U; i < keys->len && !c->failed; i++) {
        int n;

        ptc_keys(c, keys->key[i]);
        n = snprintf(wait.id, sizeof(wait.id), "\"ch%02u.t%u\"",
                     (unsigned)chapter, (unsigned)(i + 1U));
        ptc_check(c, n > 0 && (size_t)n < sizeof(wait.id),
                  "tutor task id overflow");
        ptc_wait_until(c, progress_has_task, &wait,
                       "tutor task did not reach its state verdict");
        ptc_settle(c, 30);
    }
}

static void quit_clean(PtyCtx *c)
{
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q <cr>");
    ptc_expect_exit(c, 0);
}

static void run_chapter(PtyCtx *c, u32 chapter, const char *key,
                        const char *module, const char *golden)
{
    TutorKeys keys;
    char chapter_arg[2];
    char progress[PATH_MAX];

    if (!load_keys(c, module, &keys)) {
        ptc_check(c, false, "could not load tutor task keys via Fletch");
        return;
    }
    chapter_arg[0] = (char)('0' + chapter);
    chapter_arg[1] = '\0';
    ptc_spawn(c, ptc_yew_bin(c), "tutor", chapter_arg, "--reset",
              "--clean", NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    if (!tutor_progress_path(c, progress))
        return;
    replay_tasks(c, &keys, chapter, progress);
    ptc_check(c, progress_complete(progress, chapter, key),
              "tutor task engine did not persist every verdict");
    ptc_snapshot(c, golden);

    /* Completion marks are ordinary undoable edits.  Exercise the normal
     * dirty-buffer exit and explicitly discard after the assertion. */
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q <cr>");
    ptc_settle(c, 20);
    ptc_keys(c, "d");
    ptc_expect_exit(c, 0);
}

void yew_tutor_case_ch01(PtyCtx *c)
{
    run_chapter(c, 1U, "arrows", "ch01-arrows.fl", "tutor_ch01_done");
}

void yew_tutor_case_ch02(PtyCtx *c)
{
    run_chapter(c, 2U, "units", "ch02-units.fl", "tutor_ch02_done");
}

void yew_tutor_case_ch03(PtyCtx *c)
{
    run_chapter(c, 3U, "execute", "ch03-execute.fl", "tutor_ch03_done");
}

void yew_tutor_case_ch04(PtyCtx *c)
{
    run_chapter(c, 4U, "search", "ch04-search.fl", "tutor_ch04_done");
}

void yew_tutor_case_ch05(PtyCtx *c)
{
    run_chapter(c, 5U, "workspace", "ch05-workspace.fl",
                "tutor_ch05_done");
}

void yew_tutor_case_resume(PtyCtx *c)
{
    TutorKeys keys;
    char progress[PATH_MAX];

    if (!load_keys(c, "ch05-workspace.fl", &keys)) {
        ptc_check(c, false, "could not load tutor task keys via Fletch");
        return;
    }
    ptc_spawn(c, ptc_yew_bin(c), "tutor", "5", "--reset", NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    if (!tutor_progress_path(c, progress))
        return;
    replay_tasks(c, &keys, 5U, progress);
    ptc_check(c, progress_complete(progress, 5U, "workspace"),
              "tutor resume setup did not complete Chapter 5");
    if (c->failed)
        return;

    /* The completion ticks are ordinary edits.  Save them so the first
     * live grid and the restored grid describe the same clean buffer. */
    ptc_keys(c, "s");
    ptc_settle(c, 40);
    /* Opening the command line clears the timed save message; cancelling
     * it returns to the same editor state without waiting on wall time. */
    ptc_keys(c, ": <esc>");
    ptc_settle(c, 40);
    ptc_mark_resume(c);
    quit_clean(c);

    ptc_resume(c, ptc_yew_bin(c), "tutor", "5", NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_check_resume_exact(c);
    ptc_snapshot(c, "tutor_resume");
    quit_clean(c);
}
