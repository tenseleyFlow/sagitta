#ifndef SAG_FL_VM_H
#define SAG_FL_VM_H

/* Sprint 30 deliverables 7 and 11: the VM state and the host seam. */

#include <stdbool.h>

#include "fl/diag.h"
#include "fl/gc.h"
#include "fl/value.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/intern.h"

enum {
    FL_STACK_MAX = 16384,     /* 256 KiB of FlValue                       */
    FL_FRAMES_MAX = 256,
    FL_HANDLERS_MAX = 64,
    FL_TEMP_MAX = 32          /* gc.h rule 2's cap                        */
};

typedef struct FlFrame {
    FlClosure *cl;
    const u8 *ip;
    FlValue *slots;
} FlFrame;

typedef struct FlHandler {
    u32 pc;
    FlValue *sp;
    u32 frame;
} FlHandler;

/* Spec §9's raised value, in flight. */
struct FlErr {
    u32 kind_id;      /* interned kind name: "div", "name", "motion", ... */
    FlValue payload;
};

/*
 * Sprint 34's handle table.  Rooted NOW and empty now, deliberately:
 * a subsystem that adds a root later is the failure mode where it
 * works in every test that never collects and drops objects in
 * production.
 */
typedef struct FlHandleTab {
    FlValue *v;
    u32 n;
    u32 cap;
} FlHandleTab;

typedef struct FlMotionProg FlMotionProg;

/*
 * The host seam.  Nothing in vm.c knows what a buffer is.
 *
 * fl_host_null.motion raises kind "motion", which is spec §3.1 -- a
 * headless VM without an editor host cannot run one -- and is exactly
 * what makes the spec's §14 `shout` example return "MOTION".
 */
typedef struct FlHost {
    void *ud;
    bool (*motion)(void *ud, const FlMotionProg *p, FlErr *err);
    bool (*edit_begin)(void *ud, FlErr *err);
    bool (*edit_end)(void *ud, bool ok, FlErr *err);
} FlHost;

extern const FlHost fl_host_null;

struct FlVm {
    FlValue stack[FL_STACK_MAX];
    FlValue *sp;
    FlFrame frames[FL_FRAMES_MAX];
    u32 nframes;
    FlHandler handlers[FL_HANDLERS_MAX];
    u32 nhandlers;
    FlUpval *open_upvals;        /* descending slot order                 */
    FlMap *globals;
    FlMap *modules;              /* Sprint 31 fills; rooted now           */
    FlHandleTab handles;         /* Sprint 34 fills; rooted now           */
    FlValue temp[FL_TEMP_MAX];
    u32 ntemp;
    /*
     * The compiler's in-progress function chain.  Rooted because
     * compilation allocates FlFn and FlStr and can therefore trigger a
     * collection -- root 8 in the sprint's table, and the one that is
     * least obvious until it drops a half-built function.
     */
    /*
     * Root 8: every FlFn produced during the compile currently in
     * progress.  A nested function is stored in its parent's constant
     * VEC, which is compiler scratch and not a root, so between its
     * allocation and the parent chunk reaching the arena it is
     * reachable from nothing the collector can see.
     *
     * NOTE the limit of this root: the constant pool's FlStr entries
     * are NOT covered by it, so compilation is not collection-safe and
     * fl_gc_alloc deliberately never collects -- it sets `pending`, and
     * the VM honours it at the next instruction boundary, by which time
     * every chunk is built.  Root 8 is what keeps that true for the
     * FUNCTIONS across a nested compile; making the whole compiler
     * collection-safe is not this sprint's job and is not claimed here.
     */
    FlFn *compiling[FL_FRAMES_MAX];
    u32 ncompiling;
    FlGc gc;
    const FlHost *host;
    Arena *arena;
    Interner *in;
    DiagCtx *dc;
    u64 steps;
    u64 step_limit;              /* 0 = unlimited; Sprint 32 uses it      */
#if FL_VM_TRACE
    /*
     * DoD 5's differential-dispatch driver: every executed opcode byte,
     * in order.  Compiled out entirely by default -- a trace push in the
     * hot loop is exactly the kind of cost invariant 4 does not have
     * room for, and a runtime `if (tracing)` would leave the branch
     * behind in the release build.
     */
    Bytebuf trace;
#endif
    FlValue err;                 /* the in-flight raised value            */
};

bool fl_vm_init(FlVm *vm, Arena *a, Interner *in, DiagCtx *dc);
bool fl_vm_run(FlVm *vm, FlFn *entry, FlValue *out);   /* false = raised */
void fl_vm_free(FlVm *vm);

#endif /* SAG_FL_VM_H */
