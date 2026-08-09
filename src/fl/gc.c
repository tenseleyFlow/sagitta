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
    /* Strings overwrite this with their hash in fl_str_new; for
     * everything else it is the insertion sequence number. */
    o->aux = ++vm->gc.next_seq;
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
        /* The module globals the closure reads.  A module's map is
         * reachable ONLY through the closures that were made inside it
         * once the load finishes, so missing this line collects a live
         * module's globals the first time the collector runs after an
         * import. */
        mark_obj(vm, (FlObj *)c->globals);
        for (i = 0U; i < c->nup; i++)
            mark_obj(vm, (FlObj *)c->up[i]);
        return;
    }
    case FL_UPVAL: {
        const FlUpval *uv = (const FlUpval *)o;

        /* A CLOSED upvalue owns its value; an open one aliases a stack
         * slot that root 1 already covers.  Marking both is harmless
         * and marking neither loses the closed one. */
        mark_value(vm, uv->closed);
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
    /* 5b. the builtin module maps (root 9).  Numbered after the §9
     *     table's eight because it was added by s31; the table in
     *     gc.h lists it. */
    mark_obj(vm, (FlObj *)vm->builtins);
    /* 5c. the prelude (root 10), added by s33 with §9's `error`. */
    mark_obj(vm, (FlObj *)vm->prelude);
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
    /* FL_MOTION_PROG's op array is ARENA memory, not heap: the
     * compiler builds it alongside the chunk it belongs to and it
     * dies with that arena.  Freeing it here handed an arena
     * pointer to free(). */
    default: break;
    }
    if (vm->gc.stress) {
        /* Poison before free so a use-after-free reads 0xDD rather
         * than whatever the allocator recycled into the block. */
        (void)memset(o, FL_GC_POISON, sizeof(*o));
    }
    free(o);
}

/*
 * Drop weak entries whose string did not survive the mark, then
 * REBUILD the table.
 *
 * Simply NULLing a slot is wrong in an open-addressed table: the hole
 * truncates every probe chain that ran through it, so a later lookup
 * walks off the end and reports "absent" for a string that is still
 * present.  The visible effect is mild -- a duplicate interned string,
 * since fl_str_eq compares content -- but it quietly breaks the
 * one-object-per-content property the pointer fast path is named for.
 * Rehashing the survivors costs one pass per collection.
 */
static void strtab_clear_dead(FlStrTab *t)
{
    FlStr **survivors;
    u32 live = 0U;
    u32 i;

    if (t->cap == 0U)
        return;
    survivors = calloc((size_t)t->cap, sizeof(*survivors));
    if (survivors == NULL)
        SAG_BUG("fletch gc: out of memory rebuilding the string table");
    for (i = 0U; i < t->cap; i++) {
        if (t->v[i] != NULL && t->v[i]->h.mark != 0U)
            survivors[live++] = t->v[i];
    }
    (void)memset(t->v, 0, (size_t)t->cap * sizeof(*t->v));
    for (i = 0U; i < live; i++) {
        FlStr *sv = survivors[i];
        u32 at = sv->h.aux & (t->cap - 1U);

        while (t->v[at] != NULL)
            at = (at + 1U) & (t->cap - 1U);
        t->v[at] = sv;
    }
    t->n = live;
    free(survivors);
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
    vm->gc.collections++;
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

/* ---------------------------------------------------------------- */
/* Strings: interning and heap escape (deliverable 10)              */
/* ---------------------------------------------------------------- */

/*
 * The intern table is a weak, open-addressed set of FlStr*.  Weak means
 * an entry never keeps its string alive -- sweep drops dead ones -- so
 * a program that builds a million distinct short strings pays for them
 * once and gets the table back when they die.
 */
static void strtab_grow(FlStrTab *t)
{
    u32 cap = t->cap == 0U ? 256U : t->cap * 2U;
    FlStr **v = calloc((size_t)cap, sizeof(*v));
    u32 i;

    if (v == NULL)
        SAG_BUG("fletch: out of memory growing the string table");
    for (i = 0U; i < t->cap; i++) {
        FlStr *s = t->v[i];
        u32 at;

        if (s == NULL)
            continue;
        at = s->h.aux & (cap - 1U);
        while (v[at] != NULL)
            at = (at + 1U) & (cap - 1U);
        v[at] = s;
    }
    free(t->v);
    t->v = v;
    t->cap = cap;
}

static FlStr *strtab_find(const FlStrTab *t, const char *b, u32 n, u32 hash)
{
    u32 at;

    if (t->cap == 0U)
        return NULL;
    at = hash & (t->cap - 1U);
    for (;;) {
        FlStr *s = t->v[at];

        if (s == NULL)
            return NULL;
        /*
         * The length check comes FIRST and short-circuits the compare.
         *
         * memcmp with a null pointer is undefined even for a length of
         * zero -- the standard attaches nonnull to both arguments -- so
         * `fl_str_new(vm, NULL, 0)`, which is a perfectly natural way
         * to ask for the empty string, tripped UBSan here the first
         * time the stdlib fuzzer handed it a zero-length input.
         */
        if (s->len == n && s->h.aux == hash &&
            (n == 0U || memcmp(s->b, b, n) == 0))
            return s;
        at = (at + 1U) & (t->cap - 1U);
    }
}

static void strtab_put(FlStrTab *t, FlStr *s)
{
    u32 at;

    /* Grown at half load: the table holds weak entries that sweep turns
     * into holes, and linear probing degrades badly once holes and live
     * entries together fill it. */
    if (t->cap == 0U || (t->n + 1U) * 2U > t->cap)
        strtab_grow(t);
    at = s->h.aux & (t->cap - 1U);
    while (t->v[at] != NULL)
        at = (at + 1U) & (t->cap - 1U);
    t->v[at] = s;
    t->n++;
}

FlStr *fl_str_new(FlVm *vm, const char *b, u32 n)
{
    u32 hash = fl_hash_bytes(b, n);
    FlStr *s;

    if (n <= (u32)FL_INTERN_MAX) {
        s = strtab_find(&vm->gc.strings, b, n, hash);
        if (s != NULL)
            return s;
    }
    s = fl_gc_alloc(vm, sizeof(*s) + (size_t)n + 1U, FL_STR);
    s->h.aux = hash;              /* eager: map keys need it anyway */
    s->len = n;
    /*
     * Bytes VERBATIM.  Sprint 2's escape policy cleans buffer text;
     * program and runtime strings are bytes, U+DC80..DCFF escapes
     * included, and nothing here validates or normalizes them.
     */
    if (n != 0U)
        (void)memcpy(s->b, b, n);
    s->b[n] = '\0';
    if (n <= (u32)FL_INTERN_MAX) {
        s->h.oflags |= (u16)FL_OF_INTERNED;
        strtab_put(&vm->gc.strings, s);
    }
    return s;
}

FlStr *fl_str_take(FlVm *vm, Bytebuf *bb)
{
    FlStr *s = fl_str_new(vm, (const char *)bb->data, (u32)bb->len);

    bb->len = 0U;
    return s;
}

/* ---------------------------------------------------------------- */
/* Containers                                                       */
/* ---------------------------------------------------------------- */

FlList *fl_list_new(FlVm *vm)
{
    return fl_gc_alloc(vm, sizeof(FlList), FL_LIST);
}

bool fl_list_push(FlVm *vm, FlList *l, FlValue v)
{
    if (l->n == l->cap) {
        u32 cap = l->cap == 0U ? 8U : l->cap * 2U;
        FlValue *grown = realloc(l->v, (size_t)cap * sizeof(*grown));

        if (grown == NULL)
            SAG_BUG("fletch: out of memory growing a list");
        l->v = grown;
        l->cap = cap;
        vm->gc.bytes += (size_t)(cap - l->n) * sizeof(*grown);
    }
    l->v[l->n++] = v;
    l->mods++;
    return true;
}

FlUpval **fl_gc_upvals(FlVm *vm, u32 n)
{
    FlUpval **up = calloc((size_t)n, sizeof(*up));

    if (up == NULL)
        SAG_BUG("fletch: out of memory allocating upvalues");
    vm->gc.bytes += (size_t)n * sizeof(*up);
    return up;
}

FlMap *fl_map_new(FlVm *vm)
{
    return fl_gc_alloc(vm, sizeof(FlMap), FL_MAP);
}

static void map_grow_index(FlVm *vm, FlMap *m)
{
    u32 cap = m->icap == 0U ? 8U : m->icap * 2U;
    u32 *idx = calloc((size_t)cap, sizeof(*idx));

    if (idx == NULL)
        SAG_BUG("fletch: out of memory growing a map index");
    free(m->idx);
    m->idx = idx;
    m->icap = cap;
    vm->gc.bytes += (size_t)cap * sizeof(*idx);
    fl_map_reindex(m);
}

bool fl_map_set(FlVm *vm, FlMap *m, FlValue k, FlValue v)
{
    u32 hash;
    bool found = false;
    u32 at;

    if (!fl_hashable(k))
        return false;              /* caller raises kind "key" */
    hash = fl_hash_value(k);
    if (m->icap != 0U) {
        at = fl_map_probe(m, k, hash, &found);
        if (found) {
            m->ent[m->idx[at] - 1U].v = v;   /* overwrite in place: the
                                              * key kept its position */
            return true;
        }
    }
    /* Grow at 70% load, capacity a power of two, probing linear. */
    if (m->icap == 0U || (m->n + 1U) * 10U > m->icap * 7U)
        map_grow_index(vm, m);
    if (m->n == m->cap) {
        u32 cap = m->cap == 0U ? 8U : m->cap * 2U;
        FlMapEnt *grown = realloc(m->ent, (size_t)cap * sizeof(*grown));

        if (grown == NULL)
            SAG_BUG("fletch: out of memory growing a map");
        m->ent = grown;
        m->cap = cap;
        vm->gc.bytes += (size_t)cap * sizeof(*grown);
    }
    /*
     * APPEND.  A key that was deleted and is being re-inserted lands at
     * the end rather than reviving its old row -- the alternative makes
     * iteration order depend on whether a key was ever deleted, which
     * is invisible in the source and untestable from it.
     */
    m->ent[m->n].k = k;
    m->ent[m->n].v = v;
    m->ent[m->n].hash = hash;
    m->ent[m->n].dead = false;
    m->n++;
    m->mods++;
    at = hash & (m->icap - 1U);
    while (m->idx[at] != 0U)
        at = (at + 1U) & (m->icap - 1U);
    m->idx[at] = m->n;             /* entry index + 1 */
    return true;
}
