#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "util/arena.h"
#include "util/prof.h"

static void set_ring_env(const char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv("YEW_PROF_RING"), 0);
    else
        YEW_ASSERT_EQ_I64(setenv("YEW_PROF_RING", value, 1), 0);
}

void test_prof_frame_is_one_cache_line(void)
{
    YEW_ASSERT_EQ_U64(sizeof(ProfFrame), 64U);
    YEW_ASSERT_EQ_U64(YEW_PH_COUNT, 8U);
    YEW_ASSERT_EQ_U64(offsetof(ProfFrame, total_ns), 48U);
    YEW_ASSERT_EQ_U64(offsetof(ProfFrame, flags), 58U);
}

void test_prof_off_is_all_noop(void)
{
    Arena arena;
    Prof prof;
    Bytebuf out;

    arena_init(&arena);
    yew_prof_init(&prof, &arena, false);
    YEW_ASSERT(!prof.on);
    YEW_ASSERT_NULL(prof.ring);
    YEW_ASSERT_NULL(arena.head);
    yew_prof_phase(&prof, YEW_PH_INPUT);
    yew_prof_frame_begin(&prof);
    yew_prof_frame_end(&prof, 4U, 99U, YEW_PF_RESIZE);
    yew_prof_reset(&prof);
    YEW_ASSERT_EQ_U64(prof.n, 0U);
    YEW_ASSERT_EQ_U64(prof.seq, 0U);
    bytebuf_init(&out);
    yew_prof_write(&prof, &out);
    YEW_ASSERT(strstr((const char *)out.data, "frames=0 dropped=0") != NULL);
    bytebuf_free(&out);
    arena_free_all(&arena);
}

void test_prof_ring_wrap_retains_newest(void)
{
    Arena arena;
    Prof prof;
    u32 i;

    set_ring_env("1");
    arena_init(&arena);
    yew_prof_init(&prof, &arena, true);
    YEW_ASSERT_EQ_U64(prof.cap, 64U);
    for (i = 0U; i <= prof.cap; i++) {
        yew_prof_frame_begin(&prof);
        yew_prof_frame_end(&prof, (u16)i, i, 0U);
    }
    YEW_ASSERT_EQ_U64(prof.n, 64U);
    YEW_ASSERT_EQ_U64(prof.seq, 65U);
    YEW_ASSERT_EQ_U64(prof.head, 1U);
    YEW_ASSERT_EQ_U64(prof.ring[1].seq, 1U);
    YEW_ASSERT_EQ_U64(prof.ring[0].seq, 64U);
    YEW_ASSERT_EQ_U64(prof.ring[0].bytes_out, 64U);
    set_ring_env(NULL);
    arena_free_all(&arena);
}

void test_prof_phase_reentry_and_frame_end_close(void)
{
    Arena arena;
    Prof prof;
    const ProfFrame *frame;

    set_ring_env("64");
    arena_init(&arena);
    yew_prof_init(&prof, &arena, true);
    yew_prof_frame_begin(&prof);
    yew_prof_phase(&prof, YEW_PH_INPUT);
    yew_prof_phase(&prof, YEW_PH_JOBS);
    yew_prof_phase(&prof, YEW_PH_INPUT);
    yew_prof_frame_end(&prof, 3U, 17U, YEW_PF_JOB_IO);
    frame = &prof.ring[0];
    YEW_ASSERT(frame->ph_ns[YEW_PH_INPUT] > 0U);
    YEW_ASSERT(frame->ph_ns[YEW_PH_JOBS] > 0U);
    YEW_ASSERT(frame->total_ns >= frame->ph_ns[YEW_PH_INPUT]);
    YEW_ASSERT_EQ_U64(frame->keys, 3U);
    YEW_ASSERT_EQ_U64(frame->bytes_out, 17U);
    YEW_ASSERT_EQ_U64(frame->flags, YEW_PF_JOB_IO);
    YEW_ASSERT_EQ_U64(prof.open, YEW_PH_COUNT);
    YEW_ASSERT_EQ_U64(prof.n, 1U);
    set_ring_env(NULL);
    arena_free_all(&arena);
}

void test_prof_reset_keeps_allocation(void)
{
    Arena arena;
    Prof prof;
    ProfFrame *ring;

    arena_init(&arena);
    yew_prof_init(&prof, &arena, true);
    ring = prof.ring;
    yew_prof_frame_begin(&prof);
    yew_prof_frame_end(&prof, 0U, 0U, 0U);
    memcpy(prof.mark, "before", sizeof("before"));
    yew_prof_reset(&prof);
    YEW_ASSERT(prof.on);
    YEW_ASSERT(prof.ring == ring);
    YEW_ASSERT_EQ_U64(prof.n, 0U);
    YEW_ASSERT_EQ_U64(prof.head, 0U);
    YEW_ASSERT_EQ_U64(prof.seq, 0U);
    YEW_ASSERT_EQ_U64(prof.open, YEW_PH_COUNT);
    YEW_ASSERT_EQ_STR(prof.mark, "");
    arena_free_all(&arena);
}
