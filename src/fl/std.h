#ifndef SAG_FL_STD_H
#define SAG_FL_STD_H

/*
 * Sprint 31 deliverable 1: native registration, the shared argument
 * helpers, and the capability check.
 *
 * THE ARGUMENT HELPERS ARE THE ONLY PLACE A TYPE ERROR IS WORDED.
 * There are ~200 natives; if each spelled its own message, the wording
 * would drift the first week and users would learn that Fletch's errors
 * are inconsistent rather than that their argument was wrong.  Every
 * one of them goes through fl_arg_*, which produces
 *
 *     str.slice: argument 2 must be int, found string
 *
 * and nothing else may produce a type error for an argument.
 *
 * ARITY IS NOT CHECKED HERE.  The VM checks it at CALL against
 * min_ar/max_ar before the native runs, so a native never sees the
 * wrong count and cannot forget to look.
 */

#include "fl/value.h"
#include "fl/vm.h"
#include "util/base.h"
#include "util/buf.h"

/* max_ar == FL_VARIADIC means "no upper bound". */
enum { FL_VARIADIC = 255 };

typedef struct FlNativeDef {
    const char *name;        /* "len", "find_all", ...                    */
    FlNativeFn fn;
    u8 min_ar;
    u8 max_ar;               /* FL_VARIADIC = no upper bound              */
    u32 caps;                /* FL_CAP_* required; 0 = none               */
    const char *sig;         /* "(s, i, i) -> s" -- help + the s33 ledger */
} FlNativeDef;

/* A constant field in a module map: math.pi and friends.  Registered
 * alongside the functions so `math.pi` and `math.sqrt` are read by the
 * same `.` path, which is what makes a module an ordinary frozen map
 * rather than a second kind of thing. */
typedef struct FlConstDef {
    const char *name;
    FlValue v;
} FlConstDef;

typedef struct FlModuleDef {
    const char *name;
    const FlNativeDef *defs;
    u32 n;
    const FlConstDef *consts;   /* may be NULL */
    u32 nconsts;
} FlModuleDef;

/*
 * Builds the seven builtin module maps and leaves them in vm->builtins,
 * each flagged FL_OF_FROZEN so `.` reads them exactly as it reads any
 * map (spec §4) while assignment raises "type".
 */
void fl_std_register(FlVm *vm);

/*
 * "str.len\nstr.at\n..." in REGISTRATION order, one per line.  Sprint
 * 33's coverage ledger diffs against this, so the order must be
 * deterministic -- it is: the tables are static and walked in order.
 * Returns the number of names written.
 */
u32 fl_std_list_natives(const FlVm *vm, Bytebuf *out);

/*
 * The signature a native was registered with -- "(s, i, [i]) -> s" --
 * looked up by its QUALIFIED name, or NULL.
 *
 * Read from the static FlNativeDef tables rather than from FlNative,
 * which does not carry it: a signature is documentation, and paying a
 * pointer per native at runtime to hold a string only `:help` reads
 * would be the wrong trade.
 */
const char *fl_std_sig(const char *qualified);

/* ---------------------------------------------------------------- */
/* Argument helpers                                                 */
/* ---------------------------------------------------------------- */

/*
 * Each returns false with vm->err set, so a native's prologue reads
 *
 *     if (!fl_arg_str(vm, argv, 0, &s)) return false;
 *
 * `i` is the zero-based argv index; the message reports it one-based,
 * because that is how a user counts the arguments they wrote.
 */
bool fl_arg_str(FlVm *vm, FlValue *a, u32 i, const FlStr **out);
bool fl_arg_int(FlVm *vm, FlValue *a, u32 i, i64 *out);   /* int only    */
bool fl_arg_num(FlVm *vm, FlValue *a, u32 i, double *out);/* int or float*/
bool fl_arg_list(FlVm *vm, FlValue *a, u32 i, FlList **out);
bool fl_arg_map(FlVm *vm, FlValue *a, u32 i, FlMap **out);
bool fl_arg_fn(FlVm *vm, FlValue *a, u32 i, FlValue *out);/* callable    */

/*
 * The name of the native currently executing, for those messages.  Set
 * by the VM around the call; reading it rather than passing it to every
 * helper keeps the helper signatures to what they are actually about.
 */
void fl_std_set_current(FlVm *vm, u32 name_id);

/* ---------------------------------------------------------------- */
/* Cross-module primitives                                          */
/* ---------------------------------------------------------------- */

/*
 * The ONE definition of line splitting: split on `\n`, drop one
 * trailing `\r` per line, and keep the empty final line a trailing
 * terminator produces.
 *
 * `io.read_lines` is specified as `str.split_lines` of the file's
 * content, so it calls this rather than growing a second splitter that
 * would disagree about CRLF the first time one of them was touched.
 * Returns a list value; raises nothing.
 */
FlValue fl_split_lines(FlVm *vm, const char *b, u32 n);

/*
 * fmt.str's rendering, which `io.print` joins with spaces.  Exported so
 * printing a value and formatting it cannot drift; returns false having
 * raised only if the value is one display cannot reach, which today is
 * none of them.
 */
bool fl_fmt_display(FlVm *vm, Bytebuf *out, FlValue v);

/*
 * fmt.repr's rendering, single-line.
 *
 * Exported for DoD 8's round trip, which generates values in C and
 * needs the SERIALIZER rather than a Fletch program that would have to
 * escape arbitrary bytes into source first.  Returns false having
 * raised on a value §12 cannot spell.
 */
bool fl_fmt_repr(FlVm *vm, Bytebuf *out, FlValue v);

/*
 * What a prompt prints: repr's quoting with display's elision, and a
 * caller-set depth cap (Sprint 32 §5 pins 8).  A cyclic value elides to
 * `[...]` rather than raising -- at a REPL, refusing to print a value
 * the user just built is worse than printing it approximately.
 */
bool fl_fmt_repl(FlVm *vm, Bytebuf *out, FlValue v, u32 max_depth);

/*
 * Drops `re`'s compiled-pattern cache and its arena.
 *
 * The cache is a pure function of (pattern bytes, flags) and therefore
 * process-wide rather than per-VM; this exists so tearing a VM down
 * leaves nothing behind for the valgrind lane to find.  A no-op while a
 * scan is walking a program out of it.
 */
void fl_re_cache_clear(void);

/* ---------------------------------------------------------------- */
/* Capabilities (spec §13)                                          */
/* ---------------------------------------------------------------- */

/*
 * The grant is read from the DEFINING MODULE of the calling function --
 * never from the stack top, never from a VM-global "current caps".
 *
 * Builtin frames are transparent, so `list.map(f, io.read)` checks f's
 * origin rather than list's.  That transparency is the whole mechanism:
 * without it, any stdlib function would launder authority for whatever
 * called it, and §13's promise that "a plugin calling a user-config
 * helper gains nothing" would be false.
 */
FlOrigin fl_cap_origin(const FlVm *vm);
const char *fl_cap_name(u32 cap);
const char *fl_origin_name(const FlVm *vm, const FlOrigin *o);

/* True when the caller holds every bit in `need`; otherwise raises kind
 * "capability" and returns false. */
bool fl_cap_check(FlVm *vm, u32 need);

#endif /* SAG_FL_STD_H */
