#ifndef SAG_FL_GC_H
#define SAG_FL_GC_H

/*
 * Sprint 30 deliverable 9: a precise mark-sweep collector with an
 * ENUMERATED root set.
 *
 * THE HANDLE-PROTECTION DISCIPLINE.  Pinned here, in the header every
 * caller reads, because a value reachable only from a C local is
 * invisible to the collector and the resulting bug is a wrong answer
 * once a year rather than a crash in a test:
 *
 *   1. Native arguments arrive as `FlValue *argv` pointing INTO the VM
 *      stack and stay live for the whole call.  A native must never
 *      copy an FlObj* into a C local across an allocation.
 *   2. Intermediates are protected: fl_gc_protect(vm, v) ...
 *      fl_gc_release(vm, 1), strictly LIFO, capped at 32.  Overflow is
 *      SAG_BUG -- it means the discipline leaked, not that a user
 *      program did something interesting.
 *   3. Host callbacks (Sprint 34) take handles by table SLOT, never by
 *      pointer.  A slot lookup fails cleanly against a dead object; a
 *      stale pointer does not fail at all.
 *   4. Under stress the collector runs at EVERY allocation and poisons
 *      swept memory with 0xDD, so a rule-2 violation fails on the first
 *      test that exercises it, under ASan, naming the line.
 */

#include <stdbool.h>
#include <stddef.h>

#include "fl/value.h"
#include "util/base.h"

typedef struct FlVm FlVm;

/*
 * The interned-string table is WEAK: an entry does not keep its FlStr
 * alive.  Sweep clears the dead entries BEFORE any white object is
 * freed -- see the pin in gc.c, it is the single most commonly botched
 * line in a mark-sweep collector.
 */
typedef struct FlStrTab {
    FlStr **v;
    u32 cap;
    u32 n;
} FlStrTab;

typedef struct FlGc {
    FlObj *objects;        /* the all-objects list                        */
    FlObj **gray;
    u32 ngray;
    u32 graycap;
    FlStrTab strings;      /* weak                                        */
    size_t bytes;
    size_t next_gc;
    bool stress;
    /*
     * Set by an allocation that crossed the threshold, consumed by the
     * dispatch loop.  Collection happens ONLY at opcode boundaries: mid
     * instruction the VM holds object pointers in C locals that no root
     * covers, which is exactly the situation rule 1 exists to describe.
     */
    bool pending;
    /* How many collections have run.  Observable state, because "the
     * collector ran during this program" is otherwise only inferable
     * from byte deltas -- and DoD 5 needs to assert it per dispatch
     * mode, where a wrong answer means the cgoto build has no GC. */
    u64 collections;
    /*
     * The WORST single collection pause, in nanoseconds, and the total.
     *
     * Recorded because Sprint 33's bench suite reports it and Sprint 56
     * turns it into a gate: a collector whose average is fine and whose
     * p99 is 40 ms fails invariant 4 on exactly the keystroke the user
     * notices.  Two clock reads per collection, and collections are
     * rare by construction, so this is not on any hot path.
     */
    u64 pause_max_ns;
    u64 pause_total_ns;
    /*
     * Monotone allocation counter, stamped into FlObj.aux for every
     * non-string object.  list.sort orders otherwise-incomparable
     * objects by it, because ordering them by ADDRESS would differ
     * between runs and break invariant 5 -- the same script would sort
     * a mixed list differently on two machines.
     *
     * u32, so it wraps after four billion allocations in one VM; two
     * objects could then compare equal and the sort stays STABLE
     * between them, which is a defined answer rather than a wrong one.
     */
    u32 next_seq;
} FlGc;

/* Floor for next_gc.  Sprint 33 gates config load under 1 ms, and a
 * mark phase does not fit in that budget -- so the first collection
 * must not fire during a default startup. */
enum { FL_GC_FIRST = 256U * 1024U };

/* Poison byte for swept memory under stress. */
enum { FL_GC_POISON = 0xDD };

void fl_gc_init(FlGc *gc);
void *fl_gc_alloc(FlVm *vm, size_t n, FlType t);
void fl_gc_collect(FlVm *vm);
void fl_gc_free_all(FlVm *vm);

/* Rule 2's temp-root stack.  Strictly LIFO. */
void fl_gc_protect(FlVm *vm, FlValue v);
void fl_gc_release(FlVm *vm, u32 n);

/* Interning (deliverable 10).  Short strings get one object per
 * distinct content; longer ones are plain heap objects with the hash
 * cached, because interning a 4 MiB io.read result buys a permanent
 * table entry for a string nobody looks up twice. */
FlStr *fl_str_new(FlVm *vm, const char *b, u32 n);
FlStr *fl_str_take(FlVm *vm, Bytebuf *bb);

FlList *fl_list_new(FlVm *vm);
bool fl_list_push(FlVm *vm, FlList *l, FlValue v);
/* The closure's upvalue array.  Allocated here rather than in vm.c
 * because gc.c owns the heap, and freed by obj_free with its closure. */
FlUpval **fl_gc_upvals(FlVm *vm, u32 n);

FlMap *fl_map_new(FlVm *vm);
/* Insert or overwrite.  A re-inserted deleted key APPENDS; see the
 * ordering rules in value.c. */
bool fl_map_set(FlVm *vm, FlMap *m, FlValue k, FlValue v);

#endif /* SAG_FL_GC_H */
