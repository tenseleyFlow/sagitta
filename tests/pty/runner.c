#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snapshot.h"

enum {
    RUNNER_BUDGET_MS = 60000,
    CASE_BUDGET_MS = 5000
};

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
    int n;

    if (mkdir("build", 0777) != 0 && errno != EEXIST)
        return false;
    n = snprintf(path, cap, "build/pty-%s-%u.XXXXXX", test->name, run);
    return n > 0 && (size_t)n < cap && mkdtemp(path) != NULL;
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
                     const char *demo, const char *sagitta,
                     i64 case_budget, i64 global_deadline)
{
    char state[512];

    if (!make_state_dir(test, run, state, sizeof(state))) {
        (void)fprintf(stderr, "pty: %s: cannot create state dir: %s\n",
                      test->name, strerror(errno));
        return false;
    }
    ptc_init(ctx, test, state, demo, sagitta, case_budget,
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
                           Bytebuf *diff)
{
    char filename[256];
    Bytebuf want;
    bool missing;
    bool equal;
    int n;

    *updated = false;
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
                           "golden not found: run with SAG_PTY_UPDATE=1");
            bytebuf_free(&want);
            return false;
        }
        equal = false;
    } else {
        equal = snapshot_compare(&got->snapshot, &want, diff);
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

static bool run_case(const PtyCase *test, const char *demo,
                     const char *sagitta, i64 case_budget,
                     i64 global_deadline, bool update, bool *any_updated)
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
    bool ok;

    (void)memset(&first, 0, sizeof(first));
    (void)memset(&second, 0, sizeof(second));
    bytebuf_init(&diff);
    bytebuf_init(&fdmsg);
    first_ok = run_once(&first, test, 1U, demo, sagitta, case_budget,
                        global_deadline);
    if (first_ok && ptc_now_ms() < global_deadline)
        second_ok = run_once(&second, test, 2U, demo, sagitta, case_budget,
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
            golden_ok = compare_golden(&first, update, &updated, &diff);
            if (!golden_ok) {
                (void)fprintf(stderr, "pty: %s: golden mismatch\n", test->name);
                print_buf(stderr, &diff);
                if (diff.len == 0U || diff.data[diff.len - 1U] != '\n')
                    (void)fputc('\n', stderr);
            }
        }
    }
    ok = first_ok && second_ok && stable && golden_ok;
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
    if (ok) {
        if (!remove_tree(first.state_dir) || !remove_tree(second.state_dir)) {
            (void)fprintf(stderr, "pty: %s: could not remove state dirs\n",
                          test->name);
            ok = false;
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
        ok = false;
    }
    if (!ptc_fd_hygiene(&fdmsg)) {
        (void)fprintf(stderr, "pty: %s: ", test->name);
        print_buf(stderr, &fdmsg);
        (void)fputc('\n', stderr);
        ok = false;
    }
    if (ok && !update)
        (void)printf("pty: %s: ok\n", test->name);
    ptc_dispose(&first);
    ptc_dispose(&second);
    bytebuf_free(&diff);
    bytebuf_free(&fdmsg);
    return ok;
}

static bool parse_cli(int argc, char **argv, const char **demo,
                      const char **sagitta)
{
    int i;

    *demo = NULL;
    *sagitta = NULL;
    for (i = 1; i < argc; i += 2) {
        if (i + 1 >= argc)
            return false;
        if (strcmp(argv[i], "--demo") == 0 && *demo == NULL)
            *demo = argv[i + 1];
        else if (strcmp(argv[i], "--sagitta") == 0 && *sagitta == NULL)
            *sagitta = argv[i + 1];
        else
            return false;
    }
    return *demo != NULL && *sagitta != NULL;
}

int main(int argc, char **argv)
{
    const char *demo;
    const char *sagitta;
    const char *filter = getenv("SAG_PTY_FILTER");
    const char *exclude = getenv("SAG_PTY_EXCLUDE");
    i64 budget;
    i64 case_budget;
    i64 global_deadline;
    bool update = env_truthy("SAG_PTY_UPDATE");
    bool any_updated = false;
    bool any_selected = false;
    bool ok = true;
    size_t i;

    if (!parse_cli(argc, argv, &demo, &sagitta)) {
        (void)fprintf(stderr,
                      "usage: pty_runner --demo <path> --sagitta <path>\n");
        return 2;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)fprintf(stderr, "pty: cannot ignore SIGPIPE: %s\n",
                      strerror(errno));
        return 1;
    }
    budget = parse_budget("SAG_PTY_BUDGET_MS", RUNNER_BUDGET_MS);
    case_budget = parse_budget("SAG_PTY_CASE_BUDGET_MS", CASE_BUDGET_MS);
    global_deadline = ptc_now_ms();
    global_deadline = budget > INT64_MAX - global_deadline
                          ? INT64_MAX : global_deadline + budget;
    for (i = 0U; sag_pty_cases[i].name != NULL; i++) {
        if (exclude != NULL && *exclude != '\0' &&
            strstr(sag_pty_cases[i].name, exclude) != NULL)
            continue;
        if (filter != NULL && *filter != '\0' &&
            strstr(sag_pty_cases[i].name, filter) == NULL)
            continue;
        any_selected = true;
        if (ptc_now_ms() >= global_deadline) {
            (void)fprintf(stderr, "pty: global budget exhausted after %lld ms\n",
                          (long long)budget);
            ok = false;
            break;
        }
        if (!run_case(&sag_pty_cases[i], demo, sagitta, case_budget,
                      global_deadline, update, &any_updated))
            ok = false;
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
    if (update) {
        if (!any_updated)
            (void)fprintf(stderr,
                          "pty: SAG_PTY_UPDATE=1 is review-only and never passes\n");
        return 1;
    }
    return ok ? 0 : 1;
}
