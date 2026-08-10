#ifndef YEW_FL_VM_H
#define YEW_FL_VM_H

/* Sprint 30 deliverables 7 and 11: the VM state and the host seam. */

#include <stdbool.h>

#include "fl/diag.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/value.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/intern.h"

typedef struct Ed Ed;

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
    /*
     * The pc, IN THE CALLER'S CHUNK, of the CALL that pushed this
     * frame -- not the resume point.
     *
     * §6's pitfall: `ip` at the moment a frame is pushed already points
     * PAST the call, so a trace built from it names the next source
     * line.  That is wrong often enough to be maddening and rarely
     * enough to survive review, so the call's own pc is recorded here
     * once, where it is known for free.
     */
    u32 call_pc;
    /* The native that called in, or 0.  `list.map(f, ...)` puts a
     * native between two Fletch frames and the trace has to show it. */
    u32 via_native;
    /* Explicit edit{} depth at entry.  RETURN/HALT close any scopes the
     * frame opened before control goes back to its caller. */
    u32 edit_depth;
} FlFrame;

typedef struct FlHandler {
    u32 pc;
    FlValue *sp;
    u32 frame;
    /* The edit{} depth where this catch was established.  An error closes
     * only the edit scopes it crosses on the way to this handler. */
    u32 edit_depth;
} FlHandler;

/* Spec §9's raised value, in flight. */
struct FlErr {
    u32 kind_id;      /* interned kind name: "div", "name", "motion", ... */
    FlValue payload;
};

/*
 * Root 6.  Sprint 30 reserved it for "Sprint 34's handle table" and
 * Sprint 34 found it needed for the opposite thing.
 *
 * Handles turned out to be SCALARS -- a {slot, gen} pair naming an
 * editor object, with no heap children -- so the collector needs no
 * knowledge of them at all.  What it does need is the mirror problem:
 * FlValues the HOST retains and the program cannot reach.  A hook
 * closure lives in Ed, not in any Fletch variable, and a collector
 * that cannot see it frees a function that is about to be called.
 * That is the classic embedding crash, and it is what root 6 now
 * covers.
 *
 * POINTERS TO SLOTS, not copies of values: the host owns the storage
 * and writes through it, so a copy would go stale the first time a
 * hook was replaced.  Every registered address must outlive its
 * registration -- fl_gc_host_root_add is for stable addresses only,
 * and anything in a vector that reallocates uses the provider form
 * below instead.
 */
typedef struct FlHostRoots {
    FlValue **v;
    u32 n;
    u32 cap;
} FlHostRoots;

/*
 * Root 11: mark providers, for collections that MOVE.
 *
 * The hook table is a growing vector of FlValues; registering each
 * entry's address would hand the collector pointers that the next
 * push invalidates.  A provider is called during marking and walks
 * the live collection itself.
 *
 * The callback takes the VM rather than the FlGc the sprint sketched,
 * because a provider's whole job is to call fl_gc_mark_value and that
 * needs the VM -- an FlGc-only signature would force every provider
 * to recover the VM by pointer arithmetic on its own container.
 */
typedef void (*FlGcMarkFn)(FlVm *vm, void *ctx);

typedef struct FlGcProvider {
    FlGcMarkFn mark;
    void *ctx;
} FlGcProvider;

enum { FL_GC_PROVIDERS_MAX = 8 };

typedef struct FlMotionProg FlMotionProg;

/*
 * The host seam.  Nothing in vm.c knows what a buffer is.
 *
 * fl_host_null.motion raises kind "motion", which is spec §3.1 -- a
 * headless VM without an editor host cannot run one -- and is exactly
 * what makes the spec's §14 `shout` example return "MOTION".
 */
typedef struct FlHost {
    bool (*run_begin)(FlVm *vm);
    bool (*run_end)(FlVm *vm, bool ok);
    bool (*motion)(FlVm *vm, const FlMotionProg *p);
    bool (*edit_begin)(FlVm *vm);
    bool (*edit_end)(FlVm *vm, bool ok);
} FlHost;

extern const FlHost fl_host_null;

/*
 * Editor transaction state kept in the VM so one outer Fletch entry owns
 * an implicit transaction when it mutates outside explicit edit blocks.
 * `enlisted` is private to fltxn.c; the VM owns its storage lifetime.
 */
typedef struct FlTxn {
    void *enlisted;
    u32 n;
    u32 cap;
    u32 depth;
    bool entry_active;
} FlTxn;

struct FlVm {
    FlValue stack[FL_STACK_MAX];
    FlValue *sp;
    FlFrame frames[FL_FRAMES_MAX];
    u32 nframes;
    FlHandler handlers[FL_HANDLERS_MAX];
    u32 nhandlers;
    FlUpval *open_upvals;        /* descending slot order                 */
    FlMap *globals;
    /*
     * Root 5: every loaded module's exports, keyed by its index in
     * `mods`.  The map exists to ROOT them -- lookup is `mods`, which
     * carries the (realpath, origin kind) key the cache is really on
     * and the importer chain a cycle message walks.
     */
    FlMap *modules;
    FlModTab mods;
    FlHostRoots host_roots;      /* root 6 -- see FlHostRoots above       */
    FlGcProvider providers[FL_GC_PROVIDERS_MAX];
    u32 nproviders;
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
    FlTxn txn;
    u32 edit_depth;
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
    /*
     * Root 9: the seven builtin module maps, keyed by bare name.
     * Separate from `modules` (root 5) because the two are keyed
     * differently -- builtins by IDENT, files by (realpath, origin
     * kind) -- and folding them into one map would make `import str`
     * and `import "str"` reach the same object, which spec §11 says
     * they must not.
     */
    FlMap *builtins;
    /*
     * Root 10: the PRELUDE -- names visible without an import, which
     * in 1.0 means §9's `error`.
     *
     * A separate map rather than an entry seeded into every globals
     * map, because `collect_exports` walks a module's globals and
     * would then re-export `error` from every module that was merely
     * loaded.  GET_GLOBAL falls back to it on a miss, so a module and
     * the top-level program see the same one binding, and a script
     * that declares its own `error` shadows it in its own globals
     * without disturbing anyone else's.
     */
    FlMap *prelude;
    /* The native currently executing, so fl_arg_* can name it without
     * every helper taking it as a parameter. */
    u32 cur_native;
    FlValue err;                 /* the in-flight raised value            */
    /*
     * The caret block for the last error that escaped every frame,
     * arena-allocated, or NULL.
     *
     * It lives on the VM rather than on the error map because it is a
     * PRESENTATION artifact, not part of the value: spec §9 fixes the
     * map's shape at kind/msg/trace, and a `catch` handler has no use
     * for a rendered source line.  It is captured in the unwinder
     * because that is the last moment the frames still exist.
     */
    const char *err_caret;
    /*
     * The origin a capability check falls back to when no Fletch frame
     * is on the stack -- a native invoked directly by the host.  §13:
     * the host decides what it is granting before it calls in.
     */
    FlOrigin root_origin;
    /*
     * Sprint 34: the editor this VM drives, or NULL for a headless run.
     *
     * A typed field rather than a cast of host->ud, because a void*
     * that is sometimes an Ed is exactly the cast that goes wrong once
     * -- and because every editor binding has to ask "is there a host"
     * before it does anything, so the question deserves an answer that
     * cannot be got wrong.  Set by fl_ed_attach (flapi.c) alongside
     * vm->host; the two are never set apart.
     */
    Ed *ed;
};

bool fl_vm_init(FlVm *vm, Arena *a, Interner *in, DiagCtx *dc);

/*
 * Root 6: a host-owned FlValue the collector must see.  `slot` must
 * have a STABLE ADDRESS for as long as it is registered -- a field of
 * a heap struct that outlives the registration, never an element of a
 * vector that grows.  Registering the same address twice is a bug and
 * a YEW_BUG; removing one that was never added is a no-op, so a
 * teardown path that runs twice is safe.
 */
void fl_gc_host_root_add(FlVm *vm, FlValue *slot);
void fl_gc_host_root_remove(FlVm *vm, FlValue *slot);

/* Root 11: for collections that move.  See FlGcProvider. */
void fl_gc_root_provider(FlVm *vm, FlGcMarkFn mark, void *ctx);
/* What a provider calls.  Safe on any value, including scalars. */
void fl_gc_mark_value(FlVm *vm, FlValue v);

/*
 * Builds the {kind, msg} error map into vm->err and RETURNS FALSE, so a
 * native's failure path reads `return fl_raise(vm, "type", ...)`.
 * The kind must be one of spec §9's twelve (§16-A1 added "limit").
 */
bool fl_raise(FlVm *vm, const char *kind, const char *fmt, ...);

/*
 * Call a Fletch value from C.  Used by the stdlib's higher-order
 * functions; false means it raised and vm->err holds the error.
 */
/*
 * The message an unbound name gets.  Names the SPRINT that owes the
 * surface when there is one -- `bind lands in Sprint 36` -- and reads
 * `undefined name 'x'` otherwise.  Exported so the tests can assert the
 * deferral without going through a raise.
 *
 * The returned string is a static buffer, valid until the next call.
 */
const char *fl_deferred_msg(const char *name);

/*
 * Bound how many instructions a run may execute; 0 removes the bound.
 *
 * THE ONE TIMEOUT MECHANISM.  Sprint 32's fuzzer uses it so a
 * pathological input cannot hang the campaign, and Sprint 54's plugin
 * sandbox uses it to bound a runaway plugin -- written down here so
 * nobody adds a second one.  Exceeding it raises kind "limit", which is
 * catchable: a plugin that budgeted badly should be able to say so.
 */
void fl_vm_set_step_limit(FlVm *vm, u64 steps);

/*
 * Sprint 32 §8: THERE IS NO BYTECODE VERIFIER, AND THAT IS A DECISION.
 *
 * Random bytecode never enters the VM because no code path can put it
 * there.  Fletch has no `.flc` format, no compile cache and no
 * serialization of FlChunk; the only producer is fl_compile, in this
 * process, from an AST that already parsed.  A load-time verifier would
 * guard a door that does not exist, cost several hundred lines, and --
 * worse -- create a SECOND definition of well-formed bytecode that must
 * be kept in step with the compiler forever.  Two definitions drift,
 * and the drift is found by the thing they were meant to prevent.
 *
 * Defensive dispatch was the other candidate and costs the hot loop:
 * 02-fletch.md req 7 wants ~1 us motion dispatch, and Sprint 30 already
 * bought memory safety the right way -- max_stack is computed at
 * compile time and checked ONCE PER CALL, so pushes need no check.
 * Source-level fuzzing is the primary line of defence, and it is where
 * the attack surface actually is: every byte an adversary controls
 * passes through the parser first.
 *
 * THE CONDITION ON THIS DECISION: if yew ever grows a bytecode
 * cache, a `.flc` format, or any other way to load a chunk it did not
 * just compile, A REAL VERIFIER LANDS IN THE SAME COMMIT.  Nothing may
 * load foreign bytecode without one.  See s32-repl-and-errors.md §8.
 *
 * What ships instead is a compiler assertion.  It validates OUR
 * output -- jump targets on instruction starts and in range, constant
 * and slot indices in range, and a terminator at the end -- and a
 * failure is a YEW_BUG, because it means the compiler is broken, which
 * is not a user error.  Always compiled so the tests can drive it;
 * called automatically only under FL_VM_CHECKS.
 */
enum { FL_MAX_CHUNK_CHECK = 1U << 20 };

bool fl_chunk_check(const FlFn *fn, const char **why);

bool fl_call(FlVm *vm, FlValue callee, const FlValue *args, u32 nargs,
             FlValue *out);
bool fl_vm_run(FlVm *vm, FlFn *entry, FlValue *out);   /* false = raised */
void fl_vm_free(FlVm *vm);

#endif /* YEW_FL_VM_H */
