/*
 * Sprint 20 §6a: the Pike VM.
 *
 * An NFA simulation, not a backtracker.  Two thread lists over a sparse
 * set of pc values give O(1) membership with no clearing between steps,
 * so each input codepoint costs O(m) rather than O(m log m).  There is no
 * recursion and no backtracking anywhere in this file; the word appears
 * only in comments explaining its absence.
 */
#include "search/regex_internal.h"

#include <string.h>

#include "unicode/utf8.h"
#include "util/log.h"

/* ---------------------------------------------------------------- */
/* Input cursor — reads through TextIter, never materializes (§1)    */
/* ---------------------------------------------------------------- */

typedef struct ReCursor {
    TextIter it;
    const u8 *p; /* current chunk                                     */
    u64 n;
    u64 chunk_at; /* byte offset of p[0]                              */
    bool has_chunk;
} ReCursor;

static void cursor_init(ReCursor *c)
{
    (void)memset(c, 0, sizeof(*c));
}

/* Returns the byte at `off`, refilling the chunk when needed.  The
 * TextIter is forward-only, so a backward step restarts iteration — the
 * VM only walks forward, and the anchored re-match restarts anyway. */
static bool cursor_byte(ReCursor *c, const YewReInput *in, u64 off, u8 *out)
{
    if (off >= in->window.hi)
        return false;
    if (in->tb == NULL) {
        if (off >= in->len)
            return false;
        *out = in->bytes[off];
        return true;
    }
    if (c->has_chunk && off >= c->chunk_at && off < c->chunk_at + c->n) {
        *out = c->p[off - c->chunk_at];
        return true;
    }
    if (!yew_textiter_begin(&c->it, in->tb, BYTEOFF(off)))
        return false;
    if (!yew_textiter_chunk(&c->it, in->tb, &c->p, (size_t *)&c->n) ||
        c->n == 0U)
        return false;
    c->chunk_at = off;
    c->has_chunk = true;
    *out = c->p[0];
    return true;
}

/* Decodes the codepoint starting at `off`.  Invalid bytes become
 * U+DC80-escapes so a search over a binary file neither crashes nor
 * mangles anything (§3). */
static u32 cursor_decode(ReCursor *c, const YewReInput *in, u64 off,
                         u32 *len_out)
{
    u8 buf[YEW_UTF8_MAX];
    size_t have = 0U;
    u32 cp = 0U;
    size_t used;

    while (have < YEW_UTF8_MAX) {
        u8 b;

        if (!cursor_byte(c, in, off + have, &b))
            break;
        buf[have++] = b;
    }
    if (have == 0U) {
        *len_out = 0U;
        return 0U;
    }
    used = yew_utf8_decode(buf, have, &cp);
    *len_out = (u32)(used == 0U ? 1U : used);
    return cp;
}

/* ---------------------------------------------------------------- */
/* Capture arrays — refcounted, copy-on-write                        */
/* ---------------------------------------------------------------- */

/*
 * Slots are sized to the pattern, not to YEW_RE_MAX_GROUPS.  A fixed
 * 64-slot array means every seed memsets 512 bytes and every
 * copy-on-write copies 512 bytes — and with a start thread seeded at
 * each input position that dominates the whole search.  A two-group
 * pattern needs 4 slots (32 bytes), which is 16x less traffic.
 */
typedef struct Caps {
    u32 refs;
    u64 *slot;
    struct Caps *next_free;
} Caps;

/*
 * Dead arrays go on a free list instead of being abandoned.  An
 * unanchored scan seeds a thread at every input position, so without
 * reuse the live set would grow with the INPUT (O(n*k)) rather than with
 * the thread count (O(m*k) as specified) — and a working set that leaves
 * cache at 100 KB shows up as super-linear scaling even though the
 * algorithm is linear.
 */
typedef struct CapPool {
    Arena *arena;
    Caps *free_list;
    u32 nslots;
    u64 live;
} CapPool;

static Caps *caps_new(CapPool *pool)
{
    Caps *c = pool->free_list;

    if (c != NULL) {
        pool->free_list = c->next_free;
    } else {
        c = arena_alloc(pool->arena, sizeof(*c), sizeof(void *));
        c->slot = arena_alloc(pool->arena,
                              (size_t)pool->nslots * sizeof(*c->slot),
                              sizeof(u64));
        pool->live++;
    }
    c->refs = 1U;
    c->next_free = NULL;
    (void)memset(c->slot, 0xFF, (size_t)pool->nslots * sizeof(*c->slot));
    return c;
}

static void caps_free(CapPool *pool, Caps *c)
{
    c->next_free = pool->free_list;
    pool->free_list = c;
}

static Caps *caps_share(Caps *c)
{
    if (c != NULL)
        c->refs++;
    return c;
}

static void caps_release(CapPool *pool, Caps *c)
{
    if (c == NULL || c->refs == 0U)
        return;
    c->refs--;
    if (c->refs == 0U)
        caps_free(pool, c);
}

/* Copy-on-write: a SAVE on a shared array copies once instead of every
 * thread copying at every step. */
static Caps *caps_set(CapPool *pool, Caps *c, u32 slot, u64 value)
{
    Caps *target = c;

    if (slot >= pool->nslots)
        return c;
    if (c == NULL)
        return NULL;
    if (c->refs > 1U) {
        Caps *copy = caps_new(pool);

        (void)memcpy(copy->slot, c->slot,
                     (size_t)pool->nslots * sizeof(*copy->slot));
        c->refs--;
        target = copy;
    }
    target->slot[slot] = value;
    return target;
}

/* ---------------------------------------------------------------- */
/* Thread lists — sparse set                                         */
/* ---------------------------------------------------------------- */

typedef struct ReThread {
    u32 pc;
    Caps *caps;
} ReThread;

typedef struct ReList {
    ReThread *dense;
    u32 *sparse;
    u32 n;
    u32 cap;
    u32 gen;
    u32 *stamp; /* per-pc generation; equal means present              */
} ReList;

static void list_init(ReList *l, Arena *a, u32 nprog)
{
    l->dense = arena_alloc(a, (size_t)nprog * sizeof(*l->dense),
                           sizeof(void *));
    l->sparse = arena_alloc(a, (size_t)nprog * sizeof(*l->sparse),
                            sizeof(u32));
    l->stamp = arena_alloc(a, (size_t)nprog * sizeof(*l->stamp),
                           sizeof(u32));
    (void)memset(l->stamp, 0, (size_t)nprog * sizeof(*l->stamp));
    l->cap = nprog;
    l->n = 0U;
    l->gen = 0U;
}

/* No clearing between steps: bumping the generation invalidates every
 * membership stamp at once. */
static void list_clear(ReList *l)
{
    l->n = 0U;
    l->gen++;
}

static bool list_has(const ReList *l, u32 pc)
{
    return pc < l->cap && l->stamp[pc] == l->gen + 1U;
}

static void list_mark(ReList *l, u32 pc)
{
    if (pc < l->cap)
        l->stamp[pc] = l->gen + 1U;
}

/* ---------------------------------------------------------------- */
/* Assertions                                                        */
/* ---------------------------------------------------------------- */

typedef struct ReCtx {
    const YewRe *re;
    const YewReInput *in;
    ReCursor *cur;
    u64 pos;
    u32 prev_cp;
    bool at_start; /* pos == window.lo                                 */
} ReCtx;

static bool is_word_cp(u32 cp)
{
    return yew_re_is_word(cp);
}

static bool assertion_holds(const ReCtx *x, ReOp op, u32 cp, bool have_cp)
{
    switch (op) {
    case RE_BOT:
        return x->pos == x->in->window.lo;
    case RE_EOT:
        return x->pos == x->in->window.hi;
    case RE_BOL:
        return x->pos == x->in->window.lo || x->prev_cp == (u32)'\n';
    case RE_EOL:
        /*
         * `$` matches at the window end, before an LF, and before the CR
         * of a CRLF pair — s08 keeps \r\n verbatim, so without the last
         * case every anchored search silently fails on a Windows file.
         */
        if (x->pos == x->in->window.hi)
            return true;
        if (!have_cp)
            return false;
        if (cp == (u32)'\n')
            return true;
        if (cp == (u32)'\r') {
            ReCursor probe = *x->cur;
            u32 len = 0U;
            u32 next = cursor_decode(&probe, x->in, x->pos + 1U, &len);

            return len != 0U && next == (u32)'\n';
        }
        return false;
    case RE_WORDB:
    case RE_NWORDB: {
        bool before = !x->at_start && is_word_cp(x->prev_cp);
        bool after = have_cp && is_word_cp(cp);
        bool boundary = before != after;

        return op == RE_WORDB ? boundary : !boundary;
    }
    default:
        break;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* addthread — explicit stack, never recursion                       */
/* ---------------------------------------------------------------- */

typedef struct AddFrame {
    u32 pc;
    Caps *caps;
} AddFrame;

typedef struct VmState {
    const YewRe *re;
    const ReInst *prog;
    u32 nprog;
    CapPool pool;
    AddFrame *stack;
    u32 stack_cap;
} VmState;

static void addthread(VmState *vm, ReList *l, u32 pc, Caps *caps,
                      const ReCtx *x, u32 cp, bool have_cp)
{
    u32 top = 0U;

    vm->stack[top].pc = pc;
    vm->stack[top].caps = caps;
    top = 1U;
    while (top != 0U) {
        const ReInst *ins;

        top--;
        pc = vm->stack[top].pc;
        caps = vm->stack[top].caps;
        /*
         * THE empty-width fix.  This check must live INSIDE the loop, not
         * merely at the call site: `(a*)*` chains SPLIT/JMP without
         * consuming input, and re-adding the same pc would spin forever
         * on a pattern the user typed.  Moving this check out makes the
         * dedicated unit test hang.
         */
        if (pc >= vm->nprog || list_has(l, pc)) {
            caps_release(&vm->pool, caps);
            continue;
        }
        list_mark(l, pc);
        ins = &vm->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_JMP:
            if (top + 1U > vm->stack_cap)
                YEW_BUG("regex: addthread stack overflow");
            vm->stack[top].pc = ins->x;
            vm->stack[top].caps = caps;
            top++;
            break;
        case RE_SPLIT:
            if (top + 2U > vm->stack_cap)
                YEW_BUG("regex: addthread stack overflow");
            /* Lower priority pushed first so the higher-priority branch
             * pops first — that ordering IS leftmost-first. */
            vm->stack[top].pc = ins->y;
            vm->stack[top].caps = caps_share(caps);
            top++;
            vm->stack[top].pc = ins->x;
            vm->stack[top].caps = caps;
            top++;
            break;
        case RE_SAVE:
            caps = caps_set(&vm->pool, caps, ins->arg, x->pos);
            if (top + 1U > vm->stack_cap)
                YEW_BUG("regex: addthread stack overflow");
            vm->stack[top].pc = pc + 1U;
            vm->stack[top].caps = caps;
            top++;
            break;
        case RE_BOL: case RE_EOL: case RE_BOT: case RE_EOT:
        case RE_WORDB: case RE_NWORDB:
            if (assertion_holds(x, (ReOp)ins->op, cp, have_cp)) {
                if (top + 1U > vm->stack_cap)
                    YEW_BUG("regex: addthread stack overflow");
                vm->stack[top].pc = pc + 1U;
                vm->stack[top].caps = caps;
                top++;
            } else {
                caps_release(&vm->pool, caps);
            }
            break;
        default:
            /* A consuming op or MATCH: it stays in the list. */
            if (l->n < l->cap) {
                l->dense[l->n].pc = pc;
                l->dense[l->n].caps = caps;
                l->n++;
            } else {
                caps_release(&vm->pool, caps);
            }
            break;
        }
    }
}

static bool inst_matches(const YewRe *re, const ReInst *ins, u32 cp)
{
    switch ((ReOp)ins->op) {
    case RE_CHAR:
        return cp == ins->arg;
    case RE_CLASS:
        return ins->arg < re->nclasses &&
               yew_re_class_has(&re->classes[ins->arg], cp);
    case RE_ANY:
        return ins->arg != 0U || cp != (u32)'\n';
    default:
        break;
    }
    return false;
}

/*
 * Runs the VM anchored at `start`.  Returns true on a match and fills
 * `out`.  Anchored means the match must begin exactly at `start`; the
 * unanchored search loop lives in search.c and calls this per candidate.
 */
/*
 * `anchored` distinguishes the two ways to run the VM, and the
 * difference is the whole linear-time story:
 *
 *   anchored   — the match must begin exactly at `start`.
 *   unanchored — a fresh thread is seeded at EVERY position during the
 *                one pass, until a match is recorded.
 *
 * Restarting an anchored run at each candidate position instead would
 * make an unanchored search O(n^2) — and on `a(a*)*b` over 100 000 a's
 * that is not a slow search, it is a hang.  Seeding inside the pass
 * keeps the whole scan O(n*m).  Later seeds are appended after the
 * threads already in the list, so earlier start positions keep their
 * priority and leftmost semantics hold.
 */
bool yew_re_pike_run_ex(const YewRe *re, const YewReInput *in, u64 start,
                        bool anchored, YewReMatch *out);

bool yew_re_pike_run(const YewRe *re, const YewReInput *in, u64 start,
                     YewReMatch *out)
{
    return yew_re_pike_run_ex(re, in, start, true, out);
}

bool yew_re_pike_run_ex(const YewRe *re, const YewReInput *in, u64 start,
                        bool anchored, YewReMatch *out)
{
    Arena arena;
    VmState vm;
    ReList clist;
    ReList nlist;
    ReCursor cur;
    ReCtx x;
    Caps *best = NULL;
    u64 pos = start;
    u32 prev_cp = 0U;
    bool matched = false;
    u32 i;

    if (re == NULL || in == NULL || re->nprog == 0U)
        return false;
    arena_init(&arena);
    (void)memset(&vm, 0, sizeof(vm));
    vm.re = re;
    vm.prog = re->prog;
    vm.nprog = re->nprog;
    vm.pool.arena = &arena;
    /* Only the slots this pattern can actually write. */
    vm.pool.nslots = re->ngroups * 2U;
    if (vm.pool.nslots > YEW_RE_MAX_GROUPS * 2U)
        vm.pool.nslots = YEW_RE_MAX_GROUPS * 2U;
    /* Each instruction can push at most two frames. */
    vm.stack_cap = re->nprog * 2U + 8U;
    vm.stack = arena_alloc(&arena,
                           (size_t)vm.stack_cap * sizeof(*vm.stack),
                           sizeof(void *));
    list_init(&clist, &arena, re->nprog);
    list_init(&nlist, &arena, re->nprog);
    cursor_init(&cur);

    /* The codepoint preceding `start`, needed by \b and ^ without a
     * second pass.  Scanning back a few bytes is bounded by YEW_UTF8_MAX. */
    if (start > in->window.lo) {
        u64 back = start - in->window.lo > YEW_UTF8_MAX ?
                   (u64)YEW_UTF8_MAX : start - in->window.lo;
        u64 probe = start - back;

        while (probe < start) {
            u32 len = 0U;
            u32 cp = cursor_decode(&cur, in, probe, &len);

            if (len == 0U)
                break;
            if (probe + len >= start) {
                prev_cp = cp;
                break;
            }
            probe += len;
        }
    }

    list_clear(&clist);
    list_clear(&nlist);
    for (;;) {
        u32 cp = 0U;
        u32 cp_len = 0U;
        bool have_cp;

        cp = cursor_decode(&cur, in, pos, &cp_len);
        have_cp = cp_len != 0U && pos < in->window.hi;

        x.re = re;
        x.in = in;
        x.cur = &cur;
        x.pos = pos;
        x.prev_cp = prev_cp;
        x.at_start = pos == in->window.lo;

        /*
         * Seed a new start thread here.  Appending after the existing
         * threads is what makes this leftmost: a thread that began at an
         * earlier position was added earlier and therefore outranks it.
         * Once a match is recorded we stop seeding — a later start can
         * never beat one already found.
         */
        if (!matched && (pos == start || !anchored)) {
            Caps *seed = caps_new(&vm.pool);

            addthread(&vm, &clist, 0U, seed, &x, cp, have_cp);
        }
        if (clist.n == 0U) {
            if (anchored || matched || !have_cp)
                break;
            /* No live threads, but an unanchored scan keeps walking. */
            prev_cp = cp;
            pos += cp_len;
            list_clear(&clist);
            continue;
        }

        list_clear(&nlist);
        /*
         * Successors of a consuming instruction live at the NEXT input
         * position, so they must be added with the next position's
         * context — a SAVE reached there records where the match ends.
         * Adding them with the current context records the offset before
         * the final codepoint was consumed, i.e. every match comes out
         * one codepoint short.
         */
        {
            ReCtx xn;
            u32 next_cp = 0U;
            u32 next_len = 0U;
            bool have_next = false;
            u64 next_pos = pos + (have_cp ? cp_len : 0U);

            if (have_cp) {
                next_cp = cursor_decode(&cur, in, next_pos, &next_len);
                have_next = next_len != 0U && next_pos < in->window.hi;
            }
            xn = x;
            xn.pos = next_pos;
            xn.prev_cp = have_cp ? cp : prev_cp;
            xn.at_start = next_pos == in->window.lo;

            for (i = 0U; i < clist.n; i++) {
                u32 pc = clist.dense[i].pc;
                Caps *caps = clist.dense[i].caps;
                const ReInst *ins = &re->prog[pc];

                if ((ReOp)ins->op == RE_MATCH) {
                    caps_release(&vm.pool, best);
                    best = caps;
                    matched = true;
                    /*
                     * Priority cut: threads after this one in clist are
                     * all lower priority, so none can produce a better
                     * leftmost-first answer.  Higher-priority threads
                     * already in nlist keep running.
                     */
                    for (i++; i < clist.n; i++)
                        caps_release(&vm.pool, clist.dense[i].caps);
                    break;
                }
                if (have_cp && inst_matches(re, ins, cp))
                    addthread(&vm, &nlist, pc + 1U, caps, &xn, next_cp,
                              have_next);
                else
                    caps_release(&vm.pool, caps);
            }
        }
        if (!have_cp)
            break;
        /* Swap lists. */
        {
            ReList tmp = clist;

            clist = nlist;
            nlist = tmp;
        }
        prev_cp = cp;
        pos += cp_len;
        if (clist.n == 0U && (anchored || matched))
            break;
    }

    if (matched && out != NULL) {
        u32 g;

        (void)memset(out, 0, sizeof(*out));
        out->ngroups = re->ngroups;
        for (g = 0U; g < re->ngroups && g < YEW_RE_MAX_GROUPS; g++) {
            u64 lo = best->slot[g * 2U];
            u64 hi = best->slot[g * 2U + 1U];

            if (lo == UINT64_MAX || hi == UINT64_MAX) {
                out->g[g].lo = UINT64_MAX;
                out->g[g].hi = UINT64_MAX;
                continue;
            }
            out->g[g].lo = lo;
            out->g[g].hi = hi;
        }
    }
    arena_free_all(&arena);
    return matched;
}
