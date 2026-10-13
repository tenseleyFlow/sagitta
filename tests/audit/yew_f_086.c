/*
 * YEW-F-086 — binding plans and F06 sprint contracts are absent from Git.
 *
 * Correct behavior: a clean checkout contains every binding document and
 * the Sprint 20/21 contracts consumed by check-closeouts.sh.
 *
 * Baseline failure: local ignored copies hide that all four paths are absent
 * from the repository; the closeout gate reports zero s20/s21 sprint files.
 */
#include "audit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool test_yew_f_086(char *why, size_t why_cap)
{
    pid_t pid = fork();
    pid_t waited;
    int status;

    if (pid < 0)
        return false;
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);

        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            (void)close(null_fd);
        }
        (void)execlp("git", "git", "ls-files", "--error-unmatch",
                     ".docs/plan/01-architecture.md",
                     ".docs/plan/02-fletch.md",
                     ".docs/sprints/04-execute-search/s20-regex-engine.md",
                     ".docs/sprints/04-execute-search/s21-search-replace.md",
                     (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    (void)snprintf(why, why_cap,
                   "binding/F06 contracts are not tracked by Git");
    return false;
}
