#define _POSIX_C_SOURCE 200809L

#include "mod/git/modal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include "mod/git/git.h"
#include "text/file.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "util/buf.h"

/* Requests and replies each have one tag byte followed by exact UTF-8
 * editor text. Git starts its editors serially. Rename consumes a request
 * before showing it, so a slow helper can never reopen an accepted modal. */
enum { GIT_MODAL_MAX = 1024 * 1024, GIT_MODAL_POLL_MS = 50 };

static char *bridge_path(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2U;
    char *path = yew_xmalloc(n);

    (void)snprintf(path, n, "%s/%s", dir, name);
    return path;
}

static bool bridge_read(const char *path, Bytebuf *out)
{
    struct stat st;
    u8 buf[4096];
    int fd;
    bool ok = false;

    bytebuf_init(out);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > GIT_MODAL_MAX)
        goto done;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n > 0 && (size_t)n > GIT_MODAL_MAX - out->len))
            break;
        if (n == 0) {
            ok = out->len == 0U || memchr(out->data, '\0', out->len) == NULL;
            break;
        }
        bytebuf_append(out, buf, (size_t)n);
    }
done:
    (void)close(fd);
    if (!ok)
        bytebuf_free(out);
    return ok;
}

static bool bridge_answer(GitModalBridge *bridge, bool accepted,
                           const u8 *text, size_t len)
{
    Bytebuf reply;
    char *path;
    bool ok;

    if (bridge->dir == NULL || len >= GIT_MODAL_MAX ||
        (len != 0U && memchr(text, '\0', len) != NULL))
        return false;
    path = bridge_path(bridge->dir, "answer");
    bytebuf_init(&reply);
    bytebuf_push_u8(&reply, (u8)(accepted ? '+' : '-'));
    if (accepted)
        bytebuf_append(&reply, text, len);
    ok = yew_save_committed(yew_file_write_atomic(path, reply.data,
                                                 reply.len, 0600));
    bytebuf_free(&reply);
    yew_xfree(path);
    return ok;
}

static void bridge_done(Ed *ed, bool accepted, const u8 *text,
                         size_t len, void *ctx)
{
    GitModalBridge *bridge = ctx;
    YewJob *job = yew_job_find(ed, bridge->job_id);
    bool delivered = bridge_answer(bridge, accepted, text, len);

    if (accepted && !delivered) {
        char *draft = yew_xmalloc(len + 1U);

        if (len != 0U)
            (void)memcpy(draft, text, len);
        draft[len] = '\0';
        yew_cmdline_open_modal(ed,
            bridge->sequence ? "Interactive rebase todo" : "Git commit message",
            len >= GIT_MODAL_MAX ?
                "Draft retained: editor response must be less than 1 MiB." :
                "Draft retained: cannot deliver response. Retry Ctrl-S or cancel Esc.",
            draft, true, bridge_done, bridge);
        yew_xfree(draft);
        return;
    }
    bridge->waiting = false;
    if (job != NULL && bridge->timeout_paused)
        job->timeout_ms = yew_now_ms() - job->start_ms + bridge->timeout_left;
    bridge->timeout_paused = false;
    if (!delivered) {
        (void)yew_job_signal(ed, bridge->job_id, SIGTERM);
        yew_msg(ed, YEW_MSG_ERROR, "cannot deliver Git editor response");
    } else if (!accepted) {
        yew_msg(ed, YEW_MSG_WARN,
                "Git editor cancelled; a partial rebase may need continue or abort");
    }
}

void yew_git_modal_close(Ed *ed, GitModalBridge *bridge)
{
    static const char *const names[] = {"request", "active", "answer"};
    size_t i;

    if (bridge->dir == NULL)
        return;
    if (ed->cmdline.active && ed->cmdline.input_ctx == bridge)
        yew_cmdline_close(ed, false);
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char *path = bridge_path(bridge->dir, names[i]);

        (void)unlink(path);
        yew_xfree(path);
    }
    (void)rmdir(bridge->dir);
    yew_xfree(bridge->dir);
    yew_xfree(bridge->editor_env);
    yew_xfree(bridge->sequence_env);
    *bridge = (GitModalBridge){0};
}

bool yew_git_modal_start(Ed *ed, GitModalBridge *bridge)
{
    char dir[] = "/tmp/yew-git-modal-XXXXXX";
    const char *exe = yew_job_self_exe();
    Bytebuf command;
    Bytebuf env;

    if (bridge->dir != NULL || exe == NULL || mkdtemp(dir) == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "cannot initialize Git editor bridge");
        return false;
    }
    bridge->dir = yew_xstrdup(dir);
    bridge->opener = ed->modal_keys.origin;
    bytebuf_init(&command);
    yew_shell_quote(&command, (const u8 *)exe, strlen(exe));
    bytebuf_printf(&command, " --yew-git-editor ");
    yew_shell_quote(&command, (const u8 *)dir, strlen(dir));
    bytebuf_printf(&command, " %ld", (long)getpid());
    bytebuf_push_u8(&command, 0U);
    bytebuf_init(&env);
    bytebuf_printf(&env, "GIT_EDITOR=%s message", (const char *)command.data);
    bytebuf_push_u8(&env, 0U);
    bridge->editor_env = yew_xstrdup((const char *)env.data);
    env.len = 0U;
    bytebuf_printf(&env, "GIT_SEQUENCE_EDITOR=%s sequence",
                    (const char *)command.data);
    bytebuf_push_u8(&env, 0U);
    bridge->sequence_env = yew_xstrdup((const char *)env.data);
    bytebuf_free(&env);
    bytebuf_free(&command);
    return true;
}

void yew_git_modal_tick(Ed *ed, GitModalBridge *bridge, i64 now_ms)
{
    char *request;
    char *active;
    Bytebuf bytes;
    const GitResult *result;
    struct stat st;

    if (bridge->dir == NULL)
        return;
    result = yew_git_result(ed);
    if (bridge->waiting && result != NULL && result->job_id == bridge->job_id) {
        if (ed->cmdline.active && ed->cmdline.input_ctx == bridge)
            yew_cmdline_close(ed, false);
        return;
    }
    if (bridge->waiting ||
        now_ms < bridge->next_poll)
        return;
    bridge->next_poll = now_ms + GIT_MODAL_POLL_MS;
    request = bridge_path(bridge->dir, "request");
    if (!bridge->timeout_paused && lstat(request, &st) == 0) {
        YewJob *job = yew_job_find(ed, bridge->job_id);

        if (job != NULL && job->timeout_ms > 0) {
            bridge->timeout_left = job->timeout_ms - (now_ms - job->start_ms);
            if (bridge->timeout_left < 1000)
                bridge->timeout_left = 1000;
            bridge->timeout_paused = true;
            job->timeout_ms = 0;
        }
    }
    if (ed->cmdline.active || yew_picker_active(ed)) {
        yew_xfree(request);
        return;
    }
    active = bridge_path(bridge->dir, "active");
    if (rename(request, active) == 0) {
        if (!bridge_read(active, &bytes) || bytes.len == 0U ||
            (bytes.data[0] != (u8)'s' && bytes.data[0] != (u8)'m')) {
            bridge_done(ed, false, NULL, 0U, bridge);
            yew_msg(ed, YEW_MSG_ERROR, "invalid or oversized Git editor request");
        } else {
            bool sequence = bytes.data[0] == (u8)'s';

            bridge->sequence = sequence;
            bytebuf_push_u8(&bytes, 0U);
            yew_cmdline_open_modal(ed, sequence ? "Interactive rebase todo" :
                                                  "Git commit message",
                sequence ? "Reorder lines; pick/reword/edit/squash/fixup/drop" :
                           "Edit the message; Esc stops this editor step",
                (const char *)bytes.data + 1U, true, bridge_done, bridge);
            ed->modal_keys.owner[YEW_MODAL_INPUT] = bridge->opener;
            bridge->waiting = true;
            yew_cmdline_target(ed)->cs.curs.data[0].pos = BYTEOFF(0U);
            yew_cmdline_target(ed)->cs.curs.data[0].anchor = BYTEOFF(0U);
            yew_cmdline_sync(ed);
        }
        bytebuf_free(&bytes);
    }
    yew_xfree(request);
    yew_xfree(active);
}

int yew_git_modal_helper_main(int argc, char **argv)
{
    char *end;
    long parent;
    struct stat st;
    Bytebuf bytes;
    Bytebuf request;
    char *request_path;
    char *answer_path;
    char *active_path;
    int status = 1;
    unsigned ticks;

    if (argc != 6)
        return 1;
    errno = 0;
    parent = strtol(argv[3], &end, 10);
    if (errno != 0 || *end != '\0' || parent <= 1 || parent > INT_MAX ||
        lstat(argv[2], &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 077U) != 0U ||
        (strcmp(argv[4], "sequence") != 0 && strcmp(argv[4], "message") != 0))
        return 1;
    if (!bridge_read(argv[5], &bytes)) {
        (void)fprintf(stderr, "yew: Git editor file is not readable text (limit 1 MiB)\n");
        return 1;
    }
    request_path = bridge_path(argv[2], "request");
    answer_path = bridge_path(argv[2], "answer");
    active_path = bridge_path(argv[2], "active");
    bytebuf_init(&request);
    bytebuf_push_u8(&request, (u8)(strcmp(argv[4], "sequence") == 0 ? 's' : 'm'));
    bytebuf_append(&request, bytes.data, bytes.len);
    bytebuf_free(&bytes);
    if (!yew_save_committed(yew_file_write_atomic(request_path, request.data,
                                                  request.len, 0600)))
        goto done;
    /* A dead parent or removed private directory ends the wait. The final
     * bound also covers PID reuse after an unclean parent exit. */
    for (ticks = 0U; ticks < 24U * 60U * 60U * 20U; ticks++) {
        if (kill((pid_t)parent, 0) != 0 || access(argv[2], F_OK) != 0)
            break;
        if (bridge_read(answer_path, &bytes)) {
            if (bytes.len != 0U && bytes.data[0] == (u8)'+' &&
                stat(argv[5], &st) == 0 && S_ISREG(st.st_mode) &&
                yew_save_committed(yew_file_write_atomic(argv[5], bytes.data + 1U,
                                                          bytes.len - 1U,
                                                          st.st_mode & 0777)))
                status = 0;
            bytebuf_free(&bytes);
            break;
        }
        if (lstat(answer_path, &st) == 0)
            break; /* A malformed reply is a failure, not another wait. */
        (void)poll(NULL, 0U, GIT_MODAL_POLL_MS);
    }
done:
    (void)unlink(answer_path);
    (void)unlink(active_path);
    (void)unlink(request_path);
    bytebuf_free(&request);
    yew_xfree(request_path);
    yew_xfree(answer_path);
    yew_xfree(active_path);
    return status;
}
