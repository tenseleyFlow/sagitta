/* Sprint 30 deliverable 6: AST -> bytecode, one pass. */
#include "fl/compile.h"

#include <string.h>

#include "fl/gc.h"
#include "fl/lex.h"
#include "fl/motion_tab.h"
#include "fl/opcodes.h"
#include "util/log.h"
#include "util/vec.h"

/* The compiler's scratch buffers.  Vec and Bytebuf own the
 * growth, which keeps the allocator out of this file: DoD 8
 * confines it to gc.c, and the compiler builds into scratch and
 * copies to the arena at final size. */
VEC_DECL(FlConstVec, FlValue);
/* Top-level binding names, for the redeclaration check that locals get
 * from the FlLocal array and globals would otherwise lose. */
VEC_DECL(FlNameVec, u32);
VEC_DECL(FlLineVec, FlLineRun);

enum { FL_MAX_LOOPS = 64, FL_MAX_BREAKS = 64 };

typedef struct Loop {
    u32 start;                    /* pc of the condition                  */
    u32 breaks[FL_MAX_BREAKS];
    u32 nbreaks;
    u32 conts[FL_MAX_BREAKS];
    u32 nconts;
    i32 scope_depth;
    u32 slot_floor;               /* locals below this survive the loop   */
    u32 try_depth;
} Loop;

typedef struct Compiler Compiler;

struct Compiler {
    Compiler *enclosing;
    FlVm *vm;
    DiagCtx *dc;
    u32 file_id;

    Bytebuf code;
    FlConstVec consts;
    FlNameVec globals;
    FlLineVec lines;

    FlLocal locals[FL_MAX_LOCALS];
    u32 nlocals;
    i32 scope_depth;
    FlUpvalDesc upvals[FL_MAX_UPVALS];
    u32 nupvals;

    Loop loops[FL_MAX_LOOPS];
    u32 nloops;
    u32 ntry;

    /* Running depth and its high-water mark.  max_stack is what lets the
     * VM bounds-check once per CALL instead of once per push, which is
     * the only reason the hot loop can skip stack checks and stay
     * memory-safe. */
    i32 depth;
    i32 max_depth;

    u32 name_id;
    u8 arity;
    bool failed;
    /*
     * The defining module's origin, inherited by every nested
     * function.
     *
     * fl_cap_origin treats a FL_ORIGIN_BUILTIN frame as TRANSPARENT so
     * that `list.map(f, io.read)` checks f's grants; a nested function
     * left with a zeroed origin reads as builtin and is therefore
     * transparent too -- which means a plugin's own helper would
     * launder whatever called it.  §13 says the grant comes from the
     * DEFINING module, so the defining module's origin has to reach
     * every function the module defines.
     */
    FlOrigin origin;
};

static void comp_expr(Compiler *c, const FlNode *n);
static void comp_stmt(Compiler *c, const FlNode *n);

/* ---------------------------------------------------------------- */
/* Diagnostics                                                      */
/* ---------------------------------------------------------------- */

static void cerror(Compiler *c, FlSpan sp, const char *fmt, ...)
{
    va_list ap;

    if (c->failed)
        return;                   /* one message per compile, like s29 */
    c->failed = true;
    va_start(ap, fmt);
    fl_diag_vemit(c->dc, FL_DIAG_ERROR, sp, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- */
/* Emitting                                                         */
/* ---------------------------------------------------------------- */

static void note_line(Compiler *c, FlSpan sp)
{
    FlLineRun run;

    if (c->lines.len != 0U &&
        c->lines.data[c->lines.len - 1U].line == sp.line &&
        c->lines.data[c->lines.len - 1U].col == sp.col)
        return;
    /* Keyed by INSTRUCTION START: Sprint 32's traces must name the
     * failing instruction, not the one after it. */
    run.pc = (u32)c->code.len;
    run.line = sp.line;
    run.col = sp.col;
    FlLineVec_push(&c->lines, run);
}

static void push_depth(Compiler *c, int by)
{
    c->depth += by;
    if (c->depth > c->max_depth)
        c->max_depth = c->depth;
    if (c->depth < 0)
        c->depth = 0;
}

static void emit_op(Compiler *c, FlOp op, FlSpan sp)
{
    int eff = fl_op_effect(op);

    note_line(c, sp);
    bytebuf_push_u8(&c->code, (u8)op);
    if (eff != FL_EFF_VAR)
        push_depth(c, eff);
}

static void emit_u8(Compiler *c, u8 v) { bytebuf_push_u8(&c->code, v); }

static void emit_u16(Compiler *c, u16 v)
{
    bytebuf_push_u8(&c->code, (u8)(v & 0xFFU));
    bytebuf_push_u8(&c->code, (u8)(v >> 8));
}

/* ---------------------------------------------------------------- */
/* Constant pool                                                    */
/* ---------------------------------------------------------------- */

/*
 * Dedup keyed on THE TAG PLUS THE BIT PATTERN, never on fl_equal.
 *
 * §4 makes `int == float` compare numerically, so an fl_equal-keyed
 * pool merges 5 and 5.0 into one entry and silently changes what
 * fmt.repr prints -- a bug that surfaces three sprints later inside a
 * workspace-state file.  0.0 and -0.0 stay distinct for the same
 * reason.
 */
static bool const_same(FlValue a, FlValue b)
{
    if (a.t != b.t)
        return false;
    switch ((FlType)a.t) {
    case FL_NIL:   return true;
    case FL_BOOL:  return a.as.b == b.as.b;
    case FL_INT:   return a.as.i == b.as.i;
    case FL_FLOAT: {
        /* Bit pattern, so -0.0 and 0.0 are different constants and NaN
         * dedups against itself. */
        u64 x;
        u64 y;

        (void)memcpy(&x, &a.as.f, sizeof(x));
        (void)memcpy(&y, &b.as.f, sizeof(y));
        return x == y;
    }
    default:
        /* Strings arrive already interned, so pointer identity is
         * content identity for anything short enough to matter. */
        return a.as.o == b.as.o;
    }
}

static u32 add_const(Compiler *c, FlValue v, FlSpan sp)
{
    u32 i;

    for (i = 0U; i < (u32)c->consts.len; i++) {
        if (const_same(c->consts.data[i], v))
            return i;
    }
    if (c->consts.len >= (size_t)FL_MAX_CONSTS) {
        cerror(c, sp, "too many constants in one function");
        return 0U;
    }
    FlConstVec_push(&c->consts, v);
    return (u32)c->consts.len - 1U;
}

static u32 name_const(Compiler *c, u32 intern_id, FlSpan sp)
{
    /* Names travel as interner ids, not FlStr: the AST and the chunk
     * both carry ids, and the VM resolves through the same table. */
    return add_const(c, FL_INT_V((i64)intern_id), sp);
}

/* ---------------------------------------------------------------- */
/* Jumps                                                            */
/* ---------------------------------------------------------------- */

static u32 emit_jump(Compiler *c, FlOp op, FlSpan sp)
{
    emit_op(c, op, sp);
    emit_u16(c, 0xFFFFU);         /* placeholder */
    return (u32)c->code.len - 2U;
}

static void patch_jump(Compiler *c, u32 site, FlSpan sp)
{
    size_t dist = c->code.len - (size_t)site - 2U;

    if (dist > (size_t)FL_MAX_JUMP) {
        cerror(c, sp, "function too large");
        return;
    }
    c->code.data[site] = (u8)(dist & 0xFFU);
    c->code.data[site + 1U] = (u8)(dist >> 8);
}

static void emit_loop_back(Compiler *c, u32 target, FlSpan sp)
{
    size_t dist;

    emit_op(c, FL_OP_JUMP_BACK, sp);
    dist = c->code.len + 2U - (size_t)target;
    if (dist > (size_t)FL_MAX_JUMP) {
        cerror(c, sp, "function too large");
        emit_u16(c, 0U);
        return;
    }
    emit_u16(c, (u16)dist);
}

/* ---------------------------------------------------------------- */
/* Scopes, locals, upvalues                                         */
/* ---------------------------------------------------------------- */

static void begin_scope(Compiler *c) { c->scope_depth++; }

static void emit_close_to(Compiler *c, u32 floor, FlSpan sp)
{
    bool any_captured = false;
    u32 i;

    for (i = floor; i < c->nlocals; i++) {
        if (c->locals[i].captured)
            any_captured = true;
    }
    if (c->nlocals == floor)
        return;
    if (any_captured) {
        emit_op(c, FL_OP_CLOSE_UPVALS, sp);
        emit_u8(c, (u8)floor);
    } else {
        emit_op(c, FL_OP_POPN, sp);
        emit_u8(c, (u8)(c->nlocals - floor));
    }
    push_depth(c, -(int)(c->nlocals - floor));
}

static void end_scope(Compiler *c, FlSpan sp)
{
    u32 floor = c->nlocals;

    while (floor > 0U && c->locals[floor - 1U].depth >= c->scope_depth)
        floor--;
    emit_close_to(c, floor, sp);
    c->nlocals = floor;
    c->scope_depth--;
}

/*
 * A compiler-generated slot with no source name: the three hidden
 * cells ITER_NEW pushes, and the frame's own slot 0.  These skip the
 * redeclaration check, which keys on the name -- three anonymous slots
 * would otherwise collide with each other and report the shadowing
 * error against a name the user never wrote.
 */
static void add_hidden_local(Compiler *c, FlSpan sp)
{
    if (c->nlocals >= (u32)FL_MAX_LOCALS) {
        cerror(c, sp, "too many locals in one function (max %d)",
               FL_MAX_LOCALS);
        return;
    }
    c->locals[c->nlocals].name = 0U;
    c->locals[c->nlocals].depth = c->scope_depth;
    c->locals[c->nlocals].captured = false;
    c->nlocals++;
}

static void add_local(Compiler *c, u32 name, FlSpan sp)
{
    u32 i;

    if (c->nlocals >= (u32)FL_MAX_LOCALS) {
        cerror(c, sp, "too many locals in one function (max %d)",
               FL_MAX_LOCALS);
        return;
    }
    /* Shadowing in a NESTED scope is legal (§4); redeclaring in the
     * same one is not. */
    for (i = c->nlocals; i > 0U; i--) {
        const FlLocal *l = &c->locals[i - 1U];

        if (l->depth != -1 && l->depth < c->scope_depth)
            break;
        if (l->name == name) {
            cerror(c, sp, "'%s' is already declared in this scope",
                   sag_intern_str(c->vm->in, name));
            return;
        }
    }
    c->locals[c->nlocals].name = name;
    c->locals[c->nlocals].depth = -1;   /* uninitialized until the init
                                         * expression has compiled */
    c->locals[c->nlocals].captured = false;
    c->nlocals++;
}

static void mark_initialized(Compiler *c)
{
    if (c->nlocals != 0U)
        c->locals[c->nlocals - 1U].depth = c->scope_depth;
}

static i32 resolve_local(Compiler *c, u32 name, FlSpan sp)
{
    u32 i;

    for (i = c->nlocals; i > 0U; i--) {
        if (c->locals[i - 1U].name == name) {
            if (c->locals[i - 1U].depth == -1) {
                /* `let x = x`: the local exists but is not initialized,
                 * so this reads as its own initializer rather than
                 * silently capturing an outer x. */
                cerror(c, sp, "cannot read '%s' in its own initializer",
                       sag_intern_str(c->vm->in, name));
            }
            return (i32)(i - 1U);
        }
    }
    return -1;
}

static i32 add_upval(Compiler *c, u8 index, bool is_local, FlSpan sp)
{
    u32 i;

    /* Dedup: two closures made in the same scope must share ONE
     * FlUpval, which is what makes the §14 counter example work. */
    for (i = 0U; i < c->nupvals; i++) {
        if (c->upvals[i].index == index &&
            c->upvals[i].is_local == is_local)
            return (i32)i;
    }
    if (c->nupvals >= (u32)FL_MAX_UPVALS) {
        cerror(c, sp, "too many captured variables (max %d)",
               FL_MAX_UPVALS);
        return 0;
    }
    c->upvals[c->nupvals].index = index;
    c->upvals[c->nupvals].is_local = is_local;
    return (i32)c->nupvals++;
}

static i32 resolve_upval(Compiler *c, u32 name, FlSpan sp)
{
    i32 local;
    i32 up;

    if (c->enclosing == NULL)
        return -1;
    local = resolve_local(c->enclosing, name, sp);
    if (local >= 0) {
        /* BY REFERENCE (spec §7): marking the source local captured is
         * what tells the enclosing scope to emit CLOSE_UPVALS instead
         * of a plain pop, so the closure keeps sharing the variable
         * rather than a copy of it. */
        c->enclosing->locals[local].captured = true;
        return add_upval(c, (u8)local, true, sp);
    }
    up = resolve_upval(c->enclosing, name, sp);
    if (up >= 0)
        return add_upval(c, (u8)up, false, sp);
    return -1;
}

/* ---------------------------------------------------------------- */
/* Expressions                                                      */
/* ---------------------------------------------------------------- */

static void comp_literal(Compiler *c, const FlNode *n)
{
    switch ((FlLitKind)n->as.lit.lit) {
    case FL_L_NIL:
        emit_op(c, FL_OP_NIL, n->sp);
        return;
    case FL_L_BOOL:
        emit_op(c, n->as.lit.v.b ? FL_OP_TRUE : FL_OP_FALSE, n->sp);
        return;
    case FL_L_INT:
        if (n->as.lit.v.i >= -128 && n->as.lit.v.i <= 127) {
            /* Keeps 0/1/-1 out of the constant pool entirely. */
            emit_op(c, FL_OP_INT8, n->sp);
            emit_u8(c, (u8)(i8)n->as.lit.v.i);
            return;
        }
        emit_op(c, FL_OP_CONST, n->sp);
        emit_u16(c, (u16)add_const(c, FL_INT_V(n->as.lit.v.i), n->sp));
        return;
    case FL_L_FLOAT:
        emit_op(c, FL_OP_CONST, n->sp);
        emit_u16(c, (u16)add_const(c, FL_FLOAT_V(n->as.lit.v.f), n->sp));
        return;
    default: {
        const char *s = sag_intern_str(c->vm->in, n->as.lit.v.str_id);
        /* The interned LENGTH, never strlen: §1.5 admits `\0`, and
         * measuring with strlen truncated "a\0b" to "a" without a word
         * to anyone. */
        FlStr *o = fl_str_new(c->vm, s == NULL ? "" : s,
                              (u32)sag_intern_len(c->vm->in,
                                                  n->as.lit.v.str_id));

        emit_op(c, FL_OP_CONST, n->sp);
        emit_u16(c, (u16)add_const(c, FL_OBJ_V(FL_STR, o), n->sp));
        return;
    }
    }
}

static FlOp binop_for(u8 tok)
{
    switch ((FlTokKind)tok) {
    case FL_T_PLUS:    return FL_OP_ADD;
    case FL_T_MINUS:   return FL_OP_SUB;
    case FL_T_STAR:    return FL_OP_MUL;
    case FL_T_SLASH:   return FL_OP_DIV;
    case FL_T_PERCENT: return FL_OP_MOD;
    case FL_T_EQEQ:    return FL_OP_EQ;
    case FL_T_BANGEQ:  return FL_OP_NE;
    case FL_T_LT:      return FL_OP_LT;
    case FL_T_LE:      return FL_OP_LE;
    case FL_T_GT:      return FL_OP_GT;
    default:           return FL_OP_GE;
    }
}

static void comp_motion_block(Compiler *c, const FlNode *n);
/*
 * `out_ups` receives the COMPILED FUNCTION'S capture descriptors, so
 * the caller can emit the pairs that follow its CLOSURE.  It must not
 * be the enclosing compiler's own `upvals` array -- see the note at
 * the end of the definition.
 */
static FlFn *comp_function(Compiler *enclosing, const FlNode *n,
                           u32 name_id, bool is_expr,
                           FlUpvalDesc *out_ups);

static void comp_call(Compiler *c, const FlNode *n)
{
    u32 i;

    comp_expr(c, n->as.call.callee);
    for (i = 0U; i < n->as.call.nargs; i++)
        comp_expr(c, n->as.call.args[i]);
    if (n->as.call.nargs > (u32)FL_MAX_ARGS) {
        cerror(c, n->sp, "too many call arguments (max %d)", FL_MAX_ARGS);
        return;
    }
    emit_op(c, FL_OP_CALL, n->sp);
    emit_u8(c, (u8)n->as.call.nargs);
    /* CALL is FL_EFF_VAR: n arguments and the callee collapse to one
     * result. */
    push_depth(c, -(int)n->as.call.nargs);
}

static void comp_expr(Compiler *c, const FlNode *n)
{
    if (n == NULL || c->failed) {
        if (n == NULL)
            emit_op(c, FL_OP_NIL, (FlSpan){0U, 0U, 0U, 0U});
        return;
    }
    switch ((FlAstKind)n->kind) {
    case FL_A_LIT:
        comp_literal(c, n);
        return;
    case FL_A_IDENT: {
        i32 slot = resolve_local(c, n->as.ident.name, n->sp);

        if (slot >= 0) {
            emit_op(c, FL_OP_GET_LOCAL, n->sp);
            emit_u8(c, (u8)slot);
            return;
        }
        slot = resolve_upval(c, n->as.ident.name, n->sp);
        if (slot >= 0) {
            emit_op(c, FL_OP_GET_UPVAL, n->sp);
            emit_u8(c, (u8)slot);
            return;
        }
        emit_op(c, FL_OP_GET_GLOBAL, n->sp);
        emit_u16(c, (u16)name_const(c, n->as.ident.name, n->sp));
        return;
    }
    case FL_A_BINOP: {
        FlTokKind op = (FlTokKind)n->as.bin.op;

        if (op == FL_T_AND || op == FL_T_OR) {
            /* Short circuit: the peeking jumps leave the deciding value
             * on the stack, so `a or b` yields a when a is truthy. */
            u32 site;

            comp_expr(c, n->as.bin.l);
            site = emit_jump(c, op == FL_T_AND ? FL_OP_AND_JUMP
                                               : FL_OP_OR_JUMP, n->sp);
            emit_op(c, FL_OP_POP, n->sp);
            comp_expr(c, n->as.bin.r);
            patch_jump(c, site, n->sp);
            return;
        }
        comp_expr(c, n->as.bin.l);
        comp_expr(c, n->as.bin.r);
        emit_op(c, binop_for(n->as.bin.op), n->sp);
        return;
    }
    case FL_A_UNOP:
        comp_expr(c, n->as.un.operand);
        emit_op(c, (FlTokKind)n->as.un.op == FL_T_NOT ? FL_OP_NOT
                                                      : FL_OP_NEG, n->sp);
        return;
    case FL_A_CALL:
        comp_call(c, n);
        return;
    case FL_A_INDEX:
        comp_expr(c, n->as.index.obj);
        comp_expr(c, n->as.index.idx);
        emit_op(c, FL_OP_INDEX_GET, n->sp);
        return;
    case FL_A_FIELD:
        comp_expr(c, n->as.field.obj);
        emit_op(c, FL_OP_FIELD_GET, n->sp);
        emit_u16(c, (u16)name_const(c, n->as.field.name, n->sp));
        return;
    case FL_A_LIST: {
        u32 i;

        for (i = 0U; i < n->as.list.n; i++)
            comp_expr(c, n->as.list.items[i]);
        emit_op(c, FL_OP_LIST, n->sp);
        emit_u16(c, (u16)n->as.list.n);
        push_depth(c, -(int)n->as.list.n + 1);
        return;
    }
    case FL_A_MAP: {
        u32 i;

        for (i = 0U; i < n->as.map.n; i++) {
            comp_expr(c, n->as.map.keys[i]);
            comp_expr(c, n->as.map.vals[i]);
        }
        emit_op(c, FL_OP_MAP, n->sp);
        emit_u16(c, (u16)n->as.map.n);
        push_depth(c, -(int)(n->as.map.n * 2U) + 1);
        return;
    }
    case FL_A_FN_EXPR: {
        FlUpvalDesc ups[FL_MAX_UPVALS];
        FlFn *fn = comp_function(c, n, 0U, true, ups);
        u32 k;
        u32 i;

        if (fn == NULL)
            return;
        k = add_const(c, FL_OBJ_V(FL_FN, fn), n->sp);
        emit_op(c, FL_OP_CLOSURE, n->sp);
        emit_u16(c, (u16)k);
        for (i = 0U; i < (u32)fn->nup; i++) {
            emit_u8(c, ups[i].is_local ? 1U : 0U);
            emit_u8(c, ups[i].index);
        }
        return;
    }
    case FL_A_MOTION_BLOCK:
        comp_motion_block(c, n);
        return;
    default:
        cerror(c, n->sp, "this expression is not compilable yet");
        emit_op(c, FL_OP_NIL, n->sp);
        return;
    }
}

/* ---------------------------------------------------------------- */
/* Motion blocks (deliverable 11)                                   */
/* ---------------------------------------------------------------- */

static u32 motion_node_count(const FlNode *n)
{
    u32 total = 1U;
    u32 i;

    for (i = 0U; i < n->as.motion.ninner; i++)
        total += motion_node_count(n->as.motion.inner[i]);
    return total;
}

static u32 motion_count(const FlNode *n)
{
    u32 total = 0U;
    u32 i;

    for (i = 0U; i < n->as.list.n; i++)
        total += motion_node_count(n->as.list.items[i]);
    return total;
}

static bool motion_validate_node(Compiler *c, const FlNode *n)
{
    u32 i;

    /* Headless conformance scripts deliberately use arbitrary words to
     * exercise the null motion host.  Registry validation belongs to an
     * attached editor runtime, which is also every store/library path. */
    if (c->vm->ed != NULL &&
        (FlMotionKind)n->as.motion.mkind == FL_MK_WORD) {
        const char *word = sag_intern_str(c->vm->in, n->as.motion.payload);
        u32 len = (u32)sag_intern_len(c->vm->in, n->as.motion.payload);
        Bytebuf detail;

        bytebuf_init(&detail);
        if (!fl_motion_word_validate(word, len, &detail)) {
            bytebuf_push_u8(&detail, (u8)'\0');
            cerror(c, n->sp, "%s", (const char *)detail.data);
            bytebuf_free(&detail);
            return false;
        }
        bytebuf_free(&detail);
    }
    for (i = 0U; i < n->as.motion.ninner; i++)
        if (!motion_validate_node(c, n->as.motion.inner[i]))
            return false;
    return true;
}

static bool motion_validate(Compiler *c, const FlNode *n)
{
    u32 i;

    for (i = 0U; i < n->as.list.n; i++)
        if (!motion_validate_node(c, n->as.list.items[i]))
            return false;
    return true;
}

static void motion_flatten_node(const FlNode *n, FlMotionOp *out, u32 *at)
{
    u32 start = *at;
    FlMotionOp *op = &out[start];
    u32 i;

    op->kind = n->as.motion.mkind;
    op->ch = n->as.motion.ch;
    op->flags = n->as.motion.alt ? FL_MOTION_F_ALT : 0U;
    if (n->as.motion.count_given)
        op->flags |= FL_MOTION_F_COUNT_GIVEN;
    op->count = n->as.motion.count;
    op->arg = n->as.motion.payload;
    (*at)++;
    for (i = 0U; i < n->as.motion.ninner; i++)
        motion_flatten_node(n->as.motion.inner[i], out, at);
    if ((FlMotionKind)n->as.motion.mkind == FL_MK_HIGHLIGHT)
        out[start].arg = *at - start - 1U;
}

static void motion_flatten(const FlNode *n, FlMotionOp *out, u32 *at)
{
    u32 i;

    for (i = 0U; i < n->as.list.n; i++)
        motion_flatten_node(n->as.list.items[i], out, at);
}

static void comp_motion_block(Compiler *c, const FlNode *n)
{
    /*
     * The whole block becomes ONE preassembled object and ONE dispatch.
     * 02-fletch.md req 7 targets ~1 us per motion op, and a 20-word
     * recorded macro must not pay 20 dispatches and 20 constant loads
     * to get there.
     */
    u32 total;
    FlMotionProg *p;
    u32 at = 0U;

    if (!motion_validate(c, n)) {
        emit_op(c, FL_OP_NIL, n->sp);
        return;
    }
    p = fl_gc_alloc(c->vm, sizeof(*p), FL_MOTION_PROG);
    total = motion_count(n);

    if (total != 0U) {
        p->op = arena_alloc(c->vm->arena, (size_t)total * sizeof(*p->op),
                            _Alignof(FlMotionOp));
        motion_flatten(n, p->op, &at);
    }
    p->n = at;
    emit_op(c, FL_OP_MOTION, n->sp);
    emit_u16(c, (u16)add_const(c, FL_OBJ_V(FL_MOTION_PROG, p), n->sp));
}

/* ---------------------------------------------------------------- */
/* Statements                                                       */
/* ---------------------------------------------------------------- */

/*
 * NIL_N exists so `let a\nlet b\nlet c` costs one instruction instead
 * of three.  The run is detected HERE rather than in comp_stmt because
 * a single-pass compiler cannot see the next statement from inside the
 * current one, and coalescing is the only reason the opcode is in the
 * set.  Returns how many statements it consumed (0 = not a run).
 */
static u32 comp_nil_run(Compiler *c, FlNode *const *items, u32 n, u32 at)
{
    u32 run = 0U;
    u32 i;

    while (at + run < n) {
        const FlNode *s = items[at + run];

        if (s == NULL || (FlAstKind)s->kind != FL_A_LET ||
            s->as.let.init != NULL)
            break;
        run++;
    }
    /* A run of one is cheaper as a bare NIL: two bytes against one. */
    if (run < 2U || run > 255U)
        return 0U;
    for (i = 0U; i < run; i++) {
        add_local(c, items[at + i]->as.let.name, items[at + i]->sp);
        mark_initialized(c);
    }
    emit_op(c, FL_OP_NIL_N, items[at]->sp);
    emit_u8(c, (u8)run);
    push_depth(c, (int)run);
    return run;
}

static void comp_block(Compiler *c, const FlNode *n)
{
    u32 i;

    begin_scope(c);
    for (i = 0U; i < n->as.list.n; i++) {
        u32 run = comp_nil_run(c, n->as.list.items, n->as.list.n, i);

        if (run != 0U) {
            i += run - 1U;
            continue;
        }
        comp_stmt(c, n->as.list.items[i]);
    }
    end_scope(c, n->sp);
}

static void emit_loop_exit(Compiler *c, const Loop *lp, FlSpan sp)
{
    u32 i;

    /*
     * Order matters and both halves are required at EVERY break and
     * continue site: a break inside a try must unwind the handler
     * stack, and a break out of a scope holding captured locals must
     * close them -- otherwise the closure keeps pointing at a stack
     * slot the next call reuses.
     */
    for (i = c->ntry; i > lp->try_depth; i--)
        emit_op(c, FL_OP_TRY_POP, sp);
    emit_close_to(c, lp->slot_floor, sp);
}

static void comp_while(Compiler *c, const FlNode *n)
{
    Loop *lp;
    u32 exit_site;
    u32 i;

    if (c->nloops >= (u32)FL_MAX_LOOPS) {
        cerror(c, n->sp, "loops nested too deeply");
        return;
    }
    lp = &c->loops[c->nloops++];
    (void)memset(lp, 0, sizeof(*lp));
    lp->start = (u32)c->code.len;
    lp->scope_depth = c->scope_depth;
    lp->slot_floor = c->nlocals;
    lp->try_depth = c->ntry;

    comp_expr(c, n->as.whiles.cond);
    exit_site = emit_jump(c, FL_OP_JUMP_IF_FALSE, n->sp);
    comp_stmt(c, n->as.whiles.body);
    for (i = 0U; i < lp->nconts; i++)
        patch_jump(c, lp->conts[i], n->sp);
    emit_loop_back(c, lp->start, n->sp);
    patch_jump(c, exit_site, n->sp);
    for (i = 0U; i < lp->nbreaks; i++)
        patch_jump(c, lp->breaks[i], n->sp);
    c->nloops--;
}

static void comp_for(Compiler *c, const FlNode *n)
{
    Loop *lp;
    u32 exit_site;
    u32 body_start;
    u32 i;
    bool two = n->as.fors.var2 != 0U;

    if (c->nloops >= (u32)FL_MAX_LOOPS) {
        cerror(c, n->sp, "loops nested too deeply");
        return;
    }
    begin_scope(c);
    comp_expr(c, n->as.fors.iter);
    emit_op(c, FL_OP_ITER_NEW, n->sp);
    /* Three hidden slots: subject, cursor, mods snapshot. */
    add_hidden_local(c, n->sp);
    add_hidden_local(c, n->sp);
    add_hidden_local(c, n->sp);

    lp = &c->loops[c->nloops++];
    (void)memset(lp, 0, sizeof(*lp));
    lp->start = (u32)c->code.len;
    lp->scope_depth = c->scope_depth;
    lp->try_depth = c->ntry;

    body_start = (u32)c->code.len;
    emit_op(c, two ? FL_OP_ITER_NEXT2 : FL_OP_ITER_NEXT1, n->sp);
    emit_u8(c, (u8)(c->nlocals - 3U));
    exit_site = (u32)c->code.len;
    emit_u16(c, 0xFFFFU);
    push_depth(c, two ? 2 : 1);
    (void)body_start;

    /*
     * A FRESH BINDING PER ITERATION.
     *
     * The loop variable is declared inside the body scope and closed at
     * the end of EACH iteration, so `for x in xs { fns.push(fn() x) }`
     * yields closures over distinct x's.  Closing once at loop exit
     * instead is JavaScript's `var` bug, which people file as "closures
     * are broken".  The spec does not decide this, so it is decided
     * here and tested.
     */
    begin_scope(c);
    add_local(c, n->as.fors.var, n->sp);
    mark_initialized(c);
    if (two) {
        add_local(c, n->as.fors.var2, n->sp);
        mark_initialized(c);
    }
    lp->slot_floor = c->nlocals - (two ? 2U : 1U);
    comp_stmt(c, n->as.fors.body);
    end_scope(c, n->sp);

    for (i = 0U; i < lp->nconts; i++)
        patch_jump(c, lp->conts[i], n->sp);
    emit_loop_back(c, lp->start, n->sp);
    patch_jump(c, exit_site, n->sp);
    for (i = 0U; i < lp->nbreaks; i++)
        patch_jump(c, lp->breaks[i], n->sp);
    c->nloops--;
    end_scope(c, n->sp);
}

static FlFn *comp_function(Compiler *enclosing, const FlNode *n,
                           u32 name_id, bool is_expr,
                           FlUpvalDesc *out_ups)
{
    Compiler sub;
    FlFn *fn;
    u32 i;

    (void)memset(&sub, 0, sizeof(sub));
    sub.enclosing = enclosing;
    sub.vm = enclosing->vm;
    sub.dc = enclosing->dc;
    sub.file_id = enclosing->file_id;
    sub.origin = enclosing->origin;
    sub.name_id = name_id;
    sub.arity = (u8)n->as.fn.nparams;
    bytebuf_init(&sub.code);
    sub.scope_depth = 0;

    /* Slot 0 is the callee itself, so parameters start at 1. */
    add_hidden_local(&sub, n->sp);
    for (i = 0U; i < n->as.fn.nparams; i++) {
        add_local(&sub, n->as.fn.params[i], n->sp);
        mark_initialized(&sub);
    }
    push_depth(&sub, (int)n->as.fn.nparams + 1);

    if (n->as.fn.body != NULL &&
        (FlAstKind)n->as.fn.body->kind == FL_A_BLOCK) {
        comp_block(&sub, n->as.fn.body);
        emit_op(&sub, FL_OP_RETURN_NIL, n->sp);
    } else {
        /* §2's fn_expr may take a bare expression. */
        comp_expr(&sub, n->as.fn.body);
        emit_op(&sub, FL_OP_RETURN, n->sp);
    }
    (void)is_expr;

    if (sub.failed) {
        enclosing->failed = true;
        bytebuf_free(&sub.code);
        FlConstVec_free(&sub.consts);
        FlLineVec_free(&sub.lines);
        return NULL;
    }

    fn = fl_gc_alloc(sub.vm, sizeof(*fn), FL_FN);
    /* Root 8.  From here the function lives in the enclosing
     * compiler's constant Vec -- scratch, not a root -- until
     * fl_compile pops the whole batch below. */
    if (sub.vm->ncompiling < (u32)FL_FRAMES_MAX)
        sub.vm->compiling[sub.vm->ncompiling++] = fn;
    /*
     * Copied to the arena AT FINAL SIZE, once the function is complete.
     * The builders above grow, and a growing array with live pointers
     * into it dangles -- the same pitfall Sprint 29 pinned for AST
     * child arrays.  Copying here is what makes patched jump sites safe
     * to have recorded as offsets.
     */
    fn->ch.ncode = (u32)sub.code.len;
    fn->ch.code = arena_alloc(sub.vm->arena, sub.code.len == 0U ? 1U
                                                                : sub.code.len,
                              1U);
    if (sub.code.len != 0U)
        (void)memcpy(fn->ch.code, sub.code.data, sub.code.len);
    fn->ch.nconsts = (u32)sub.consts.len;
    if (sub.consts.len != 0U) {
        fn->ch.consts = arena_alloc(sub.vm->arena,
                                    sub.consts.len * sizeof(FlValue),
                                    _Alignof(FlValue));
        (void)memcpy(fn->ch.consts, sub.consts.data,
                     sub.consts.len * sizeof(FlValue));
    }
    fn->ch.nlines = (u32)sub.lines.len;
    if (sub.lines.len != 0U) {
        fn->ch.lines = arena_alloc(sub.vm->arena,
                                   sub.lines.len * sizeof(FlLineRun),
                                   _Alignof(FlLineRun));
        (void)memcpy(fn->ch.lines, sub.lines.data,
                     sub.lines.len * sizeof(FlLineRun));
    }
    fn->ch.file_id = sub.file_id;
    fn->origin = sub.origin;
    fn->name_id = name_id;
    fn->arity = sub.arity;
    fn->nup = (u8)sub.nupvals;
    fn->max_stack = (u16)(sub.max_depth < 0 ? 0 : sub.max_depth);

    /*
     * The enclosing compiler emits the capture pairs after CLOSURE, so
     * hand it the descriptors -- INTO ITS OWN BUFFER, not over its
     * `upvals` array.
     *
     * This used to be `enclosing->upvals[i] = sub.upvals[i]`, which
     * destroyed the enclosing function's own capture descriptors every
     * time it contained a nested function.  One level deep nothing
     * noticed, because a function whose only upvalues come from the
     * child it just compiled has the same descriptors either way.  Two
     * levels deep the middle function's real descriptor -- "capture
     * local `n` from the frame below" -- was overwritten by the inner
     * function's "capture upvalue 0", so the outermost CLOSURE emitted
     * a pair pointing at an upvalue array that did not exist and the
     * VM dereferenced NULL.  Sprint 33's §7 file segfaulted on it.
     */
    for (i = 0U; i < sub.nupvals; i++)
        out_ups[i] = sub.upvals[i];

    bytebuf_free(&sub.code);
    FlConstVec_free(&sub.consts);
    FlLineVec_free(&sub.lines);
    return fn;
}

static void comp_assign_target(Compiler *c, const FlNode *tgt,
                               const FlNode *val)
{
    switch ((FlAstKind)tgt->kind) {
    case FL_A_IDENT: {
        i32 slot = resolve_local(c, tgt->as.ident.name, tgt->sp);

        comp_expr(c, val);
        if (slot >= 0) {
            emit_op(c, FL_OP_SET_LOCAL, tgt->sp);
            emit_u8(c, (u8)slot);
            return;
        }
        slot = resolve_upval(c, tgt->as.ident.name, tgt->sp);
        if (slot >= 0) {
            emit_op(c, FL_OP_SET_UPVAL, tgt->sp);
            emit_u8(c, (u8)slot);
            return;
        }
        emit_op(c, FL_OP_SET_GLOBAL, tgt->sp);
        emit_u16(c, (u16)name_const(c, tgt->as.ident.name, tgt->sp));
        return;
    }
    case FL_A_INDEX:
        comp_expr(c, tgt->as.index.obj);
        comp_expr(c, tgt->as.index.idx);
        comp_expr(c, val);
        emit_op(c, FL_OP_INDEX_SET, tgt->sp);
        return;
    default:
        comp_expr(c, tgt->as.field.obj);
        comp_expr(c, val);
        emit_op(c, FL_OP_FIELD_SET, tgt->sp);
        emit_u16(c, (u16)name_const(c, tgt->as.field.name, tgt->sp));
        return;
    }
}

/*
 * Spec §6: "a module's top-level let, fn and macro bindings are its
 * GLOBALS."  Not a naming detail -- s31 resolves imports by looking a
 * name up in the exporting module's global map, and s32's REPL compiles
 * each line as its own chunk, so a top-level binding held in a frame
 * slot would vanish between lines and export as nothing.
 */
static bool at_module_top(const Compiler *c)
{
    /* scope_depth 0 is the module body; a `let` inside ANY block --
     * including an `if` arm at the top of the file -- is a local. */
    return c->enclosing == NULL && c->scope_depth == 0;
}

/*
 * Declares a module global, refusing a duplicate.  Locals get this from
 * the FlLocal array; globals live in a runtime map that happily
 * overwrites, so without this a second `let x` in one file would
 * silently replace the first instead of naming the mistake.
 */
static void declare_global(Compiler *c, u32 name, FlSpan sp)
{
    size_t i;

    for (i = 0U; i < c->globals.len; i++) {
        if (c->globals.data[i] == name) {
            cerror(c, sp, "'%s' is already declared in this module",
                   sag_intern_str(c->vm->in, name));
            return;
        }
    }
    FlNameVec_push(&c->globals, name);
}

static void comp_stmt(Compiler *c, const FlNode *n)
{
    if (n == NULL || c->failed)
        return;
    switch ((FlAstKind)n->kind) {
    case FL_A_LET:
        if (at_module_top(c)) {
            u32 k = name_const(c, n->as.let.name, n->sp);

            declare_global(c, n->as.let.name, n->sp);
            if (n->as.let.init != NULL)
                comp_expr(c, n->as.let.init);
            else
                emit_op(c, FL_OP_NIL, n->sp);
            emit_op(c, FL_OP_DEF_GLOBAL, n->sp);
            emit_u16(c, (u16)k);
            return;
        }
        add_local(c, n->as.let.name, n->sp);
        if (n->as.let.init != NULL)
            comp_expr(c, n->as.let.init);
        else
            emit_op(c, FL_OP_NIL, n->sp);
        mark_initialized(c);
        return;
    case FL_A_ASSIGN:
        comp_assign_target(c, n->as.assign.tgt, n->as.assign.val);
        return;
    case FL_A_EXPR_STMT:
        comp_expr(c, n->as.expr_stmt.expr);
        emit_op(c, FL_OP_POP, n->sp);
        return;
    case FL_A_BLOCK:
        comp_block(c, n);
        return;
    case FL_A_IF: {
        u32 else_site;
        u32 end_site;

        comp_expr(c, n->as.ifs.cond);
        else_site = emit_jump(c, FL_OP_JUMP_IF_FALSE, n->sp);
        comp_stmt(c, n->as.ifs.then);
        end_site = emit_jump(c, FL_OP_JUMP, n->sp);
        patch_jump(c, else_site, n->sp);
        if (n->as.ifs.els != NULL)
            comp_stmt(c, n->as.ifs.els);
        patch_jump(c, end_site, n->sp);
        return;
    }
    case FL_A_WHILE:
        comp_while(c, n);
        return;
    case FL_A_FOR:
        comp_for(c, n);
        return;
    case FL_A_BREAK:
    case FL_A_CONTINUE: {
        Loop *lp;
        u32 site;

        if (c->nloops == 0U) {
            cerror(c, n->sp, "'%s' outside a loop",
                   (FlAstKind)n->kind == FL_A_BREAK ? "break" : "continue");
            return;
        }
        lp = &c->loops[c->nloops - 1U];
        emit_loop_exit(c, lp, n->sp);
        site = emit_jump(c, FL_OP_JUMP, n->sp);
        if ((FlAstKind)n->kind == FL_A_BREAK) {
            if (lp->nbreaks < (u32)FL_MAX_BREAKS)
                lp->breaks[lp->nbreaks++] = site;
        } else {
            if (lp->nconts < (u32)FL_MAX_BREAKS)
                lp->conts[lp->nconts++] = site;
        }
        return;
    }
    case FL_A_RETURN:
        if (n->as.ret.value != NULL) {
            comp_expr(c, n->as.ret.value);
            emit_op(c, FL_OP_RETURN, n->sp);
        } else {
            emit_op(c, FL_OP_RETURN_NIL, n->sp);
        }
        return;
    case FL_A_FN: {
        FlUpvalDesc ups[FL_MAX_UPVALS];
        FlFn *fn;
        u32 k;
        u32 i;

        /* A top-level fn is a global (see at_module_top); a nested one
         * is a local.  Either way the name is bound BEFORE the body
         * compiles, so a function can call itself. */
        if (at_module_top(c))
            declare_global(c, n->as.fn.name, n->sp);
        else {
            add_local(c, n->as.fn.name, n->sp);
            mark_initialized(c);
        }
        fn = comp_function(c, n, n->as.fn.name, false, ups);
        if (fn == NULL)
            return;
        k = add_const(c, FL_OBJ_V(FL_FN, fn), n->sp);
        emit_op(c, FL_OP_CLOSURE, n->sp);
        emit_u16(c, (u16)k);
        for (i = 0U; i < (u32)fn->nup; i++) {
            emit_u8(c, ups[i].is_local ? 1U : 0U);
            emit_u8(c, ups[i].index);
        }
        if (at_module_top(c)) {
            emit_op(c, FL_OP_DEF_GLOBAL, n->sp);
            emit_u16(c, (u16)name_const(c, n->as.fn.name, n->sp));
        }
        return;
    }
    case FL_A_MACRO: {
        /*
         * §4: `macro name = @[...]` is `let name = fn() @[...]`.
         *
         * A FUNCTION, not the block itself.  Compiling the motion block
         * inline bound the name to the block's RESULT and ran the
         * motions at definition time -- so `macro dup = @[...]` at the
         * top of a config fired against the buffer the moment the file
         * loaded, and against the null host it raised "motion" before
         * anything else in the file could run.  That is what §14's
         * example found the day `import` started working.
         *
         * The sugar is resolved by handing comp_function a synthesised
         * zero-parameter fn node whose body is the block, so nothing
         * downstream needs to know macros exist and the closure,
         * upvalue and origin handling are all the ones fn already has.
         */
        FlUpvalDesc ups[FL_MAX_UPVALS];
        FlNode syn;
        FlFn *mf;
        u32 k;
        u32 i;

        (void)memset(&syn, 0, sizeof(syn));
        syn.kind = (u8)FL_A_FN;
        syn.sp = n->sp;
        syn.as.fn.body = n->as.macro.body;
        syn.as.fn.params = NULL;
        syn.as.fn.nparams = 0U;
        syn.as.fn.name = n->as.macro.name;
        if (at_module_top(c))
            declare_global(c, n->as.macro.name, n->sp);
        else {
            add_local(c, n->as.macro.name, n->sp);
            mark_initialized(c);
        }
        mf = comp_function(c, &syn, n->as.macro.name, false, ups);
        if (mf == NULL)
            return;
        /* §6: a macro frame prints as `macro m`. */
        mf->fnkind = (u8)FL_FN_MACRO;
        k = add_const(c, FL_OBJ_V(FL_FN, mf), n->sp);
        emit_op(c, FL_OP_CLOSURE, n->sp);
        emit_u16(c, (u16)k);
        for (i = 0U; i < (u32)mf->nup; i++) {
            emit_u8(c, ups[i].is_local ? 1U : 0U);
            emit_u8(c, ups[i].index);
        }
        if (at_module_top(c)) {
            emit_op(c, FL_OP_DEF_GLOBAL, n->sp);
            emit_u16(c, (u16)name_const(c, n->as.macro.name, n->sp));
        }
        return;
    }
    case FL_A_EDIT:
        emit_op(c, FL_OP_EDIT_BEGIN, n->sp);
        comp_stmt(c, n->as.edit.body);
        emit_op(c, FL_OP_EDIT_END, n->sp);
        return;
    case FL_A_TRY: {
        u32 handler_site;
        u32 end_site;

        handler_site = emit_jump(c, FL_OP_TRY_PUSH, n->sp);
        c->ntry++;
        comp_stmt(c, n->as.trys.body);
        emit_op(c, FL_OP_TRY_POP, n->sp);
        c->ntry--;
        end_site = emit_jump(c, FL_OP_JUMP, n->sp);
        patch_jump(c, handler_site, n->sp);
        begin_scope(c);
        add_local(c, n->as.trys.var, n->sp);
        mark_initialized(c);
        push_depth(c, 1);          /* the raised value lands in the slot */
        comp_stmt(c, n->as.trys.handler);
        end_scope(c, n->sp);
        patch_jump(c, end_site, n->sp);
        return;
    }
    case FL_A_IMPORT: {
        /*
         * The constant carries what to RESOLVE -- the bare name, or the
         * quoted path -- and the byte says which.  The binding uses
         * `name` either way, so `import "lib/x.fl" as x` binds x.
         */
        u32 what = n->as.import.is_string ? n->as.import.path
                                          : n->as.import.name;

        emit_op(c, FL_OP_IMPORT, n->sp);
        emit_u16(c, (u16)name_const(c, what, n->sp));
        emit_u8(c, n->as.import.is_string ? 1U : 0U);
        if (at_module_top(c)) {
            u32 nk = name_const(c, n->as.import.name, n->sp);

            declare_global(c, n->as.import.name, n->sp);
            emit_op(c, FL_OP_DEF_GLOBAL, n->sp);
            emit_u16(c, (u16)nk);
            return;
        }
        /* §11: import is a statement, so it is legal inside a block --
         * there it binds a local and hits the same cache. */
        add_local(c, n->as.import.name, n->sp);
        mark_initialized(c);
        return;
    }
    default:
        comp_expr(c, n);
        emit_op(c, FL_OP_POP, n->sp);
        return;
    }
}

/* ---------------------------------------------------------------- */
/* Entry point                                                      */
/* ---------------------------------------------------------------- */

static FlFn *compile_program(FlVm *vm, DiagCtx *dc, const FlProgram *p,
                             u32 file_id, FlOrigin origin, u8 fnkind)
{
    Compiler top;
    FlFn *fn;
    u32 i;
    u32 root_base = vm->ncompiling;
    FlSpan end = {file_id, 0U, 0U, 0U};

    (void)memset(&top, 0, sizeof(top));
    top.vm = vm;
    top.dc = dc;
    top.file_id = file_id;
    top.origin = origin;
    bytebuf_init(&top.code);
    add_hidden_local(&top, end);   /* slot 0: the top-level "callee" */
    push_depth(&top, 1);

    for (i = 0U; i < p->n; i++) {
        const FlNode *st = p->stmts[i];

        /*
         * §2: the prompt's last expression is the entry's VALUE.  Only
         * the last, and only a bare expression -- `let x = 1` prints
         * nothing because it evaluates to nothing, which is what makes
         * a prompt readable.
         */
        if (fnkind == (u8)FL_FN_REPL && i + 1U == p->n &&
            st->kind == FL_A_EXPR_STMT) {
            comp_expr(&top, st->as.expr_stmt.expr);
            emit_op(&top, FL_OP_RETURN, st->sp);
            break;
        }
        comp_stmt(&top, st);
    }
    /*
     * The trailing HALT belongs to the last statement's line, not to
     * line 0.  A zero here is not cosmetic: s32's traceback reads the
     * line runs, and an error unwinding to the module frame would
     * print `file:0:0`, which no editor can jump to.
     */
    if (p->n != 0U)
        end = p->stmts[p->n - 1U]->sp;
    else
        end.line = end.col = 1U;
    emit_op(&top, FL_OP_HALT, end);

    if (top.failed) {
        vm->ncompiling = root_base;
        bytebuf_free(&top.code);
        FlConstVec_free(&top.consts);
        FlNameVec_free(&top.globals);
        FlLineVec_free(&top.lines);
        return NULL;
    }
    fn = fl_gc_alloc(vm, sizeof(*fn), FL_FN);
    fn->ch.ncode = (u32)top.code.len;
    fn->ch.code = arena_alloc(vm->arena,
                              top.code.len == 0U ? 1U : top.code.len, 1U);
    if (top.code.len != 0U)
        (void)memcpy(fn->ch.code, top.code.data, top.code.len);
    fn->ch.nconsts = (u32)top.consts.len;
    if (top.consts.len != 0U) {
        fn->ch.consts = arena_alloc(vm->arena,
                                    top.consts.len * sizeof(FlValue),
                                    _Alignof(FlValue));
        (void)memcpy(fn->ch.consts, top.consts.data,
                     top.consts.len * sizeof(FlValue));
    }
    fn->ch.nlines = (u32)top.lines.len;
    if (top.lines.len != 0U) {
        fn->ch.lines = arena_alloc(vm->arena,
                                   top.lines.len * sizeof(FlLineRun),
                                   _Alignof(FlLineRun));
        (void)memcpy(fn->ch.lines, top.lines.data,
                     top.lines.len * sizeof(FlLineRun));
    }
    fn->ch.file_id = file_id;
    fn->max_stack = (u16)(top.max_depth < 0 ? 0 : top.max_depth);
    fn->origin = origin;
    /* s32 §6 names the outermost frame from this: `<script>` for a
     * file, `<repl>` for one prompt entry. */
    fn->fnkind = fnkind;
#if FL_VM_CHECKS
    {
        const char *why = NULL;

        /* Our own output, checked in checked builds.  A failure here is
         * a compiler bug, not a user error, so it is a SAG_BUG. */
        if (!fl_chunk_check(fn, &why))
            SAG_BUG("fl compiler emitted a bad chunk: %s",
                    why == NULL ? "?" : why);
    }
#endif
    /*
     * The batch is released here, not per function: a nested function
     * must stay rooted until its PARENT's chunk has reached the arena,
     * and the parent is the last thing built.  The top-level fn itself
     * is unrooted on return by design -- the caller runs it, and
     * fl_vm_run puts it on the stack before the first allocation that
     * could collect.
     */
    vm->ncompiling = root_base;
    bytebuf_free(&top.code);
    FlConstVec_free(&top.consts);
    FlNameVec_free(&top.globals);
    FlLineVec_free(&top.lines);
    return fn;
}

FlFn *fl_compile(FlVm *vm, DiagCtx *dc, const FlProgram *p, u32 file_id,
                 FlOrigin origin)
{
    return compile_program(vm, dc, p, file_id, origin, (u8)FL_FN_SCRIPT);
}

FlFn *fl_compile_repl(FlVm *vm, DiagCtx *dc, const FlProgram *p, u32 file_id,
                      FlOrigin origin)
{
    return compile_program(vm, dc, p, file_id, origin, (u8)FL_FN_REPL);
}
