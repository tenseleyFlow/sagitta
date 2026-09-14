#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snapshot.h"

enum {
    /*
     * The whole-suite ceiling.  Each case spawns a real editor under a
     * pty and waits for synchronized frames, so the suite's cost grows
     * with the case count: Sprint 21's fourteen search cases took it
     * past the previous 60 s and the runner reported the case that
     * happened to be running as a timeout, which named the wrong
     * culprit entirely.  Raised with the suite it has to cover; the
     * PER-CASE budget is the one that catches a hung editor, and it is
     * deliberately unchanged.
     */
    RUNNER_BUDGET_MS = 180000,
    CASE_BUDGET_MS = 5000
};

typedef enum PtyCaseRun {
    PTY_CASE_PASS = 0,
    PTY_CASE_GOLDEN_MISMATCH,
    PTY_CASE_HARD_FAIL
} PtyCaseRun;

typedef enum PtyVerdict {
    PTY_VERDICT_PASS = 0,
    PTY_VERDICT_FAIL,
    PTY_VERDICT_XFAIL,
    PTY_VERDICT_XPASS
} PtyVerdict;

typedef enum XfailDebtStatus {
    XFAIL_DEBT_IO = 0,
    XFAIL_DEBT_ABSENT,
    XFAIL_DEBT_ACTIVE,
    XFAIL_DEBT_FIXED
} XfailDebtStatus;

static bool env_truthy(const char *name)
{
    const char *value = getenv(name);

    return value != NULL && *value != '\0' && strcmp(value, "0") != 0;
}

static i64 parse_budget(const char *name, i64 fallback)
{
    const char *value = getenv(name);
    i64 parsed = 0;

    if (value == NULL || *value == '\0')
        return fallback;
    while (*value != '\0') {
        unsigned digit;

        if (*value < '0' || *value > '9')
            return fallback;
        digit = (unsigned)(*value - '0');
        if (parsed > (INT64_MAX - (i64)digit) / 10)
            return fallback;
        parsed = parsed * 10 + (i64)digit;
        value++;
    }
    return parsed > 0 ? parsed : fallback;
}

static char *path_join(const char *left, const char *right)
{
    size_t nl = strlen(left);
    size_t nr = strlen(right);
    bool slash = nl != 0U && left[nl - 1U] != '/';
    char *path;

    if (nl > SIZE_MAX - nr - (slash ? 2U : 1U))
        return NULL;
    path = malloc(nl + nr + (slash ? 2U : 1U));
    if (path == NULL)
        return NULL;
    (void)memcpy(path, left, nl);
    if (slash)
        path[nl++] = '/';
    (void)memcpy(path + nl, right, nr + 1U);
    return path;
}

static XfailDebtStatus xfail_debt_line_status(const char *line,
                                               const char *id)
{
    char prefix[32];
    const char *last;
    const char *status;
    size_t status_len;
    int n = snprintf(prefix, sizeof(prefix), "| %s |", id);

    if (n < 0 || (size_t)n >= sizeof(prefix) ||
        strncmp(line, prefix, (size_t)n) != 0)
        return XFAIL_DEBT_ABSENT;
    last = strrchr(line, '|');
    if (last == NULL || last == line)
        return XFAIL_DEBT_ABSENT;
    status = last;
    while (status != line && status[-1] != '|')
        status--;
    if (status == line)
        return XFAIL_DEBT_ABSENT;
    while (status < last && (*status == ' ' || *status == '\t'))
        status++;
    status_len = (size_t)(last - status);
    while (status_len != 0U &&
           (status[status_len - 1U] == ' ' ||
            status[status_len - 1U] == '\t'))
        status_len--;
    if (status_len == 5U && memcmp(status, "fixed", 5U) == 0)
        return XFAIL_DEBT_FIXED;
    if ((status_len == 4U && memcmp(status, "open", 4U) == 0) ||
        (status_len == 8U && memcmp(status, "deferred", 8U) == 0) ||
        (status_len == 7U && memcmp(status, "wontfix", 7U) == 0))
        return XFAIL_DEBT_ACTIVE;
    return XFAIL_DEBT_ABSENT;
}

static XfailDebtStatus xfail_debt_status(const char *id)
{
    char line[4096];
    FILE *file = fopen(".docs/audits/xfail-debt.md", "rb");
    XfailDebtStatus status = XFAIL_DEBT_ABSENT;

    if (file == NULL)
        return XFAIL_DEBT_IO;
    while (fgets(line, sizeof(line), file) != NULL) {
        XfailDebtStatus found = xfail_debt_line_status(line, id);

        if (found != XFAIL_DEBT_ABSENT) {
            status = found;
            break;
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return XFAIL_DEBT_IO;
    return status;
}

static PtyVerdict pty_verdict(const PtyCase *test, PtyCaseRun run)
{
    if (test->xfail_id == NULL)
        return run == PTY_CASE_PASS ? PTY_VERDICT_PASS : PTY_VERDICT_FAIL;
    if (run == PTY_CASE_PASS)
        return PTY_VERDICT_XPASS;
    /* YEW-F-026: only a deterministic comparison against an existing
     * golden satisfies XFAIL; setup, execution, stability, and hygiene
     * failures remain hard failures. */
    return run == PTY_CASE_GOLDEN_MISMATCH ? PTY_VERDICT_XFAIL
                                           : PTY_VERDICT_FAIL;
}

static bool remove_tree(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    bool ok = true;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (dir == NULL)
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char *child;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        child = path_join(path, entry->d_name);
        if (child == NULL || !remove_tree(child))
            ok = false;
        free(child);
    }
    if (closedir(dir) != 0)
        ok = false;
    if (rmdir(path) != 0)
        ok = false;
    return ok;
}

static bool make_state_dir(const PtyCase *test, unsigned run,
                           char *path, size_t cap)
{
    static const char config[] = "let lsp = {servers: {}}\n";
    char config_dir[PATH_MAX];
    char config_path[PATH_MAX];
    FILE *file;
    int n;

    if (mkdir("build", 0777) != 0 && errno != EEXIST)
        return false;
    n = snprintf(path, cap, "build/pty-%s-%u.XXXXXX", test->name, run);
    if (n <= 0 || (size_t)n >= cap || mkdtemp(path) == NULL)
        return false;
    /* PTY goldens exercise the editor, not whichever language servers are
     * installed on the host.  A real user-layer config keeps automatic LSP
     * startup enabled in production while making these sessions hermetic. */
    n = snprintf(config_dir, sizeof(config_dir), "%s/yew", path);
    if (n <= 0 || (size_t)n >= sizeof(config_dir) ||
        mkdir(config_dir, 0700) != 0)
        return false;
    n = snprintf(config_path, sizeof(config_path), "%s/init.fl", config_dir);
    if (n <= 0 || (size_t)n >= sizeof(config_path))
        return false;
    file = fopen(config_path, "wb");
    if (file == NULL)
        return false;
    {
        bool ok = fwrite(config, 1U, sizeof(config) - 1U, file) ==
                  sizeof(config) - 1U;

        if (fclose(file) != 0)
            ok = false;
        return ok;
    }
}

static bool read_file(const char *path, Bytebuf *out, bool *missing)
{
    FILE *file;
    u8 chunk[8192];

    *missing = false;
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT)
            *missing = true;
        return false;
    }
    for (;;) {
        size_t n = fread(chunk, 1U, sizeof(chunk), file);

        if (n != 0U)
            bytebuf_append(out, chunk, n);
        if (n < sizeof(chunk)) {
            if (ferror(file)) {
                (void)fclose(file);
                return false;
            }
            break;
        }
    }
    return fclose(file) == 0;
}

static bool write_file(const char *path, const Bytebuf *data)
{
    FILE *file = fopen(path, "wb");
    bool ok;

    if (file == NULL)
        return false;
    ok = fwrite(data->data, 1U, data->len, file) == data->len;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

/*
 * YEW_PTY_EXCLUDE is a COMMA-SEPARATED list of substrings.  It was a
 * single substring, and the valgrind lane silently ran a case it meant
 * to skip the moment a second name was added -- a skip list that fails
 * open is worse than no skip list.
 */
static bool excluded(const char *name, const char *list)
{
    const char *at = list;

    if (list == NULL || *list == '\0')
        return false;
    while (*at != '\0') {
        const char *comma = strchr(at, ',');
        size_t n = comma == NULL ? strlen(at) : (size_t)(comma - at);
        size_t i;

        for (i = 0U; n != 0U && name[i] != '\0'; i++) {
            if (strncmp(name + i, at, n) == 0)
                return true;
        }
        if (comma == NULL)
            break;
        at = comma + 1;
    }
    return false;
}

static bool valid_golden_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (name == NULL || *name == '\0')
        return false;
    while (*p != '\0') {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
        p++;
    }
    return true;
}

static bool run_once(PtyCtx *ctx, const PtyCase *test, unsigned run,
                     const char *demo, const char *yew,
                     i64 case_budget, i64 global_deadline)
{
    char state[512];

    if (!make_state_dir(test, run, state, sizeof(state))) {
        (void)fprintf(stderr, "pty: %s: cannot create state dir: %s\n",
                      test->name, strerror(errno));
        return false;
    }
    ptc_init(ctx, test, state, demo, yew, case_budget,
             global_deadline);
    test->fn(ctx);
    if (!ctx->failed && !ctx->snapshot_taken)
        ptc_check(ctx, false, "case did not take its required snapshot");
    ptc_cleanup(ctx);
    return !ctx->failed;
}

static void print_buf(FILE *file, const Bytebuf *buf)
{
    if (buf->len != 0U)
        (void)fwrite(buf->data, 1U, buf->len, file);
}

static bool compare_independent(const PtyCtx *first, const PtyCtx *second,
                                Bytebuf *diff)
{
    if (first->golden_name == NULL || second->golden_name == NULL ||
        strcmp(first->golden_name, second->golden_name) != 0) {
        bytebuf_printf(diff, "independent executions selected different goldens");
        return false;
    }
    return snapshot_compare(&first->snapshot, &second->snapshot, diff);
}

static bool compare_golden(const PtyCtx *got, bool update, bool *updated,
                           bool *mismatched, Bytebuf *diff)
{
    char filename[256];
    Bytebuf want;
    bool missing;
    bool equal;
    int n;

    *updated = false;
    *mismatched = false;
    if (!valid_golden_name(got->golden_name)) {
        bytebuf_printf(diff, "invalid golden name");
        return false;
    }
    n = snprintf(filename, sizeof(filename), "tests/pty/goldens/%s.golden",
                 got->golden_name);
    if (n < 0 || (size_t)n >= sizeof(filename)) {
        bytebuf_printf(diff, "golden path too long");
        return false;
    }
    bytebuf_init(&want);
    if (!read_file(filename, &want, &missing)) {
        if (!missing) {
            bytebuf_printf(diff, "cannot read golden %s: %s", filename,
                           strerror(errno));
            bytebuf_free(&want);
            return false;
        }
        if (!update) {
            bytebuf_printf(diff,
                           "golden not found: run with YEW_PTY_UPDATE=1");
            bytebuf_free(&want);
            return false;
        }
        equal = false;
    } else {
        equal = snapshot_compare(&got->snapshot, &want, diff);
        *mismatched = !equal;
    }
    if (!equal && update) {
        if ((mkdir("tests/pty/goldens", 0777) != 0 && errno != EEXIST) ||
            !write_file(filename, &got->snapshot)) {
            diff->len = 0U;
            bytebuf_printf(diff, "cannot update golden %s: %s", filename,
                           strerror(errno));
            bytebuf_free(&want);
            return false;
        }
        (void)printf("golden updated: %s\n", got->golden_name);
        *updated = true;
        bytebuf_free(&want);
        return true;
    }
    bytebuf_free(&want);
    return equal;
}

static void preserve_failure(const PtyCtx *ctx)
{
    (void)fprintf(stderr, "pty state preserved: %s\n", ctx->state_dir);
}

static PtyCaseRun run_case(const PtyCase *test, const char *demo,
                           const char *yew, i64 case_budget,
                           i64 global_deadline, bool update,
                           bool *any_updated)
{
    PtyCtx first;
    PtyCtx second;
    Bytebuf diff;
    Bytebuf fdmsg;
    bool first_ok;
    bool second_ok = false;
    bool stable = false;
    bool golden_ok = false;
    bool updated = false;
    bool golden_mismatch = false;
    bool expected_mismatch;
    bool cleanup_ok = true;
    bool ok;
    PtyCaseRun result;

    (void)memset(&first, 0, sizeof(first));
    (void)memset(&second, 0, sizeof(second));
    bytebuf_init(&diff);
    bytebuf_init(&fdmsg);
    first_ok = run_once(&first, test, 1U, demo, yew, case_budget,
                        global_deadline);
    if (first_ok && ptc_now_ms() < global_deadline)
        second_ok = run_once(&second, test, 2U, demo, yew, case_budget,
                             global_deadline);
    if (first_ok && second_ok) {
        stable = compare_independent(&first, &second, &diff);
        if (!stable) {
            (void)fprintf(stderr, "pty: %s: unstable snapshot\n", test->name);
            print_buf(stderr, &diff);
            if (diff.len == 0U || diff.data[diff.len - 1U] != '\n')
                (void)fputc('\n', stderr);
        } else {
            diff.len = 0U;
            golden_ok = compare_golden(&first, update, &updated,
                                       &golden_mismatch, &diff);
            if (!golden_ok) {
                if (test->xfail_id == NULL || !golden_mismatch) {
                    (void)fprintf(stderr, "pty: %s: golden mismatch\n",
                                  test->name);
                    print_buf(stderr, &diff);
                    if (diff.len == 0U || diff.data[diff.len - 1U] != '\n')
                        (void)fputc('\n', stderr);
                }
            }
        }
    }
    ok = first_ok && second_ok && stable && golden_ok;
    expected_mismatch = !update && first_ok && second_ok && stable &&
                        golden_mismatch;
    if (!first_ok) {
        (void)fprintf(stderr, "pty: %s: %s\n", test->name,
                      first.failure[0] == '\0' ? "first execution failed"
                                                : first.failure);
    }
    if (first_ok && !second_ok) {
        (void)fprintf(stderr, "pty: %s: %s\n", test->name,
                      second.failure[0] == '\0' ? "second execution failed"
                                                 : second.failure);
    }
    if (updated)
        *any_updated = true;
    if (ok || (test->xfail_id != NULL && expected_mismatch)) {
        if (!remove_tree(first.state_dir) || !remove_tree(second.state_dir)) {
            (void)fprintf(stderr, "pty: %s: could not remove state dirs\n",
                          test->name);
            cleanup_ok = false;
        }
    } else {
        if (first.state_dir != NULL)
            preserve_failure(&first);
        if (second.state_dir != NULL)
            preserve_failure(&second);
    }
    if (!ptc_sweep_all()) {
        (void)fprintf(stderr,
                      "pty: %s: live child cleanup exceeded one second\n",
                      test->name);
        cleanup_ok = false;
    }
    if (!ptc_fd_hygiene(&fdmsg)) {
        (void)fprintf(stderr, "pty: %s: ", test->name);
        print_buf(stderr, &fdmsg);
        (void)fputc('\n', stderr);
        cleanup_ok = false;
    }
    if (ok && cleanup_ok && !update && test->xfail_id == NULL)
        (void)printf("pty: %s: ok\n", test->name);
    if (!cleanup_ok)
        result = PTY_CASE_HARD_FAIL;
    else if (ok)
        result = PTY_CASE_PASS;
    else if (expected_mismatch)
        result = PTY_CASE_GOLDEN_MISMATCH;
    else
        result = PTY_CASE_HARD_FAIL;
    ptc_dispose(&first);
    ptc_dispose(&second);
    bytebuf_free(&diff);
    bytebuf_free(&fdmsg);
    return result;
}

static bool selftest_xfail_verdict(void)
{
    static const char active[] =
        "| YEW-F-026 | pty | `case` | reason | open |\n";
    static const char fixed[] =
        "| YEW-F-026 | pty | `case` | reason | fixed |\n";
    PtyCase plain = {"plain", "modern", 24U, 80U, NULL, NULL};
    PtyCase marked = {
        "marked", "modern", 24U, 80U, NULL, "YEW-F-026"
    };

    return xfail_debt_line_status(active, "YEW-F-026") ==
               XFAIL_DEBT_ACTIVE &&
           xfail_debt_line_status(fixed, "YEW-F-026") ==
               XFAIL_DEBT_FIXED &&
           pty_verdict(&plain, PTY_CASE_PASS) == PTY_VERDICT_PASS &&
           pty_verdict(&plain, PTY_CASE_GOLDEN_MISMATCH) ==
               PTY_VERDICT_FAIL &&
           pty_verdict(&marked, PTY_CASE_GOLDEN_MISMATCH) ==
               PTY_VERDICT_XFAIL &&
           pty_verdict(&marked, PTY_CASE_PASS) == PTY_VERDICT_XPASS &&
           pty_verdict(&marked, PTY_CASE_HARD_FAIL) == PTY_VERDICT_FAIL;
}

static int run_selftests(void)
{
    bool ok = selftest_xfail_verdict();

    (void)printf("%s pty_xfail_and_xpass_are_distinct\n",
                 ok ? "PASS" : "FAIL");
    (void)printf("pty-runner-selftest: 1 test, %u failure%s\n",
                 ok ? 0U : 1U, ok ? "s" : "");
    return ok ? 0 : 1;
}

static bool parse_cli(int argc, char **argv, const char **demo,
                      const char **yew)
{
    int i;

    *demo = NULL;
    *yew = NULL;
    for (i = 1; i < argc; i += 2) {
        if (i + 1 >= argc)
            return false;
        if (strcmp(argv[i], "--demo") == 0 && *demo == NULL)
            *demo = argv[i + 1];
        else if (strcmp(argv[i], "--yew") == 0 && *yew == NULL)
            *yew = argv[i + 1];
        else
            return false;
    }
    return *demo != NULL && *yew != NULL;
}

int main(int argc, char **argv)
{
    const char *demo;
    const char *yew;
    const char *filter = getenv("YEW_PTY_FILTER");
    const char *exclude = getenv("YEW_PTY_EXCLUDE");
    i64 budget;
    i64 case_budget;
    i64 global_deadline;
    bool update = env_truthy("YEW_PTY_UPDATE");
    bool any_updated = false;
    bool any_selected = false;
    bool ok = true;
    size_t xfailed = 0U;
    size_t xpassed = 0U;
    size_t i;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return run_selftests();
    if (!parse_cli(argc, argv, &demo, &yew)) {
        (void)fprintf(stderr,
                      "usage: pty_runner --demo <path> --yew <path>\n");
        return 2;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)fprintf(stderr, "pty: cannot ignore SIGPIPE: %s\n",
                      strerror(errno));
        return 1;
    }
    budget = parse_budget("YEW_PTY_BUDGET_MS", RUNNER_BUDGET_MS);
    case_budget = parse_budget("YEW_PTY_CASE_BUDGET_MS", CASE_BUDGET_MS);
    global_deadline = ptc_now_ms();
    global_deadline = budget > INT64_MAX - global_deadline
                          ? INT64_MAX : global_deadline + budget;
    for (i = 0U; yew_pty_cases[i].name != NULL; i++) {
        PtyCaseRun run;
        PtyVerdict verdict;

        if (excluded(yew_pty_cases[i].name, exclude))
            continue;
        if (filter != NULL && *filter != '\0' &&
            strstr(yew_pty_cases[i].name, filter) == NULL)
            continue;
        any_selected = true;
        if (yew_pty_cases[i].xfail_id != NULL) {
            XfailDebtStatus debt =
                xfail_debt_status(yew_pty_cases[i].xfail_id);

            if (update) {
                (void)fprintf(stderr,
                              "CONFIG %s: cannot update an XFAIL case\n",
                              yew_pty_cases[i].name);
                ok = false;
                continue;
            }
            if (debt != XFAIL_DEBT_ACTIVE) {
                if (debt == XFAIL_DEBT_FIXED)
                    (void)fprintf(stderr,
                                  "CONFIG %s: XFAIL id %s is already fixed\n",
                                  yew_pty_cases[i].name,
                                  yew_pty_cases[i].xfail_id);
                else if (debt == XFAIL_DEBT_ABSENT)
                    (void)fprintf(stderr,
                                  "CONFIG %s: unknown XFAIL id %s\n",
                                  yew_pty_cases[i].name,
                                  yew_pty_cases[i].xfail_id);
                else
                    (void)fprintf(stderr,
                                  "CONFIG %s: cannot read XFAIL debt ledger\n",
                                  yew_pty_cases[i].name);
                ok = false;
                continue;
            }
        }
        if (ptc_now_ms() >= global_deadline) {
            (void)fprintf(stderr, "pty: global budget exhausted after %lld ms\n",
                          (long long)budget);
            ok = false;
            break;
        }
        run = run_case(&yew_pty_cases[i], demo, yew, case_budget,
                       global_deadline, update, &any_updated);
        verdict = pty_verdict(&yew_pty_cases[i], run);
        if (verdict == PTY_VERDICT_XFAIL) {
            (void)printf("XFAIL %s [%s] (golden mismatch)\n",
                         yew_pty_cases[i].name,
                         yew_pty_cases[i].xfail_id);
            xfailed++;
        } else if (verdict == PTY_VERDICT_XPASS) {
            (void)printf("XPASS %s [%s] (golden matched; remove XFAIL)\n",
                         yew_pty_cases[i].name,
                         yew_pty_cases[i].xfail_id);
            xpassed++;
            ok = false;
        } else if (verdict == PTY_VERDICT_FAIL) {
            ok = false;
        }
    }
    if (!any_selected) {
        (void)fprintf(stderr, "pty: filter selected no cases\n");
        ok = false;
    }
    if (!ptc_sweep_all()) {
        (void)fprintf(stderr,
                      "pty: live child cleanup exceeded one second at exit\n");
        ok = false;
    }
    if (xfailed != 0U || xpassed != 0U)
        (void)printf("pty: xfailed=%zu xpassed=%zu\n", xfailed, xpassed);
    if (update) {
        if (!any_updated)
            (void)fprintf(stderr,
                          "pty: YEW_PTY_UPDATE=1 is review-only and never passes\n");
        return 1;
    }
    return ok ? 0 : 1;
}
