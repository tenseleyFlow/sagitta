/*
 * YEW-F-027 through YEW-F-044 — bans accept intent-forbidden spellings.
 *
 * Correct behavior: Sprint 58 F15 q2 requires each ban to reject a
 * plausible refactor that still violates the rule's stated intent, not
 * merely its preferred token spelling.  The companion shell fixture builds
 * an isolated minimal repository, plants one seed, and runs the real gate.
 * A zero status means the gate caught the seed; every baseline probe misses.
 */
#include "audit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool ban_probe(const char *id)
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
        (void)execl("/bin/sh", "sh", "tests/audit/f15_ban_miss.sh", id,
                    (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#define BAN_MISS_TEST(num, id, description)                                  \
    bool test_yew_f_##num(char *why, size_t why_cap)                         \
    {                                                                         \
        bool caught = ban_probe(id);                                          \
                                                                              \
        if (!caught)                                                          \
            (void)snprintf(why, why_cap, "%s passed bans.sh", description); \
        return caught;                                                        \
    }

BAN_MISS_TEST(027, "YEW-F-027", "macro-forwarded Fletch format")
BAN_MISS_TEST(028, "YEW-F-028", "macro-forwarded Fletch abort")
BAN_MISS_TEST(029, "YEW-F-029", "macro-forwarded qsort")
BAN_MISS_TEST(030, "YEW-F-030", "token-pasted __attr" "ibute__")
BAN_MISS_TEST(031, "YEW-F-031", "token-pasted constr" "uctor")
BAN_MISS_TEST(032, "YEW-F-032", "token-pasted p" "thread_create")
BAN_MISS_TEST(033, "YEW-F-033", "__TIME" "STAMP__ reproducibility poison")
BAN_MISS_TEST(034, "YEW-F-034", "macro-forwarded mmap")
BAN_MISS_TEST(035, "YEW-F-035", "macro-forwarded malloc")
BAN_MISS_TEST(036, "YEW-F-036", "variable-NULL getcwd allocation")
BAN_MISS_TEST(037, "YEW-F-037", "variable-NULL realpath allocation")
BAN_MISS_TEST(038, "YEW-F-038", "locale-dependent mbtowc")
BAN_MISS_TEST(039, "YEW-F-039", "native dlvsym loading")
BAN_MISS_TEST(040, "YEW-F-040", "macro-forwarded strerror_r")
BAN_MISS_TEST(041, "YEW-F-041", "glibc backtrace_symbols_fd")
BAN_MISS_TEST(042, "YEW-F-042", "GNU getopt_long_only")
BAN_MISS_TEST(043, "YEW-F-043", "continued-line long double")
BAN_MISS_TEST(044, "YEW-F-044", "parenthesized disabled-shim success")

#undef BAN_MISS_TEST
