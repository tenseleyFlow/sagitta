/*
 * YEW-F-097 — priority gate rejects findings discovered after lower fixes.
 *
 * Correct behavior: a Medium/Low closure fails only when a Critical/High
 * finding was already open.  A later review may discover a new Critical
 * finding without making earlier, already-merged remediation impossible.
 *
 * Baseline failure: check-priority.sh requires every Critical/High fixing
 * commit to precede every Medium/Low fixing commit.  It therefore rejects
 * F001's September Medium fix because F082 was discovered in October.
 */
#include "audit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool test_yew_f_097(char *why, size_t why_cap)
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
        (void)execl("/bin/sh", "sh", "scripts/check-priority.sh",
                    (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    (void)snprintf(why, why_cap,
                   "priority gate rejects a later-discovered Critical");
    return false;
}
