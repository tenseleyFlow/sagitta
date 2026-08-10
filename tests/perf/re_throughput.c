/*
 * Sprint 20: search throughput and the no-materialization proof.
 *
 * Two rates and one memory ceiling:
 *
 *   literal      >= 500 MiB/s   the memchr/BMH path, which is what most
 *                               real searches in an editor actually are
 *   non-literal  >=  50 MiB/s   the lazy DFA path
 *   RSS delta    <   4 MiB      over a 1 GiB buffer
 *
 * The memory ceiling is the interesting one.  It is DoD 2's teeth: the
 * engine reads through iterators and must never materialize the input,
 * so searching a gigabyte has to cost the same handful of kilobytes as
 * searching a line.  A regression that "just" copies the buffer once
 * would sail past both rate gates and be caught only here.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "search/regex.h"
#include "util/arena.h"

/* Overridable so the gate can be iterated on locally without a 1 GiB
 * allocation; CI runs the real size. */
#define DEFAULT_BYTES (1024ULL * 1024ULL * 1024ULL)

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "re_throughput: clock_gettime: %s\n",
                      strerror(errno));
        return -1;
    }
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

/* Peak RSS in bytes; ru_maxrss is a high-water mark, so a reading taken
 * after the buffer is populated already includes it. */
static u64 peak_rss(void)
{
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0U;
    return (u64)ru.ru_maxrss * 1024ULL;
}

static u64 env_u64(const char *name, u64 fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0')
        return fallback;
    parsed = strtoull(value, &end, 10);
    if (end == NULL || *end != '\0' || parsed == 0ULL)
        return fallback;
    return (u64)parsed;
}

static bool read_limits(const char *path, u64 *lit_mibs, u64 *dfa_mibs,
                        u64 *rss_bytes)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL)
        return false;
    *lit_mibs = 0U;
    *dfa_mibs = 0U;
    *rss_bytes = 0U;
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[96];
        unsigned long long value;

        if (sscanf(line, "%95s %llu", name, &value) != 2 || value == 0ULL)
            continue;
        if (strcmp(name, "re.throughput_literal_mibs") == 0)
            *lit_mibs = (u64)value;
        else if (strcmp(name, "re.throughput_dfa_mibs") == 0)
            *dfa_mibs = (u64)value;
        else if (strcmp(name, "re.throughput_rss_bytes") == 0)
            *rss_bytes = (u64)value;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return *lit_mibs != 0U && *dfa_mibs != 0U && *rss_bytes != 0U;
}

/* Deterministic filler: line-structured ASCII that looks like source, so
 * the class and boundary work the DFA does is representative rather than
 * a degenerate single-character run. */
static void fill(u8 *buf, u64 n)
{
    static const char row[] =
        "static int compute_value(int argument, int other) { return 0; }\n";
    u64 rowlen = (u64)(sizeof(row) - 1U);
    u64 at = 0U;

    while (at < n) {
        u64 take = n - at < rowlen ? n - at : rowlen;

        (void)memcpy(buf + at, row, (size_t)take);
        at += take;
    }
}

static double mibs(u64 bytes, i64 ns)
{
    if (ns <= 0)
        return 0.0;
    return ((double)bytes / (1024.0 * 1024.0)) /
           ((double)ns / 1e9);
}

int main(int argc, char **argv)
{
    u64 bytes = env_u64("YEW_RE_THROUGHPUT_BYTES", DEFAULT_BYTES);
    u64 lit_limit = 0U;
    u64 dfa_limit = 0U;
    u64 rss_limit = 0U;
    u8 *buf;
    Arena arena;
    YewRe *lit_re;
    YewRe *dfa_re;
    YewReInput in;
    u64 rss_before;
    u64 rss_after;
    i64 start;
    i64 end;
    double rate;
    int status = 0;

    if (argc != 3 || strcmp(argv[1], "--baseline") != 0) {
        (void)fprintf(stderr, "usage: re_throughput --baseline FILE\n");
        return 2;
    }
    if (!read_limits(argv[2], &lit_limit, &dfa_limit, &rss_limit)) {
        (void)fprintf(stderr, "re_throughput: cannot read %s\n", argv[2]);
        return 2;
    }
    buf = malloc((size_t)bytes);
    if (buf == NULL) {
        (void)fprintf(stderr,
                      "re_throughput: cannot allocate %llu bytes\n",
                      (unsigned long long)bytes);
        return 2;
    }
    fill(buf, bytes);
    /* The needle sits at the very end so a successful search still has to
     * scan the whole buffer — a hit near the front would measure how
     * fast we can give up, not how fast we can scan. */
    (void)memcpy(buf + bytes - 16U, "ZQNEEDLEQZ\n", 11U);

    arena_init(&arena);
    /* Whole-pattern literal: the VM is never entered. */
    lit_re = yew_re_compile(&arena, "ZQNEEDLEQZ", 10U, 0U, NULL);
    /* No extractable literal prefix, so this is the DFA's job. */
    dfa_re = yew_re_compile(&arena, "[qQ][0-9]+[zZ]", 14U, 0U, NULL);
    if (lit_re == NULL || dfa_re == NULL) {
        (void)fprintf(stderr, "re_throughput: patterns did not compile\n");
        free(buf);
        arena_free_all(&arena);
        return 2;
    }
    in = yew_re_input_bytes(buf, bytes);

    /* Baseline AFTER the buffer is populated, so its pages are already
     * counted and the delta is the engine's own footprint. */
    rss_before = peak_rss();

    start = now_ns();
    {
        YewReMatch m;
        bool found = yew_re_search(lit_re, &in, BYTEOFF(0U), &m);

        end = now_ns();
        if (!found || m.g[0].lo != bytes - 16U) {
            (void)fprintf(stderr,
                          "re_throughput: literal search did not find the "
                          "needle at the end\n");
            status = 1;
        }
    }
    rate = mibs(bytes, end - start);
    (void)printf("re.throughput literal      %8.1f MiB/s (limit %llu)\n",
                 rate, (unsigned long long)lit_limit);
    if (rate < (double)lit_limit) {
        (void)fprintf(stderr,
                      "re_throughput: literal path %.1f MiB/s is below "
                      "%llu MiB/s\n", rate,
                      (unsigned long long)lit_limit);
        status = 1;
    }

    start = now_ns();
    /* yew_re_test routes through the lazy DFA; the pattern never matches,
     * so the scan covers the whole buffer. */
    (void)yew_re_test(dfa_re, &in, BYTEOFF(0U));
    end = now_ns();
    rate = mibs(bytes, end - start);
    (void)printf("re.throughput non-literal  %8.1f MiB/s (limit %llu)\n",
                 rate, (unsigned long long)dfa_limit);
    if (rate < (double)dfa_limit) {
        (void)fprintf(stderr,
                      "re_throughput: DFA path %.1f MiB/s is below "
                      "%llu MiB/s\n", rate,
                      (unsigned long long)dfa_limit);
        status = 1;
    }

    rss_after = peak_rss();
    {
        u64 delta = rss_after > rss_before ? rss_after - rss_before : 0U;

        (void)printf("re.throughput rss delta    %8llu bytes (limit %llu)\n",
                     (unsigned long long)delta,
                     (unsigned long long)rss_limit);
        if (delta > rss_limit) {
            (void)fprintf(stderr,
                          "re_throughput: searching %llu bytes grew RSS by "
                          "%llu bytes — something materialized the input\n",
                          (unsigned long long)bytes,
                          (unsigned long long)delta);
            status = 1;
        }
    }
    free(buf);
    arena_free_all(&arena);
    return status;
}
