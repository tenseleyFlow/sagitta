#define _POSIX_C_SOURCE 200809L

/* Sprint 51: 20,000 porcelain-v2 entries must parse inside one key budget. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod/git/git.h"
#include "util/base.h"

enum {
    GIT_STATUS_ENTRIES = 20000,
    GIT_STATUS_SAMPLES = 9,
    GIT_STATUS_BUDGET_NS = 8000000
};

static volatile u64 git_status_sink;

static void snapshot_begin(GitSnapshot *snap)
{
    (void)memset(snap, 0, sizeof(*snap));
    arena_init(&snap->a);
}

static void snapshot_end(GitSnapshot *snap)
{
    arena_free_all(&snap->a);
}

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_git_status: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static void sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at > 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool build_fixture(u8 **out, size_t *out_len)
{
    static const char prefix[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0";
    static const char row_fmt[] =
        "1 .M N... 100644 100644 100644 "
        "0123456789012345678901234567890123456789 "
        "abcdefabcdefabcdefabcdefabcdefabcdefabcd file-%05u.c";
    const size_t row_cap = sizeof(row_fmt) + 16U;
    size_t cap = sizeof(prefix) - 1U + row_cap * GIT_STATUS_ENTRIES;
    u8 *buf = malloc(cap);
    size_t at = 0U;
    u32 i;

    if (buf == NULL)
        return false;
    (void)memcpy(buf, prefix, sizeof(prefix) - 1U);
    at += sizeof(prefix) - 1U;
    for (i = 0U; i < GIT_STATUS_ENTRIES; i++) {
        int wrote = snprintf((char *)buf + at, cap - at, row_fmt, i);

        if (wrote <= 0 || (size_t)wrote + 1U > cap - at) {
            free(buf);
            return false;
        }
        at += (size_t)wrote + 1U;
    }
    *out = buf;
    *out_len = at;
    return true;
}

/* There is no allocator hook in the C11 core.  As with perf_mouse, prove
 * the stronger structural property: the pure parser names no heap allocator
 * and does not call the heap-backed generic stable sort. */
static bool parser_uses_heap(const char **offender)
{
    static const char *const banned[] = {
        "malloc(", "calloc(", "realloc(", "free(",
        "yew_xmalloc", "yew_xcalloc", "yew_xrealloc", "yew_sort_stable"
    };
    const char *path = getenv("YEW_PERF_GIT_STATUS_SRC");
    FILE *file = fopen(path != NULL ? path : "src/mod/git/porcelain.c", "r");
    char line[512];

    if (file == NULL) {
        *offender = "unreadable parser source";
        return true;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t i;

        for (i = 0U; i < YEW_ARRAY_LEN(banned); i++) {
            if (strstr(line, banned[i]) != NULL) {
                *offender = banned[i];
                (void)fclose(file);
                return true;
            }
        }
    }
    if (ferror(file)) {
        *offender = "parser source read error";
        (void)fclose(file);
        return true;
    }
    (void)fclose(file);
    return false;
}

static bool parse_once(const u8 *buf, size_t len, u64 *elapsed)
{
    GitSnapshot snap;
    GitParseErr err;
    u64 started;
    bool ok;

    (void)memset(&err, 0, sizeof(err));
    snapshot_begin(&snap);
    started = now_ns();
    ok = yew_git_parse_status(&snap, buf, (u64)len, &err);
    *elapsed = now_ns() - started;
    if (!ok || snap.entries.len != GIT_STATUS_ENTRIES) {
        (void)fprintf(stderr,
                      "perf_git_status: parse failed at %llu: %s (%zu entries)\n",
                      (unsigned long long)err.off, err.message,
                      snap.entries.len);
        snapshot_end(&snap);
        return false;
    }
    git_status_sink ^= (u64)snap.entries.len;
    git_status_sink ^= (u8)snap.entries.data[0].path[0];
    git_status_sink ^= (u8)snap.entries.data[snap.entries.len - 1U].path[0];
    snapshot_end(&snap);
    return true;
}

int main(void)
{
    u8 *fixture = NULL;
    size_t fixture_len = 0U;
    u64 samples[GIT_STATUS_SAMPLES];
    u64 warmup;
    u64 median;
    const char *offender = NULL;
    size_t i;
    int status = 0;

    if (!build_fixture(&fixture, &fixture_len)) {
        (void)fputs("perf_git_status: could not build fixture\n", stderr);
        return 2;
    }
    if (!parse_once(fixture, fixture_len, &warmup)) {
        free(fixture);
        return 1;
    }
    for (i = 0U; i < GIT_STATUS_SAMPLES; i++) {
        if (!parse_once(fixture, fixture_len, &samples[i])) {
            free(fixture);
            return 1;
        }
    }
    sort_u64(samples, YEW_ARRAY_LEN(samples));
    median = samples[YEW_ARRAY_LEN(samples) / 2U];
    (void)printf("git.status parse 20000  %.3f ms (limit 8.000 ms)\n",
                 (double)median / 1000000.0);
    if (median > GIT_STATUS_BUDGET_NS) {
        (void)fprintf(stderr,
                      "perf_git_status: median %.3f ms exceeds 8.000 ms\n",
                      (double)median / 1000000.0);
        status = 1;
    }
    if (parser_uses_heap(&offender)) {
        (void)fprintf(stderr,
                      "perf_git_status: parser allocates outside arena: %s\n",
                      offender);
        status = 1;
    }
    free(fixture);
    return status;
}
