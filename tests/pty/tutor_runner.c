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

void yew_tutor_case_ch01(PtyCtx *c)
{
    TutorKeys keys;
    char progress[PATH_MAX];
    u32 i;
    int n;

    if (!load_keys(c, "ch01-arrows.fl", &keys)) {
        ptc_check(c, false, "could not load Chapter 1 task keys via Fletch");
        return;
    }
    ptc_spawn(c, ptc_yew_bin(c), "tutor", "1", "--reset", "--clean",
              NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    for (i = 0U; i < keys.len; i++) {
        ptc_keys(c, keys.key[i]);
        ptc_settle(c, 30);
    }
    n = snprintf(progress, sizeof(progress), "%s/yew/tutor/tutor.fl",
                 c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(progress),
              "Chapter 1 progress path overflow");
    ptc_check(c,
              file_contains(progress,
                  "arrows: { done: [\"ch01.t1\", \"ch01.t2\", "
                  "\"ch01.t3\", \"ch01.t4\", \"ch01.t5\", "
                  "\"ch01.t6\", \"ch01.t7\"], complete: true }"),
              "Chapter 1 task engine did not persist every verdict");
    ptc_snapshot(c, "tutor_ch01_done");

    /* Completion marks are ordinary undoable edits.  Exercise the normal
     * dirty-buffer exit and explicitly discard after the assertion. */
    ptc_allow_restore(c);
    ptc_keys(c, "<esc> : q <cr>");
    ptc_settle(c, 20);
    ptc_keys(c, "d");
    ptc_expect_exit(c, 0);
}
