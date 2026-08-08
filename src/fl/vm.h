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
    FlFn **compiling;
    u32 ncompiling;
    FlGc gc;
    const FlHost *host;
    Arena *arena;
    Interner *in;
    DiagCtx *dc;
    u64 steps;
    u64 step_limit;              /* 0 = unlimited; Sprint 32 uses it      */
    FlValue err;                 /* the in-flight raised value            */
};

bool fl_vm_init(FlVm *vm, Arena *a, Interner *in, DiagCtx *dc);
bool fl_vm_run(FlVm *vm, FlFn *entry, FlValue *out);   /* false = raised */
void fl_vm_free(FlVm *vm);

#endif /* SAG_FL_VM_H */
