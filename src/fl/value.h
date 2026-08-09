#ifndef SAG_FL_VALUE_H
#define SAG_FL_VALUE_H

/*
 * Sprint 30 deliverables 1-4: the Fletch value representation.
 *
 * NO NaN BOXING.  The sprint settles this with reasons and forbids
 * relitigating it; the short version is that Fletch integers are i64
 * (spec §4) and NaN payloads carry at most 51 bits, so the most common
 * value type would have to heap-box to save eight bytes -- and pointer
 * payloads assume 47-48 significant address bits, which arm64 with
 * 52-bit VA and top-byte-ignore does not guarantee.
 */

#include <stdbool.h>
#include <stddef.h>

#include "fl/diag.h"
#include "util/base.h"
#include "util/buf.h"

typedef enum FlType {
    FL_NIL = 0, FL_BOOL, FL_INT, FL_FLOAT,           /* immediates        */
    FL_STR, FL_LIST, FL_MAP, FL_FN, FL_CLOSURE, FL_NATIVE, /* GC objects  */
    FL_MOTION_PROG,                                  /* §11's flat block  */
    /*
     * Upvalues are their OWN tag, not a reused closure one.  The
     * collector switches on the tag to trace children and to free
     * owned arrays, so a differently-shaped object wearing FL_CLOSURE
     * has its `closed` value read as a pointer and handed to free().
     * That is a crash on the first collection, and it is what happened.
     */
    FL_UPVAL,
    /*
     * RESERVED handle tags, spec §4.
     *
     * They exist so the type enum is stable and `type_of` can name them,
     * and they are CONSTRUCTIBLE ONLY FROM SPRINT 34.  Nothing in this
     * sprint may build one: nothing allocates them, and DoD 10 greps to
     * confirm the names appear in the enum, the name table and the
     * deferral comment and nowhere else.
     */
    FL_BUF, FL_CURSOR, FL_SPAN, FL_WIN, FL_REGEX,
    FL_TYPE_COUNT
} FlType;

typedef struct FlObj FlObj;

typedef struct FlValue {
    u8 t;        /* FlType                                                */
    u8 flags;    /* FL_VF_* -- spare; zero today                          */
    u16 rsv0;
    u32 rsv1;
    union { bool b; i64 i; double f; FlObj *o; } as;
} FlValue;

/*
 * The two rsv fields are deliberate slack, not padding to reclaim: they
 * keep the struct at 16 bytes either way and give Sprint 34 somewhere to
 * put a handle generation counter ({u32 slot; u32 gen;}) without a
 * layout change.  Do not repurpose them.
 */
_Static_assert(sizeof(FlValue) == 16, "FlValue size budget (s30 DoD 2)");
_Static_assert(FL_TYPE_COUNT <= 32, "type tag fits u8 with room to spare");

#define FL_NIL_V        ((FlValue){ .t = FL_NIL, .flags = 0U, .rsv0 = 0U, \
                                    .rsv1 = 0U, .as.i = 0 })
#define FL_BOOL_V(x)    ((FlValue){ .t = FL_BOOL, .flags = 0U, .rsv0 = 0U, \
                                    .rsv1 = 0U, .as.b = (x) })
#define FL_INT_V(x)     ((FlValue){ .t = FL_INT, .flags = 0U, .rsv0 = 0U, \
                                    .rsv1 = 0U, .as.i = (x) })
#define FL_FLOAT_V(x)   ((FlValue){ .t = FL_FLOAT, .flags = 0U, .rsv0 = 0U, \
                                    .rsv1 = 0U, .as.f = (x) })
#define FL_OBJ_V(tg, p) ((FlValue){ .t = (u8)(tg), .flags = 0U, .rsv0 = 0U, \
                                    .rsv1 = 0U, .as.o = (FlObj *)(p) })

/* Object header flags. */
enum { FL_OF_INTERNED = 1U << 0, FL_OF_FROZEN = 1U << 1 };

struct FlObj {
    u8 t;             /* FlType of the owner                              */
    u8 mark;          /* GC: 0 white, 1 gray/black                        */
    u16 oflags;       /* FL_OF_*                                          */
    u32 aux;          /* type-specific: str hash, fn arity                */
    FlObj *gc_next;   /* the all-objects list                             */
};

_Static_assert(sizeof(FlObj) == 16, "FlObj header budget (s30 DoD 2)");

/*
 * Immutable, and one allocation: the C99 flexible array member puts the
 * header and the bytes in the same block.  `aux` holds the FNV-1a hash,
 * computed eagerly at construction rather than lazily -- map keys need
 * it, and a lazy branch costs more than walking bytes already in cache.
 *
 * Bytes are stored VERBATIM, U+DC80..DCFF escapes included.  Sprint 2's
 * cleaning policy is for buffer text; program and runtime strings are
 * bytes, and nothing in value.c validates, normalizes or cleans them.
 */
typedef struct FlStr {
    FlObj h;
    u32 len;
    char b[];
} FlStr;

typedef struct FlList {
    FlObj h;
    FlValue *v;
    u32 n;
    u32 cap;
    /* STRUCTURAL modifications only -- push/pop/insert/remove/clear, not
     * element assignment.  Iteration checks it to refuse a container
     * reshaped underneath it. */
    u32 mods;
} FlList;

typedef struct FlMapEnt {
    FlValue k;
    FlValue v;
    u32 hash;
    bool dead;
} FlMapEnt;

/*
 * Insertion-ordered, and deterministically so.
 *
 * The dense `ent` array IS the iteration order; `idx` is only a lookup
 * accelerator.  Invariant 5 -- same state, same bytes -- is what forces
 * this: an order that depended on hashing or on allocation addresses
 * would make a saved workspace render differently on another machine.
 * The rules that keep it true are in value.c, next to the code that
 * would otherwise break them.
 */
typedef struct FlMap {
    FlObj h;
    FlMapEnt *ent;
    u32 n;
    u32 cap;
    u32 *idx;      /* open-addressed; holds entry index + 1, 0 = empty    */
    u32 icap;
    u32 ndead;
    u32 mods;
} FlMap;

typedef struct FlLineRun {
    u32 pc;
    u32 line;
    u32 col;
} FlLineRun;

typedef struct FlChunk {
    u8 *code;
    u32 ncode;
    FlValue *consts;
    u32 nconsts;
    /*
     * Runs carry line AND col, keyed by INSTRUCTION START pc.  Sprint
     * 32's traces print `file:line:col` and cannot reconstruct a column
     * afterwards; keying by instruction start is what makes a lookup
     * name the failing instruction rather than the one after it.
     */
    FlLineRun *lines;
    u32 nlines;
    u32 file_id;
} FlChunk;

/*
 * Capability origin, spec §13.  Every FlFn carries the origin of the
 * module that DEFINED it, and that is the only thing a capability check
 * ever reads -- see fl_cap_origin in std.c.
 *
 * `kind` and `path_id` are separate on purpose.  The kind decides what
 * a grant means (a plugin's caps are prompted for, a config's are
 * implicit); the path is what an error message must name to be
 * actionable.  Collapsing them into one module index, as Sprint 30's
 * placeholder did, makes the cache key in §11 -- (realpath, origin
 * kind) -- inexpressible, and that key is what stops a plugin from
 * borrowing a config helper's authority.
 */
typedef enum {
    FL_ORIGIN_BUILTIN = 0,   /* the seven modules; transparent to §13    */
    FL_ORIGIN_CONFIG,
    FL_ORIGIN_WORKSPACE,
    FL_ORIGIN_PLUGIN,
    FL_ORIGIN_CLI,
    FL_ORIGIN_REPL
} FlOriginKind;

/* Capability bits (spec §13).  `shell` and `net` have no stdlib surface
 * in 1.0 -- they exist so Sprint 54 can prompt for them. */
enum {
    FL_CAP_FS_READ  = 1U << 0,
    FL_CAP_FS_WRITE = 1U << 1,
    FL_CAP_SHELL    = 1U << 2,
    FL_CAP_NET      = 1U << 3
};

typedef struct FlOrigin {
    u8 kind;         /* FlOriginKind                                     */
    u32 path_id;     /* interned REALPATH; 0 for builtins                */
    u32 caps;        /* FL_CAP_*                                         */
} FlOrigin;

/*
 * What a stack trace calls this function (Sprint 32 §6).
 *
 * A closed table, and s33 asserts every row.  It cannot be derived from
 * what is already here: a module's top-level chunk and a script's are
 * both anonymous with a path, and only the thing that COMPILED them
 * knows which is which.
 */
typedef enum {
    FL_FN_NORMAL = 0,   /* `f`, or `<fn>` when name_id is 0            */
    FL_FN_MACRO,        /* `macro m`                                   */
    FL_FN_MODULE,       /* `<module init>`                             */
    FL_FN_SCRIPT,       /* `<script>`                                  */
    FL_FN_REPL          /* `<repl>`                                    */
} FlFnKind;

typedef struct FlFn {
    FlObj h;
    FlChunk ch;
    u32 name_id;     /* 0 = anonymous; traces print "<fn>"                */
    u8 arity;
    u8 nup;
    u8 fnkind;       /* FlFnKind -- what a trace calls this frame         */
    u16 max_stack;   /* computed by the compiler                          */
    FlOrigin origin;
} FlFn;

typedef struct FlUpval {
    FlObj h;
    FlValue *slot;         /* points into the stack while open            */
    FlValue closed;        /* the value once the frame is gone            */
    struct FlUpval *next;  /* open-upvalue list, sorted by slot           */
} FlUpval;

typedef struct FlClosure {
    FlObj h;
    FlFn *fn;
    FlUpval **up;
    u32 nup;
    /*
     * The GLOBALS MAP THIS FUNCTION SEES.
     *
     * Spec §7 makes a top-level binding a global and §11 makes a
     * module's non-underscore ones its exports, so "global" has to mean
     * "global to the module" -- with one shared map, module B defining
     * `helper` overwrites module A's the moment A imports B.
     *
     * It lives on the CLOSURE rather than on FlFn because the map is
     * created when a module RUNS, long after its functions were
     * compiled; a nested closure inherits it from the closure that
     * made it, which is exactly the lexical answer.  Without this, a
     * function defined in a module and called from its importer looked
     * its own imports up in the CALLER's globals and found nothing.
     */
    FlMap *globals;
} FlClosure;

/* One motion word, preassembled.  §11: a whole block is ONE dispatch,
 * because req 7 targets ~1 us per motion op and a 20-word recorded macro
 * must not pay 20 dispatches and 20 constant loads to get there. */
typedef struct FlMotionOp {
    u8 kind;
    u8 ch;
    u8 flags;
    u32 count;
    u32 arg;
} FlMotionOp;

typedef struct FlMotionProg {
    FlObj h;
    FlMotionOp *op;
    u32 n;
} FlMotionProg;

typedef struct FlVm FlVm;
typedef struct FlErr FlErr;

/*
 * A native returns false with vm->err set to raise -- there is no
 * separate error out-parameter, because a native that wants to raise
 * calls fl_raise like everything else and two ways to report one thing
 * is how they drift.
 *
 * argv points INTO the VM stack and stays live for the whole call
 * (gc.h rule 1).  Never copy an FlObj* out of argv into a C local
 * across an allocation.
 */
typedef bool (*FlNativeFn)(FlVm *vm, FlValue *argv, u32 argc, FlValue *out);

typedef struct FlNative {
    FlObj h;
    FlNativeFn fn;
    u32 name_id;
    u8 min_ar;
    u8 max_ar;
    u32 caps;
} FlNative;

/* ---------------------------------------------------------------- */
/* Predicates and accessors                                         */
/* ---------------------------------------------------------------- */

static inline bool fl_is_obj(FlValue v)
{
    return v.t >= (u8)FL_STR && v.t <= (u8)FL_UPVAL;
}

static inline FlObj *fl_as_obj(FlValue v) { return v.as.o; }

/* The spelling `type_of` reports, and what a diagnostic names.  Indexed
 * by FlType, so the reserved handle tags appear here -- naming a type
 * the program cannot yet construct is not the same as constructing
 * one. */
const char *fl_type_name(FlType t);

/* Spec §5.1: nil and false are falsey; EVERYTHING else is truthy,
 * including 0, 0.0 and "".  Pinned because "0 is falsey" is the single
 * most common assumption carried in from other languages. */
bool fl_truthy(FlValue v);

/* Spec §5.2 equality.  Int and float compare across the numeric tower;
 * everything else requires the same tag. */
bool fl_equal(FlValue a, FlValue b);

/* FNV-1a over the bytes.  Only string/int/bool keys reach it -- spec §4
 * limits map keys to those, and a float key raises kind "key" at the
 * call site rather than hashing to something surprising here. */
u32 fl_hash_bytes(const char *b, u32 n);
bool fl_hashable(FlValue v);
u32 fl_hash_value(FlValue v);

/* Strings <= this many bytes are interned; longer ones are plain heap
 * objects with a cached hash.  Interning a 4 MiB io.read result would
 * buy a permanent table entry for a string nobody looks up twice. */
enum { FL_INTERN_MAX = 64 };

/* Compares (len, hash, bytes).  The pointer check inside is a fast path
 * only -- see value.c for why a pointer-only compare is wrong once a
 * size threshold exists. */
bool fl_str_eq(const FlStr *a, const FlStr *b);

/* ---------------------------------------------------------------- */
/* Map operations.  Order is insertion order; see value.c.          */
/* ---------------------------------------------------------------- */

bool fl_map_get(const FlMap *m, FlValue k, FlValue *out);
bool fl_map_del(FlMap *m, FlValue k);
/* Empties in one pass.  Iterating with fl_map_del does NOT work --
 * deletion compacts, which moves the entries out from under the
 * cursor.  See the comment in value.c. */
void fl_map_clear(FlMap *m);
void fl_map_compact(FlMap *m);
u32 fl_map_count(const FlMap *m);
/* Walks live entries in insertion order.  `*cursor` starts at 0. */
bool fl_map_iter(const FlMap *m, u32 *cursor, FlValue *k, FlValue *v);

/* Shared with gc.c, which owns insertion because insertion allocates.
 * Probe returns the index slot; `*found` says whether it holds the key
 * or is the place to put it. */
u32 fl_map_probe(const FlMap *m, FlValue k, u32 hash, bool *found);
void fl_map_reindex(FlMap *m);

#endif /* SAG_FL_VALUE_H */
