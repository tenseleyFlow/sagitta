#ifndef YEW_UTIL_PROF_H
#define YEW_UTIL_PROF_H

#include <stdbool.h>

#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

typedef enum {
    YEW_PH_POLL,
    YEW_PH_INPUT,
    YEW_PH_DISPATCH,
    YEW_PH_JOBS,
    YEW_PH_LAYOUT,
    YEW_PH_SYN,
    YEW_PH_RENDER,
    YEW_PH_WRITE,
    YEW_PH_COUNT
} YewPhase;

typedef struct ProfFrame {
    u64 seq;
    u64 t_mono_ns;
    u32 ph_ns[YEW_PH_COUNT];
    u32 total_ns;
    u32 bytes_out;
    u16 keys;
    u16 flags;
} ProfFrame;

_Static_assert(sizeof(ProfFrame) == 64, "one frame is one cache line");

enum {
    YEW_PF_FULL_DAMAGE = 1U << 0,
    YEW_PF_RESIZE = 1U << 1,
    YEW_PF_JOB_IO = 1U << 2,
    YEW_PF_MARK = 1U << 3,
    YEW_PF_BURST_CAP = 1U << 4
};

typedef struct Prof {
    bool on;
    ProfFrame *ring;
    u32 cap;
    u32 head;
    u32 n;
    u64 seq;
    u64 frame_t0;
    u64 phase_t0;
    YewPhase open;
    u64 overhead_ns;
    char mark[32];
} Prof;

u64 yew_now_ns(void);
void yew_prof_init(Prof *p, Arena *a, bool on);
void yew_prof_frame_begin(Prof *p);
void yew_prof_phase(Prof *p, YewPhase ph);
void yew_prof_frame_end(Prof *p, u16 keys, u32 bytes_out, u16 flags);
void yew_prof_reset(Prof *p);
void yew_prof_write(const Prof *p, Bytebuf *out);

#endif
