/*
 * Sprint 30 deliverable 7: the Fletch VM.
 *
 * THE SWITCH IS THE REFERENCE IMPLEMENTATION.  Labels-as-values are a
 * GNU extension and outside the C11 subset 00-decisions.md pins, so the
 * portable build must be complete and correct on its own; the
 * computed-goto twin is an optimisation behind FL_COMPUTED_GOTO, and
 * this is the one translation unit permitted to relax the dialect.
 *
 * BOTH BODIES COME FROM ONE opcodes.def.  DoD 5 requires byte-identical
 * traces from the two modes, and the only durable way to get that is
 * for neither to carry its own copy of the instruction list.
 */
#include "fl/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/opcodes.h"
#include "fl/std.h"
#include "util/log.h"

/* ---------------------------------------------------------------- */
/* The null host (deliverable 11)                                   */
/* ---------------------------------------------------------------- */

static bool null_motion(void *ud, const FlMotionProg *p, FlErr *err)
{
    (void)ud;
    (void)p;
    /*
     * Spec §3.1: a headless VM without an editor host raises kind
     * "motion".  This is exactly what makes the spec's §14 `shout`
     * example return "MOTION", and it is a real answer rather than a
     * stub -- Sprint 34 supplies a host that does the work.
     */
    err->kind_id = 0U;      /* filled by the caller, which owns the interner */
    return false;
}

static bool null_edit_begin(void *ud, FlErr *err)
{
    (void)ud;
    (void)err;
    return true;            /* §10: with no host a transaction is a no-op */
}

static bool null_edit_end(void *ud, bool ok, FlErr *err)
{
    (void)ud;
    (void)ok;
    (void)err;
    return true;
}

const FlHost fl_host_null = {
    NULL, null_motion, null_edit_begin, null_edit_end
};

/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

bool fl_vm_init(FlVm *vm, Arena *a, Interner *in, DiagCtx *dc)
{
    (void)memset(vm, 0, sizeof(*vm));
    vm->arena = a;
    vm->in = in;
    vm->dc = dc;
    vm->host = &fl_host_null;
    vm->sp = vm->stack;
    fl_gc_init(&vm->gc);
    /*
     * The GC-stress lane, per s30 DoD 7.  Read here rather than passed
     * in, because the point is to run the WHOLE existing suite under
     * stress without every test learning about it -- a lane that needs
     * each caller to opt in is a lane that covers whatever people
     * remembered to change.
     *
     * ~30x slower, so it is its own lane and never `make test`.
     */
    if (getenv("FL_GC_STRESS") != NULL)
        vm->gc.stress = true;
#if FL_VM_TRACE
    bytebuf_init(&vm->trace);
#endif
    vm->globals = fl_map_new(vm);
    vm->modules = fl_map_new(vm);
    vm->builtins = fl_map_new(vm);
    vm->err = FL_NIL_V;
    return true;
}

void fl_vm_free(FlVm *vm)
{
    /* The handle table is Sprint 34's; it is rooted and empty here, so
     * there is nothing of ours to release beyond the heap itself. */
#if FL_VM_TRACE
    bytebuf_free(&vm->trace);
#endif
    /* `re`'s compiled-pattern cache is process-wide -- a compiled
     * program depends on the pattern and nothing else -- so it is not
     * ours to own, only ours to leave empty. */
    fl_re_cache_clear();
    fl_gc_free_all(vm);
}

/* ---------------------------------------------------------------- */
/* Raising                                                          */
/* ---------------------------------------------------------------- */

static FlValue make_str(FlVm *vm, const char *s)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s, (u32)strlen(s)));
}

/*
 * Builds the error map {kind, msg} and leaves it in vm->err.  Sprint 32
 * adds `trace` when the value escapes every frame; the shape is fixed
 * here so a handler written today keeps working.
 */
bool fl_raise(FlVm *vm, const char *kind, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    FlMap *m;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m = fl_map_new(vm);
    /* Protected across the two allocations below: a collection between
     * them would otherwise sweep the half-built map, which is gc.h
     * rule 2 in its most literal form. */
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, m));
    (void)fl_map_set(vm, m, make_str(vm, "kind"), make_str(vm, kind));
    (void)fl_map_set(vm, m, make_str(vm, "msg"), make_str(vm, buf));
    fl_gc_release(vm, 1U);
    vm->err = FL_OBJ_V(FL_MAP, m);
    return false;      /* so a native can `return fl_raise(...)` */
}

/* ---------------------------------------------------------------- */
/* Upvalues                                                         */
/* ---------------------------------------------------------------- */

static FlUpval *capture_upval(FlVm *vm, FlValue *slot)
{
    FlUpval **link = &vm->open_upvals;
    FlUpval *uv = vm->open_upvals;

    /* The open list is kept in descending slot order, so the search
     * stops as soon as it passes the slot.  Sharing matters: two
     * closures made in the same scope must get the SAME FlUpval, which
     * is what makes the §14 counter example count. */
    while (uv != NULL && uv->slot > slot) {
        link = &uv->next;
        uv = uv->next;
    }
    if (uv != NULL && uv->slot == slot)
        return uv;
    {
        FlUpval *fresh = fl_gc_alloc(vm, sizeof(*fresh), FL_UPVAL);

        fresh->slot = slot;
        fresh->closed = FL_NIL_V;
        fresh->next = uv;
        *link = fresh;
        return fresh;
    }
}

static void close_upvals(FlVm *vm, const FlValue *floor)
{
    while (vm->open_upvals != NULL && vm->open_upvals->slot >= floor) {
        FlUpval *uv = vm->open_upvals;

        /* Copy the value INTO the upvalue and repoint, so the closure
         * keeps reading the same variable after the frame is gone. */
        uv->closed = *uv->slot;
        uv->slot = &uv->closed;
        vm->open_upvals = uv->next;
        uv->next = NULL;
    }
}

/* ---------------------------------------------------------------- */
/* Dispatch                                                         */
/* ---------------------------------------------------------------- */

/*
 * THE INSTRUCTION BOUNDARY, written once and used by both dispatchers.
 *
 * The switch loop reaches the top of `for (;;)` between instructions;
 * the computed-goto path never does -- `goto *dispatch[*ip++]` lands on
 * the next opcode's label directly, which is the entire point of it.
 * So the boundary work cannot live at the loop top, or the cgoto build
 * silently never collects and never enforces the step limit: a build
 * that passes the GC-stress tests by not having a GC.
 *
 * Both paths therefore expand THIS macro, for the same reason
 * opcodes.def is one file: a second copy of the rule is a second thing
 * to keep in step, and DoD 5 requires the two modes to be identical.
 */
#define VM_BOUNDARY()                                                     \
    do {                                                                  \
        /*                                                                \
         * Collection happens HERE and only here.  Mid-instruction the    \
         * VM holds object pointers in C locals that no root covers, so   \
         * fl_gc_alloc only ever sets the flag and this is where it is    \
         * safe to honour it.                                             \
         */                                                               \
        if (vm->gc.pending) {                                             \
            frame->ip = ip;                                               \
            fl_gc_collect(vm);                                            \
        }                                                                 \
        if (vm->step_limit != 0U && ++vm->steps > vm->step_limit) {       \
            fl_raise(vm, "limit", "step limit exceeded");                 \
            goto raised;                                                  \
        }                                                                 \
    } while (0)

/*
 * Every instruction is fetched at exactly two places -- the switch
 * loop's head and the cgoto VM_NEXT -- so tracing both covers the whole
 * stream in either mode with no per-arm bookkeeping.
 */
#if FL_VM_TRACE
#  define VM_TRACE(op) bytebuf_push_u8(&vm->trace, (u8)(op))
#else
#  define VM_TRACE(op) ((void)0)
#endif

#if FL_COMPUTED_GOTO
#  define VM_CASE(N)  L_##N:
#  define VM_NEXT()                                                       \
    do {                                                                  \
        u8 next_op;                                                       \
                                                                          \
        VM_BOUNDARY();                                                    \
        next_op = *ip++;                                                  \
        VM_TRACE(next_op);                                                \
        goto *dispatch[next_op];                                          \
    } while (0)
#else
#  define VM_CASE(N)  case FL_OP_##N:
#  define VM_NEXT()   break
#endif

static u16 read_u16(const u8 **ip)
{
    u16 v = (u16)((u32)(*ip)[0] | ((u32)(*ip)[1] << 8));

    *ip += 2;
    return v;
}

bool fl_vm_run(FlVm *vm, FlFn *entry, FlValue *out);

/*
 * The dispatch loop, re-entrant.
 *
 * `base` is the frame count this execution started at: RETURN hands
 * control back when the frame stack drops to it, rather than only at
 * zero.  That is what lets a native call back into Fletch --
 * list.map's callback, re.replace_fn's replacement -- without a second
 * interpreter or a longjmp.
 *
 * An unwind must ALSO respect `base`: a handler established outside
 * this execution belongs to the loop that owns it, so we return false
 * and let the native's caller propagate.  Catching it here would
 * resume the outer function's code inside the inner loop, with the
 * outer loop still sitting in its own frame -- two dispatchers running
 * one frame stack.
 */
static bool vm_exec(FlVm *vm, u32 base, FlValue *out)
{
    FlFrame *frame;
    const u8 *ip;

#if FL_COMPUTED_GOTO
    /*
     * PADDED TO 256.  The table is indexed by a byte read straight out
     * of the chunk, so it has no equivalent of the switch's default
     * arm: every slot the instruction set does not use must point at a
     * shared bad-opcode label, or a corrupt byte jumps into whatever
     * follows the array.  Never leave it short.
     */
    static void *const dispatch[256] = {
#  define FL_OP(N, o, e, d) &&L_##N,
#  include "fl/opcodes.def"
#  undef FL_OP
        [FL_OP__COUNT ... 255] = &&L_BAD_OP
    };
#endif

    /* Resume from whatever frame the caller set up. */
    frame = &vm->frames[vm->nframes - 1U];
    ip = frame->ip;

    for (;;) {
        u8 op;

        VM_BOUNDARY();
        op = *ip++;
        VM_TRACE(op);
#if FL_COMPUTED_GOTO
        goto *dispatch[op];
#else
        switch (op) {
#endif

        VM_CASE(CONST) {
            u16 k = read_u16(&ip);

            *vm->sp++ = frame->cl->fn->ch.consts[k];
            VM_NEXT();
        }
        VM_CASE(NIL)   { *vm->sp++ = FL_NIL_V; VM_NEXT(); }
        VM_CASE(TRUE)  { *vm->sp++ = FL_BOOL_V(true); VM_NEXT(); }
        VM_CASE(FALSE) { *vm->sp++ = FL_BOOL_V(false); VM_NEXT(); }
        VM_CASE(INT8)  { *vm->sp++ = FL_INT_V((i8)*ip++); VM_NEXT(); }
        VM_CASE(NIL_N) {
            u8 n = *ip++;
            u8 i;

            for (i = 0U; i < n; i++)
                *vm->sp++ = FL_NIL_V;
            VM_NEXT();
        }
        VM_CASE(POP)  { vm->sp--; VM_NEXT(); }
        VM_CASE(POPN) { vm->sp -= *ip++; VM_NEXT(); }
        VM_CASE(DUP)  { *vm->sp = vm->sp[-1]; vm->sp++; VM_NEXT(); }

        VM_CASE(GET_LOCAL) { *vm->sp++ = frame->slots[*ip++]; VM_NEXT(); }
        VM_CASE(SET_LOCAL) { frame->slots[*ip++] = *--vm->sp; VM_NEXT(); }
        VM_CASE(GET_UPVAL) {
            *vm->sp++ = *frame->cl->up[*ip++]->slot;
            VM_NEXT();
        }
        VM_CASE(SET_UPVAL) {
            *frame->cl->up[*ip++]->slot = *--vm->sp;
            VM_NEXT();
        }
        VM_CASE(CLOSE_UPVALS) {
            u8 s = *ip++;

            close_upvals(vm, frame->slots + s);
            vm->sp = frame->slots + s;
            VM_NEXT();
        }

        VM_CASE(GET_GLOBAL) {
            u16 k = read_u16(&ip);
            FlValue name = frame->cl->fn->ch.consts[k];
            FlValue got;

            if (!fl_map_get(vm->globals, name, &got)) {
                /* §4: no implicit globals.  A miss is an error, not
                 * nil, because "typo yields nil" is how a config file
                 * silently does nothing. */
                fl_raise(vm, "name", "undefined name '%s'",
                         sag_intern_str(vm->in, (u32)name.as.i));
                goto raised;
            }
            *vm->sp++ = got;
            VM_NEXT();
        }
        VM_CASE(SET_GLOBAL) {
            u16 k = read_u16(&ip);
            FlValue name = frame->cl->fn->ch.consts[k];

            if (!fl_map_get(vm->globals, name, NULL)) {
                fl_raise(vm, "name", "undefined name '%s'",
                         sag_intern_str(vm->in, (u32)name.as.i));
                goto raised;
            }
            (void)fl_map_set(vm, vm->globals, name, *--vm->sp);
            VM_NEXT();
        }
        VM_CASE(DEF_GLOBAL) {
            u16 k = read_u16(&ip);

            (void)fl_map_set(vm, vm->globals,
                             frame->cl->fn->ch.consts[k], *--vm->sp);
            VM_NEXT();
        }

        VM_CASE(ADD) {
            FlValue b = *--vm->sp;
            FlValue a = *--vm->sp;

            if (a.t == (u8)FL_INT && b.t == (u8)FL_INT) {
                *vm->sp++ = FL_INT_V(a.as.i + b.as.i);
            } else if (a.t == (u8)FL_STR && b.t == (u8)FL_STR) {
                const FlStr *x = (const FlStr *)a.as.o;
                const FlStr *y = (const FlStr *)b.as.o;
                Bytebuf bb;

                bytebuf_init(&bb);
                bytebuf_append(&bb, x->b, x->len);
                bytebuf_append(&bb, y->b, y->len);
                *vm->sp++ = FL_OBJ_V(FL_STR, fl_str_take(vm, &bb));
                bytebuf_free(&bb);
            } else if (a.t == (u8)FL_LIST && b.t == (u8)FL_LIST) {
                /*
                 * §5: '+' concatenates list+list into a NEW list --
                 * neither operand is mutated, which is what lets
                 * `acc = acc + [x]` in a loop stay correct when a
                 * closure captured an earlier acc.
                 *
                 * a and b were popped, so nothing roots them across
                 * the allocations below; rule 2's temp stack does.
                 */
                const FlList *x = (const FlList *)a.as.o;
                const FlList *y = (const FlList *)b.as.o;
                FlList *out;
                u32 i;

                fl_gc_protect(vm, a);
                fl_gc_protect(vm, b);
                out = fl_list_new(vm);
                fl_gc_protect(vm, FL_OBJ_V(FL_LIST, out));
                for (i = 0U; i < x->n; i++)
                    (void)fl_list_push(vm, out, x->v[i]);
                for (i = 0U; i < y->n; i++)
                    (void)fl_list_push(vm, out, y->v[i]);
                fl_gc_release(vm, 3U);
                *vm->sp++ = FL_OBJ_V(FL_LIST, out);
            } else if ((a.t == (u8)FL_INT || a.t == (u8)FL_FLOAT) &&
                       (b.t == (u8)FL_INT || b.t == (u8)FL_FLOAT)) {
                double x = a.t == (u8)FL_INT ? (double)a.as.i : a.as.f;
                double y = b.t == (u8)FL_INT ? (double)b.as.i : b.as.f;

                *vm->sp++ = FL_FLOAT_V(x + y);
            } else {
                fl_raise(vm, "type", "cannot add %s and %s",
                         fl_type_name((FlType)a.t), fl_type_name((FlType)b.t));
                goto raised;
            }
            VM_NEXT();
        }
#define FL_VM_ARITH(NAME, OPI, OPF)                                        \
        VM_CASE(NAME) {                                                    \
            FlValue b = *--vm->sp;                                         \
            FlValue a = *--vm->sp;                                         \
                                                                           \
            if (a.t == (u8)FL_INT && b.t == (u8)FL_INT) {                  \
                *vm->sp++ = FL_INT_V(a.as.i OPI b.as.i);                   \
            } else if ((a.t == (u8)FL_INT || a.t == (u8)FL_FLOAT) &&       \
                       (b.t == (u8)FL_INT || b.t == (u8)FL_FLOAT)) {       \
                double x = a.t == (u8)FL_INT ? (double)a.as.i : a.as.f;    \
                double y = b.t == (u8)FL_INT ? (double)b.as.i : b.as.f;    \
                                                                           \
                *vm->sp++ = FL_FLOAT_V(x OPF y);                           \
            } else {                                                       \
                fl_raise(vm, "type", "cannot apply '%s' to %s and %s",     \
                         #OPI, fl_type_name((FlType)a.t),                  \
                         fl_type_name((FlType)b.t));                       \
                goto raised;                                               \
            }                                                              \
            VM_NEXT();                                                     \
        }
        FL_VM_ARITH(SUB, -, -)
        FL_VM_ARITH(MUL, *, *)
#undef FL_VM_ARITH
        VM_CASE(DIV) {
            FlValue b = *--vm->sp;
            FlValue a = *--vm->sp;

            if (a.t == (u8)FL_INT && b.t == (u8)FL_INT) {
                if (b.as.i == 0) {
                    fl_raise(vm, "div", "division by zero");
                    goto raised;
                }
                /* Truncates toward zero, which is C's behaviour and the
                 * spec's. */
                *vm->sp++ = FL_INT_V(a.as.i / b.as.i);
            } else {
                double x = a.t == (u8)FL_INT ? (double)a.as.i : a.as.f;
                double y = b.t == (u8)FL_INT ? (double)b.as.i : b.as.f;

                *vm->sp++ = FL_FLOAT_V(x / y);
            }
            VM_NEXT();
        }
        VM_CASE(MOD) {
            FlValue b = *--vm->sp;
            FlValue a = *--vm->sp;

            if (a.t != (u8)FL_INT || b.t != (u8)FL_INT) {
                fl_raise(vm, "type", "'%%' needs two ints");
                goto raised;
            }
            if (b.as.i == 0) {
                fl_raise(vm, "div", "modulo by zero");
                goto raised;
            }
            *vm->sp++ = FL_INT_V(a.as.i % b.as.i);   /* sign of the dividend */
            VM_NEXT();
        }
        VM_CASE(NEG) {
            FlValue a = vm->sp[-1];

            if (a.t == (u8)FL_INT)
                vm->sp[-1] = FL_INT_V(-a.as.i);
            else if (a.t == (u8)FL_FLOAT)
                vm->sp[-1] = FL_FLOAT_V(-a.as.f);
            else {
                fl_raise(vm, "type", "cannot negate %s",
                         fl_type_name((FlType)a.t));
                goto raised;
            }
            VM_NEXT();
        }
        VM_CASE(NOT) {
            vm->sp[-1] = FL_BOOL_V(!fl_truthy(vm->sp[-1]));
            VM_NEXT();
        }

        VM_CASE(EQ) {
            FlValue b = *--vm->sp;

            vm->sp[-1] = FL_BOOL_V(fl_equal(vm->sp[-1], b));
            VM_NEXT();
        }
        VM_CASE(NE) {
            FlValue b = *--vm->sp;

            vm->sp[-1] = FL_BOOL_V(!fl_equal(vm->sp[-1], b));
            VM_NEXT();
        }
#define FL_VM_CMP(NAME, OP)                                                \
        VM_CASE(NAME) {                                                    \
            FlValue b = *--vm->sp;                                         \
            FlValue a = *--vm->sp;                                         \
                                                                           \
            if (a.t == (u8)FL_INT && b.t == (u8)FL_INT) {                  \
                *vm->sp++ = FL_BOOL_V(a.as.i OP b.as.i);                   \
            } else if ((a.t == (u8)FL_INT || a.t == (u8)FL_FLOAT) &&       \
                       (b.t == (u8)FL_INT || b.t == (u8)FL_FLOAT)) {       \
                double x = a.t == (u8)FL_INT ? (double)a.as.i : a.as.f;    \
                double y = b.t == (u8)FL_INT ? (double)b.as.i : b.as.f;    \
                                                                           \
                *vm->sp++ = FL_BOOL_V(x OP y);                             \
            } else if (a.t == (u8)FL_STR && b.t == (u8)FL_STR) {           \
                const FlStr *x = (const FlStr *)a.as.o;                    \
                const FlStr *y = (const FlStr *)b.as.o;                    \
                u32 n = x->len < y->len ? x->len : y->len;                 \
                int c = n == 0U ? 0 : memcmp(x->b, y->b, n);               \
                                                                           \
                if (c == 0)                                                \
                    c = x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);  \
                *vm->sp++ = FL_BOOL_V(c OP 0);                             \
            } else {                                                       \
                fl_raise(vm, "type", "cannot compare %s and %s",           \
                         fl_type_name((FlType)a.t),                        \
                         fl_type_name((FlType)b.t));                       \
                goto raised;                                               \
            }                                                              \
            VM_NEXT();                                                     \
        }
        FL_VM_CMP(LT, <)
        FL_VM_CMP(LE, <=)
        FL_VM_CMP(GT, >)
        FL_VM_CMP(GE, >=)
#undef FL_VM_CMP

        VM_CASE(JUMP)      { u16 d = read_u16(&ip); ip += d; VM_NEXT(); }
        VM_CASE(JUMP_BACK) { u16 d = read_u16(&ip); ip -= d; VM_NEXT(); }
        VM_CASE(JUMP_IF_FALSE) {
            u16 d = read_u16(&ip);

            if (!fl_truthy(*--vm->sp))
                ip += d;
            VM_NEXT();
        }
        VM_CASE(JUMP_IF_TRUE) {
            u16 d = read_u16(&ip);

            if (fl_truthy(*--vm->sp))
                ip += d;
            VM_NEXT();
        }
        VM_CASE(OR_JUMP) {
            u16 d = read_u16(&ip);

            if (fl_truthy(vm->sp[-1]))
                ip += d;                 /* peek: the value stays */
            VM_NEXT();
        }
        VM_CASE(AND_JUMP) {
            u16 d = read_u16(&ip);

            if (!fl_truthy(vm->sp[-1]))
                ip += d;
            VM_NEXT();
        }

        VM_CASE(CALL) {
            u8 n = *ip++;
            FlValue callee = vm->sp[-(int)n - 1];
            FlClosure *target;

            /*
             * Natives push no frame: they run to completion inside this
             * instruction and leave one value where the callee sat.
             *
             * Arity is checked HERE and never inside a native, so all
             * ~200 of them report it identically and none can forget.
             * The capability check is the native's own business,
             * because §13 reads the CALLER's origin and only the native
             * knows which bit it needs.
             */
            if (callee.t == (u8)FL_NATIVE) {
                FlNative *nat = (FlNative *)callee.as.o;
                FlValue *argv = vm->sp - (int)n;
                FlValue res = FL_NIL_V;

                if (n < nat->min_ar ||
                    (nat->max_ar != 255U && n > nat->max_ar)) {
                    const char *nm = sag_intern_str(vm->in, nat->name_id);

                    if (nat->max_ar == 255U)
                        fl_raise(vm, "arity",
                                 "%s expects at least %u argument%s, got %u",
                                 nm, (unsigned)nat->min_ar,
                                 nat->min_ar == 1U ? "" : "s", (unsigned)n);
                    else if (nat->min_ar == nat->max_ar)
                        fl_raise(vm, "arity",
                                 "%s expects %u argument%s, got %u",
                                 nm, (unsigned)nat->min_ar,
                                 nat->min_ar == 1U ? "" : "s", (unsigned)n);
                    else
                        fl_raise(vm, "arity",
                                 "%s expects %u..%u arguments, got %u",
                                 nm, (unsigned)nat->min_ar,
                                 (unsigned)nat->max_ar, (unsigned)n);
                    goto raised;
                }
                /* frame->ip must be current: a native may raise, and the
                 * unwind reads it to find the handler. */
                frame->ip = ip;
                vm->cur_native = nat->name_id;
                if (!nat->fn(vm, argv, (u32)n, &res))
                    goto raised;
                vm->sp -= (int)n + 1;      /* args and the callee */
                *vm->sp++ = res;
                VM_NEXT();
            }
            if (callee.t != (u8)FL_CLOSURE) {
                fl_raise(vm, "type", "cannot call %s",
                         fl_type_name((FlType)callee.t));
                goto raised;
            }
            target = (FlClosure *)callee.as.o;
            if (n != target->fn->arity) {
                fl_raise(vm, "arity", "expected %u arguments, got %u",
                         (unsigned)target->fn->arity, (unsigned)n);
                goto raised;
            }
            if (vm->nframes >= (u32)FL_FRAMES_MAX) {
                /* Catchable "limit", not sag_bug: infinite recursion is
                 * user-triggerable, and §16-A1 exists so it does not
                 * have to be misfiled under one of the other kinds. */
                fl_raise(vm, "limit", "call depth exceeded");
                goto raised;
            }
            if (vm->sp + target->fn->max_stack >= vm->stack + FL_STACK_MAX) {
                fl_raise(vm, "limit", "value stack exhausted");
                goto raised;
            }
            frame->ip = ip;
            frame = &vm->frames[vm->nframes++];
            frame->cl = target;
            frame->ip = target->fn->ch.code;
            frame->slots = vm->sp - n - 1;
            ip = frame->ip;
            VM_NEXT();
        }
        VM_CASE(RETURN) {
            FlValue result = *--vm->sp;

            close_upvals(vm, frame->slots);
            vm->nframes--;
            if (vm->nframes == base) {
                if (out != NULL)
                    *out = result;
                return true;
            }
            vm->sp = frame->slots;
            *vm->sp++ = result;
            frame = &vm->frames[vm->nframes - 1U];
            ip = frame->ip;
            VM_NEXT();
        }
        VM_CASE(RETURN_NIL) {
            close_upvals(vm, frame->slots);
            vm->nframes--;
            if (vm->nframes == base) {
                if (out != NULL)
                    *out = FL_NIL_V;
                return true;
            }
            vm->sp = frame->slots;
            *vm->sp++ = FL_NIL_V;
            frame = &vm->frames[vm->nframes - 1U];
            ip = frame->ip;
            VM_NEXT();
        }
        VM_CASE(CLOSURE) {
            u16 k = read_u16(&ip);
            FlFn *fn = (FlFn *)frame->cl->fn->ch.consts[k].as.o;
            FlClosure *made = fl_gc_alloc(vm, sizeof(*made), FL_CLOSURE);
            u32 i;

            made->fn = fn;
            made->nup = fn->nup;
            if (fn->nup != 0U)
                made->up = fl_gc_upvals(vm, fn->nup);
            for (i = 0U; i < (u32)fn->nup; i++) {
                u8 is_local = *ip++;
                u8 idx = *ip++;

                made->up[i] = is_local != 0U
                                  ? capture_upval(vm, frame->slots + idx)
                                  : frame->cl->up[idx];
            }
            *vm->sp++ = FL_OBJ_V(FL_CLOSURE, made);
            VM_NEXT();
        }

        VM_CASE(LIST) {
            u16 n = read_u16(&ip);
            FlList *l = fl_list_new(vm);
            u32 i;

            for (i = 0U; i < (u32)n; i++)
                (void)fl_list_push(vm, l, vm->sp[-(int)n + (int)i]);
            vm->sp -= n;
            *vm->sp++ = FL_OBJ_V(FL_LIST, l);
            VM_NEXT();
        }
        VM_CASE(MAP) {
            u16 n = read_u16(&ip);
            FlMap *m = fl_map_new(vm);
            u32 i;

            for (i = 0U; i < (u32)n; i++) {
                FlValue k = vm->sp[-2 * (int)n + 2 * (int)i];
                FlValue v = vm->sp[-2 * (int)n + 2 * (int)i + 1];

                if (!fl_hashable(k)) {
                    fl_raise(vm, "key", "%s is not a valid map key",
                             fl_type_name((FlType)k.t));
                    goto raised;
                }
                (void)fl_map_set(vm, m, k, v);
            }
            vm->sp -= 2 * n;
            *vm->sp++ = FL_OBJ_V(FL_MAP, m);
            VM_NEXT();
        }
        VM_CASE(INDEX_GET) {
            FlValue i = *--vm->sp;
            FlValue c = *--vm->sp;

            if (c.t == (u8)FL_LIST) {
                FlList *l = (FlList *)c.as.o;

                if (i.t != (u8)FL_INT) {
                    fl_raise(vm, "type", "list index must be an int");
                    goto raised;
                }
                if (i.as.i < 0 || (u32)i.as.i >= l->n) {
                    fl_raise(vm, "index", "index %lld out of range (len %u)",
                             (long long)i.as.i, (unsigned)l->n);
                    goto raised;
                }
                *vm->sp++ = l->v[i.as.i];
            } else if (c.t == (u8)FL_MAP) {
                FlValue got;

                if (!fl_map_get((FlMap *)c.as.o, i, &got)) {
                    fl_raise(vm, "key", "no such key");
                    goto raised;
                }
                *vm->sp++ = got;
            } else {
                fl_raise(vm, "type", "cannot index %s",
                         fl_type_name((FlType)c.t));
                goto raised;
            }
            VM_NEXT();
        }
        VM_CASE(INDEX_SET) {
            FlValue v = *--vm->sp;
            FlValue i = *--vm->sp;
            FlValue c = *--vm->sp;

            if (c.t == (u8)FL_LIST && i.t == (u8)FL_INT) {
                FlList *l = (FlList *)c.as.o;

                if (i.as.i < 0 || (u32)i.as.i >= l->n) {
                    fl_raise(vm, "index", "index %lld out of range (len %u)",
                             (long long)i.as.i, (unsigned)l->n);
                    goto raised;
                }
                l->v[i.as.i] = v;       /* element assignment is not
                                         * structural: mods unchanged */
            } else if (c.t == (u8)FL_MAP) {
                if (!fl_hashable(i)) {
                    fl_raise(vm, "key", "%s is not a valid map key",
                             fl_type_name((FlType)i.t));
                    goto raised;
                }
                (void)fl_map_set(vm, (FlMap *)c.as.o, i, v);
            } else {
                fl_raise(vm, "type", "cannot assign into %s",
                         fl_type_name((FlType)c.t));
                goto raised;
            }
            VM_NEXT();
        }
        VM_CASE(FIELD_GET) {
            u16 k = read_u16(&ip);
            FlValue name = frame->cl->fn->ch.consts[k];
            FlValue c = *--vm->sp;
            FlValue got;
            const char *s = sag_intern_str(vm->in, (u32)name.as.i);
            FlValue key = make_str(vm, s == NULL ? "" : s);

            /* §4: `.name` IS `["name"]`, and modules read the same way. */
            if (c.t != (u8)FL_MAP || !fl_map_get((FlMap *)c.as.o, key, &got)) {
                fl_raise(vm, "key", "no field '%s' on %s",
                         s == NULL ? "?" : s, fl_type_name((FlType)c.t));
                goto raised;
            }
            *vm->sp++ = got;
            VM_NEXT();
        }
        VM_CASE(FIELD_SET) {
            u16 k = read_u16(&ip);
            FlValue name = frame->cl->fn->ch.consts[k];
            FlValue v = *--vm->sp;
            FlValue c = *--vm->sp;
            const char *s = sag_intern_str(vm->in, (u32)name.as.i);

            if (c.t != (u8)FL_MAP) {
                fl_raise(vm, "type", "cannot set a field on %s",
                         fl_type_name((FlType)c.t));
                goto raised;
            }
            if ((c.as.o->oflags & (u16)FL_OF_FROZEN) != 0U) {
                fl_raise(vm, "type", "object is frozen");
                goto raised;
            }
            (void)fl_map_set(vm, (FlMap *)c.as.o,
                             make_str(vm, s == NULL ? "" : s), v);
            VM_NEXT();
        }

        VM_CASE(ITER_NEW) {
            FlValue subject = vm->sp[-1];

            /* Three hidden slots: subject, cursor, and a snapshot of
             * the container's structural mods counter. */
            *vm->sp++ = FL_INT_V(0);
            *vm->sp++ = FL_INT_V(subject.t == (u8)FL_LIST
                                     ? (i64)((FlList *)subject.as.o)->mods
                                     : (subject.t == (u8)FL_MAP
                                            ? (i64)((FlMap *)subject.as.o)->mods
                                            : 0));
            VM_NEXT();
        }
        VM_CASE(ITER_NEXT1) {
            u8 base = *ip++;
            u16 d = read_u16(&ip);
            FlValue subject = frame->slots[base];
            i64 cursor = frame->slots[base + 1].as.i;
            i64 snapshot = frame->slots[base + 2].as.i;

            if (subject.t == (u8)FL_LIST) {
                FlList *l = (FlList *)subject.as.o;

                if ((i64)l->mods != snapshot) {
                    fl_raise(vm, "index", "list modified during iteration");
                    goto raised;
                }
                if (cursor >= (i64)l->n) {
                    ip += d;
                    VM_NEXT();
                }
                frame->slots[base + 1] = FL_INT_V(cursor + 1);
                *vm->sp++ = l->v[cursor];
            } else if (subject.t == (u8)FL_MAP) {
                FlMap *m = (FlMap *)subject.as.o;
                u32 cur = (u32)cursor;
                FlValue k;
                FlValue v;

                if ((i64)m->mods != snapshot) {
                    fl_raise(vm, "key", "map modified during iteration");
                    goto raised;
                }
                if (!fl_map_iter(m, &cur, &k, &v)) {
                    ip += d;
                    VM_NEXT();
                }
                frame->slots[base + 1] = FL_INT_V((i64)cur);
                *vm->sp++ = k;
            } else {
                fl_raise(vm, "type", "cannot iterate %s",
                         fl_type_name((FlType)subject.t));
                goto raised;
            }
            VM_NEXT();
        }
        VM_CASE(ITER_NEXT2) {
            u8 base = *ip++;
            u16 d = read_u16(&ip);
            FlValue subject = frame->slots[base];
            u32 cur = (u32)frame->slots[base + 1].as.i;
            i64 snapshot = frame->slots[base + 2].as.i;
            FlValue k;
            FlValue v;

            if (subject.t != (u8)FL_MAP) {
                fl_raise(vm, "type", "two-variable for needs a map");
                goto raised;
            }
            /*
             * The same guard ITER_NEXT1 carries.  Without it a
             * `for k, v in m` body could delete keys and the walk
             * would keep going over a reshaped entry array -- the
             * one-variable form refused and the two-variable form
             * did not, which is the sort of gap nobody finds until a
             * config does it.
             *
             * BEFORE the exhaustion check: a mutation that empties the
             * map must still report, not end the loop quietly.
             */
            if ((i64)((FlMap *)subject.as.o)->mods != snapshot) {
                fl_raise(vm, "key", "map modified during iteration");
                goto raised;
            }
            if (!fl_map_iter((FlMap *)subject.as.o, &cur, &k, &v)) {
                ip += d;
                VM_NEXT();
            }
            frame->slots[base + 1] = FL_INT_V((i64)cur);
            *vm->sp++ = k;
            *vm->sp++ = v;
            VM_NEXT();
        }

        VM_CASE(MOTION) {
            u16 k = read_u16(&ip);
            FlMotionProg *p =
                (FlMotionProg *)frame->cl->fn->ch.consts[k].as.o;
            FlErr e = {0U, FL_NIL_V};

            if (!vm->host->motion(vm->host->ud, p, &e)) {
                /* The null host lands here every time, which is spec
                 * §3.1 and what makes §14's shout return "MOTION". */
                fl_raise(vm, "motion", "no editor host");
                goto raised;
            }
            *vm->sp++ = FL_NIL_V;
            VM_NEXT();
        }
        VM_CASE(IMPORT) {
            u16 k = read_u16(&ip);
            FlValue name = frame->cl->fn->ch.consts[k];

            /* No silent stub: the opcode exists so the compiler can
             * emit it, and it says which sprint owes the behaviour. */
            fl_raise(vm, "import",
                     "import of '%s' is not implemented until Sprint 31",
                     sag_intern_str(vm->in, (u32)name.as.i));
            goto raised;
        }
        VM_CASE(EDIT_BEGIN) {
            FlErr e = {0U, FL_NIL_V};

            (void)vm->host->edit_begin(vm->host->ud, &e);
            VM_NEXT();
        }
        VM_CASE(EDIT_END) {
            FlErr e = {0U, FL_NIL_V};

            (void)vm->host->edit_end(vm->host->ud, true, &e);
            VM_NEXT();
        }

        VM_CASE(THROW) {
            vm->err = *--vm->sp;
            goto raised;
        }
        VM_CASE(TRY_PUSH) {
            u16 d = read_u16(&ip);
            FlHandler *h;

            if (vm->nhandlers >= (u32)FL_HANDLERS_MAX) {
                fl_raise(vm, "limit", "too many nested try blocks");
                goto raised;
            }
            h = &vm->handlers[vm->nhandlers++];
            h->pc = (u32)(ip - frame->cl->fn->ch.code) + d;
            h->sp = vm->sp;
            h->frame = vm->nframes;
            VM_NEXT();
        }
        VM_CASE(TRY_POP) {
            if (vm->nhandlers != 0U)
                vm->nhandlers--;
            VM_NEXT();
        }

        VM_CASE(HALT) {
            if (out != NULL)
                *out = FL_NIL_V;
            return true;
        }
        VM_CASE(LIST_APPEND) {
            u8 s = *ip++;
            FlValue v = *--vm->sp;

            (void)fl_list_push(vm, (FlList *)frame->slots[s].as.o, v);
            VM_NEXT();
        }
        VM_CASE(NOT_NIL) {
            vm->sp[-1] = FL_BOOL_V(vm->sp[-1].t != (u8)FL_NIL);
            VM_NEXT();
        }
        VM_CASE(TRACE_LINE) {
            (void)read_u16(&ip);
            VM_NEXT();
        }

#if FL_COMPUTED_GOTO
        L_BAD_OP:
            SAG_BUG("fl vm: opcode %u at pc %u", (unsigned)op,
                    (unsigned)(ip - frame->cl->fn->ch.code - 1));
#else
        default:
            SAG_BUG("fl vm: opcode %u at pc %u", (unsigned)op,
                    (unsigned)(ip - frame->cl->fn->ch.code - 1));
        }
#endif
        continue;

    raised:
        /*
         * UNWIND, in this order: close upvalues, truncate frames,
         * restore sp, push the error, jump.  Any other order either
         * leaves a closure pointing at a slot the handler is about to
         * overwrite, or restores sp from a frame that no longer exists.
         */
        if (vm->nhandlers == 0U ||
            vm->handlers[vm->nhandlers - 1U].frame <= base) {
            /* No handler of OURS.  Hand the raise back; if we are a
             * nested execution the native returns false and the outer
             * loop unwinds into its own handler. */
            if (out != NULL)
                *out = vm->err;
            return false;
        }
        {
            FlHandler *h = &vm->handlers[--vm->nhandlers];

            close_upvals(vm, h->sp);
            vm->nframes = h->frame;
            frame = &vm->frames[vm->nframes - 1U];
            vm->sp = h->sp;
            *vm->sp++ = vm->err;
            vm->err = FL_NIL_V;
            ip = frame->cl->fn->ch.code + h->pc;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Entry points                                                     */
/* ---------------------------------------------------------------- */

bool fl_vm_run(FlVm *vm, FlFn *entry, FlValue *out)
{
    FlClosure *cl = fl_gc_alloc(vm, sizeof(*cl), FL_CLOSURE);

    cl->fn = entry;
    cl->up = NULL;
    cl->nup = 0U;

    vm->sp = vm->stack;
    *vm->sp++ = FL_OBJ_V(FL_CLOSURE, cl);
    vm->frames[vm->nframes].cl = cl;
    vm->frames[vm->nframes].ip = entry->ch.code;
    vm->frames[vm->nframes].slots = vm->stack;
    vm->nframes++;
    return vm_exec(vm, 0U, out);
}

/*
 * Call a Fletch value from C -- list.map's callback, re.replace_fn's
 * replacement, and Sprint 34's editor hooks.
 *
 * The arguments are pushed onto the VM stack rather than kept in a C
 * array, so they are covered by root 1 for the whole call: a callback
 * that allocates would otherwise collect its own arguments out from
 * under itself, which is gc.h rule 1 with a callback in the middle.
 */
bool fl_call(FlVm *vm, FlValue callee, const FlValue *args, u32 nargs,
             FlValue *out)
{
    u32 base = vm->nframes;
    FlValue *slots;
    u32 i;

    if (callee.t == (u8)FL_NATIVE) {
        FlNative *nat = (FlNative *)callee.as.o;

        if (nargs < nat->min_ar ||
            (nat->max_ar != 255U && nargs > nat->max_ar))
            return fl_raise(vm, "arity", "%s got %u arguments",
                            sag_intern_str(vm->in, nat->name_id),
                            (unsigned)nargs);
        vm->cur_native = nat->name_id;
        /* argv must live on the stack for rule 1, same as a CALL. */
        slots = vm->sp;
        for (i = 0U; i < nargs; i++)
            *vm->sp++ = args[i];
        {
            bool ok = nat->fn(vm, slots, nargs, out);

            vm->sp = slots;
            return ok;
        }
    }
    if (callee.t != (u8)FL_CLOSURE)
        return fl_raise(vm, "type", "cannot call %s",
                        fl_type_name((FlType)callee.t));
    {
        FlClosure *cl = (FlClosure *)callee.as.o;

        if (nargs != cl->fn->arity)
            return fl_raise(vm, "arity", "expected %u arguments, got %u",
                            (unsigned)cl->fn->arity, (unsigned)nargs);
        if (vm->nframes >= (u32)FL_FRAMES_MAX)
            return fl_raise(vm, "limit", "call depth exceeded");
        slots = vm->sp;
        *vm->sp++ = callee;
        for (i = 0U; i < nargs; i++)
            *vm->sp++ = args[i];
        vm->frames[vm->nframes].cl = cl;
        vm->frames[vm->nframes].ip = cl->fn->ch.code;
        vm->frames[vm->nframes].slots = slots;
        vm->nframes++;
        if (!vm_exec(vm, base, out)) {
            vm->sp = slots;
            return false;
        }
        vm->sp = slots;
        return true;
    }
}
