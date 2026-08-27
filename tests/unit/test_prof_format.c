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

static Bytebuf read_fixture(const char *path)
{
    Bytebuf out;
    u8 block[512];
    FILE *file = fopen(path, "rb");
    size_t n;

    YEW_ASSERT_NOT_NULL(file);
    bytebuf_init(&out);
    while ((n = fread(block, 1U, sizeof(block), file)) != 0U)
        bytebuf_append(&out, block, n);
    YEW_ASSERT(!ferror(file));
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    return out;
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
    assert_contains(&out,
                    "KEYPAINT           0        0        0        0 calls=0\n");
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

void test_prof_format_keypaint_matches_external_population(void)
{
    enum { KEY_FRAMES = 1000U, BACKGROUND_FRAMES = 25U };
    ProfFrame *frames = calloc(KEY_FRAMES + BACKGROUND_FRAMES,
                               sizeof(*frames));
    Prof prof;
    Bytebuf out;
    u32 i;

    YEW_ASSERT_NOT_NULL(frames);
    for (i = 0U; i < KEY_FRAMES; i++) {
        frames[i].seq = i;
        frames[i].total_ns = i + 1U;
        frames[i].keys = 1U;
        frames[i].bytes_out = 1U;
        frames[i].flags = YEW_PF_KEY_PAINT;
    }
    for (; i < KEY_FRAMES + BACKGROUND_FRAMES; i++) {
        frames[i].seq = i;
        frames[i].total_ns = 1000000U;
    }
    frames[KEY_FRAMES + BACKGROUND_FRAMES - 2U].keys = 2U;
    frames[KEY_FRAMES + BACKGROUND_FRAMES - 2U].bytes_out = 1U;
    frames[KEY_FRAMES + BACKGROUND_FRAMES - 1U].keys = 1U;
    /* The performance OSC tag is output, but zero visible bytes is not a
     * paint in the external causality protocol. */
    frames[KEY_FRAMES + BACKGROUND_FRAMES - 1U].bytes_out = 24U;
    synthetic_prof(&prof, frames, KEY_FRAMES + BACKGROUND_FRAMES);
    bytebuf_init(&out);
    yew_prof_write(&prof, &out);
    assert_contains(&out,
                    "TOTAL            513      923  1000000  1000000\n");
    assert_contains(&out,
                    "KEYPAINT         500      900      990     1000 calls=1000\n");
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

void test_prof_frames_format_keeps_last_n_in_order(void)
{
    ProfFrame frames[5];
    Prof prof;
    Bytebuf out;

    fill_linear(frames, YEW_ARRAY_LEN(frames));
    frames[3].flags = YEW_PF_MARK;
    frames[4].flags = YEW_PF_JOB_IO | YEW_PF_BURST_CAP;
    synthetic_prof(&prof, frames, YEW_ARRAY_LEN(frames));
    prof.batch = true;
    bytebuf_init(&out);
    yew_prof_write_frames(&prof, &out, 2U);
    assert_contains(&out,
                    "# yew prof frames v1  frames=5 shown=2 mark=fixture "
                    "mode=batch phases=dispatch,jobs,syn\n");
    YEW_ASSERT_NULL(strstr((const char *)out.data,
                           "\n2 100002 30 "));
    assert_contains(&out, "\n3 100003 40 9000 4 8 0 0 0 0 0 1 3 MARK\n");
    assert_contains(&out,
                    "\n4 100004 50 9000 5 10 0 0 0 0 0 1 4 JOB,BURST\n");
    bytebuf_free(&out);
}

void test_prof_format_committed_golden(void)
{
    ProfFrame frames[3];
    Prof prof;
    Bytebuf out;
    Bytebuf golden;

    fill_linear(frames, YEW_ARRAY_LEN(frames));
    frames[1].flags = YEW_PF_KEY_PAINT;
    frames[2].flags = YEW_PF_KEY_PAINT | YEW_PF_FULL_DAMAGE | YEW_PF_MARK;
    synthetic_prof(&prof, frames, YEW_ARRAY_LEN(frames));
    bytebuf_init(&out);
    yew_prof_write(&prof, &out);
    golden = read_fixture("tests/unit/fixtures/prof-report.golden");
    YEW_ASSERT_EQ_U64(out.len, golden.len);
    YEW_ASSERT_EQ_MEM(out.data, golden.data, out.len);
    bytebuf_free(&golden);
    bytebuf_free(&out);
}
