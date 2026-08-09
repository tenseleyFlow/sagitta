/*
 * Sprint 32 §8: fl_chunk_check -- a COMPILER ASSERTION, not a security
 * boundary.  The decision it belongs to is recorded in vm.h.
 *
 * It validates OUR compiler's output.  Nothing here defends against
 * hostile bytecode, because no code path can deliver any: Fletch has no
 * bytecode loader, no .flc format and no compile cache, so the only
 * producer of an FlChunk is fl_compile, in this process, from an AST
 * that already parsed.
 *
 * The walk decodes by OPERAND WIDTH from the same opcodes.def the VM
 * dispatches on, so a new instruction is covered the day it lands
 * rather than the day someone remembers this file.
 */
#include "fl/vm.h"

#include <stdio.h>
#include <string.h>

#include "fl/opcodes.h"

/* CLOSURE carries a variable tail the operand string cannot describe:
 * n pairs of (is_local, index), where n is the referenced function's
 * upvalue count.  Decoding it needs the constant, so it is the one
 * instruction whose length is computed rather than looked up. */
static u32 op_len(const FlChunk *ch, u32 pc, bool *ok)
{
    u8 op = ch->code[pc];
    const char *ops = fl_op_operands((FlOp)op);
    u32 n = 1U;
    size_t i;

    *ok = true;
    if (op >= (u8)FL_OP__COUNT) {
        *ok = false;
        return 1U;
    }
    for (i = 0U; ops[i] != '\0'; i++)
        n += ops[i] == 'w' ? 2U : 1U;
    if ((FlOp)op == FL_OP_CLOSURE) {
        u16 k;

        if (pc + 3U > ch->ncode) {
            *ok = false;
            return n;
        }
        k = (u16)((u16)ch->code[pc + 1U] | ((u16)ch->code[pc + 2U] << 8));
        if (k >= ch->nconsts || ch->consts[k].t != (u8)FL_FN) {
            *ok = false;
            return n;
        }
        n += (u32)((const FlFn *)ch->consts[k].as.o)->nup * 2U;
    }
    return n;
}

static u16 word_at(const FlChunk *ch, u32 pc)
{
    return (u16)((u16)ch->code[pc] | ((u16)ch->code[pc + 1U] << 8));
}

/*
 * The stack-depth pass, over the CONTROL-FLOW GRAPH rather than a
 * linear sum.
 *
 * A linear walk is wrong the moment there is a branch, and the property
 * being checked is the one the VM's memory safety actually rests on:
 * Sprint 30 checks `max_stack` ONCE PER CALL and then lets every push
 * go unchecked, so a max_stack the compiler under-counted is a silent
 * stack overrun.  Every reachable instruction is visited with the depth
 * it is reached at, and a second path reaching it with a different
 * depth is itself an error -- that is a compiler bug, and it is exactly
 * the shape a mis-patched jump takes.
 */
static bool depth_pass(const FlFn *fn, const bool *starts, const char **why)
{
    const FlChunk *ch = &fn->ch;
    static i32 depth[FL_MAX_CHUNK_CHECK];
    static bool seen[FL_MAX_CHUNK_CHECK];
    static u32 work[FL_MAX_CHUNK_CHECK];
    u32 nwork = 0U;
    i32 high = 0;
    bool ok;

    (void)starts;
    (void)memset(seen, 0, (size_t)ch->ncode);
    /*
     * Depth is SLOTS-RELATIVE, matching the compiler's own accounting:
     * slot 0 holds the callee and the parameters follow, so an empty
     * expression stack is arity + 1 and not zero.  Counting from zero
     * makes CLOSE_UPVALS -- which assigns an absolute slot offset --
     * disagree with everything around it.
     */
    depth[0] = (i32)fn->arity + 1;
    seen[0] = true;
    work[nwork++] = 0U;
    while (nwork != 0U) {
        u32 pc = work[--nwork];
        i32 d = depth[pc];
        u8 op = ch->code[pc];
        u32 n = op_len(ch, pc, &ok);
        u32 after = pc + n;
        int eff = fl_op_effect((FlOp)op);
        i64 target = -1;

        if (!ok)
            return true;              /* pass 1 already reported it */
        /*
         * Every FL_EFF_VAR opcode, spelled out.  The effect table says
         * "it depends on an operand" and this is where the dependency
         * lives; a default that quietly treated them as zero is how the
         * checker would agree with a compiler that was wrong.
         */
        switch ((FlOp)op) {
        case FL_OP_CALL:
            /* n arguments and the callee collapse to one result. */
            d -= (i32)ch->code[pc + 1U];
            break;
        case FL_OP_NIL_N:
            d += (i32)ch->code[pc + 1U];
            break;
        case FL_OP_POPN:
            d -= (i32)ch->code[pc + 1U];
            break;
        case FL_OP_CLOSE_UPVALS:
            /* ABSOLUTE: sp becomes slots + s. */
            d = (i32)ch->code[pc + 1U];
            break;
        case FL_OP_LIST:
            d -= (i32)word_at(ch, pc + 1U);
            d += 1;
            break;
        case FL_OP_MAP:
            /* The operand counts PAIRS. */
            d -= 2 * (i32)word_at(ch, pc + 1U);
            d += 1;
            break;
        case FL_OP_RETURN:
        case FL_OP_RETURN_NIL:
        case FL_OP_HALT:
            continue;                 /* this path ends here */
        default:
            if (eff != FL_EFF_VAR) {
                d += eff;
            } else {
                *why = "an FL_EFF_VAR opcode the depth pass does not model";
                return false;
            }
            break;
        }
        if (d < 0) {
            *why = "stack depth goes negative";
            return false;
        }
        if (d > high)
            high = d;
        switch ((FlOp)op) {
        case FL_OP_JUMP:
            target = (i64)after + (i64)word_at(ch, pc + 1U);
            after = (u32)target;
            target = -1;              /* unconditional: one successor */
            break;
        case FL_OP_JUMP_IF_FALSE:
        case FL_OP_JUMP_IF_TRUE:
        case FL_OP_OR_JUMP:
        case FL_OP_AND_JUMP:
        case FL_OP_TRY_PUSH:
            target = (i64)after + (i64)word_at(ch, pc + 1U);
            break;
        case FL_OP_JUMP_BACK:
            after = (u32)((i64)after - (i64)word_at(ch, pc + 1U));
            break;
        default:
            break;
        }
        if (after < ch->ncode) {
            if (!seen[after]) {
                seen[after] = true;
                depth[after] = d;
                work[nwork++] = after;
            } else if (depth[after] != d) {
                *why = "two paths reach one instruction at different depths";
                return false;
            }
        }
        if (target >= 0 && (u32)target < ch->ncode) {
            /*
             * A handler is entered with the raised value pushed, so
             * TRY_PUSH's target starts one deeper than the fallthrough.
             */
            i32 td = (FlOp)op == FL_OP_TRY_PUSH ? d + 1 : d;

            if (!seen[target]) {
                seen[target] = true;
                depth[target] = td;
                work[nwork++] = (u32)target;
            } else if (depth[target] != td) {
                *why = "two paths reach one branch target at different depths";
                return false;
            }
        }
    }
    if (high > (i32)fn->max_stack) {
        *why = "max_stack is smaller than the depth the code reaches";
        return false;
    }
    return true;
}

/*
 * Takes the FUNCTION, not the chunk.
 *
 * §8's snippet says `fl_chunk_check(const FlChunk *)`, but three of the
 * four checks it asks for cannot be done from a chunk: an upvalue index
 * is bounded by `nup`, a local slot by the frame budget, and the
 * depth check compares against `max_stack` -- all of which live on
 * FlFn.  A checker that took only the chunk could verify jump targets
 * and nothing else.
 */
bool fl_chunk_check(const FlFn *fn, const char **why)
{
    const FlChunk *ch = &fn->ch;
    /* One byte per pc: is an instruction allowed to start here? */
    static bool starts[FL_MAX_CHUNK_CHECK];
    u32 pc = 0U;
    u32 i;
    bool ok;

    *why = NULL;
    if (ch->ncode == 0U) {
        *why = "empty chunk";
        return false;
    }
    if (ch->ncode > (u32)FL_MAX_CHUNK_CHECK) {
        /* Not a failure: the checker is a debugging aid and declines
         * rather than growing a megabyte of scratch. */
        return true;
    }
    (void)memset(starts, 0, (size_t)ch->ncode);

    /* Pass 1: instruction starts, opcode validity, index ranges. */
    while (pc < ch->ncode) {
        u8 op = ch->code[pc];
        u32 n;

        starts[pc] = true;
        n = op_len(ch, pc, &ok);
        if (!ok) {
            *why = "unknown opcode or malformed CLOSURE operand";
            return false;
        }
        if (pc + n > ch->ncode) {
            *why = "instruction runs past the end of the chunk";
            return false;
        }
        switch ((FlOp)op) {
        case FL_OP_CONST:
        case FL_OP_DEF_GLOBAL:
        case FL_OP_GET_GLOBAL:
        case FL_OP_SET_GLOBAL:
        case FL_OP_CLOSURE:
            if (word_at(ch, pc + 1U) >= ch->nconsts) {
                *why = "constant index out of range";
                return false;
            }
            break;
        case FL_OP_GET_LOCAL:
        case FL_OP_SET_LOCAL:
            /* A slot lives inside the frame's own budget; past it the
             * read lands in the caller's stack. */
            if ((u32)ch->code[pc + 1U] >= (u32)fn->max_stack + 1U) {
                *why = "local slot outside the frame";
                return false;
            }
            break;
        case FL_OP_GET_UPVAL:
        case FL_OP_SET_UPVAL:
            if ((u32)ch->code[pc + 1U] >= (u32)fn->nup) {
                *why = "upvalue index out of range";
                return false;
            }
            break;
        default:
            break;
        }
        pc += n;
    }

    /* Pass 2: jump targets land on an instruction start, in range. */
    pc = 0U;
    while (pc < ch->ncode) {
        u8 op = ch->code[pc];
        u32 n = op_len(ch, pc, &ok);
        u32 after = pc + n;
        i64 target = -1;

        switch ((FlOp)op) {
        case FL_OP_JUMP:
        case FL_OP_JUMP_IF_FALSE:
        case FL_OP_JUMP_IF_TRUE:
        case FL_OP_OR_JUMP:
        case FL_OP_AND_JUMP:
        case FL_OP_TRY_PUSH:
            target = (i64)after + (i64)word_at(ch, pc + 1U);
            break;
        case FL_OP_JUMP_BACK:
            target = (i64)after - (i64)word_at(ch, pc + 1U);
            break;
        default:
            break;
        }
        if (target >= 0) {
            if (target < 0 || (u32)target > ch->ncode) {
                *why = "jump target outside the chunk";
                return false;
            }
            if ((u32)target != ch->ncode && !starts[target]) {
                *why = "jump target is not an instruction start";
                return false;
            }
        }
        pc = after;
    }

    /*
     * Pass 3: the terminator.
     *
     * Reachability is not walked -- that needs the control-flow graph
     * and would be a second definition of what the compiler emits, the
     * exact duplication §8 rejects.  What IS checked is the property
     * the VM depends on: the last instruction ends the chunk, so
     * execution cannot run off the end into whatever follows.
     */
    pc = 0U;
    i = 0U;
    while (pc < ch->ncode) {
        i = pc;
        pc += op_len(ch, pc, &ok);
    }
    switch ((FlOp)ch->code[i]) {
    case FL_OP_HALT:
    case FL_OP_RETURN:
    case FL_OP_RETURN_NIL:
        break;
    default:
        *why = "chunk does not end in a terminator";
        return false;
    }
    return depth_pass(fn, starts, why);
}
