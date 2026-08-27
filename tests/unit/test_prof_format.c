#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/buf.h"
#include "util/prof.h"

static void synthetic_prof(Prof *prof, ProfFrame *frames, u32 n)
{
    memset(prof, 0, sizeof(*prof));
    prof->on = true;
    prof->ring = frames;
    prof->cap = n;
    prof->n = n;
    prof->head = 0U;
    prof->seq = n;
    prof->overhead_ns = 23U;
    memcpy(prof->mark, "fixture", sizeof("fixture"));
}

static void fill_linear(ProfFrame *frames, u32 n)
{
    u32 i;

    memset(frames, 0, (size_t)n * sizeof(*frames));
    for (i = 0U; i < n; i++) {
        frames[i].seq = i;
        frames[i].t_mono_ns = 100000U + i;
        frames[i].ph_ns[YEW_PH_INPUT] = i + 1U;
        frames[i].ph_ns[YEW_PH_DISPATCH] = (i + 1U) * 2U;
        frames[i].ph_ns[YEW_PH_POLL] = 9000U;
        frames[i].total_ns = (i + 1U) * 10U;
        frames[i].keys = 1U;
        frames[i].bytes_out = i;
    }
}

static void assert_contains(const Bytebuf *out, const char *needle)
{
    YEW_ASSERT_NOT_NULL(out->data);
    YEW_ASSERT(strstr((const char *)out->data, needle) != NULL);
}

void test_prof_format_empty(void)
{
    Prof prof;
    Bytebuf out;

    memset(&prof, 0, sizeof(prof));
    bytebuf_init(&out);
    yew_prof_write(&prof, &out);
    assert_contains(&out, "# yew prof v1  frames=0 dropped=0 overhead_ns=0 mark=-\n");
    assert_contains(&out, "input              0        0        0        0");
    assert_contains(&out, "TOTAL              0        0        0        0\n");
    assert_contains(&out, "poll     (asleep, excluded)\n");
    bytebuf_free(&out);
}

void test_prof_format_synthetic_ring(void)
{
    ProfFrame frames[200];
    Prof prof;
    Bytebuf first;
    Bytebuf second;

    fill_linear(frames, YEW_ARRAY_LEN(frames));
    frames[199].flags = YEW_PF_FULL_DAMAGE | YEW_PF_RESIZE;
    synthetic_prof(&prof, frames, YEW_ARRAY_LEN(frames));
    bytebuf_init(&first);
    bytebuf_init(&second);
    yew_prof_write(&prof, &first);
    yew_prof_write(&prof, &second);
    YEW_ASSERT_EQ_U64(first.len, second.len);
    YEW_ASSERT_EQ_MEM(first.data, second.data, first.len);
    assert_contains(&first, "frames=200 dropped=0 overhead_ns=23 mark=fixture");
    assert_contains(&first, "input            100      180      198      200");
    assert_contains(&first, "dispatch         200      360      396      400");
    assert_contains(&first, "unaccounted");
    assert_contains(&first, "  199         2000     1    199  FULL,RESIZE  dispatch 400");
    assert_contains(&first, "  190         1910     1    190  -            dispatch 382");
    bytebuf_free(&second);
    bytebuf_free(&first);
}

static void check_percentile_case(u32 n, u32 p50, u32 p90, u32 p99)
{
    ProfFrame *frames = calloc(n, sizeof(*frames));
    Prof prof;
    Bytebuf out;
    char expected[96];
    u32 i;

    YEW_ASSERT_NOT_NULL(frames);
    for (i = 0U; i < n; i++) {
        frames[i].seq = i;
        frames[i].ph_ns[YEW_PH_INPUT] = i + 1U;
        frames[i].total_ns = i + 1U;
    }
    synthetic_prof(&prof, frames, n);
    bytebuf_init(&out);
    yew_prof_write(&prof, &out);
    (void)snprintf(expected, sizeof(expected),
                   "input       %8u %8u %8u %8u", p50, p90, p99, n);
    assert_contains(&out, expected);
    bytebuf_free(&out);
    free(frames);
}

void test_prof_format_percentile_edges(void)
{
    check_percentile_case(1U, 1U, 1U, 1U);
    check_percentile_case(2U, 1U, 2U, 2U);
    check_percentile_case(3U, 2U, 3U, 3U);
    check_percentile_case(100U, 50U, 90U, 99U);
    check_percentile_case(4096U, 2048U, 3687U, 4056U);
}
