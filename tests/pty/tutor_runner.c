#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "tutor_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static bool write_text_file(const char *path, const char *text)
{
    size_t len = strlen(text);
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return false;
    while (at < len) {
        ssize_t n = write(fd, text + at, len - at);

        if (n > 0)
            at += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else {
            (void)close(fd);
            return false;
        }
    }
    return close(fd) == 0;
}

static bool install_hostile_config(PtyCtx *c, char marker[PATH_MAX])
{
    char config_dir[PATH_MAX];
    char config[PATH_MAX];
    char source[PATH_MAX + 96U];
    int n;

    n = snprintf(config_dir, sizeof(config_dir), "%s/yew", c->state_dir);
    if (n <= 0 || (size_t)n >= sizeof(config_dir) ||
        (mkdir(config_dir, 0700) != 0 && errno != EEXIST))
        return false;
    n = snprintf(config, sizeof(config), "%s/init.fl", config_dir);
    if (n <= 0 || (size_t)n >= sizeof(config))
        return false;
    n = snprintf(marker, PATH_MAX, "%s/hostile-loaded", c->state_dir);
    if (n <= 0 || n >= PATH_MAX)
        return false;
    n = snprintf(source, sizeof(source),
                 "import io\nio.write(\"%s\", \"loaded\")\n", marker);
    return n > 0 && (size_t)n < sizeof(source) &&
           write_text_file(config, source);
}

static bool command_output(char *const argv[], Bytebuf *out)
{
    int pipes[2];
    int status = 0;
    pid_t pid;
    bool ok;

    bytebuf_init(out);
    if (pipe(pipes) != 0)
        return false;
    pid = fork();
    if (pid == 0) {
        (void)close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    (void)close(pipes[1]);
    if (pid < 0) {
        (void)close(pipes[0]);
        return false;
    }
    ok = read_fd_all(pipes[0], out);
    if (close(pipes[0]) != 0)
        ok = false;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            ok = false;
            break;
        }
    }
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool tutor_check(PtyCtx *c, u32 chapter)
{
    char chapter_arg[2];
    int null_fd;
    int status = 0;
    pid_t pid;

    if (chapter == 0U || chapter > 7U)
        return false;
    chapter_arg[0] = (char)('0' + chapter);
    chapter_arg[1] = '\0';
    pid = fork();
    if (pid == 0) {
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0 ||
            setenv("XDG_STATE_HOME", c->state_dir, 1) != 0)
            _exit(126);
        if (null_fd != STDOUT_FILENO && null_fd != STDERR_FILENO)
            (void)close(null_fd);
        execl(ptc_yew_bin(c), ptc_yew_bin(c), "tutor", chapter_arg,
              "--check", (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return false;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool tutor_git_postconditions(PtyCtx *c)
{
    char repo[PATH_MAX];
    char *status_argv[] = {
        (char *)"git", (char *)"-C", repo, (char *)"status",
        (char *)"--porcelain", NULL
    };
    char *log_argv[] = {
        (char *)"git", (char *)"-C", repo, (char *)"log", (char *)"-1",
        (char *)"--pretty=%s", NULL
    };
    static const char message[] = "tutor checkpoint\n";
    Bytebuf output;
    bool ok;
    int n;

    n = snprintf(repo, sizeof(repo), "%s/yew/tutor", c->state_dir);
    if (n <= 0 || (size_t)n >= sizeof(repo))
        return false;
    if (!command_output(status_argv, &output)) {
        bytebuf_free(&output);
        return false;
    }
    ok = output.len == 0U;
    bytebuf_free(&output);
    if (!ok || !command_output(log_argv, &output)) {
        bytebuf_free(&output);
        return false;
    }
    ok = output.len == sizeof(message) - 1U &&
         memcmp(output.data, message, sizeof(message) - 1U) == 0;
    bytebuf_free(&output);
    return ok;
}

static bool tutor_git_config_is(const char *repo, const char *key,
                                const char *expected)
{
    char *argv[] = {
        (char *)"git", (char *)"-C", (char *)repo, (char *)"config",
        (char *)"--local", (char *)"--get", (char *)key, NULL
    };
    Bytebuf output;
    size_t expected_len = strlen(expected);
    bool ok;

    if (!command_output(argv, &output)) {
        bytebuf_free(&output);
        return false;
    }
    ok = output.len == expected_len &&
         memcmp(output.data, expected, expected_len) == 0;
    bytebuf_free(&output);
    return ok;
}

static bool tutor_git_policy_is_local(PtyCtx *c)
{
    char repo[PATH_MAX];
    int n = snprintf(repo, sizeof(repo), "%s/yew/tutor", c->state_dir);

    return n > 0 && (size_t)n < sizeof(repo) &&
           tutor_git_config_is(repo, "user.name", "Yew Tutor\n") &&
           tutor_git_config_is(repo, "user.email", "tutor@yew.invalid\n") &&
           tutor_git_config_is(repo, "commit.gpgsign", "false\n") &&
           tutor_git_config_is(repo, "tag.gpgsign", "false\n") &&
           tutor_git_config_is(repo, "core.hooksPath", ".git/no-hooks\n");
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
                 "], complete: true");
    return n > 0 && (size_t)n < sizeof(expected) - at &&
           file_contains(path, expected);
}

typedef struct TutorProgressWait {
    const char *path;
    char id[24];
} TutorProgressWait;

typedef struct TutorCursorWait {
    int row;
    int col;
} TutorCursorWait;

static bool progress_has_task(const PtyCtx *c, const void *arg)
{
    const TutorProgressWait *wait = arg;

    (void)c;
    return file_contains(wait->path, wait->id);
}

static bool tutor_cursor_at(const PtyCtx *c, const void *arg)
{
    const TutorCursorWait *wait = arg;

    return c->vt.cur_vis && c->vt.cur_r == wait->row &&
           c->vt.cur_c == wait->col;
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
                        const char *module)
{
    TutorKeys keys;
    char chapter_arg[2];
    char hostile_marker[PATH_MAX];
    char progress[PATH_MAX];

    if (!load_keys(c, module, &keys)) {
        ptc_check(c, false, "could not load tutor task keys via Fletch");
        return;
    }
    ptc_check(c, install_hostile_config(c, hostile_marker),
              "installing hostile tutor config");
    if (c->failed)
        return;
    chapter_arg[0] = (char)('0' + chapter);
    chapter_arg[1] = '\0';
    ptc_spawn(c, ptc_yew_bin(c), "tutor", chapter_arg, "--reset",
              "--clean", NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_check(c, access(hostile_marker, F_OK) != 0 && errno == ENOENT,
              "tutor --clean executed hostile user config");
    if (!tutor_progress_path(c, progress))
        return;
    replay_tasks(c, &keys, chapter, progress);
    ptc_check(c, progress_complete(progress, chapter, key),
              "tutor task engine did not persist every verdict");
    if (chapter == 5U) {
        static const TutorCursorWait final_cursor = {14, 47};

        /* The final task persists its verdict from the editor state before
         * the corresponding frame necessarily reaches a loaded PTY. */
        ptc_wait_until(c, tutor_cursor_at, &final_cursor,
                       "Chapter 5 final cursor frame did not arrive");
    }
    if (chapter == 7U) {
        ptc_check(c, tutor_git_policy_is_local(c),
                  "tutor Git repository retained inherited policy");
        ptc_check(c, tutor_git_postconditions(c),
                  "tutor Git lesson did not commit a clean repository");
    }
    /* YEW-F-070: a PTY golden is named by its registered case so the
     * static orphan check can prove that deleting the case also makes the
     * golden stale.  Do not pass an independently maintained name through
     * this shared runner. */
    ptc_snapshot(c, c->test->name);

    /* Completion marks are ordinary undoable edits.  Later chapters may
     * finish with another buffer focused, so discard explicitly instead of
     * assuming :q must open the dirty-buffer prompt. */
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q ! <cr>");
    ptc_expect_exit(c, 0);
    ptc_check(c, tutor_check(c, chapter),
              "yew tutor --check rejected completed replay verdicts");
}

void yew_tutor_case_ch01(PtyCtx *c)
{
    run_chapter(c, 1U, "arrows", "ch01-arrows.fl");
}

void yew_tutor_case_ch02(PtyCtx *c)
{
    run_chapter(c, 2U, "units", "ch02-units.fl");
}

void yew_tutor_case_ch03(PtyCtx *c)
{
    run_chapter(c, 3U, "execute", "ch03-execute.fl");
}

void yew_tutor_case_ch04(PtyCtx *c)
{
    run_chapter(c, 4U, "search", "ch04-search.fl");
}

void yew_tutor_case_ch05(PtyCtx *c)
{
    run_chapter(c, 5U, "workspace", "ch05-workspace.fl");
}

void yew_tutor_case_ch06(PtyCtx *c)
{
    run_chapter(c, 6U, "fletch", "ch06-fletch.fl");
}

void yew_tutor_case_ch07(PtyCtx *c)
{
    run_chapter(c, 7U, "fuss", "ch07-fuss.fl");
}

void yew_tutor_case_nogit(PtyCtx *c)
{
    char empty_path[PATH_MAX];
    char progress[PATH_MAX];
    char chapter[PATH_MAX];
    int n;

    n = snprintf(empty_path, sizeof(empty_path), "%s/empty-path",
                 c->workspace_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(empty_path),
              "tutor no-Git PATH overflow");
    if (c->failed)
        return;
    ptc_check(c, mkdir(empty_path, 0700) == 0,
              "creating tutor no-Git PATH");
    c->exec_path = "empty-path";
    ptc_spawn(c, ptc_yew_bin(c), "tutor", "7", "--reset", "--clean",
              NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    if (!tutor_progress_path(c, progress))
        return;
    ptc_check(c, file_contains(progress, "skipped: true") &&
                     file_contains(progress, "complete: false") &&
                     !file_contains(progress, "\"ch07.t"),
              "Git-unavailable tutor chapter was not marked skipped");
    n = snprintf(chapter, sizeof(chapter), "%s/yew/tutor/ch07.txt",
                 c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(chapter),
              "tutor no-Git chapter path overflow");
    ptc_check(c, !c->failed && file_contains(
                     chapter, "Git is unavailable in this build or on PATH"),
              "Git-unavailable tutor chapter did not announce requirement");
    ptc_snapshot(c, "tutor_nogit");
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q ! <cr>");
    ptc_expect_exit(c, 0);
    ptc_check(c, tutor_check(c, 7U),
              "yew tutor --check rejected skipped Git lesson");
}

void yew_tutor_case_list(PtyCtx *c)
{
    static const char tail[] = "7 fuss      incomplete FUSS mode and git";
    char root[PATH_MAX];
    int n;

    ptc_allow_primary(c);
    ptc_spawn(c, ptc_yew_bin(c), "tutor", "--list", NULL);
    ptc_no_altscreen(c);
    ptc_wait_output(c, tail, sizeof(tail) - 1U);
    ptc_expect_exit(c, 0);
    n = snprintf(root, sizeof(root), "%s/yew/tutor", c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(root),
              "tutor list state path overflow");
    ptc_check(c, !c->failed && access(root, F_OK) != 0 && errno == ENOENT,
              "tutor --list created state");
    ptc_snapshot(c, "tutor_list");
}

void yew_tutor_case_reset(PtyCtx *c)
{
    static const char sentinel[] = "reset must remove this\n";
    char chapter[PATH_MAX];
    char progress[PATH_MAX];
    int n;

    ptc_spawn(c, ptc_yew_bin(c), "tutor", "1", "--reset", "--clean",
              NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_mark_resume(c);
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q ! <cr>");
    ptc_expect_exit(c, 0);
    n = snprintf(chapter, sizeof(chapter), "%s/yew/tutor/ch01.txt",
                 c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(chapter),
              "tutor reset chapter path overflow");
    if (!tutor_progress_path(c, progress))
        return;
    ptc_check(c, !c->failed && write_text_file(chapter, sentinel) &&
                     write_text_file(progress, sentinel),
              "seeding tutor reset sentinels");
    ptc_resume(c, ptc_yew_bin(c), "tutor", "1", "--reset", "--clean",
               NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_check_resume_exact(c);
    ptc_check(c, file_contains(chapter, "YEW TUTOR 1/7") &&
                     !file_contains(chapter, "reset must remove this") &&
                     !file_contains(progress, "reset must remove this"),
              "tutor --reset did not restore a pristine workspace");
    ptc_snapshot(c, "tutor_reset");
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q ! <cr>");
    ptc_expect_exit(c, 0);
}

void yew_tutor_case_clean(PtyCtx *c)
{
    char marker[PATH_MAX];

    ptc_check(c, install_hostile_config(c, marker),
              "installing hostile tutor config");
    if (c->failed)
        return;
    ptc_spawn(c, ptc_yew_bin(c), "tutor", "1", "--reset", "--clean",
              NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_check(c, access(marker, F_OK) != 0 && errno == ENOENT,
              "tutor --clean executed hostile user config");
    ptc_snapshot(c, "tutor_clean");
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q ! <cr>");
    ptc_expect_exit(c, 0);
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
