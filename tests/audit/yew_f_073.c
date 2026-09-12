/*
 * YEW-F-073 — baseline history policy is not enforced.
 *
 * Correct behavior: the baseline guard rejects a baseline-changing commit
 * unless its message records old-to-new values and why they moved.
 *
 * Baseline failure: the companion fixture creates an unexplained rebaseline
 * in an isolated Git repository and the real guard accepts it.
 */
#include "audit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool test_yew_f_073(char *why, size_t why_cap)
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
        (void)execl("/bin/sh", "sh",
                    "tests/audit/f15_baseline_history.sh", (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    (void)snprintf(why, why_cap,
                   "unexplained baseline-only commit passed the guard");
    return false;
}
