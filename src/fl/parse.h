#ifndef SAG_FL_PARSE_H
#define SAG_FL_PARSE_H

/*
 * Sprint 29 deliverables 3-5: the Fletch parser, its diagnostics, and
 * the AST dumper the goldens are written against.
 *
 * Recursive descent for statements, precedence climbing for expressions
 * over spec §2's ladder.  Nothing is evaluated, resolved or checked for
 * arity here: Sprint 30 compiles, Sprint 31 resolves stdlib names.
 */

#include <stddef.h>

#include "fl/ast.h"
#include "fl/diag.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

/* Spec §2's grammar.  Errors are reported through `dc`; the returned
 * program is still walkable after one, because recovery keeps parsing. */
FlProgram fl_parse(Arena *a, DiagCtx *dc, Interner *in,
                   const char *src, size_t len, u32 file_id);

/*
 * Spec §12's grammar, and ONLY that grammar.
 *
 * The "grants nothing, runs nothing" property is structural rather than
 * enforced: this entry point has no production that can reach a call, a
 * bare identifier as a value, an import, a function literal, a motion
 * block, an operator or a statement -- so a state file from a stranger
 * cannot express one.  Returns NULL when the input is not a pure
 * literal.
 */
FlNode *fl_parse_literal(Arena *a, DiagCtx *dc, Interner *in,
                         const char *src, size_t len, u32 file_id);

/*
 * Deterministic s-expressions, one statement per line.  No pointers and
 * no addresses appear, so two dumps of the same source are byte-equal
 * across runs and compilers -- which is what makes this the substrate
 * for every golden in the sprint.
 */
void fl_ast_dump(Bytebuf *out, const FlProgram *p, const Interner *in);
void fl_ast_dump_node(Bytebuf *out, const FlNode *n, const Interner *in);

/* Spec §9.3 territory: how many diagnostics one parse may emit before
 * it stops trying.  Fuzz hygiene as much as user experience. */
enum { FL_PARSE_MAX_ERRORS = 20 };

/* Recursion bound.  The fuzzer finds unbounded descent in seconds
 * otherwise, and a stack overflow is not a diagnosable error. */
enum { FL_PARSE_MAX_DEPTH = 256 };

#endif /* SAG_FL_PARSE_H */
