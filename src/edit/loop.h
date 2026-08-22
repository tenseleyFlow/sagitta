#ifndef YEW_EDIT_LOOP_H
#define YEW_EDIT_LOOP_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

#define YEW_INPUT_BURST_MAX (256U * 1024U)

typedef struct Ed Ed;
typedef struct Key Key;
typedef struct YewTimer YewTimer;
typedef u64 TimerId;
typedef void (*TimerFn)(Ed *ed, void *ctx);

#define YEW_TIMER_NONE ((TimerId)0)

typedef struct TimerHeap {
    YewTimer *v;
    size_t len;
    size_t cap;
    u64 next_id;
    u64 next_order;
} TimerHeap;

void yew_timers_init(TimerHeap *timers);
void yew_timers_free(TimerHeap *timers);
TimerId yew_timer_add(TimerHeap *timers, i64 at_ms, TimerFn fn, void *ctx);
bool yew_timer_cancel(TimerHeap *timers, TimerId id);
i64 yew_timers_deadline(const TimerHeap *timers, i64 now_ms);
void yew_timers_fire(TimerHeap *timers, Ed *ed, i64 now_ms);

i64 yew_now_ms(void);
int yew_loop_deadline(const Ed *ed, i64 now_ms);
void yew_loop_dispatch_event(Ed *ed, const Key *key, i64 now_ms);
u32 yew_loop_settle_jobs(Ed *ed);
int yew_loop_run(Ed *ed);

#endif
