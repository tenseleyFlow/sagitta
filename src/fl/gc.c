/*
 * Sprint 30 deliverable 9: the mark-sweep collector.
 *
 * THE ONLY FILE IN src/fl/ THAT TOUCHES THE ALLOCATOR (DoD 8).  The
 * collector owns the heap; everything else takes memory from the arena
 * or from here.
 */
#include "fl/gc.h"

#include <stdlib.h>
#include <string.h>

#include "fl/vm.h"
#include "util/log.h"

void fl_gc_init(FlGc *gc)
{
    (void)memset(gc, 0, sizeof(*gc));
    gc->next_gc = (size_t)FL_GC_FIRST;
}

/* ---------------------------------------------------------------- */
/* Allocation                                                       */
/* ---------------------------------------------------------------- */

static void *xalloc(size_t n)
{
    void *p = calloc(1U, n == 0U ? 1U : n);

    if (p == NULL)
        SAG_BUG("fletch: out of memory");
    return p;
}

void *fl_gc_alloc(FlVm *vm, size_t n, FlType t)
{
    FlObj *o = xalloc(n);

    o->t = (u8)t;
    o->mark = 0U;
    o->oflags = 0U;
    o->aux = 0U;
    o->gc_next = vm->gc.objects;
    vm->gc.objects = o;
    vm->gc.bytes += n;
    /*
     * `pending`, not a collection here.
     *
     * The caller is mid-instruction and is holding object pointers in C
     * locals that no root covers -- that is precisely the situation
     * gc.h rule 1 describes.  The dispatch loop tests this at the top
     * of the next instruction, where the stack is the whole truth.
     */
    if (vm->gc.stress || vm->gc.bytes > vm->gc.next_gc)
        vm->gc.pending = true;
    return o;
}

/* ---------------------------------------------------------------- */
/* Temp roots (gc.h rule 2)                                          */
/* ---------------------------------------------------------------- */

void fl_gc_protect(FlVm *vm, FlValue v)
{
    if (vm->ntemp >= (u32)FL_TEMP_MAX)
        SAG_BUG("fletch gc: temp-root stack overflow (protection leak)");
    vm->temp[vm->ntemp++] = v;
}

void fl_gc_release(FlVm *vm, u32 n)
{
    if (n > vm->ntemp)
        SAG_BUG("fletch gc: released more temp roots than were protected");
    vm->ntemp -= n;
}

/* ---------------------------------------------------------------- */
/* Mark                                                             */
/* ---------------------------------------------------------------- */

static void gray_push(FlVm *vm, FlObj *o)
{
    if (vm->gc.ngray == vm->gc.graycap) {
        u32 cap = vm->gc.graycap == 0U ? 64U : vm->gc.graycap * 2U;
        FlObj **grown = realloc(vm->gc.gray, (size_t)cap * sizeof(*grown));

        if (grown == NULL)
            SAG_BUG("fletch gc: out of memory growing the gray stack");
        vm->gc.gray = grown;
        vm->gc.graycap = cap;
    }
    vm->gc.gray[vm->gc.ngray++] = o;
}

static void mark_obj(FlVm *vm, FlObj *o)
{
    if (o == NULL || o->mark != 0U)
        return;
    o->mark = 1U;
    gray_push(vm, o);
}

static void mark_value(FlVm *vm, FlValue v)
{
    if (fl_is_obj(v))
        mark_obj(vm, v.as.o);
}

static void mark_chunk(FlVm *vm, const FlChunk *ch)
{
    u32 i;

    for (i = 0U; i < ch->nconsts; i++)
        mark_value(vm, ch->consts[i]);
}

/* Trace one gray object's children black. */
static void blacken(FlVm *vm, FlObj *o)
{
    switch ((FlType)o->t) {
    case FL_STR:
        return;                       /* no children */
    case FL_LIST: {
        FlList *l = (FlList *)o;
        u32 i;

        for (i = 0U; i < l->n; i++)
            mark_value(vm, l->v[i]);
        return;
    }
    case FL_MAP: {
        FlMap *m = (FlMap *)o;
        u32 i;

        /*
         * Dead rows are marked too.  Their key and value are still in
         * the dense array until a compaction removes them, and a sweep
         * that freed them would leave the array pointing at poison for
         * any iteration that runs before that compaction.
         */
        for (i = 0U; i < m->n; i++) {
            mark_value(vm, m->ent[i].k);
            mark_value(vm, m->ent[i].v);
        }
        return;
    }
    case FL_FN:
        mark_chunk(vm, &((FlFn *)o)->ch);
        return;
    case FL_CLOSURE: {
        FlClosure *c = (FlClosure *)o;
        u32 i;

        mark_obj(vm, (FlObj *)c->fn);
        for (i = 0U; i < c->nup; i++)
            mark_obj(vm, (FlObj *)c->up[i]);
        return;
    }
    case FL_NATIVE:
    case FL_MOTION_PROG:
        return;
    default:
        return;
    }
}

/*
 * THE ROOT SET, in the order the sprint enumerates it.  Adding a root
 * is a review event; roots 5, 6 and 8 are here before their consumers
 * exist on purpose.
 */
static void mark_roots(FlVm *vm)
{
    FlValue *slot;
    u32 i;
    FlUpval *uv;

    /* 1. the value stack */
    for (slot = vm->stack; slot < vm->sp; slot++)
        mark_value(vm, *slot);
    /* 2. call frames -- each closure reaches its fn and the chunk's
     *    constants through blacken(). */
    for (i = 0U; i < vm->nframes; i++)
        mark_obj(vm, (FlObj *)vm->frames[i].cl);
    /* 3. open upvalues.  An open one aliases a stack slot already
     *    covered by root 1, but the FlUpval OBJECT itself is only
     *    reachable from here and from its closure. */
    for (uv = vm->open_upvals; uv != NULL; uv = uv->next)
        mark_obj(vm, (FlObj *)uv);
    /* 4. globals */
    mark_obj(vm, (FlObj *)vm->globals);
    /* 5. modules -- Sprint 31 fills this; rooted now */
    mark_obj(vm, (FlObj *)vm->modules);
    /* 6. handles -- Sprint 34 fills this; rooted now */
    for (i = 0U; i < vm->handles.n; i++)
        mark_value(vm, vm->handles.v[i]);
    /* 7. the temp-root protection stack */
    for (i = 0U; i < vm->ntemp; i++)
        mark_value(vm, vm->temp[i]);
    /* 8. the compiler's in-progress functions.  Compilation allocates,
     *    so a collection can land in the middle of building one. */
    for (i = 0U; i < vm->ncompiling; i++)
        mark_obj(vm, (FlObj *)vm->compiling[i]);
    /* The in-flight raised value is not a numbered root because it is
     * not a container the program can reach -- but dropping it would
     * lose the error a handler is about to catch. */
    mark_value(vm, vm->err);
}

/* ---------------------------------------------------------------- */
/* Sweep                                                            */
/* ---------------------------------------------------------------- */

static void obj_free(FlVm *vm, FlObj *o)
{
    switch ((FlType)o->t) {
    case FL_LIST: free(((FlList *)o)->v); break;
    case FL_MAP:
        free(((FlMap *)o)->ent);
        free(((FlMap *)o)->idx);
        break;
    case FL_CLOSURE: free(((FlClosure *)o)->up); break;
    case FL_MOTION_PROG: free(((FlMotionProg *)o)->op); break;
    default: break;
    }
    if (vm->gc.stress) {
        /* Poison before free so a use-after-free reads 0xDD rather
         * than whatever the allocator recycled into the block. */
        (void)memset(o, FL_GC_POISON, sizeof(*o));
    }
    free(o);
}

/* Drop weak entries whose string did not survive the mark. */
static void strtab_clear_dead(FlStrTab *t)
{
    u32 i;

    for (i = 0U; i < t->cap; i++) {
        if (t->v[i] != NULL && t->v[i]->h.mark == 0U)
            t->v[i] = NULL;
    }
}

static void sweep(FlVm *vm)
{
    FlObj **link = &vm->gc.objects;
    FlObj *o = vm->gc.objects;

    /*
     * WEAK REFERENCES FIRST.
     *
     * The interned-string table does not keep its entries alive, so
     * every dead entry has to be cleared BEFORE any white object is
     * freed.  Sweeping first leaves the table pointing into freed --
     * and under stress, poisoned -- memory for the rest of the sweep,
     * and the next intern reads it.  The sprint calls this the most
     * commonly botched line in a mark-sweep collector, and the order
     * here is the whole fix.
     */
    strtab_clear_dead(&vm->gc.strings);

    while (o != NULL) {
        FlObj *next = o->gc_next;

        if (o->mark != 0U) {
            o->mark = 0U;             /* white again for the next cycle */
            link = &o->gc_next;
        } else {
            *link = next;
            obj_free(vm, o);
        }
        o = next;
    }
}

void fl_gc_collect(FlVm *vm)
{
    vm->gc.pending = false;
    vm->gc.ngray = 0U;
    mark_roots(vm);
    while (vm->gc.ngray != 0U)
        blacken(vm, vm->gc.gray[--vm->gc.ngray]);
    sweep(vm);
    /*
     * Recomputed from what survived rather than decremented during the
     * sweep: obj_free does not know each object's original size, and a
     * running total that drifts would either collect constantly or
     * never again.
     */
    {
        size_t live = 0U;
        const FlObj *o;

        for (o = vm->gc.objects; o != NULL; o = o->gc_next)
            live += sizeof(FlObj);
        vm->gc.bytes = live;
    }
    vm->gc.next_gc = vm->gc.bytes * 2U;
    if (vm->gc.next_gc < (size_t)FL_GC_FIRST)
        vm->gc.next_gc = (size_t)FL_GC_FIRST;
}

void fl_gc_free_all(FlVm *vm)
{
    FlObj *o = vm->gc.objects;

    while (o != NULL) {
        FlObj *next = o->gc_next;

        obj_free(vm, o);
        o = next;
    }
    vm->gc.objects = NULL;
    free(vm->gc.gray);
    vm->gc.gray = NULL;
    vm->gc.ngray = 0U;
    vm->gc.graycap = 0U;
    free(vm->gc.strings.v);
    vm->gc.strings.v = NULL;
    vm->gc.strings.cap = 0U;
    vm->gc.strings.n = 0U;
    vm->gc.bytes = 0U;
}
