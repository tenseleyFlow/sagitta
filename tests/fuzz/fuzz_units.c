#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "unicode/coords.h"

enum { UNIT_FUZZ_BYTES = 2048, UNIT_FUZZ_MIN_ITERS = 100000 };

typedef struct {
    u64 rng;
    u64 hash;
} FuzzRun;

static u64 random_next(FuzzRun *run)
{
    u64 x = run->rng;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    run->rng = x;
    return x * UINT64_C(2685821657736338717);
}

static bool parse_u64(const char *arg, const char *prefix, u64 *out)
{
    char *end;
    unsigned long long value;
    size_t n = strlen(prefix);

    if (strncmp(arg, prefix, n) != 0)
        return false;
    errno = 0;
    value = strtoull(arg + n, &end, 0);
    if (errno != 0 || end == arg + n || *end != '\0')
        return false;
    *out = (u64)value;
    return true;
}

static bool fail(const UnitOps *ops, const char *what, u64 op, ByteOff p,
                 ByteOff got, u64 len)
{
    (void)fprintf(stderr,
                  "fuzz_units: engine=%s op=%llu p=%llu got=%llu len=%llu: %s\n",
                  ops->name, (unsigned long long)op,
                  (unsigned long long)p.v, (unsigned long long)got.v,
                  (unsigned long long)len, what);
    return false;
}

static bool check_one(FuzzRun *run, UnitCtx *u, const UnitOps *ops,
                      ByteOff p, bool alt, u64 op)
{
    u64 len = sag_textbuf_len(u->tb);
    u64 gen = u->tb->gen;
    ByteOff next = ops->next(u, p, alt);
    ByteOff prev = ops->prev(u, p, alt);
    ByteOff home = ops->home(u, p, alt);
    ByteOff end = ops->end(u, p, alt);
    Span span = ops->span(u, p, alt);
    ByteOff values[] = {next, prev, home, end, BYTEOFF(span.lo),
                        BYTEOFF(span.hi)};
    size_t i;

    if (u->tb->gen != gen)
        return fail(ops, "changed buffer generation", op, p, p, len);
    for (i = 0U; i < SAG_ARRAY_LEN(values); i++) {
        if (values[i].v > len)
            return fail(ops, "returned out-of-range offset", op, p,
                        values[i], len);
        if (!sag_is_grapheme_boundary(u->tb, values[i]))
            return fail(ops, "returned non-grapheme boundary", op, p,
                        values[i], len);
    }
    if ((p.v < len && next.v <= p.v) || (p.v == len && next.v != len))
        return fail(ops, "next did not advance", op, p, next, len);
    if ((p.v != 0U && prev.v >= p.v) || (p.v == 0U && prev.v != 0U))
        return fail(ops, "prev did not retreat", op, p, prev, len);
    if (home.v > p.v || end.v < p.v || home.v > end.v)
        return fail(ops, "home/end do not contain p", op, p, home, len);
    if (span.lo != home.v || span.hi != end.v)
        return fail(ops, "span disagrees with home/end", op, p,
                    BYTEOFF(span.hi), len);
    run->hash ^= next.v + (prev.v << 1U) + (span.lo << 2U) +
                 (span.hi << 3U) + op;
    run->hash *= UINT64_C(1099511628211);
    return true;
}

static bool check_termination(UnitCtx *u, const UnitOps *ops, bool alt)
{
    u64 len = sag_textbuf_len(u->tb);
    ByteOff p = BYTEOFF(0U);
    u64 steps = 0U;

    while (p.v < len && steps <= len) {
        ByteOff next = ops->next(u, p, alt);

        if (next.v <= p.v || next.v > len)
            return false;
        p = next;
        steps++;
    }
    if (p.v != len)
        return false;
    while (p.v != 0U && steps <= len * 2U + 1U) {
        ByteOff prev = ops->prev(u, p, alt);

        if (prev.v >= p.v)
            return false;
        p = prev;
        steps++;
    }
    return p.v == 0U;
}

static TextBuf *random_buffer(FuzzRun *run)
{
    u8 *bytes = malloc(UNIT_FUZZ_BYTES);
    size_t i;

    if (bytes == NULL)
        return NULL;
    for (i = 0U; i < UNIT_FUZZ_BYTES; i++) {
        u64 value = random_next(run);

        if (i % 73U == 72U)
            bytes[i] = (u8)'\n';
        else if ((value & 7U) < 5U)
            bytes[i] = (u8)(32U + value % 95U);
        else
            bytes[i] = (u8)value;
    }
    return sag_textbuf_from_owned_bytes(bytes, UNIT_FUZZ_BYTES);
}

static bool check_large_single_line(void)
{
    static const UnitOps *const engines[] = {
        &sag_unit_line, &sag_unit_word, &sag_unit_block, &sag_unit_char,
    };
    const size_t n = 1024U * 1024U;
    u8 *bytes = malloc(n);
    TextBuf *tb;
    Buffer buffer = {0};
    UnitCtx u;
    size_t i;

    if (bytes == NULL)
        return false;
    (void)memset(bytes, 'a', n);
    tb = sag_textbuf_from_owned_bytes(bytes, n);
    if (tb == NULL)
        return false;
    buffer.tb = tb;
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    for (i = 0U; i < SAG_ARRAY_LEN(engines); i++) {
        ByteOff next = engines[i]->next(&u, BYTEOFF(0U), false);
        ByteOff prev = engines[i]->prev(&u, BYTEOFF(n), true);

        if (next.v == 0U || next.v > n || prev.v >= n ||
            !sag_is_grapheme_boundary(tb, next) ||
            !sag_is_grapheme_boundary(tb, prev)) {
            sag_textbuf_free(tb);
            return false;
        }
    }
    sag_textbuf_free(tb);
    return true;
}

int main(int argc, char **argv)
{
    static const UnitOps *const engines[] = {
        &sag_unit_line, &sag_unit_word, &sag_unit_block, &sag_unit_char,
    };
    u64 seed = 1U;
    u64 iterations = UNIT_FUZZ_MIN_ITERS;
    FuzzRun run;
    TextBuf *tb;
    Buffer buffer = {0};
    UnitCtx u;
    u64 op;
    int argi;

    for (argi = 1; argi < argc; argi++) {
        if (parse_u64(argv[argi], "--seed=", &seed) ||
            parse_u64(argv[argi], "--iters=", &iterations))
            continue;
        (void)fprintf(stderr, "usage: %s [--seed=N] [--iters=N]\n",
                      argv[0]);
        return 2;
    }
    if (iterations < UNIT_FUZZ_MIN_ITERS)
        iterations = UNIT_FUZZ_MIN_ITERS;
    run = (FuzzRun){seed == 0U ? UINT64_C(0x9e3779b97f4a7c15) : seed,
                    UINT64_C(1469598103934665603)};
    tb = random_buffer(&run);
    if (tb == NULL)
        return 2;
    buffer.tb = tb;
    buffer.tabwidth = 4U;
    u = (UnitCtx){tb, &buffer, NULL};
    for (op = 0U; op < iterations; op++) {
        const UnitOps *ops = engines[random_next(&run) %
                                     SAG_ARRAY_LEN(engines)];
        ByteOff raw = BYTEOFF(random_next(&run) %
                              (sag_textbuf_len(tb) + 1U));
        ByteOff p = sag_is_grapheme_boundary(tb, raw)
                        ? raw
                        : sag_grapheme_prev(tb, raw);
        bool alt = (random_next(&run) & 1U) != 0U;

        if (!check_one(&run, &u, ops, p, alt, op)) {
            sag_textbuf_free(tb);
            return 1;
        }
        if ((op & 4095U) == 0U && !check_termination(&u, ops, alt)) {
            (void)fprintf(stderr,
                          "fuzz_units: engine=%s termination failed\n",
                          ops->name);
            sag_textbuf_free(tb);
            return 1;
        }
    }
    sag_textbuf_free(tb);
    if (!check_large_single_line()) {
        (void)fprintf(stderr, "fuzz_units: 1 MiB single-line check failed\n");
        return 1;
    }
    (void)printf("fuzz_units: seed=%llu ops=%llu hash=%016llx ok\n",
                 (unsigned long long)seed,
                 (unsigned long long)iterations,
                 (unsigned long long)run.hash);
    return 0;
}
