/*
 * Sprint 20 §6b: the lazy DFA.
 *
 * A DFA state is a SET of NFA program counters plus the assertion
 * context it was built under.  States are built on demand and cached, so
 * a scan over plain text stops doing epsilon-closure work almost
 * immediately and becomes a table walk.
 *
 * It answers one question — "is there a match" — and nothing else.  That
 * is deliberate.  A DFA cannot report capture groups (a set of states
 * knows nothing about which path reached it), and making it report the
 * match SPAN requires the reverse program and a bounded backward scan,
 * which is its own piece of work.  Keeping the DFA to a boolean means
 * there is no way for it to disagree with the Pike VM about anything
 * subtle: either both say "match" or the test suite fails.
 *
 * Cache policy: on overflow the whole cache is flushed and the scan
 * continues.  A pattern that thrashes runs SLOWER on a DFA rebuilding
 * every state per character than on the VM, so after a second flush the
 * caller is told to give up and use the VM instead.
 */
#include "search/regex_internal.h"

#include <string.h>

#include "unicode/utf8.h"
#include "util/log.h"
#include "util/sort.h"

enum {
    SAG_DFA_MAX_STATES = 1024,
    SAG_DFA_MAX_BYTES = 1024U * 1024U,
    SAG_DFA_BUCKETS = 2048,
    /* Two flushes in one search means the working set does not fit; a
     * third would just be more rebuilding. */
    SAG_DFA_MAX_FLUSHES = 2
};

/* Assertion context, folded into the state key: two states with the same
 * pc set but different surroundings can step differently, so they are
 * different states. */
enum {
    DFA_AT_BOL = 1U << 0,
    DFA_AT_EOL = 1U << 1,
    DFA_AFTER_WORD = 1U << 2,
    DFA_BEFORE_WORD = 1U << 3,
    DFA_AT_BOT = 1U << 4,
    DFA_AT_EOT = 1U << 5
};

typedef struct DfaState {
    u32 *pcs;
    u32 npcs;
    u32 hash;
    u8 ctx;
    bool matched;
    i32 next; /* chain within the bucket, -1 ends it */
} DfaState;

typedef struct Dfa {
    const SagRe *re;
    const ReInst *prog;
    u32 nprog;
    Arena arena;
    DfaState *states;
    u32 nstates;
    i32 buckets[SAG_DFA_BUCKETS];
    u32 flushes;
    u64 bytes;
    /* Scratch reused by every closure so a step allocates nothing. */
    u32 *work;
    u32 *stamp;
    u32 gen;
    u32 *stack;
} Dfa;

static bool ctx_holds(u8 ctx, ReOp op)
{
    switch (op) {
    case RE_BOL:
        return (ctx & DFA_AT_BOL) != 0U;
    case RE_EOL:
        return (ctx & DFA_AT_EOL) != 0U;
    case RE_BOT:
        return (ctx & DFA_AT_BOT) != 0U;
    case RE_EOT:
        return (ctx & DFA_AT_EOT) != 0U;
    case RE_WORDB:
        return ((ctx & DFA_AFTER_WORD) != 0U) !=
               ((ctx & DFA_BEFORE_WORD) != 0U);
    case RE_NWORDB:
        return ((ctx & DFA_AFTER_WORD) != 0U) ==
               ((ctx & DFA_BEFORE_WORD) != 0U);
    default:
        break;
    }
    return false;
}

/* Epsilon-closure of `seed` under `ctx`, into d->work.  Iterative for the
 * same reason the VM's is: (a*)* chains splits without consuming input,
 * and a recursive closure would blow the C stack on a typed pattern. */
static u32 closure(Dfa *d, const u32 *seed, u32 nseed, u8 ctx,
                   bool *matched)
{
    u32 top = 0U;
    u32 n = 0U;
    u32 i;

    d->gen++;
    *matched = false;
    for (i = 0U; i < nseed; i++)
        d->stack[top++] = seed[i];
    while (top != 0U) {
        u32 pc = d->stack[--top];
        const ReInst *ins;

        if (pc >= d->nprog || d->stamp[pc] == d->gen)
            continue;
        d->stamp[pc] = d->gen;
        ins = &d->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_JMP:
            d->stack[top++] = ins->x;
            break;
        case RE_SPLIT:
            d->stack[top++] = ins->y;
            d->stack[top++] = ins->x;
            break;
        case RE_SAVE:
            /* Captures are meaningless to a DFA; step over them. */
            d->stack[top++] = pc + 1U;
            break;
        case RE_BOL: case RE_EOL: case RE_BOT: case RE_EOT:
        case RE_WORDB: case RE_NWORDB:
            if (ctx_holds(ctx, (ReOp)ins->op))
                d->stack[top++] = pc + 1U;
            break;
        case RE_MATCH:
            *matched = true;
            break;
        default:
            /* Consuming instruction: it belongs to the state. */
            d->work[n++] = pc;
            break;
        }
    }
    return n;
}

static int cmp_u32(const void *a, const void *b, void *ctx)
{
    u32 x = *(const u32 *)a;
    u32 y = *(const u32 *)b;

    (void)ctx;
    if (x != y)
        return x < y ? -1 : 1;
    return 0;
}

static u32 hash_pcs(const u32 *pcs, u32 n, u8 ctx)
{
    u32 h = 2166136261U ^ (u32)ctx;
    u32 i;

    for (i = 0U; i < n; i++) {
        h ^= pcs[i];
        h *= 16777619U;
    }
    return h;
}

static void dfa_flush(Dfa *d)
{
    /* State ids are indices, so a flush is a reset plus fresh arena —
     * no per-state teardown. */
    u32 i;

    for (i = 0U; i < SAG_DFA_BUCKETS; i++)
        d->buckets[i] = -1;
    d->nstates = 0U;
    d->bytes = 0U;
    d->flushes++;
}

/* Interns the closure currently in d->work as a state id, or -1 when the
 * cache overflowed and could not be flushed further. */
static i32 dfa_intern(Dfa *d, u32 n, u8 ctx, bool matched)
{
    u32 h;
    u32 bucket;
    i32 at;
    DfaState *st;

    if (n != 0U)
        sag_sort_stable(d->work, n, sizeof(*d->work), cmp_u32, NULL);
    h = hash_pcs(d->work, n, ctx);
    bucket = h % SAG_DFA_BUCKETS;
    for (at = d->buckets[bucket]; at >= 0; at = d->states[at].next) {
        st = &d->states[at];
        if (st->hash == h && st->ctx == ctx && st->npcs == n &&
            st->matched == matched &&
            (n == 0U ||
             memcmp(st->pcs, d->work, (size_t)n * sizeof(*st->pcs)) == 0))
            return at;
    }
    if (d->nstates == SAG_DFA_MAX_STATES ||
        d->bytes + (u64)n * sizeof(u32) > SAG_DFA_MAX_BYTES) {
        if (d->flushes >= SAG_DFA_MAX_FLUSHES)
            return -1;
        dfa_flush(d);
        bucket = h % SAG_DFA_BUCKETS;
    }
    st = &d->states[d->nstates];
    st->pcs = n == 0U ? NULL :
              arena_alloc(&d->arena, (size_t)n * sizeof(*st->pcs),
                          sizeof(u32));
    if (n != 0U)
        (void)memcpy(st->pcs, d->work, (size_t)n * sizeof(*st->pcs));
    st->npcs = n;
    st->hash = h;
    st->ctx = ctx;
    st->matched = matched;
    st->next = d->buckets[bucket];
    d->buckets[bucket] = (i32)d->nstates;
    d->bytes += (u64)n * sizeof(u32);
    return (i32)d->nstates++;
}

static bool inst_takes(const SagRe *re, const ReInst *ins, u32 cp)
{
    switch ((ReOp)ins->op) {
    case RE_CHAR:
        return cp == ins->arg;
    case RE_CLASS:
        return ins->arg < re->nclasses &&
               sag_re_class_has(&re->classes[ins->arg], cp);
    case RE_ANY:
        return ins->arg != 0U || cp != (u32)'\n';
    default:
        break;
    }
    return false;
}

/* One byte of input, chunk-aware; the DFA reads through the same
 * iterator discipline as everything else in src/search (§1's law). */
static bool dfa_byte(const SagReInput *in, u64 off, u8 *out)
{
    TextIter it;
    const u8 *chunk = NULL;
    size_t n = 0U;

    if (off >= in->window.hi)
        return false;
    if (in->tb == NULL) {
        if (off >= in->len)
            return false;
        *out = in->bytes[off];
        return true;
    }
    if (!sag_textiter_begin(&it, in->tb, BYTEOFF(off)) ||
        !sag_textiter_chunk(&it, in->tb, &chunk, &n) || n == 0U)
        return false;
    *out = chunk[0];
    return true;
}

static u32 dfa_decode(const SagReInput *in, u64 off, u32 *len_out)
{
    u8 buf[SAG_UTF8_MAX];
    size_t have = 0U;
    u32 cp = 0U;
    size_t used;

    while (have < SAG_UTF8_MAX) {
        u8 b;

        if (!dfa_byte(in, off + have, &b))
            break;
        buf[have++] = b;
    }
    if (have == 0U) {
        *len_out = 0U;
        return 0U;
    }
    used = sag_utf8_decode(buf, have, &cp);
    *len_out = (u32)(used == 0U ? 1U : used);
    return cp;
}

static u8 ctx_at(const SagReInput *in, u64 pos, u32 prev_cp, bool has_prev,
                 u32 cp, bool have_cp)
{
    u8 ctx = 0U;

    if (pos == in->window.lo)
        ctx |= (u8)(DFA_AT_BOT | DFA_AT_BOL);
    else if (has_prev && prev_cp == (u32)'\n')
        ctx |= DFA_AT_BOL;
    if (pos == in->window.hi)
        ctx |= (u8)(DFA_AT_EOT | DFA_AT_EOL);
    else if (have_cp && (cp == (u32)'\n' || cp == (u32)'\r'))
        ctx |= DFA_AT_EOL;
    if (has_prev && sag_re_is_word(prev_cp))
        ctx |= DFA_AFTER_WORD;
    if (have_cp && sag_re_is_word(cp))
        ctx |= DFA_BEFORE_WORD;
    return ctx;
}

/*
 * Unanchored "does it match at or after `from`".  Returns SAG_DFA_YES,
 * SAG_DFA_NO, or SAG_DFA_GIVE_UP when the cache thrashed and the caller
 * should fall back to the Pike VM.
 */
int sag_re_dfa_test(const SagRe *re, const SagReInput *in, u64 from)
{
    Dfa d;
    u64 pos;
    u32 prev_cp = 0U;
    bool has_prev = false;
    int verdict = SAG_DFA_NO;
    u32 *cur;
    u32 ncur;

    if (re == NULL || in == NULL || re->nprog == 0U)
        return SAG_DFA_NO;
    (void)memset(&d, 0, sizeof(d));
    d.re = re;
    d.prog = re->prog;
    d.nprog = re->nprog;
    arena_init(&d.arena);
    d.states = arena_alloc(&d.arena,
                           SAG_DFA_MAX_STATES * sizeof(*d.states),
                           sizeof(void *));
    d.work = arena_alloc(&d.arena, (size_t)re->nprog * sizeof(u32),
                         sizeof(u32));
    d.stamp = arena_alloc(&d.arena, (size_t)re->nprog * sizeof(u32),
                          sizeof(u32));
    d.stack = arena_alloc(&d.arena,
                          (size_t)(re->nprog * 2U + 8U) * sizeof(u32),
                          sizeof(u32));
    (void)memset(d.stamp, 0, (size_t)re->nprog * sizeof(u32));
    cur = arena_alloc(&d.arena, (size_t)re->nprog * sizeof(u32),
                      sizeof(u32));
    dfa_flush(&d);
    d.flushes = 0U;

    ncur = 0U;
    pos = from < in->window.lo ? in->window.lo : from;
    for (;;) {
        u32 cp = 0U;
        u32 cp_len = 0U;
        bool have_cp;
        u8 ctx;
        u32 n;
        bool matched = false;
        u32 seed[64];
        u32 nseed = 0U;
        u32 i;

        cp = dfa_decode(in, pos, &cp_len);
        have_cp = cp_len != 0U && pos < in->window.hi;
        ctx = ctx_at(in, pos, prev_cp, has_prev, cp, have_cp);

        /* Unanchored: the start pc is re-seeded at every position, which
         * is the DFA equivalent of the VM's per-position seeding. */
        if (ncur + 1U < re->nprog) {
            for (i = 0U; i < ncur; i++)
                d.work[i] = cur[i];
            n = ncur;
        } else {
            n = 0U;
        }
        (void)seed;
        (void)nseed;
        {
            u32 *combined = arena_alloc(&d.arena,
                                        (size_t)(n + 1U) * sizeof(u32),
                                        sizeof(u32));

            (void)memcpy(combined, d.work, (size_t)n * sizeof(u32));
            combined[n] = 0U; /* the start instruction */
            n = closure(&d, combined, n + 1U, ctx, &matched);
        }
        if (matched) {
            verdict = SAG_DFA_YES;
            break;
        }
        if (dfa_intern(&d, n, ctx, matched) < 0) {
            verdict = SAG_DFA_GIVE_UP;
            break;
        }
        if (!have_cp)
            break;
        /* Step every live instruction on this codepoint. */
        ncur = 0U;
        for (i = 0U; i < n; i++) {
            if (inst_takes(re, &re->prog[d.work[i]], cp))
                cur[ncur++] = d.work[i] + 1U;
        }
        prev_cp = cp;
        has_prev = true;
        pos += cp_len;
    }
    arena_free_all(&d.arena);
    return verdict;
}
