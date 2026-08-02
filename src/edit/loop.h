#ifndef SAG_EDIT_LOOP_H
#define SAG_EDIT_LOOP_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

#define SAG_INPUT_BURST_MAX (256U * 1024U)

typedef struct Ed Ed;
typedef struct SagTimer SagTimer;
typedef u64 TimerId;
typedef void (*TimerFn)(Ed *ed, void *ctx);

#define SAG_TIMER_NONE ((TimerId)0)

typedef struct TimerHeap {
    SagTimer *v;
    size_t len;
    size_t cap;
    u64 next_id;
    u64 next_order;
} TimerHeap;

void sag_timers_init(TimerHeap *timers);
void sag_timers_free(TimerHeap *timers);
TimerId sag_timer_add(TimerHeap *timers, i64 at_ms, TimerFn fn, void *ctx);
bool sag_timer_cancel(TimerHeap *timers, TimerId id);
i64 sag_timers_deadline(const TimerHeap *timers, i64 now_ms);
void sag_timers_fire(TimerHeap *timers, Ed *ed, i64 now_ms);

i64 sag_now_ms(void);
int sag_loop_deadline(const Ed *ed, i64 now_ms);
int sag_loop_run(Ed *ed);

#endif
