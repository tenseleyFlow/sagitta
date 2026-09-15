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
    YEW_DFA_MAX_STATES = 1024,
    YEW_DFA_MAX_BYTES = 1024U * 1024U,
    YEW_DFA_BUCKETS = 2048,
    YEW_DFA_EDGE_CACHE = 4096,
    YEW_DFA_EDGE_WAYS = 4,
    YEW_DFA_EDGE_SETS = YEW_DFA_EDGE_CACHE / YEW_DFA_EDGE_WAYS,
    /* Two flushes in one search means the working set does not fit; a
     * third would just be more rebuilding. */
    YEW_DFA_MAX_FLUSHES = 2
};

enum { DFA_EDGE_MISS = -1, DFA_EDGE_SKIP_BOL = -2 };

_Static_assert((YEW_DFA_EDGE_CACHE & (YEW_DFA_EDGE_CACHE - 1U)) == 0U,
               "DFA edge cache must be a power of two");
_Static_assert((YEW_DFA_EDGE_SETS & (YEW_DFA_EDGE_SETS - 1U)) == 0U,
               "DFA edge sets must be a power of two");

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
    /* Lazily filled transitions for ASCII, -1 meaning "not computed".
     * Assertion patterns add a compact context dimension when they use at
     * most two distinct context bits. */
    i32 *next_ascii;
} DfaState;

typedef struct DfaEdge {
    u32 state;
    u32 cp;
    i32 next;
    u8 ctx;
    bool valid;
} DfaEdge;

typedef struct Dfa {
    const YewRe *re;
    const ReInst *prog;
    u32 nprog;
    Arena arena;
    DfaState *states;
    u32 nstates;
    i32 buckets[YEW_DFA_BUCKETS];
    u32 flushes;
    u64 bytes;
    /* Scratch reused by every closure so a step allocates nothing. */
    u32 *work;
    u32 *stamp;
    u32 gen;
    u32 *stack;
    /* Scratch for building the next kernel; allocated ONCE.  Allocating
     * it per input position made a 64 MiB search grow RSS by 268 MB,
     * which the throughput gate's memory ceiling caught. */
    u32 *combined;
    DfaEdge *edges;
    u8 *edge_hand;
    u8 ctx_index[64];
    u32 ascii_ctx_variants;
    u8 ctx_mask;
    bool has_assert;
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

    for (i = 0U; i < YEW_DFA_BUCKETS; i++)
        d->buckets[i] = -1;
    if (d->edges != NULL)
        (void)memset(d->edges, 0,
                     YEW_DFA_EDGE_CACHE * sizeof(*d->edges));
    if (d->edge_hand != NULL)
        (void)memset(d->edge_hand, 0,
                     YEW_DFA_EDGE_SETS * sizeof(*d->edge_hand));
    d->nstates = 0U;
    d->bytes = 0U;
    d->flushes++;
}

static u32 edge_set(u32 state, u32 cp, u8 ctx)
{
    u32 hash = state * 0x9e3779b1U;

    hash ^= cp * 0x85ebca6bU;
    hash ^= (u32)ctx * 0xc2b2ae35U;
    hash ^= hash >> 16U;
    return hash & (YEW_DFA_EDGE_SETS - 1U);
}

static inline bool edge_get(const Dfa *d, u32 state, u32 cp, u8 ctx,
                            i32 *next)
{
    u32 set = edge_set(state, cp, ctx);
    u32 base = set * YEW_DFA_EDGE_WAYS;
    u32 way;

    for (way = 0U; way < YEW_DFA_EDGE_WAYS; way++) {
        const DfaEdge *edge = &d->edges[base + way];

        if (edge->valid && edge->state == state && edge->cp == cp &&
            edge->ctx == ctx) {
            *next = edge->next;
            return true;
        }
    }
    return false;
}

static inline void edge_put(Dfa *d, u32 state, u32 cp, u8 ctx, i32 next)
{
    u32 set = edge_set(state, cp, ctx);
    u32 base = set * YEW_DFA_EDGE_WAYS;
    u32 way;
    DfaEdge *edge = NULL;

    for (way = 0U; way < YEW_DFA_EDGE_WAYS; way++) {
        if (!d->edges[base + way].valid) {
            edge = &d->edges[base + way];
            break;
        }
    }
    if (edge == NULL) {
        way = d->edge_hand[set];
        d->edge_hand[set] =
            (u8)((way + 1U) & (YEW_DFA_EDGE_WAYS - 1U));
        edge = &d->edges[base + way];
    }

    edge->state = state;
    edge->cp = cp;
    edge->next = next;
    edge->ctx = ctx;
    edge->valid = true;
}

/* Interns the closure currently in d->work as a state id, or -1 when the
 * cache overflowed and could not be flushed further. */
static i32 dfa_intern(Dfa *d, u32 n, u8 ctx, bool matched)
{
    u64 state_bytes = (u64)n * sizeof(u32) +
                      (u64)d->ascii_ctx_variants * 128U * sizeof(i32);
    u32 h;
    u32 bucket;
    i32 at;
    DfaState *st;

    if (n != 0U)
        yew_sort_stable(d->work, n, sizeof(*d->work), cmp_u32, NULL);
    h = hash_pcs(d->work, n, ctx);
    bucket = h % YEW_DFA_BUCKETS;
    for (at = d->buckets[bucket]; at >= 0; at = d->states[at].next) {
        st = &d->states[at];
        if (st->hash == h && st->ctx == ctx && st->npcs == n &&
            st->matched == matched &&
            (n == 0U ||
             memcmp(st->pcs, d->work, (size_t)n * sizeof(*st->pcs)) == 0))
            return at;
    }
    if (d->nstates == YEW_DFA_MAX_STATES ||
        state_bytes > YEW_DFA_MAX_BYTES - d->bytes) {
        if (d->flushes >= YEW_DFA_MAX_FLUSHES)
            return -1;
        dfa_flush(d);
        bucket = h % YEW_DFA_BUCKETS;
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
    st->next_ascii = NULL;
    if (d->ascii_ctx_variants != 0U) {
        u32 slots = 128U * d->ascii_ctx_variants;
        u32 k;

        st->next_ascii = arena_alloc(&d->arena, slots * sizeof(i32),
                                     sizeof(i32));
        for (k = 0U; k < slots; k++)
            st->next_ascii[k] = -1;
    }
    d->buckets[bucket] = (i32)d->nstates;
    d->bytes += state_bytes;
    return (i32)d->nstates++;
}

static bool inst_takes(const YewRe *re, const ReInst *ins, u32 cp)
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

/* One byte of input, chunk-aware; the DFA reads through the same
 * iterator discipline as everything else in src/search (§1's law). */
static bool dfa_byte(const YewReInput *in, u64 off, u8 *out)
{
    TextIter it;
    const u8 *chunk = NULL;
    u64 n = 0U;

    if (off >= in->window.hi)
        return false;
    if (in->tb == NULL) {
        if (off >= in->len)
            return false;
        *out = in->bytes[off];
        return true;
    }
    if (!yew_textiter_begin(&it, in->tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&it, in->tb, &chunk, &n) || n == 0U)
        return false;
    *out = chunk[0];
    return true;
}

static u32 dfa_decode_slow(const YewReInput *in, u64 off, u32 *len_out)
{
    u8 buf[YEW_UTF8_MAX];
    size_t have = 0U;
    u32 cp = 0U;
    size_t used;

    while (have < YEW_UTF8_MAX) {
        u8 b;

        if (!dfa_byte(in, off + have, &b))
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

static inline u32 dfa_decode(const YewReInput *in, u64 off, u32 *len_out)
{
    /* Keep the flat-ASCII branch small enough to inline into the DFA loop.
     * Calling the general gather path for every byte consumed most of the
     * remaining YEW-F-072 assertion-search budget. */
    if (in->tb == NULL && off < in->window.hi && off < in->len &&
        in->bytes[off] < 0x80U) {
        *len_out = 1U;
        return in->bytes[off];
    }
    return dfa_decode_slow(in, off, len_out);
}

static inline bool dfa_is_word(u32 cp)
{
    if (cp < 128U) {
        u32 lower = cp | 0x20U;

        return lower - (u32)'a' < 26U || cp - (u32)'0' < 10U ||
               cp == (u32)'_';
    }
    return yew_re_is_word(cp);
}

static inline u8 ctx_at(const YewReInput *in, u64 pos, u32 prev_cp,
                        bool has_prev, u32 cp, bool have_cp, u8 mask)
{
    u8 ctx = 0U;

    if ((mask & DFA_AT_BOT) != 0U && pos == in->window.lo)
        ctx |= DFA_AT_BOT;
    if ((mask & DFA_AT_BOL) != 0U &&
        (pos == in->window.lo ||
         (has_prev && prev_cp == (u32)'\n')))
        ctx |= DFA_AT_BOL;
    if ((mask & DFA_AT_EOT) != 0U && pos == in->window.hi)
        ctx |= DFA_AT_EOT;
    if ((mask & DFA_AT_EOL) != 0U &&
        (pos == in->window.hi ||
         (have_cp && (cp == (u32)'\n' || cp == (u32)'\r'))))
        ctx |= DFA_AT_EOL;
    if ((mask & DFA_AFTER_WORD) != 0U && has_prev &&
        dfa_is_word(prev_cp))
        ctx |= DFA_AFTER_WORD;
    if ((mask & DFA_BEFORE_WORD) != 0U && have_cp && dfa_is_word(cp))
        ctx |= DFA_BEFORE_WORD;
    return ctx;
}

/*
 * Unanchored "does it match at or after `from`".  Returns YEW_DFA_YES,
 * YEW_DFA_NO, or YEW_DFA_GIVE_UP when the cache thrashed and the caller
 * should fall back to the Pike VM.
 */
static int dfa_scan(const YewRe *re, const YewReInput *in, u64 from,
                    u64 *end_out);

int yew_re_dfa_test(const YewRe *re, const YewReInput *in, u64 from)
{
    return dfa_scan(re, in, from, NULL);
}

/* Reports where the earliest match ENDS.  Not where it starts, and not
 * necessarily the end of the leftmost match — see yew_re_search. */
int yew_re_dfa_find_end(const YewRe *re, const YewReInput *in, u64 from,
                        u64 *end_out)
{
    return dfa_scan(re, in, from, end_out);
}

static int dfa_scan(const YewRe *re, const YewReInput *in, u64 from,
                    u64 *end_out)
{
    Dfa d;
    u64 pos;
    int verdict = YEW_DFA_NO;
    u32 *cur;
    u32 ncur;
    i32 state;
    u32 scan_cp = 0U;
    u32 scan_cp_len = 0U;
    bool have_scan_cp = false;
    bool scan_word = false;

    if (re == NULL || in == NULL || re->nprog == 0U)
        return YEW_DFA_NO;
    (void)memset(&d, 0, sizeof(d));
    d.re = re;
    d.prog = re->prog;
    d.nprog = re->nprog;
    arena_init(&d.arena);
    d.states = arena_alloc(&d.arena,
                           YEW_DFA_MAX_STATES * sizeof(*d.states),
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

    /* Assertions make a transition depend on the surrounding text as well
     * as the byte.  Compress the context bits a pattern actually reads so
     * common anchor and word-boundary patterns still get direct tables. */
    {
        u32 k;
        u32 variants = 1U;

        d.ctx_mask = 0U;
        for (k = 0U; k < re->nprog; k++) {
            ReOp op = (ReOp)re->prog[k].op;

            if (op == RE_BOL)
                d.ctx_mask |= DFA_AT_BOL;
            else if (op == RE_EOL)
                d.ctx_mask |= DFA_AT_EOL;
            else if (op == RE_BOT)
                d.ctx_mask |= DFA_AT_BOT;
            else if (op == RE_EOT)
                d.ctx_mask |= DFA_AT_EOT;
            else if (op == RE_WORDB || op == RE_NWORDB)
                d.ctx_mask |= (u8)(DFA_AFTER_WORD | DFA_BEFORE_WORD);
        }
        d.has_assert = d.ctx_mask != 0U;
        for (k = 1U; k <= DFA_AT_EOT; k <<= 1U) {
            if ((d.ctx_mask & (u8)k) != 0U)
                variants <<= 1U;
        }
        if (variants <= 4U)
            d.ascii_ctx_variants = variants;
        for (k = 0U; k < YEW_ARRAY_LEN(d.ctx_index); k++) {
            u32 bit;
            u8 dense = 0U;
            u8 out_bit = 1U;

            for (bit = 1U; bit <= DFA_AT_EOT; bit <<= 1U) {
                if ((d.ctx_mask & (u8)bit) == 0U)
                    continue;
                if ((k & bit) != 0U)
                    dense |= out_bit;
                out_bit <<= 1U;
            }
            d.ctx_index[k] = dense;
        }
    }
    d.combined = arena_alloc(&d.arena, (size_t)(re->nprog + 2U) *
                             sizeof(u32), sizeof(u32));
    if (d.has_assert && d.ascii_ctx_variants == 0U)
        d.edges = arena_alloc(&d.arena,
                              YEW_DFA_EDGE_CACHE * sizeof(*d.edges),
                              sizeof(void *));
    if (d.has_assert && d.ascii_ctx_variants == 0U)
        d.edge_hand = arena_alloc(&d.arena,
                                  YEW_DFA_EDGE_SETS *
                                      sizeof(*d.edge_hand),
                                  sizeof(u8));
    dfa_flush(&d);
    d.flushes = 0U;

    pos = from < in->window.lo ? in->window.lo : from;

    /*
     * The transition is an explicit edge (state, codepoint) -> state.
     *
     * Getting this wrong is subtle and silent: an earlier version cached
     * the new state under the codepoint at the CURRENT position, while
     * the state had actually been reached by stepping on the PREVIOUS
     * one.  Every cached edge was therefore off by one character, and
     * the only visible symptom was a DFA that disagreed with the VM on
     * about one pattern in fifty.
     */
    {
        u8 ctx0;
        bool matched0 = false;
        u32 n0;

        u32 prev0 = 0U;
        bool has_prev0 = false;

        /*
         * The codepoint before `pos`, for the same reason the VM walks
         * back for it: a scan that starts mid-text still has to know
         * whether it stands after a newline or a word character.
         * Seeding with has_prev=false instead reports "no context",
         * which reads as "not a line start" — /^$/ over "foo\n" from 4
         * then missed the empty last line, because the DFA could not
         * see the '\n' it was standing behind.  Bounded by
         * YEW_UTF8_MAX, so this is a few bytes, not a rescan.
         */
        if (pos > in->window.lo) {
            u64 back = pos - in->window.lo > YEW_UTF8_MAX ?
                       (u64)YEW_UTF8_MAX : pos - in->window.lo;
            u64 probe = pos - back;

            while (probe < pos) {
                u32 plen = 0U;
                u32 pcp = dfa_decode(in, probe, &plen);

                if (plen == 0U)
                    break;
                if (probe + plen >= pos) {
                    prev0 = pcp;
                    has_prev0 = true;
                    break;
                }
                probe += plen;
            }
        }
        scan_cp = dfa_decode(in, pos, &scan_cp_len);
        have_scan_cp = scan_cp_len != 0U && pos < in->window.hi;
        if ((d.ctx_mask & (DFA_AFTER_WORD | DFA_BEFORE_WORD)) != 0U &&
            have_scan_cp)
            scan_word = dfa_is_word(scan_cp);
        ctx0 = d.has_assert ?
               ctx_at(in, pos, prev0, has_prev0, scan_cp, have_scan_cp,
                      d.ctx_mask) : 0U;
        d.combined[0] = 0U; /* the start instruction */
        n0 = closure(&d, d.combined, 1U, ctx0, &matched0);
        state = dfa_intern(&d, n0, ctx0, matched0);
        if (state < 0) {
            arena_free_all(&d.arena);
            return YEW_DFA_GIVE_UP;
        }
    }

    for (;;) {
        u32 cp = scan_cp;
        u32 cp_len = scan_cp_len;
        bool have_cp = have_scan_cp;
        u32 next_cp;
        u32 next_cp_len = 0U;
        bool have_next_cp;
        bool next_word = false;
        u64 next_pos;
        i32 next;
        u8 next_ctx = 0U;
        u32 i;

        if (d.states[state].matched) {
            verdict = YEW_DFA_YES;
            if (end_out != NULL)
                *end_out = pos;
            break;
        }
        if (!have_cp)
            break;
        next_pos = pos + cp_len;
        next_cp = dfa_decode(in, next_pos, &next_cp_len);
        have_next_cp = next_cp_len != 0U &&
                       next_pos < in->window.hi;

        next = DFA_EDGE_MISS;
        if (d.has_assert) {
            /* This is ctx_at(next_pos), flattened for the byte loop.  Word
             * status is carried forward so each codepoint is classified
             * once rather than as both this iteration's next and the next
             * iteration's current codepoint. */
            if ((d.ctx_mask & DFA_AT_BOL) != 0U && cp == (u32)'\n')
                next_ctx |= DFA_AT_BOL;
            if (next_pos == in->window.hi) {
                next_ctx |= d.ctx_mask & (DFA_AT_EOT | DFA_AT_EOL);
            } else if ((d.ctx_mask & DFA_AT_EOL) != 0U && have_next_cp &&
                       (next_cp == (u32)'\n' || next_cp == (u32)'\r')) {
                next_ctx |= DFA_AT_EOL;
            }
            if ((d.ctx_mask & (DFA_AFTER_WORD | DFA_BEFORE_WORD)) != 0U) {
                if (have_next_cp)
                    next_word = dfa_is_word(next_cp);
                if ((d.ctx_mask & DFA_AFTER_WORD) != 0U && scan_word)
                    next_ctx |= DFA_AFTER_WORD;
                if ((d.ctx_mask & DFA_BEFORE_WORD) != 0U && next_word)
                    next_ctx |= DFA_BEFORE_WORD;
            }
        }
        if (cp < 128U && d.states[state].next_ascii != NULL) {
            u32 slot = cp * d.ascii_ctx_variants +
                       d.ctx_index[next_ctx];

            next = d.states[state].next_ascii[slot];
        } else if (d.edges != NULL) {
            (void)edge_get(&d, (u32)state, cp, next_ctx, &next);
        }

        if (next < 0) {
            bool matched = false;
            u32 n;
            i32 from_state = state;
            u32 from_flushes = d.flushes;

            if (next == DFA_EDGE_SKIP_BOL) {
                u64 scan_hi = in->window.hi < in->len ?
                              in->window.hi : in->len;
                const u8 *newline;

                /* YEW-F-072: a BOL-only state with no consuming
                 * instruction cannot change before the next LF.  The
                 * transition table encodes that fact so unrelated regexes
                 * pay no branch in their byte loop. */
                if (pos >= scan_hi)
                    break;
                newline = memchr(in->bytes + (size_t)pos, '\n',
                                 (size_t)(scan_hi - pos));
                if (newline == NULL)
                    break;
                pos = (u64)(newline - in->bytes);
                scan_cp = (u32)'\n';
                scan_cp_len = 1U;
                have_scan_cp = true;
                scan_word = false;
                continue;
            }

            /* Step every live instruction of this state on `cp`, then
             * re-seed the start instruction: this scan is unanchored, so
             * a match may begin at the next position too. */
            ncur = 0U;
            for (i = 0U; i < d.states[state].npcs; i++) {
                u32 pc = d.states[state].pcs[i];

                if (inst_takes(re, &re->prog[pc], cp))
                    cur[ncur++] = pc + 1U;
            }
            for (i = 0U; i < ncur; i++)
                d.combined[i] = cur[i];
            d.combined[ncur] = 0U;
            n = closure(&d, d.combined, ncur + 1U, next_ctx, &matched);
            next = dfa_intern(&d, n, next_ctx, matched);
            if (next < 0) {
                verdict = YEW_DFA_GIVE_UP;
                break;
            }
            /* Record the edge we just walked.  A flush may have
             * invalidated `from_state`, so only cache when it survived. */
            if (d.flushes == from_flushes) {
                if (cp < 128U && (u32)from_state < d.nstates &&
                    d.states[from_state].next_ascii != NULL) {
                    u32 slot = cp * d.ascii_ctx_variants +
                               d.ctx_index[next_ctx];
                    i32 cached = next;

                    if (in->tb == NULL && d.ctx_mask == DFA_AT_BOL &&
                        d.states[from_state].npcs == 0U &&
                        cp != (u32)'\n' && next == from_state)
                        cached = DFA_EDGE_SKIP_BOL;
                    d.states[from_state].next_ascii[slot] = cached;
                } else if (d.edges != NULL) {
                    /* YEW-F-072: assertion transitions depend on the
                     * following context as well as state and codepoint.
                     * Caching that complete key avoids rebuilding and
                     * sorting the same closure for every byte of a large
                     * search without weakening ^, $, or word boundaries. */
                    edge_put(&d, (u32)from_state, cp, next_ctx, next);
                }
            }
        }
        state = next;
        pos += cp_len;
        scan_cp = next_cp;
        scan_cp_len = next_cp_len;
        have_scan_cp = have_next_cp;
        scan_word = next_word;
    }
    arena_free_all(&d.arena);
    return verdict;
}
