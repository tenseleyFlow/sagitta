/* The differential oracle.  See re_ref.h for why it backtracks. */
#include "re_ref.h"

#include <string.h>

#include "unicode/utf8.h"
#include "unicode/wordbreak.h"

enum {
    REF_STEP_BUDGET = 1000000,
    REF_MAX_NODES = 512
};

/* ---------------------------------------------------------------- */
/* A tiny parser onto a tiny AST                                    */
/* ---------------------------------------------------------------- */

typedef enum {
    N_EMPTY,
    N_CHAR,
    N_ANY,
    N_CLASS,
    N_CAT,
    N_ALT,
    N_STAR,
    N_PLUS,
    N_QUEST,
    N_GROUP,
    N_BOL,
    N_EOL,
    N_WORDB,
    N_NWORDB
} RefKind;

typedef struct RefRange {
    u32 lo;
    u32 hi;
} RefRange;

typedef struct RefNode {
    u8 kind;
    bool greedy;
    bool negate;
    u32 cp;
    u32 group;
    RefRange ranges[32];
    u32 nranges;
    i32 a;
    i32 b;
} RefNode;

typedef struct RefProg {
    RefNode nodes[REF_MAX_NODES];
    u32 n;
    const u8 *pat;
    size_t len;
    size_t at;
    u32 ngroups;
    bool failed;   /* malformed pattern                                */
    bool unsupported; /* outside the shared subset                     */
} RefProg;

static i32 node_new(RefProg *p, RefKind kind)
{
    RefNode *n;

    if (p->n == REF_MAX_NODES) {
        p->unsupported = true;
        return -1;
    }
    n = &p->nodes[p->n];
    (void)memset(n, 0, sizeof(*n));
    n->kind = (u8)kind;
    n->greedy = true;
    n->a = -1;
    n->b = -1;
    return (i32)p->n++;
}

static i32 parse_alt(RefProg *p);

static bool ref_is_word(u32 cp)
{
    /* Must match the engine's definition, which is category-based. */
    extern bool yew_re_is_word(u32 cp);

    return yew_re_is_word(cp);
}

static u32 take_cp(RefProg *p)
{
    u32 cp = 0U;
    size_t n = yew_utf8_decode(p->pat + p->at, p->len - p->at, &cp);

    p->at += n == 0U ? 1U : n;
    return cp;
}

static void add_range(RefNode *n, u32 lo, u32 hi)
{
    if (n->nranges < YEW_ARRAY_LEN(n->ranges)) {
        n->ranges[n->nranges].lo = lo;
        n->ranges[n->nranges].hi = hi;
        n->nranges++;
    }
}

/* Only the class syntax the fuzzer's generator emits. */
static i32 parse_class(RefProg *p)
{
    i32 id = node_new(p, N_CLASS);
    RefNode *n;
    bool first = true;

    if (id < 0)
        return -1;
    n = &p->nodes[id];
    if (p->at < p->len && p->pat[p->at] == '^') {
        n->negate = true;
        p->at++;
    }
    for (;;) {
        u32 lo;

        if (p->at >= p->len) {
            p->failed = true;
            return -1;
        }
        if (p->pat[p->at] == ']' && !first) {
            p->at++;
            break;
        }
        first = false;
        if (p->pat[p->at] == '\\') {
            /* Escapes inside classes are outside this oracle's subset. */
            p->unsupported = true;
            return -1;
        }
        lo = take_cp(p);
        if (p->at + 1U < p->len && p->pat[p->at] == '-' &&
            p->pat[p->at + 1U] != ']') {
            u32 hi;

            p->at++;
            hi = take_cp(p);
            if (hi < lo) {
                p->failed = true;
                return -1;
            }
            add_range(&p->nodes[id], lo, hi);
            continue;
        }
        add_range(&p->nodes[id], lo, lo);
    }
    return id;
}

static i32 parse_atom(RefProg *p)
{
    u8 c;

    if (p->at >= p->len)
        return node_new(p, N_EMPTY);
    c = p->pat[p->at];
    switch (c) {
    case '(': {
        i32 id;
        i32 body;
        u32 group;

        p->at++;
        if (p->at + 1U < p->len && p->pat[p->at] == '?' &&
            p->pat[p->at + 1U] == ':') {
            p->at += 2U;
            group = 0U;
        } else if (p->at < p->len && p->pat[p->at] == '?') {
            p->unsupported = true;
            return -1;
        } else {
            if (p->ngroups >= YEW_REF_MAX_GROUPS) {
                p->unsupported = true;
                return -1;
            }
            group = p->ngroups++;
        }
        body = parse_alt(p);
        if (body < 0 || p->at >= p->len || p->pat[p->at] != ')') {
            p->failed = true;
            return -1;
        }
        p->at++;
        id = node_new(p, N_GROUP);
        if (id < 0)
            return -1;
        p->nodes[id].group = group;
        p->nodes[id].a = body;
        return id;
    }
    case '[':
        p->at++;
        return parse_class(p);
    case '.':
        p->at++;
        return node_new(p, N_ANY);
    case '^':
        p->at++;
        return node_new(p, N_BOL);
    case '$':
        p->at++;
        return node_new(p, N_EOL);
    case ')':
    case '*':
    case '+':
    case '?':
        p->failed = true;
        return -1;
    case '\\': {
        i32 id;

        p->at++;
        if (p->at >= p->len) {
            p->failed = true;
            return -1;
        }
        c = p->pat[p->at];
        if (c == 'b' || c == 'B') {
            p->at++;
            return node_new(p, c == 'b' ? N_WORDB : N_NWORDB);
        }
        /* Perl classes and the rest are outside the subset. */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            p->unsupported = true;
            return -1;
        }
        p->at++;
        id = node_new(p, N_CHAR);
        if (id >= 0)
            p->nodes[id].cp = c;
        return id;
    }
    default:
        break;
    }
    {
        i32 id = node_new(p, N_CHAR);

        if (id >= 0)
            p->nodes[id].cp = take_cp(p);
        return id;
    }
}

static i32 parse_piece(RefProg *p)
{
    i32 a = parse_atom(p);

    if (a < 0)
        return -1;
    for (;;) {
        u8 c;
        i32 q;

        if (p->at >= p->len)
            break;
        c = p->pat[p->at];
        if (c == '*')
            q = node_new(p, N_STAR);
        else if (c == '+')
            q = node_new(p, N_PLUS);
        else if (c == '?')
            q = node_new(p, N_QUEST);
        else if (c == '{') {
            /* Bounded repeats are outside this oracle. */
            p->unsupported = true;
            return -1;
        } else {
            break;
        }
        if (q < 0)
            return -1;
        p->at++;
        p->nodes[q].a = a;
        if (p->at < p->len && p->pat[p->at] == '?') {
            p->nodes[q].greedy = false;
            p->at++;
        }
        a = q;
    }
    return a;
}

static i32 parse_concat(RefProg *p)
{
    i32 left = -1;

    while (p->at < p->len && p->pat[p->at] != '|' && p->pat[p->at] != ')') {
        i32 piece = parse_piece(p);
        i32 cat;

        if (piece < 0)
            return -1;
        if (left < 0) {
            left = piece;
            continue;
        }
        cat = node_new(p, N_CAT);
        if (cat < 0)
            return -1;
        p->nodes[cat].a = left;
        p->nodes[cat].b = piece;
        left = cat;
    }
    return left < 0 ? node_new(p, N_EMPTY) : left;
}

static i32 parse_alt(RefProg *p)
{
    i32 left = parse_concat(p);

    if (left < 0)
        return -1;
    while (p->at < p->len && p->pat[p->at] == '|') {
        i32 right;
        i32 alt;

        p->at++;
        right = parse_concat(p);
        if (right < 0)
            return -1;
        alt = node_new(p, N_ALT);
        if (alt < 0)
            return -1;
        p->nodes[alt].a = left;
        p->nodes[alt].b = right;
        left = alt;
    }
    return left;
}

/* ---------------------------------------------------------------- */
/* The backtracking matcher                                          */
/* ---------------------------------------------------------------- */

typedef struct RefRun {
    const RefProg *p;
    const u8 *hay;
    size_t haylen;
    u64 lo[YEW_REF_MAX_GROUPS];
    u64 hi[YEW_REF_MAX_GROUPS];
    bool set[YEW_REF_MAX_GROUPS];
    i64 steps;
    bool budget_out;
} RefRun;

/* Continuation-passing so leftmost-first priority falls out of the
 * evaluation order, exactly as a backtracker gives it. */
typedef bool (*RefCont)(RefRun *r, u64 pos, void *ctx);

typedef struct ContFrame {
    const i32 *seq;
    u32 n;
    u32 at;
    RefCont next;
    void *next_ctx;
} ContFrame;

static bool match_node(RefRun *r, i32 id, u64 pos, RefCont k, void *ctx);

static bool cont_done(RefRun *r, u64 pos, void *ctx)
{
    u64 *end = ctx;

    (void)r;
    *end = pos;
    return true;
}

typedef struct CatCtx {
    i32 b;
    RefCont k;
    void *ctx;
} CatCtx;

static bool cont_cat(RefRun *r, u64 pos, void *ctx)
{
    CatCtx *c = ctx;

    return match_node(r, c->b, pos, c->k, c->ctx);
}

typedef struct RepCtx {
    i32 id;
    u64 last;
    RefCont k;
    void *ctx;
} RepCtx;

static bool match_star(RefRun *r, i32 id, u64 pos, RefCont k, void *ctx);

static bool cont_star(RefRun *r, u64 pos, void *ctx)
{
    RepCtx *c = ctx;

    /* An empty iteration would loop forever; stop and let the
     * continuation run, which is what a careful backtracker does. */
    if (pos == c->last)
        return c->k(r, pos, c->ctx);
    return match_star(r, c->id, pos, c->k, c->ctx);
}

static bool match_star(RefRun *r, i32 id, u64 pos, RefCont k, void *ctx)
{
    const RefNode *n = &r->p->nodes[id];
    RepCtx rc;

    if (r->steps-- <= 0) {
        r->budget_out = true;
        return false;
    }
    rc.id = id;
    rc.last = pos;
    rc.k = k;
    rc.ctx = ctx;
    if (n->greedy) {
        if (match_node(r, n->a, pos, cont_star, &rc))
            return true;
        return k(r, pos, ctx);
    }
    if (k(r, pos, ctx))
        return true;
    return match_node(r, n->a, pos, cont_star, &rc);
}

static u32 decode_at(const RefRun *r, u64 pos, u32 *len)
{
    u32 cp = 0U;
    size_t n;

    if (pos >= r->haylen) {
        *len = 0U;
        return 0U;
    }
    n = yew_utf8_decode(r->hay + pos, r->haylen - pos, &cp);
    *len = (u32)(n == 0U ? 1U : n);
    return cp;
}

static u32 decode_before(const RefRun *r, u64 pos)
{
    u64 back = pos > 4U ? pos - 4U : 0U;
    u32 prev = 0U;

    while (back < pos) {
        u32 len = 0U;
        u32 cp = decode_at(r, back, &len);

        if (len == 0U)
            break;
        if (back + len >= pos) {
            prev = cp;
            break;
        }
        back += len;
    }
    return prev;
}

static bool class_has(const RefNode *n, u32 cp)
{
    u32 i;
    bool in = false;

    for (i = 0U; i < n->nranges; i++)
        in = in || (cp >= n->ranges[i].lo && cp <= n->ranges[i].hi);
    return n->negate ? !in : in;
}

static bool match_node(RefRun *r, i32 id, u64 pos, RefCont k, void *ctx)
{
    const RefNode *n;

    if (id < 0)
        return false;
    if (r->steps-- <= 0) {
        r->budget_out = true;
        return false;
    }
    n = &r->p->nodes[id];
    switch ((RefKind)n->kind) {
    case N_EMPTY:
        return k(r, pos, ctx);
    case N_CHAR: {
        u32 len = 0U;
        u32 cp = decode_at(r, pos, &len);

        if (len == 0U || cp != n->cp)
            return false;
        return k(r, pos + len, ctx);
    }
    case N_ANY: {
        u32 len = 0U;
        u32 cp = decode_at(r, pos, &len);

        if (len == 0U || cp == (u32)'\n')
            return false;
        return k(r, pos + len, ctx);
    }
    case N_CLASS: {
        u32 len = 0U;
        u32 cp = decode_at(r, pos, &len);

        if (len == 0U || !class_has(n, cp))
            return false;
        return k(r, pos + len, ctx);
    }
    case N_CAT: {
        CatCtx cc;

        cc.b = n->b;
        cc.k = k;
        cc.ctx = ctx;
        return match_node(r, n->a, pos, cont_cat, &cc);
    }
    case N_ALT:
        /* Leftmost-first: the earlier branch wins. */
        if (match_node(r, n->a, pos, k, ctx))
            return true;
        return match_node(r, n->b, pos, k, ctx);
    case N_STAR:
        return match_star(r, id, pos, k, ctx);
    case N_PLUS: {
        RepCtx rc;

        rc.id = id;
        rc.last = pos;
        rc.k = k;
        rc.ctx = ctx;
        /* One mandatory iteration, then star semantics. */
        return match_node(r, n->a, pos, cont_star, &rc);
    }
    case N_QUEST:
        if (n->greedy) {
            if (match_node(r, n->a, pos, k, ctx))
                return true;
            return k(r, pos, ctx);
        }
        if (k(r, pos, ctx))
            return true;
        return match_node(r, n->a, pos, k, ctx);
    case N_GROUP: {
        /* Captures are recorded on the way through; a failed branch
         * leaves stale values, which is why only a successful overall
         * match's captures are ever read. */
        u64 saved_lo = r->lo[n->group];
        u64 saved_hi = r->hi[n->group];
        bool saved_set = r->set[n->group];
        CatCtx cc;
        bool ok;

        (void)cc;
        r->lo[n->group] = pos;
        ok = match_node(r, n->a, pos, k, ctx);
        if (!ok) {
            r->lo[n->group] = saved_lo;
            r->hi[n->group] = saved_hi;
            r->set[n->group] = saved_set;
        }
        return ok;
    }
    case N_BOL:
        if (pos == 0U || (pos > 0U && r->hay[pos - 1U] == (u8)'\n'))
            return k(r, pos, ctx);
        return false;
    case N_EOL:
        if (pos == r->haylen || r->hay[pos] == (u8)'\n' ||
            (r->hay[pos] == (u8)'\r' && pos + 1U < r->haylen &&
             r->hay[pos + 1U] == (u8)'\n'))
            return k(r, pos, ctx);
        return false;
    case N_WORDB:
    case N_NWORDB: {
        u32 len = 0U;
        u32 cp = decode_at(r, pos, &len);
        bool before = pos != 0U && ref_is_word(decode_before(r, pos));
        bool after = len != 0U && ref_is_word(cp);
        bool boundary = before != after;

        if ((RefKind)n->kind == N_WORDB ? boundary : !boundary)
            return k(r, pos, ctx);
        return false;
    }
    default:
        break;
    }
    return false;
}

YewRefResult yew_ref_search(const char *pat, size_t patlen, const u8 *hay,
                            size_t haylen, u64 from, YewRefMatch *out)
{
    RefProg p;
    RefRun r;
    i32 root;
    u64 start;

    (void)memset(&p, 0, sizeof(p));
    p.pat = (const u8 *)pat;
    p.len = patlen;
    p.ngroups = 1U; /* group 0 */
    root = parse_alt(&p);
    if (p.unsupported)
        return YEW_REF_UNKNOWN;
    if (root < 0 || p.failed || p.at != patlen)
        return YEW_REF_UNKNOWN; /* the engine may still reject it */

    for (start = from; start <= haylen; start++) {
        u64 end = 0U;
        u32 g;

        (void)memset(&r, 0, sizeof(r));
        r.p = &p;
        r.hay = hay;
        r.haylen = haylen;
        r.steps = REF_STEP_BUDGET;
        for (g = 0U; g < YEW_REF_MAX_GROUPS; g++) {
            r.lo[g] = 0U;
            r.hi[g] = 0U;
            r.set[g] = false;
        }
        if (match_node(&r, root, start, cont_done, &end)) {
            if (out != NULL) {
                (void)memset(out, 0, sizeof(*out));
                out->ngroups = p.ngroups;
                out->lo[0] = start;
                out->hi[0] = end;
                out->set[0] = true;
            }
            return YEW_REF_MATCH;
        }
        if (r.budget_out)
            return YEW_REF_UNKNOWN;
        /* Only advance to the next codepoint boundary. */
        if (start < haylen) {
            u32 len = 0U;

            (void)decode_at(&r, start, &len);
            if (len > 1U)
                start += len - 1U;
        }
    }
    return YEW_REF_NO_MATCH;
}
