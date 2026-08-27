#define _POSIX_C_SOURCE 200809L

#include "util/prof.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/log.h"
#include "util/sort.h"

enum {
    YEW_PROF_RING_DEFAULT = 4096U,
    YEW_PROF_RING_MIN = 64U,
    YEW_PROF_RING_MAX = 262144U,
    YEW_PROF_OVERHEAD_SAMPLES = 1024U,
    YEW_PROF_WORST_FRAMES = 10U
};

static const char *const phase_names[YEW_PH_COUNT] = {
    "poll", "input", "dispatch", "jobs", "layout", "syn", "render",
    "write"
};

static int compare_u64(const void *lhs, const void *rhs, void *ctx)
{
    const u64 a = *(const u64 *)lhs;
    const u64 b = *(const u64 *)rhs;
    (void)ctx;
    return (a > b) - (a < b);
}

static int compare_u32(const void *lhs, const void *rhs, void *ctx)
{
    const u32 a = *(const u32 *)lhs;
    const u32 b = *(const u32 *)rhs;
    (void)ctx;
    return (a > b) - (a < b);
}

static int compare_frame_worst(const void *lhs, const void *rhs, void *ctx)
{
    const ProfFrame *a = lhs;
    const ProfFrame *b = rhs;
    (void)ctx;
    if (a->total_ns != b->total_ns)
        return a->total_ns < b->total_ns ? 1 : -1;
    return (a->seq > b->seq) - (a->seq < b->seq);
}

u64 yew_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("prof: monotonic clock failed");
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static u32 ring_cap_from_env(void)
{
    const char *value = getenv("YEW_PROF_RING");
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0')
        return YEW_PROF_RING_DEFAULT;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
        return YEW_PROF_RING_DEFAULT;
    if (parsed < YEW_PROF_RING_MIN)
        return YEW_PROF_RING_MIN;
    if (parsed > YEW_PROF_RING_MAX)
        return YEW_PROF_RING_MAX;
    return (u32)parsed;
}

static u64 measure_overhead(void)
{
    u64 samples[YEW_PROF_OVERHEAD_SAMPLES];
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(samples); i++) {
        u64 start = yew_now_ns();
        u64 end = yew_now_ns();
        samples[i] = end - start;
    }
    yew_sort_stable(samples, YEW_ARRAY_LEN(samples), sizeof(samples[0]),
                    compare_u64, NULL);
    return (samples[511] + samples[512]) / 2U;
}

void yew_prof_init(Prof *p, Arena *a, bool on)
{
    memset(p, 0, sizeof(*p));
    p->open = YEW_PH_COUNT;
    p->on = on;
    if (!on)
        return;
    p->cap = ring_cap_from_env();
    p->ring = arena_alloc(a, (size_t)p->cap * sizeof(*p->ring),
                          _Alignof(ProfFrame));
    memset(p->ring, 0, (size_t)p->cap * sizeof(*p->ring));
    p->overhead_ns = measure_overhead();
}

static u32 duration_u32(u64 start, u64 end)
{
    u64 elapsed = end >= start ? end - start : 0U;
    return elapsed > UINT32_MAX ? UINT32_MAX : (u32)elapsed;
}

static void add_duration(u32 *dst, u64 start, u64 end)
{
    u64 total = (u64)*dst + duration_u32(start, end);
    *dst = total > UINT32_MAX ? UINT32_MAX : (u32)total;
}

void yew_prof_frame_begin(Prof *p)
{
    ProfFrame *frame;
    u64 now;
    u32 poll_ns = 0U;

    if (!p->on)
        return;
    now = yew_now_ns();
    if (p->open == YEW_PH_POLL && p->phase_t0 != 0U)
        poll_ns = duration_u32(p->phase_t0, now);
    frame = &p->ring[p->head];
    memset(frame, 0, sizeof(*frame));
    frame->seq = p->seq;
    frame->t_mono_ns = now;
    frame->ph_ns[YEW_PH_POLL] = poll_ns;
    p->frame_t0 = now;
    p->phase_t0 = 0U;
    p->open = YEW_PH_COUNT;
}

void yew_prof_phase(Prof *p, YewPhase ph)
{
    u64 now;

    if (!p->on)
        return;
    if (ph >= YEW_PH_COUNT)
        YEW_BUG("prof: invalid phase %d", (int)ph);
    now = yew_now_ns();
    if (p->frame_t0 != 0U && p->open < YEW_PH_COUNT)
        add_duration(&p->ring[p->head].ph_ns[p->open], p->phase_t0, now);
    p->open = ph;
    p->phase_t0 = now;
}

void yew_prof_frame_end(Prof *p, u16 keys, u32 bytes_out, u16 flags)
{
    ProfFrame *frame;
    u64 now;

    if (!p->on || p->frame_t0 == 0U)
        return;
    now = yew_now_ns();
    frame = &p->ring[p->head];
    if (p->open < YEW_PH_COUNT)
        add_duration(&frame->ph_ns[p->open], p->phase_t0, now);
    frame->total_ns = duration_u32(p->frame_t0, now);
    frame->keys = keys;
    frame->bytes_out = bytes_out;
    if (p->mark_pending) {
        flags |= YEW_PF_MARK;
        p->mark_pending = false;
    }
    frame->flags = flags;
    p->head = (p->head + 1U) % p->cap;
    if (p->n < p->cap)
        p->n++;
    p->seq++;
    p->frame_t0 = 0U;
    p->phase_t0 = 0U;
    p->open = YEW_PH_COUNT;
}

bool yew_prof_mark(Prof *p, const char *label, size_t len)
{
    if (!p->on || label == NULL || len == 0U || len >= sizeof(p->mark))
        return false;
    memcpy(p->mark, label, len);
    p->mark[len] = '\0';
    p->mark_pending = true;
    return true;
}

void yew_prof_reset(Prof *p)
{
    if (!p->on)
        return;
    p->head = 0U;
    p->n = 0U;
    p->seq = 0U;
    p->frame_t0 = 0U;
    p->phase_t0 = 0U;
    p->open = YEW_PH_COUNT;
    p->mark[0] = '\0';
    p->mark_pending = false;
}

static const ProfFrame *chronological_frame(const Prof *p, u32 index)
{
    u32 oldest = (p->head + p->cap - p->n) % p->cap;
    return &p->ring[(oldest + index) % p->cap];
}

static u32 percentile(u32 *values, u32 n, u32 pct)
{
    u64 rank;

    if (n == 0U)
        return 0U;
    yew_sort_stable(values, n, sizeof(values[0]), compare_u32, NULL);
    rank = ((u64)n * pct + 99U) / 100U;
    if (rank == 0U)
        rank = 1U;
    return values[rank - 1U];
}

static void append_share(Bytebuf *out, u64 numerator, u64 denominator)
{
    u64 permille = denominator == 0U ? 0U : numerator * 1000U / denominator;
    bytebuf_printf(out, "%3" PRIu64 ".%" PRIu64 "%%",
                   permille / 10U, permille % 10U);
}

static void append_flags(Bytebuf *out, u16 flags)
{
    static const struct {
        u16 bit;
        const char *name;
    } names[] = {
        {YEW_PF_FULL_DAMAGE, "FULL"},
        {YEW_PF_RESIZE, "RESIZE"},
        {YEW_PF_JOB_IO, "JOB"},
        {YEW_PF_MARK, "MARK"},
        {YEW_PF_BURST_CAP, "BURST"}
    };
    size_t i;
    bool any = false;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        if ((flags & names[i].bit) == 0U)
            continue;
        if (any)
            bytebuf_push_u8(out, ',');
        bytebuf_append(out, names[i].name, strlen(names[i].name));
        any = true;
    }
    if (!any)
        bytebuf_push_u8(out, '-');
}

static YewPhase top_phase(const ProfFrame *frame)
{
    YewPhase best = YEW_PH_INPUT;
    YewPhase ph;

    for (ph = YEW_PH_DISPATCH; ph < YEW_PH_COUNT; ph++) {
        if (frame->ph_ns[ph] > frame->ph_ns[best])
            best = ph;
    }
    return best;
}

void yew_prof_write(const Prof *p, Bytebuf *out)
{
    u32 *values;
    ProfFrame *worst;
    u64 total_sum = 0U;
    u64 phase_sum[YEW_PH_COUNT] = {0U};
    u64 accounted = 0U;
    u32 i;
    YewPhase ph;

    bytebuf_printf(out,
                   "# yew prof v1  frames=%" PRIu32 " dropped=%" PRIu64
                   " overhead_ns=%" PRIu64 " mark=%s%s\n",
                   p->n, p->seq >= p->n ? p->seq - p->n : 0U,
                   p->overhead_ns, p->mark[0] == '\0' ? "-" : p->mark,
                   p->batch ? " mode=batch phases=dispatch,jobs,syn" : "");
    bytebuf_append(out,
                   "phase        p50      p90      p99      max    share   calls\n",
                   sizeof("phase        p50      p90      p99      max    share   calls\n") - 1U);
    if (p->n == 0U) {
        values = NULL;
        worst = NULL;
    } else {
        values = yew_xreallocarray(NULL, p->n, sizeof(*values));
        worst = yew_xreallocarray(NULL, p->n, sizeof(*worst));
    }
    for (i = 0U; i < p->n; i++) {
        const ProfFrame *frame = chronological_frame(p, i);
        total_sum += frame->total_ns;
        worst[i] = *frame;
        for (ph = YEW_PH_POLL; ph < YEW_PH_COUNT; ph++)
            phase_sum[ph] += frame->ph_ns[ph];
    }
    for (ph = YEW_PH_INPUT; ph < YEW_PH_COUNT; ph++) {
        u32 calls = 0U;
        u32 p50;
        u32 p90;
        u32 p99;
        u32 max;

        for (i = 0U; i < p->n; i++) {
            values[i] = chronological_frame(p, i)->ph_ns[ph];
            if (values[i] != 0U)
                calls++;
        }
        p50 = percentile(values, p->n, 50U);
        p90 = percentile(values, p->n, 90U);
        p99 = percentile(values, p->n, 99U);
        max = p->n == 0U ? 0U : values[p->n - 1U];
        bytebuf_printf(out, "%-11s %8" PRIu32 " %8" PRIu32 " %8" PRIu32
                       " %8" PRIu32 "  ",
                       phase_names[ph], p50, p90, p99, max);
        append_share(out, phase_sum[ph], total_sum);
        bytebuf_printf(out, " %8" PRIu32 "\n", calls);
        accounted += phase_sum[ph];
    }
    {
        u64 unaccounted = total_sum > accounted ? total_sum - accounted : 0U;
        bytebuf_append(out, "unaccounted                                      ",
                       sizeof("unaccounted                                      ") - 1U);
        append_share(out, unaccounted, total_sum);
        bytebuf_push_u8(out, '\n');
    }
    for (i = 0U; i < p->n; i++)
        values[i] = chronological_frame(p, i)->total_ns;
    {
        u32 p50 = percentile(values, p->n, 50U);
        u32 p90 = percentile(values, p->n, 90U);
        u32 p99 = percentile(values, p->n, 99U);
        u32 max = p->n == 0U ? 0U : values[p->n - 1U];
        bytebuf_printf(out, "TOTAL       %8" PRIu32 " %8" PRIu32 " %8" PRIu32
                       " %8" PRIu32 "\n", p50, p90, p99, max);
    }
    {
        u32 keypaint = 0U;
        u32 p50;
        u32 p90;
        u32 p99;
        u32 max;

        /* The external latency harness injects one key spelling at a time
         * and samples only completed paints.  Keep TOTAL as the all-wake
         * diagnostic, but expose the matching population so its p99 can be
         * cross-checked without admitting timer/job wakes or no-op keys. */
        for (i = 0U; i < p->n; i++) {
            const ProfFrame *frame = chronological_frame(p, i);

            if (frame->keys == 1U && frame->bytes_out != 0U)
                values[keypaint++] = frame->total_ns;
        }
        p50 = percentile(values, keypaint, 50U);
        p90 = percentile(values, keypaint, 90U);
        p99 = percentile(values, keypaint, 99U);
        max = keypaint == 0U ? 0U : values[keypaint - 1U];
        bytebuf_printf(out,
                       "KEYPAINT    %8" PRIu32 " %8" PRIu32 " %8" PRIu32
                       " %8" PRIu32 " calls=%" PRIu32 "\n",
                       p50, p90, p99, max, keypaint);
    }
    bytebuf_append(out,
                   "poll     (asleep, excluded)\n"
                   "--- worst frames\n"
                   "  seq      total_ns  keys  bytes  flags        top phase\n",
                   sizeof("poll     (asleep, excluded)\n"
                          "--- worst frames\n"
                          "  seq      total_ns  keys  bytes  flags        top phase\n") - 1U);
    yew_sort_stable(worst, p->n, sizeof(*worst), compare_frame_worst, NULL);
    for (i = 0U; i < p->n && i < YEW_PROF_WORST_FRAMES; i++) {
        YewPhase top = top_phase(&worst[i]);
        Bytebuf flags;

        bytebuf_init(&flags);
        append_flags(&flags, worst[i].flags);
        bytebuf_printf(out, "%7" PRIu64 " %12" PRIu32 " %5" PRIu16
                       " %6" PRIu32 "  %-12.*s %s %" PRIu32 "\n",
                       worst[i].seq, worst[i].total_ns, worst[i].keys,
                       worst[i].bytes_out, (int)flags.len,
                       flags.len == 0U ? "" : (const char *)flags.data,
                       phase_names[top], worst[i].ph_ns[top]);
        bytebuf_free(&flags);
    }
    free(worst);
    free(values);
    bytebuf_reserve(out, out->len + 1U);
    out->data[out->len] = '\0';
}

void yew_prof_write_frames(const Prof *p, Bytebuf *out, u32 limit)
{
    u32 shown = limit < p->n ? limit : p->n;
    u32 first = p->n - shown;
    u32 i;

    bytebuf_printf(out,
                   "# yew prof frames v1  frames=%" PRIu32
                   " shown=%" PRIu32 " mark=%s%s\n",
                   p->n, shown, p->mark[0] == '\0' ? "-" : p->mark,
                   p->batch ? " mode=batch phases=dispatch,jobs,syn" : "");
    bytebuf_append(out,
                   "seq t_mono_ns total_ns poll input dispatch jobs layout "
                   "syn render write keys bytes flags\n",
                   sizeof("seq t_mono_ns total_ns poll input dispatch jobs "
                          "layout syn render write keys bytes flags\n") - 1U);
    for (i = first; i < p->n; i++) {
        const ProfFrame *frame = chronological_frame(p, i);

        bytebuf_printf(out,
                       "%" PRIu64 " %" PRIu64 " %" PRIu32
                       " %" PRIu32 " %" PRIu32 " %" PRIu32
                       " %" PRIu32 " %" PRIu32 " %" PRIu32
                       " %" PRIu32 " %" PRIu32 " %" PRIu16
                       " %" PRIu32 " ",
                       frame->seq, frame->t_mono_ns, frame->total_ns,
                       frame->ph_ns[YEW_PH_POLL],
                       frame->ph_ns[YEW_PH_INPUT],
                       frame->ph_ns[YEW_PH_DISPATCH],
                       frame->ph_ns[YEW_PH_JOBS],
                       frame->ph_ns[YEW_PH_LAYOUT],
                       frame->ph_ns[YEW_PH_SYN],
                       frame->ph_ns[YEW_PH_RENDER],
                       frame->ph_ns[YEW_PH_WRITE], frame->keys,
                       frame->bytes_out);
        append_flags(out, frame->flags);
        bytebuf_push_u8(out, '\n');
    }
    bytebuf_reserve(out, out->len + 1U);
    out->data[out->len] = '\0';
}
