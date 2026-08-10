/*
 * Sprint 20 DoD 3: the no-backtracking proof, in CI.
 *
 * "There is no backtracking" is a structural claim about the code, and a
 * grep can check that the word does not appear.  What a grep cannot check
 * is that the *behaviour* is linear — that someone has not accidentally
 * reintroduced exponential work through a clever optimization.  These
 * patterns are the classic backtracker killers; against a backtracking
 * engine each one takes longer than the universe has existed at n=100.
 *
 * The scaling assertion is the real test.  An absolute time budget only
 * catches a slow machine; a RATIO catches super-linearity even when the
 * machine is fast, so t(10^5)/t(10^4) <= 15 is what actually proves the
 * property.  (15 rather than 10 because the smaller run is short enough
 * for fixed costs — compile, arena setup — to skew the quotient.)
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "search/regex.h"
#include "util/arena.h"

enum {
    SCALE_SMALL = 10000,
    SCALE_LARGE = 100000,
    RATIO_LIMIT = 15
};

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "re_pathological: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static bool read_limits(const char *path, i64 *hard_ns, i64 *ratio)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL)
        return false;
    *hard_ns = 0;
    *ratio = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[96];
        long long value;

        if (sscanf(line, "%95s %lld", name, &value) != 2 || value <= 0)
            continue;
        if (strcmp(name, "re.pathological_100k_ns") == 0)
            *hard_ns = (i64)value;
        else if (strcmp(name, "re.pathological_scale_ratio") == 0)
            *ratio = (i64)value;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return *hard_ns > 0 && *ratio > 0;
}

/* Runs `pat` against `n` copies of 'a' (with no trailing 'b', so every
 * one of these patterns must fail — the worst case). */
static bool time_run(const char *pat, size_t n, i64 *ns_out, bool *matched)
{
    Arena arena;
    YewReErr err = {0, NULL};
    YewRe *re;
    YewReInput in;
    u8 *hay;
    i64 start;
    i64 end;

    arena_init(&arena);
    re = yew_re_compile(&arena, pat, strlen(pat), 0U, &err);
    if (re == NULL) {
        (void)fprintf(stderr, "re_pathological: /%s/ did not compile: %s\n",
                      pat, err.msg == NULL ? "?" : err.msg);
        arena_free_all(&arena);
        return false;
    }
    hay = malloc(n == 0U ? 1U : n);
    if (hay == NULL) {
        arena_free_all(&arena);
        return false;
    }
    (void)memset(hay, 'a', n);
    in = yew_re_input_bytes(hay, (u64)n);
    start = now_ns();
    *matched = yew_re_search(re, &in, BYTEOFF(0U), NULL);
    end = now_ns();
    free(hay);
    arena_free_all(&arena);
    if (start < 0 || end < 0)
        return false;
    *ns_out = end - start;
    return true;
}

/* Best of three.  A single sample of a sub-millisecond run is mostly
 * scheduler noise, and noise in the denominator turns a linear engine
 * into a failing ratio. */
static bool time_best(const char *pat, size_t n, i64 *ns_out)
{
    i64 best = 0;
    int i;

    for (i = 0; i < 3; i++) {
        i64 ns = 0;
        bool m = false;

        if (!time_run(pat, n, &ns, &m))
            return false;
        if (i == 0 || ns < best)
            best = ns;
    }
    *ns_out = best;
    return true;
}

static bool scaling_case(const char *pat, i64 hard_ns, i64 ratio_limit,
                         int *status)
{
    i64 small = 0;
    i64 large = 0;
    double ratio;

    if (!time_best(pat, SCALE_SMALL, &small) ||
        !time_best(pat, SCALE_LARGE, &large))
        return false;
    /* Guard the quotient: a sub-microsecond small run makes the ratio
     * meaningless rather than impressive. */
    if (small < 1000)
        small = 1000;
    ratio = (double)large / (double)small;
    (void)printf("re.pathological /%-12s/ 10^4=%8lld ns  10^5=%9lld ns  "
                 "ratio=%.2f\n",
                 pat, (long long)small, (long long)large, ratio);
    if (large > hard_ns) {
        (void)fprintf(stderr,
                      "re_pathological: /%s/ took %lld ns at 10^5, "
                      "limit %lld ns\n",
                      pat, (long long)large, (long long)hard_ns);
        *status = 1;
    }
    if (ratio > (double)ratio_limit) {
        (void)fprintf(stderr,
                      "re_pathological: /%s/ scaled %.2fx from 10^4 to "
                      "10^5 (limit %lldx) — that is super-linear, which "
                      "means backtracking crept back in\n",
                      pat, ratio, (long long)ratio_limit);
        *status = 1;
    }
    return true;
}

/* The classic exponential case: a?{n}a{n} against n a's.  A backtracker
 * explores 2^n paths; n=25 is where it stops finishing at all. */
static bool nested_quantifier_case(i64 *ns_out)
{
    char pat[256];
    size_t at = 0U;
    int i;
    bool m = false;

    for (i = 0; i < 25; i++)
        at += (size_t)snprintf(pat + at, sizeof(pat) - at, "a?");
    for (i = 0; i < 25; i++)
        at += (size_t)snprintf(pat + at, sizeof(pat) - at, "a");
    return time_run(pat, 25U, ns_out, &m) && m;
}

int main(int argc, char **argv)
{
    static const char *const patterns[] = {
        "a(a*)*b",
        "(a|a)*b",
        "(a+)+b",
        "(a*)*b"
    };
    i64 hard_ns = 0;
    i64 ratio_limit = 0;
    i64 ns = 0;
    size_t i;
    int status = 0;

    if (argc != 3 || strcmp(argv[1], "--baseline") != 0) {
        (void)fprintf(stderr,
                      "usage: re_pathological --baseline FILE\n");
        return 2;
    }
    if (!read_limits(argv[2], &hard_ns, &ratio_limit)) {
        (void)fprintf(stderr, "re_pathological: cannot read %s\n",
                      argv[2]);
        return 2;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(patterns); i++) {
        if (!scaling_case(patterns[i], hard_ns, ratio_limit, &status))
            return 2;
    }
    if (!nested_quantifier_case(&ns))
        return 2;
    (void)printf("re.pathological /a?{25}a{25}/ %lld ns\n",
                 (long long)ns);
    if (ns > 5000000) {
        (void)fprintf(stderr,
                      "re_pathological: a?{25}a{25} took %lld ns, "
                      "limit 5000000\n", (long long)ns);
        status = 1;
    }
    /* The .*.*.*=.* shape: a 10 KiB line with no '=' at all. */
    {
        Arena arena;
        YewRe *re;
        YewReInput in;
        u8 *hay = malloc(10240U);
        i64 start;
        i64 end;

        if (hay == NULL)
            return 2;
        (void)memset(hay, 'x', 10240U);
        arena_init(&arena);
        re = yew_re_compile(&arena, ".*.*.*.*=.*", 11U, 0U, NULL);
        if (re == NULL) {
            free(hay);
            arena_free_all(&arena);
            return 2;
        }
        in = yew_re_input_bytes(hay, 10240U);
        start = now_ns();
        (void)yew_re_search(re, &in, BYTEOFF(0U), NULL);
        end = now_ns();
        free(hay);
        arena_free_all(&arena);
        (void)printf("re.pathological /.*.*.*.*=.*/ %lld ns\n",
                     (long long)(end - start));
        if (end - start > hard_ns) {
            (void)fprintf(stderr,
                          "re_pathological: .*.*.*.*=.* took %lld ns, "
                          "limit %lld\n",
                          (long long)(end - start), (long long)hard_ns);
            status = 1;
        }
    }
    return status;
}
