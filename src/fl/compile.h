#ifndef YEW_FL_COMPILE_H
#define YEW_FL_COMPILE_H

/*
 * Sprint 30 deliverable 6: the single-pass AST-to-bytecode compiler.
 *
 * Consumes an AST that already parsed clean -- Sprint 29 guarantees
 * that -- and emits an FlFn whose chunk is immutable once returned.
 *
 * Compile errors go through DiagCtx with Sprint 29's caret rendering
 * and are NEVER catchable: spec §9 draws the line at compile errors
 * being diagnostics and runtime errors being values.  fl_compile
 * returns NULL after emitting, and the caller reports and exits.
 */

#include "fl/ast.h"
#include "fl/diag.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "util/base.h"

enum {
    FL_MAX_LOCALS = 256,
    FL_MAX_UPVALS = 256,
    FL_MAX_CONSTS = 65535,
    FL_MAX_JUMP = 65535,
    FL_MAX_ARGS = 255
};

/* depth -1 means declared but NOT yet initialized, which is what makes
 * `let x = x` an error naming the shadow rather than a silent capture
 * of an outer x. */
typedef struct FlLocal {
    u32 name;
    i32 depth;
    bool captured;
} FlLocal;

typedef struct FlUpvalDesc {
    u8 index;
    bool is_local;
} FlUpvalDesc;

/*
 * As fl_compile, but for one REPL entry: when the program's LAST
 * top-level statement is a bare expression, its value is RETURNed
 * instead of popped, which is the only difference between `1 + 2` at a
 * prompt and `1 + 2` in a file.  Everything else -- scoping, globals,
 * capabilities -- is identical, so a line that works at the prompt
 * works unchanged in a script.
 */
FlFn *fl_compile_repl(FlVm *vm, DiagCtx *dc, const FlProgram *p,
                      u32 file_id, FlOrigin origin);

FlFn *fl_compile(FlVm *vm, DiagCtx *dc, const FlProgram *p,
                 u32 file_id, FlOrigin origin);

/* As fl_compile, with one release-safe TRACE_LINE marker before each
 * top-level statement.  Batch profiling observes those markers; ordinary
 * scripts keep their bytecode unchanged. */
FlFn *fl_compile_profiled(FlVm *vm, DiagCtx *dc, const FlProgram *p,
                          u32 file_id, FlOrigin origin);

#endif /* YEW_FL_COMPILE_H */
