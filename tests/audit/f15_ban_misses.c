/*
 * YEW-F-027 through YEW-F-071 — bans accept intent-forbidden spellings.
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
BAN_MISS_TEST(045, "YEW-F-045", "decimal local Unicode width table")
BAN_MISS_TEST(046, "YEW-F-046", "decimal syntax color")
BAN_MISS_TEST(047, "YEW-F-047", "local syntax width arithmetic")
BAN_MISS_TEST(048, "YEW-F-048", "direct posix_openpt outside the harness")
BAN_MISS_TEST(049, "YEW-F-049", "split-name golden update in CI")
BAN_MISS_TEST(050, "YEW-F-050", "piece-tree pread")
BAN_MISS_TEST(051, "YEW-F-051", "manual destructive shadow row fill")
BAN_MISS_TEST(052, "YEW-F-052", "indirect FUSS pane-root replacement")
BAN_MISS_TEST(053, "YEW-F-053", "libc random() in deterministic fuzzing")
BAN_MISS_TEST(054, "YEW-F-054", "clipboard shell through execl")
BAN_MISS_TEST(055, "YEW-F-055", "job data appended to shell text")
BAN_MISS_TEST(056, "YEW-F-056", "split-literal OSC 52 query")
BAN_MISS_TEST(057, "YEW-F-057", "direct tcflush outside tty.c")
BAN_MISS_TEST(058, "YEW-F-058", "register wrapper outside routing scan")
BAN_MISS_TEST(059, "YEW-F-059", "option wrapper outside routing scan")
BAN_MISS_TEST(060, "YEW-F-060", "package-git wrapper on startup path")
BAN_MISS_TEST(061, "YEW-F-061", "local register width lookup")
BAN_MISS_TEST(062, "YEW-F-062", "renamed register column arithmetic")
BAN_MISS_TEST(063, "YEW-F-063", "comment-only register helper names")
BAN_MISS_TEST(064, "YEW-F-064", "copied piece model in fuzz oracle")
BAN_MISS_TEST(065, "YEW-F-065", "hand-edited generated Unicode table")
BAN_MISS_TEST(066, "YEW-F-066", "process termination through _Exit")
BAN_MISS_TEST(067, "YEW-F-067", "AI body logging under a renamed variable")
BAN_MISS_TEST(068, "YEW-F-068", "static unregistered unit test")
BAN_MISS_TEST(069, "YEW-F-069", "missing PTY registry")
BAN_MISS_TEST(070, "YEW-F-070", "computed missing PTY golden")
BAN_MISS_TEST(071, "YEW-F-071", "orphan golden hidden by dead registry row")

#undef BAN_MISS_TEST
