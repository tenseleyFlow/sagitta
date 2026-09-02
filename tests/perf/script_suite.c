/* Sprint 37: wall-clock gate for the complete isolated script suite. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct Options {
    const char *runner;
    const char *yew;
    const char *fakelsp;
    const char *exclude;
    const char *baseline;
} Options;

static int64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) +
           (int64_t)ts.tv_nsec;
}

static void delay_ns(int64_t ns)
{
    struct timespec delay;

    if (ns <= 0)
        return;
    delay.tv_sec = (time_t)(ns / INT64_C(1000000000));
    delay.tv_nsec = (long)(ns % INT64_C(1000000000));
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static bool parse_options(int argc, char **argv, Options *options)
{
    int i;

    *options = (Options){0};
    for (i = 1; i < argc; i += 2) {
        if (i + 1 >= argc)
            return false;
        if (strcmp(argv[i], "--runner") == 0)
            options->runner = argv[i + 1];
        else if (strcmp(argv[i], "--yew") == 0)
            options->yew = argv[i + 1];
        else if (strcmp(argv[i], "--fakelsp") == 0)
            options->fakelsp = argv[i + 1];
        else if (strcmp(argv[i], "--exclude") == 0)
            options->exclude = argv[i + 1];
        else if (strcmp(argv[i], "--baseline") == 0)
            options->baseline = argv[i + 1];
        else
            return false;
    }
    return options->runner != NULL && options->yew != NULL &&
           options->baseline != NULL;
}

static size_t build_runner_argv(const Options *options, char **argv)
{
    size_t argc = 0U;

    argv[argc++] = (char *)options->runner;
    if (options->exclude != NULL) {
        argv[argc++] = (char *)"--exclude";
        argv[argc++] = (char *)options->exclude;
    }
    argv[argc++] = (char *)"--yew";
    argv[argc++] = (char *)options->yew;
    if (options->fakelsp != NULL) {
        argv[argc++] = (char *)"--fakelsp";
        argv[argc++] = (char *)options->fakelsp;
    }
    argv[argc] = NULL;
    return argc;
}

static bool selftest_runner_options(void)
{
    char *input[] = {
        (char *)"perf-script-suite",
        (char *)"--runner", (char *)"runner",
        (char *)"--yew", (char *)"yew",
        (char *)"--fakelsp", (char *)"fakelsp",
        (char *)"--exclude", (char *)"skip",
        (char *)"--baseline", (char *)"baseline"
    };
    Options options;
    char *runner_argv[8];
    size_t argc;

    if (!parse_options(11, input, &options))
        return false;
    argc = build_runner_argv(&options, runner_argv);
    return argc == 7U && strcmp(runner_argv[0], "runner") == 0 &&
           strcmp(runner_argv[1], "--exclude") == 0 &&
           strcmp(runner_argv[2], "skip") == 0 &&
           strcmp(runner_argv[3], "--yew") == 0 &&
           strcmp(runner_argv[4], "yew") == 0 &&
           strcmp(runner_argv[5], "--fakelsp") == 0 &&
           strcmp(runner_argv[6], "fakelsp") == 0 &&
           runner_argv[7] == NULL;
}

static bool load_limit(const char *path, int64_t *limit)
{
    FILE *file = fopen(path, "r");
    char line[256];
    bool found = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char metric[96];
        long long value;

        if (sscanf(line, "%95s %lld", metric, &value) != 2 || value <= 0)
            continue;
        if (strcmp(metric, "script_suite_wall_ns") == 0) {
            *limit = (int64_t)value;
            found = true;
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return found;
}

static int64_t injected_delay(void)
{
    const char *text = getenv("YEW_SCRIPT_SUITE_INJECT_NS");
    char *end;
    long long value;

    if (text == NULL || *text == '\0')
        return 0;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0)
        return -1;
    return (int64_t)value;
}

static int wait_for_suite(pid_t pid, int64_t deadline, int *status)
{
    static const struct timespec poll_delay = {0, 1000000};

    for (;;) {
        pid_t waited = waitpid(pid, status, WNOHANG);

        if (waited == pid)
            return 0;
        if (waited < 0 && errno != EINTR)
            return -1;
        if (now_ns() >= deadline) {
            /*
             * The runner owns a process group containing its current
             * yew child.  Killing only the runner here would leave that
             * child running after the gate returned.
             */
            if (kill(-pid, SIGKILL) != 0 && errno != ESRCH)
                return -1;
            do {
                waited = waitpid(pid, status, 0);
            } while (waited < 0 && errno == EINTR);
            return waited == pid ? 1 : -1;
        }
        (void)nanosleep(&poll_delay, NULL);
    }
}

static bool establish_child_group(pid_t pid)
{
    if (setpgid(pid, pid) == 0)
        return true;
    /* The child also calls setpgid before exec.  EACCES means it won that
     * race; ESRCH means it has already exited and waitpid will report it. */
    return errno == EACCES || errno == ESRCH;
}

static void kill_and_reap_group(pid_t pid)
{
    int status;
    pid_t waited;

    (void)kill(-pid, SIGKILL);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
}

/*
 * A fast liveness proof for the timeout path.  The synthetic runner and its
 * child both retain the pipe's write end.  POLLHUP therefore proves the
 * whole process group died; killing only the direct child would leave the
 * pipe open and fail this check.
 */
static int selftest_group_timeout(void)
{
    int pipefd[2];
    pid_t pid;
    int status;
    int waited;
    struct pollfd observed;
    int polled;

    if (pipe(pipefd) != 0)
        return 2;
    pid = fork();
    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return 2;
    }
    if (pid == 0) {
        pid_t descendant;

        (void)close(pipefd[0]);
        if (setpgid(0, 0) != 0)
            _exit(126);
        descendant = fork();
        if (descendant < 0)
            _exit(126);
        for (;;)
            pause();
    }
    (void)close(pipefd[1]);
    if (!establish_child_group(pid)) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        (void)close(pipefd[0]);
        return 2;
    }
    waited = wait_for_suite(pid, now_ns() + INT64_C(20000000), &status);
    observed.fd = pipefd[0];
    observed.events = POLLIN | POLLHUP;
    observed.revents = 0;
    do {
        polled = poll(&observed, 1U, 500);
    } while (polled < 0 && errno == EINTR);
    (void)close(pipefd[0]);
    if (waited != 1 || polled != 1 ||
        (observed.revents & POLLHUP) == 0) {
        (void)fprintf(stderr,
                      "perf-script-suite: process-group selftest failed\n");
        return 1;
    }
    (void)printf("perf-script-suite: process-group timeout selftest ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    Options options;
    int64_t limit = 0;
    int64_t inject = injected_delay();
    int64_t start;
    int64_t elapsed;
    pid_t pid;
    int status;
    int waited;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        if (!selftest_runner_options()) {
            (void)fprintf(stderr,
                          "perf-script-suite: runner options selftest failed\n");
            return 1;
        }
        (void)printf("perf-script-suite: runner options selftest ok\n");
        return selftest_group_timeout();
    }
    if (!parse_options(argc, argv, &options)) {
        (void)fprintf(stderr,
                      "usage: %s --runner PATH --yew PATH "
                      "[--fakelsp PATH] [--exclude LIST] "
                      "--baseline PATH | --selftest\n", argv[0]);
        return 2;
    }
    if (!load_limit(options.baseline, &limit)) {
        (void)fprintf(stderr, "perf-script-suite: invalid baseline %s\n",
                      options.baseline);
        return 2;
    }
    if (inject < 0) {
        (void)fprintf(stderr,
                      "perf-script-suite: invalid YEW_SCRIPT_SUITE_INJECT_NS\n");
        return 2;
    }
    start = now_ns();
    if (start < 0) {
        (void)fprintf(stderr, "perf-script-suite: clock_gettime failed\n");
        return 2;
    }
    pid = fork();
    if (pid < 0) {
        (void)fprintf(stderr, "perf-script-suite: fork failed: %s\n",
                      strerror(errno));
        return 2;
    }
    if (pid == 0) {
        char *runner_argv[8];

        if (setpgid(0, 0) != 0)
            _exit(126);
        (void)build_runner_argv(&options, runner_argv);
        execv(options.runner, runner_argv);
        _exit(127);
    }
    if (!establish_child_group(pid)) {
        (void)fprintf(stderr,
                      "perf-script-suite: cannot create process group: %s\n",
                      strerror(errno));
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        return 2;
    }
    waited = wait_for_suite(pid, start + limit, &status);
    if (waited < 0) {
        (void)fprintf(stderr, "perf-script-suite: wait failed: %s\n",
                      strerror(errno));
        kill_and_reap_group(pid);
        return 2;
    }
    if (waited > 0) {
        (void)printf("script_suite_wall_ns timeout limit=%lld FAIL\n",
                     (long long)limit);
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void)fprintf(stderr,
                      "perf-script-suite: runner failed with status %d\n",
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 2;
    }
    delay_ns(inject);
    elapsed = now_ns() - start;
    if (elapsed < 0) {
        (void)fprintf(stderr, "perf-script-suite: clock_gettime failed\n");
        return 2;
    }
    (void)printf("script_suite_wall_ns %lld limit=%lld%s\n",
                 (long long)elapsed, (long long)limit,
                 elapsed <= limit ? " ok" : " FAIL");
    return elapsed <= limit ? 0 : 1;
}
