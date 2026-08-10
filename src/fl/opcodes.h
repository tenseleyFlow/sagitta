#ifndef YEW_FL_OPCODES_H
#define YEW_FL_OPCODES_H

/*
 * Sprint 30 deliverable 5: everything derived from opcodes.def.
 *
 * The enum, the operand shapes, the stack effects and the printable
 * names all come from the one X-macro, so the switch dispatcher, the
 * computed-goto table and the disassembler cannot disagree about what
 * an opcode is.  DoD 5 asks for byte-identical traces from the two
 * dispatch modes; this is the mechanism that makes that structural
 * rather than a thing to remember.
 */

#include "fl/value.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

/* Stack effect sentinel: the delta depends on an operand or the frame,
 * so the compiler computes it at the emit site instead. */
enum { FL_EFF_VAR = -128 };

typedef enum FlOp {
#define FL_OP(name, ops, eff, doc) FL_OP_##name,
#include "fl/opcodes.def"
#undef FL_OP
    FL_OP__COUNT
} FlOp;

_Static_assert(FL_OP__COUNT == 60, "s30 DoD 2: the instruction set is 60 opcodes");
/* 0xF0..0xFF stays free so a future superinstruction range can be added
 * without renumbering anything that a saved chunk might contain. */
_Static_assert(FL_OP__COUNT <= 0xF0, "s30 DoD 2: room reserved above the set");

/* Printable name, operand string ("", "b", "s", "w", "bw"), and net
 * stack effect (or FL_EFF_VAR).  Indexed by FlOp. */
const char *fl_op_name(FlOp op);
const char *fl_op_operands(FlOp op);
int fl_op_effect(FlOp op);

/* Total encoded length of the instruction at `code[at]`, opcode byte
 * included.  CLOSURE is variable -- it carries n capture pairs after
 * its operand -- so this needs the chunk, not just the byte. */
u32 fl_op_length(const FlChunk *ch, u32 at);

/*
 * Deterministic disassembly, one instruction per line:
 *
 *   0042  12:5  GET_LOCAL   3 ; x
 *
 * No pointers and no addresses, because this is the compiler's golden
 * surface exactly as fl_ast_dump was Sprint 29's.
 */
void fl_disasm_chunk(Bytebuf *out, const FlChunk *ch, const Interner *in);
void fl_disasm_op(Bytebuf *out, const FlChunk *ch, u32 at,
                  const Interner *in);

#endif /* YEW_FL_OPCODES_H */
