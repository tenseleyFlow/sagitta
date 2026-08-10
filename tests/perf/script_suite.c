/* Sprint 37: wall-clock gate for the complete isolated script suite. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
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
    const char *sagitta;
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
        else if (strcmp(argv[i], "--sagitta") == 0)
            options->sagitta = argv[i + 1];
        else if (strcmp(argv[i], "--baseline") == 0)
            options->baseline = argv[i + 1];
        else
            return false;
    }
    return options->runner != NULL && options->sagitta != NULL &&
           options->baseline != NULL;
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
    const char *text = getenv("SAG_SCRIPT_SUITE_INJECT_NS");
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
            if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
                return -1;
            do {
                waited = waitpid(pid, status, 0);
            } while (waited < 0 && errno == EINTR);
            return waited == pid ? 1 : -1;
        }
        (void)nanosleep(&poll_delay, NULL);
    }
}

int main(int argc, char **argv)
{
    Options options;
    int64_t limit;
    int64_t inject = injected_delay();
    int64_t start;
    int64_t elapsed;
    pid_t pid;
    int status;
    int waited;

    if (!parse_options(argc, argv, &options)) {
        (void)fprintf(stderr,
                      "usage: %s --runner PATH --sagitta PATH "
                      "--baseline PATH\n", argv[0]);
        return 2;
    }
    if (!load_limit(options.baseline, &limit)) {
        (void)fprintf(stderr, "perf-script-suite: invalid baseline %s\n",
                      options.baseline);
        return 2;
    }
    if (inject < 0) {
        (void)fprintf(stderr,
                      "perf-script-suite: invalid SAG_SCRIPT_SUITE_INJECT_NS\n");
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
        execl(options.runner, options.runner, "--sagitta", options.sagitta,
              (char *)NULL);
        _exit(127);
    }
    waited = wait_for_suite(pid, start + limit, &status);
    if (waited < 0) {
        (void)fprintf(stderr, "perf-script-suite: wait failed: %s\n",
                      strerror(errno));
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
