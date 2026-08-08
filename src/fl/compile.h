#ifndef SAG_FL_COMPILE_H
#define SAG_FL_COMPILE_H

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

FlFn *fl_compile(FlVm *vm, DiagCtx *dc, const FlProgram *p,
                 u32 file_id, FlOrigin origin);

#endif /* SAG_FL_COMPILE_H */
