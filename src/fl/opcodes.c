/* Sprint 30 deliverable 5: the tables and the disassembler, both
 * generated from opcodes.def so neither can drift from the dispatcher. */
#include "fl/opcodes.h"

#include <string.h>

#include "util/log.h"

static const char *const op_names[FL_OP__COUNT] = {
#define FL_OP(name, ops, eff, doc) #name,
#include "fl/opcodes.def"
#undef FL_OP
};

static const char *const op_operands[FL_OP__COUNT] = {
#define FL_OP(name, ops, eff, doc) ops,
#include "fl/opcodes.def"
#undef FL_OP
};

static const int op_effects[FL_OP__COUNT] = {
#define FL_OP(name, ops, eff, doc) (eff),
#include "fl/opcodes.def"
#undef FL_OP
};

const char *fl_op_name(FlOp op)
{
    if ((u32)op >= (u32)FL_OP__COUNT)
        return "BAD_OP";
    return op_names[op];
}

const char *fl_op_operands(FlOp op)
{
    if ((u32)op >= (u32)FL_OP__COUNT)
        return "";
    return op_operands[op];
}

int fl_op_effect(FlOp op)
{
    if ((u32)op >= (u32)FL_OP__COUNT)
        return 0;
    return op_effects[op];
}

/* Bytes consumed by the operand string alone. */
static u32 operand_width(const char *ops)
{
    u32 n = 0U;
    size_t i;

    for (i = 0U; ops[i] != '\0'; i++) {
        switch (ops[i]) {
        case 'b': case 's': n += 1U; break;
        case 'w':           n += 2U; break;
        default:
            YEW_BUG("fletch: unknown operand shape '%c'", ops[i]);
        }
    }
    return n;
}

u32 fl_op_length(const FlChunk *ch, u32 at)
{
    FlOp op;
    u32 len;

    if (ch == NULL || at >= ch->ncode)
        return 1U;
    op = (FlOp)ch->code[at];
    len = 1U + operand_width(fl_op_operands(op));
    if (op == FL_OP_CLOSURE) {
        /*
         * CLOSURE is the one variable-length instruction: its u16
         * constant is followed by one (is_local, idx) pair per upvalue.
         * The count lives on the FlFn in the constant pool rather than
         * in the stream, so decoding needs the chunk -- which is why
         * this takes one instead of a bare byte pointer.
         */
        u16 k = (u16)((u32)ch->code[at + 1U] |
                      ((u32)ch->code[at + 2U] << 8));

        if (k < ch->nconsts && ch->consts[k].t == (u8)FL_FN) {
            const FlFn *fn = (const FlFn *)ch->consts[k].as.o;

            len += (u32)fn->nup * 2U;
        }
    }
    return len;
}

/* ---------------------------------------------------------------- */
/* Disassembly                                                      */
/* ---------------------------------------------------------------- */

/* line:col for `pc`, from the run table.  Runs are sorted and keyed by
 * instruction start, so the last run at or before `pc` is the one. */
static void line_at(const FlChunk *ch, u32 pc, u32 *line, u32 *col)
{
    u32 lo = 0U;
    u32 hi = ch->nlines;

    *line = 0U;
    *col = 0U;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;

        if (ch->lines[mid].pc <= pc)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo != 0U) {
        *line = ch->lines[lo - 1U].line;
        *col = ch->lines[lo - 1U].col;
    }
}

/* The trailing `; comment` for operands worth naming: a constant's
 * value, a global's interned spelling.  Kept short and deterministic --
 * this is golden output. */
static void annotate(Bytebuf *out, const FlChunk *ch, FlOp op, u32 k,
                     const Interner *in)
{
    const FlValue *c;

    if (k >= ch->nconsts)
        return;
    c = &ch->consts[k];
    switch (op) {
    case FL_OP_GET_GLOBAL:
    case FL_OP_SET_GLOBAL:
    case FL_OP_DEF_GLOBAL:
    case FL_OP_FIELD_GET:
    case FL_OP_FIELD_SET:
    case FL_OP_IMPORT:
        if (c->t == (u8)FL_INT && in != NULL) {
            const char *s = yew_intern_str(in, (u32)c->as.i);

            if (s != NULL)
                bytebuf_printf(out, " ; %s", s);
        }
        return;
    case FL_OP_CONST:
        switch ((FlType)c->t) {
        case FL_NIL:   bytebuf_printf(out, " ; nil"); return;
        case FL_BOOL:  bytebuf_printf(out, " ; %s",
                                      c->as.b ? "true" : "false"); return;
        case FL_INT:   bytebuf_printf(out, " ; %lld",
                                      (long long)c->as.i); return;
        case FL_FLOAT: bytebuf_printf(out, " ; %.17g", c->as.f); return;
        case FL_STR: {
            const FlStr *s = (const FlStr *)c->as.o;
            u32 i;

            bytebuf_printf(out, " ; \"");
            for (i = 0U; i < s->len && i < 24U; i++) {
                u8 b = (u8)s->b[i];

                if (b == (u8)'"' || b == (u8)'\\')
                    bytebuf_printf(out, "\\%c", (char)b);
                else if (b < 0x20U)
                    bytebuf_printf(out, "\\x%02X", (unsigned)b);
                else
                    bytebuf_push_u8(out, b);
            }
            bytebuf_printf(out, "%s\"", s->len > 24U ? "..." : "");
            return;
        }
        default:
            bytebuf_printf(out, " ; %s", fl_type_name((FlType)c->t));
            return;
        }
    default:
        return;
    }
}

void fl_disasm_op(Bytebuf *out, const FlChunk *ch, u32 at,
                  const Interner *in)
{
    FlOp op;
    const char *ops;
    u32 line = 0U;
    u32 col = 0U;
    u32 p = at + 1U;
    size_t i;
    u32 first = 0U;
    bool have_first = false;

    if (ch == NULL || at >= ch->ncode)
        return;
    op = (FlOp)ch->code[at];
    line_at(ch, at, &line, &col);
    bytebuf_printf(out, "%04u  %u:%u  %-13s", (unsigned)at,
                   (unsigned)line, (unsigned)col, fl_op_name(op));
    ops = fl_op_operands(op);
    for (i = 0U; ops[i] != '\0'; i++) {
        u32 v;

        switch (ops[i]) {
        case 'b':
            v = ch->code[p];
            p += 1U;
            bytebuf_printf(out, " %u", (unsigned)v);
            break;
        case 's':
            bytebuf_printf(out, " %d", (int)(i8)ch->code[p]);
            v = ch->code[p];
            p += 1U;
            break;
        default:
            v = (u32)ch->code[p] | ((u32)ch->code[p + 1U] << 8);
            p += 2U;
            bytebuf_printf(out, " %u", (unsigned)v);
            break;
        }
        if (!have_first) {
            first = v;
            have_first = true;
        }
    }
    if (have_first)
        annotate(out, ch, op, first, in);
    if (op == FL_OP_CLOSURE && first < ch->nconsts &&
        ch->consts[first].t == (u8)FL_FN) {
        const FlFn *fn = (const FlFn *)ch->consts[first].as.o;
        u32 u;

        for (u = 0U; u < (u32)fn->nup; u++) {
            bytebuf_printf(out, " (%s %u)",
                           ch->code[p] != 0U ? "local" : "upval",
                           (unsigned)ch->code[p + 1U]);
            p += 2U;
        }
    }
    bytebuf_push_u8(out, (u8)'\n');
}

void fl_disasm_chunk(Bytebuf *out, const FlChunk *ch, const Interner *in)
{
    u32 at = 0U;

    if (out == NULL || ch == NULL)
        return;
    while (at < ch->ncode) {
        u32 len = fl_op_length(ch, at);

        fl_disasm_op(out, ch, at, in);
        at += len == 0U ? 1U : len;
    }
}
