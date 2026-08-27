#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "util/calib.h"

enum {
    CALIB_WARMUPS = 3,
    CALIB_RUNS = 11,
    CHASE_BYTES = 4 * 1024 * 1024,
    CHASE_LOADS = 2 * 1024 * 1024,
    SCALAR_ITERS = 4 * 1024 * 1024,
    COPY_BYTES = 64 * 1024,
    COPY_ITERS = 16 * 1024
};

typedef u64 (*BenchFn)(void *ctx);

typedef struct ChaseCtx {
    u64 *storage;
    volatile u64 *next;
    u64 *order;
    size_t count;
} ChaseCtx;

typedef struct CopyCtx {
    u8 *src;
    u8 *dst;
} CopyCtx;

static volatile u64 calib_sink;
static void *(*volatile calib_memcpy)(void *, const void *, size_t) = memcpy;

static bool now_ns(u64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
    return true;
}

static u64 random_next(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void stable_sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at != 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool median_bench(BenchFn fn, void *ctx, u64 *median)
{
    u64 samples[CALIB_RUNS];
    unsigned run;

    for (run = 0U; run < CALIB_WARMUPS + CALIB_RUNS; run++) {
        u64 begin;
        u64 end;

        if (!now_ns(&begin))
            return false;
        calib_sink ^= fn(ctx);
        if (!now_ns(&end) || end <= begin)
            return false;
        if (run >= CALIB_WARMUPS)
            samples[run - CALIB_WARMUPS] = end - begin;
    }
    stable_sort_u64(samples, YEW_ARRAY_LEN(samples));
    *median = samples[YEW_ARRAY_LEN(samples) / 2U];
    return true;
}

static bool chase_init(ChaseCtx *ctx)
{
    u64 random = UINT64_C(0x9e3779b97f4a7c15);
    size_t i;

    ctx->count = CHASE_BYTES / sizeof(ctx->storage[0]);
    ctx->storage = malloc(ctx->count * sizeof(ctx->storage[0]));
    ctx->next = ctx->storage;
    ctx->order = malloc(ctx->count * sizeof(ctx->order[0]));
    if (ctx->storage == NULL || ctx->order == NULL)
        return false;
    for (i = 0U; i < ctx->count; i++)
        ctx->order[i] = (u64)i;
    for (i = ctx->count; i > 1U; i--) {
        size_t other = (size_t)(random_next(&random) % (u64)i);
        u64 value = ctx->order[i - 1U];

        ctx->order[i - 1U] = ctx->order[other];
        ctx->order[other] = value;
    }
    for (i = 0U; i < ctx->count; i++)
        ctx->storage[ctx->order[i]] = ctx->order[(i + 1U) % ctx->count];
    return true;
}

static u64 bench_chase(void *opaque)
{
    ChaseCtx *ctx = opaque;
    u64 at = ctx->order[0];
    size_t i;

    for (i = 0U; i < CHASE_LOADS; i++)
        at = ctx->next[at];
    return at;
}

static u64 bench_scalar(void *opaque)
{
    u64 x = *(const u64 *)opaque;
    u64 sum = 0U;
    size_t i;

    for (i = 0U; i < SCALAR_ITERS; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        if ((x & UINT64_C(0x100000001)) != 0U)
            sum += x ^ (u64)i;
        else
            sum ^= x + (u64)i;
    }
    return sum ^ x;
}

static bool copy_init(CopyCtx *ctx)
{
    size_t i;

    ctx->src = malloc(COPY_BYTES);
    ctx->dst = malloc(COPY_BYTES);
    if (ctx->src == NULL || ctx->dst == NULL)
        return false;
    for (i = 0U; i < COPY_BYTES; i++) {
        ctx->src[i] = (u8)(i * 131U + 17U);
        ctx->dst[i] = 0U;
    }
    return true;
}

static u64 bench_copy(void *opaque)
{
    CopyCtx *ctx = opaque;
    u64 checksum = 0U;
    size_t i;

    for (i = 0U; i < COPY_ITERS; i++) {
        size_t at = i * 131U % COPY_BYTES;

        ctx->src[at] ^= (u8)i;
        (void)calib_memcpy(ctx->dst, ctx->src, COPY_BYTES);
        checksum += ctx->dst[at];
    }
    return checksum;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    int i;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    }
    return NULL;
}

static bool args_valid(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--runner-id") == 0 ||
             strcmp(argv[i], "--reference") == 0) &&
            i + 1 < argc) {
            i++;
        } else {
            return false;
        }
    }
    return true;
}

static void token_copy(char *dst, size_t cap, const char *src)
{
    size_t at = 0U;

    while (*src != '\0' && at + 1U < cap) {
        unsigned char byte = (unsigned char)*src++;

        if ((byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
            (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
            (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
            byte == (unsigned char)'.' || byte == (unsigned char)'_' ||
            byte == (unsigned char)'-') {
            dst[at++] = (char)byte;
        } else if (at != 0U && dst[at - 1U] != '_') {
            dst[at++] = '_';
        }
    }
    if (at == 0U && cap >= sizeof("unknown")) {
        (void)memcpy(dst, "unknown", sizeof("unknown") - 1U);
        at = sizeof("unknown") - 1U;
    }
    dst[at] = '\0';
}

static void cpu_model(char *out, size_t cap, const char *fallback)
{
    FILE *file = fopen("/proc/cpuinfo", "r");
    char line[512];

    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            char *colon;
            char *value;

            if (strncmp(line, "model name", 10U) != 0 &&
                strncmp(line, "Hardware", 8U) != 0)
                continue;
            colon = strchr(line, ':');
            if (colon == NULL)
                continue;
            value = colon + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            line[strcspn(line, "\r\n")] = '\0';
            token_copy(out, cap, value);
            (void)fclose(file);
            return;
        }
        (void)fclose(file);
    }
    token_copy(out, cap, fallback);
}

int main(int argc, char **argv)
{
    const char *runner_id = arg_value(argc, argv, "--runner-id");
    const char *reference_path = arg_value(argc, argv, "--reference");
    ChaseCtx chase = {NULL, NULL, NULL, 0U};
    CopyCtx copy = {NULL, NULL};
    u64 scalar_seed = UINT64_C(0xd1b54a32d192ed03);
    CalibReference reference;
    CalibVec measured;
    struct utsname host;
    char model[256];
    char runner_token[80];
    char release_token[128];
    char reference_error[160];
    bool have_reference = false;
    u32 scale = 0U;
    int status = 0;

    if (!args_valid(argc, argv)) {
        (void)fprintf(stderr,
                      "usage: %s [--runner-id ID] [--reference FILE]\n",
                      argv[0]);
        return 2;
    }
    if (runner_id == NULL)
        runner_id = "local-unknown";
    if (uname(&host) != 0 || !chase_init(&chase) || !copy_init(&copy)) {
        (void)fprintf(stderr, "calib: setup failed: %s\n", strerror(errno));
        status = 2;
        goto done;
    }
    cpu_model(model, sizeof(model), host.machine);
    token_copy(runner_token, sizeof(runner_token), runner_id);
    token_copy(release_token, sizeof(release_token), host.release);
    if (!median_bench(bench_chase, &chase, &measured.c1) ||
        !median_bench(bench_scalar, &scalar_seed, &measured.c2) ||
        !median_bench(bench_copy, &copy, &measured.c3)) {
        (void)fprintf(stderr, "calib: measurement clock failed\n");
        status = 2;
        goto done;
    }
    if (reference_path != NULL) {
        have_reference = yew_calib_reference_read(
            reference_path, &reference, reference_error,
            sizeof(reference_error));
        if (!have_reference) {
            (void)fprintf(stderr, "calib: %s: %s; advisory only\n",
                          reference_path, reference_error);
        } else if (strcmp(reference.arch, host.machine) != 0) {
            (void)fprintf(stderr,
                          "calib: reference arch %s != host arch %s; "
                          "advisory only\n",
                          reference.arch, host.machine);
            have_reference = false;
        }
    }
    if (have_reference)
        scale = yew_calib_scale_permille(&measured, &reference.vec);

    (void)printf("# yew calibration measurement v1\n");
    (void)printf("runner_id %s\n", runner_id);
    (void)printf("arch %s\n", host.machine);
    (void)printf("kernel_release %s\n", host.release);
    (void)printf("cpu_model %s\n", model);
    (void)printf("cache_key %s--%s--%s\n", runner_token, release_token,
                 model);
    (void)printf("c1_chase_ns %llu\n",
                 (unsigned long long)measured.c1);
    (void)printf("c2_scalar_ns %llu\n",
                 (unsigned long long)measured.c2);
    (void)printf("c3_bandwidth_ns %llu\n",
                 (unsigned long long)measured.c3);
    if (have_reference && scale != 0U) {
        bool gating = reference.designated &&
                      strcmp(reference.runner_id, runner_id) == 0 &&
                      yew_calib_scale_is_gateable(scale);

        (void)printf("reference_runner_id %s\n", reference.runner_id);
        (void)printf("scale_permille %u\n", scale);
        (void)printf("mode %s\n", gating ? "GATING" : "ADVISORY");
    } else {
        (void)printf("scale_permille unavailable\n");
        (void)printf("mode ADVISORY\n");
    }
    (void)printf("sink %llu\n", (unsigned long long)calib_sink);

done:
    free(copy.dst);
    free(copy.src);
    free(chase.order);
    free(chase.storage);
    return status;
}
