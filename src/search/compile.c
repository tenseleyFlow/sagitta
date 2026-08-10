/* Sprint 20 §5: AST -> Thompson program, plus the reverse program. */
#include "search/regex_internal.h"

#include <string.h>

#include "unicode/utf8.h"
#include "util/log.h"

typedef struct Emit {
    ReInst *prog;
    u32 n;
    u32 cap;
    Arena *arena;
    bool overflow;
    bool reverse;
} Emit;

static u32 emit(Emit *e, ReOp op, u32 x, u32 y, u32 arg)
{
    u32 at;

    if (e->overflow)
        return 0U;
    /*
     * The program limit is checked DURING emission, not after: {m,n}
     * expands by copying, so `(a{100}){100}` would otherwise allocate a
     * ten-megabyte program before anyone noticed it was too big.
     */
    if (e->n >= YEW_RE_MAX_PROG) {
        e->overflow = true;
        return 0U;
    }
    if (e->n == e->cap) {
        u32 cap = e->cap == 0U ? 32U : e->cap * 2U;
        ReInst *grown;

        if (cap > YEW_RE_MAX_PROG)
            cap = YEW_RE_MAX_PROG;
        grown = arena_alloc(e->arena, (size_t)cap * sizeof(*grown),
                            sizeof(u32));
        if (e->n != 0U)
            (void)memcpy(grown, e->prog, (size_t)e->n * sizeof(*grown));
        e->prog = grown;
        e->cap = cap;
    }
    at = e->n++;
    e->prog[at].op = (u8)op;
    e->prog[at].x = x;
    e->prog[at].y = y;
    e->prog[at].arg = arg;
    return at;
}

static void patch_x(Emit *e, u32 at, u32 target)
{
    if (!e->overflow && at < e->n)
        e->prog[at].x = target;
}

static void patch_y(Emit *e, u32 at, u32 target)
{
    if (!e->overflow && at < e->n)
        e->prog[at].y = target;
}

static void gen(Emit *e, const ReAst *a);

/* Concatenation is the only place direction matters: the reverse program
 * is the same AST with operand order flipped, which is what lets one
 * compiler serve both scan directions (§6c). */
static void gen_cat(Emit *e, const ReAst *a)
{
    if (e->reverse) {
        gen(e, a->b);
        gen(e, a->a);
        return;
    }
    gen(e, a->a);
    gen(e, a->b);
}

static void gen_repeat(Emit *e, const ReAst *a)
{
    u32 i;

    for (i = 0U; i < a->min; i++) {
        gen(e, a->a);
        if (e->overflow)
            return;
    }
    if (a->max == UINT32_MAX) {
        /* m copies then a star. */
        u32 split = emit(e, RE_SPLIT, 0U, 0U, 0U);
        u32 body = e->n;

        gen(e, a->a);
        (void)emit(e, RE_JMP, split, 0U, 0U);
        if (a->greedy) {
            patch_x(e, split, body);
            patch_y(e, split, e->n);
        } else {
            patch_x(e, split, e->n);
            patch_y(e, split, body);
        }
        return;
    }
    /* (max - min) optional copies. */
    {
        u32 opt = a->max - a->min;
        u32 *splits = opt == 0U ? NULL :
                      arena_alloc(e->arena, (size_t)opt * sizeof(u32),
                                  sizeof(u32));

        for (i = 0U; i < opt; i++) {
            u32 split = emit(e, RE_SPLIT, 0U, 0U, 0U);
            u32 body = e->n;

            splits[i] = split;
            gen(e, a->a);
            if (e->overflow)
                return;
            if (a->greedy)
                patch_x(e, split, body);
            else
                patch_y(e, split, body);
        }
        /* Every optional copy exits to the same end. */
        for (i = 0U; i < opt; i++) {
            if (a->greedy)
                patch_y(e, splits[i], e->n);
            else
                patch_x(e, splits[i], e->n);
        }
    }
}

static void gen(Emit *e, const ReAst *a)
{
    if (a == NULL || e->overflow)
        return;
    switch ((ReAstKind)a->kind) {
    case RE_A_EMPTY:
        break;
    case RE_A_CHAR:
        (void)emit(e, RE_CHAR, 0U, 0U, a->cp);
        break;
    case RE_A_CLASS:
        (void)emit(e, RE_CLASS, 0U, 0U, a->cls);
        break;
    case RE_A_ANY:
        (void)emit(e, RE_ANY, 0U, 0U, a->dotall ? 1U : 0U);
        break;
    case RE_A_CAT:
        gen_cat(e, a);
        break;
    case RE_A_ALT: {
        u32 split = emit(e, RE_SPLIT, 0U, 0U, 0U);
        u32 jmp;

        patch_x(e, split, e->n);
        gen(e, a->a);
        jmp = emit(e, RE_JMP, 0U, 0U, 0U);
        patch_y(e, split, e->n);
        gen(e, a->b);
        patch_x(e, jmp, e->n);
        break;
    }
    case RE_A_STAR: {
        u32 split = emit(e, RE_SPLIT, 0U, 0U, 0U);
        u32 body = e->n;

        gen(e, a->a);
        (void)emit(e, RE_JMP, split, 0U, 0U);
        if (a->greedy) {
            patch_x(e, split, body);
            patch_y(e, split, e->n);
        } else {
            patch_x(e, split, e->n);
            patch_y(e, split, body);
        }
        break;
    }
    case RE_A_PLUS: {
        u32 body = e->n;
        u32 split;

        gen(e, a->a);
        split = emit(e, RE_SPLIT, 0U, 0U, 0U);
        if (a->greedy) {
            patch_x(e, split, body);
            patch_y(e, split, e->n);
        } else {
            patch_x(e, split, e->n);
            patch_y(e, split, body);
        }
        break;
    }
    case RE_A_QUEST: {
        u32 split = emit(e, RE_SPLIT, 0U, 0U, 0U);
        u32 body = e->n;

        gen(e, a->a);
        if (a->greedy) {
            patch_x(e, split, body);
            patch_y(e, split, e->n);
        } else {
            patch_x(e, split, e->n);
            patch_y(e, split, body);
        }
        break;
    }
    case RE_A_REPEAT:
        gen_repeat(e, a);
        break;
    case RE_A_GROUP:
        /* Group 0 is the whole match and is saved by the caller.  In the
         * reverse program the end slot is written first, since the scan
         * runs the other way. */
        if (a->group != 0U) {
            u32 first = e->reverse ? a->group * 2U + 1U : a->group * 2U;
            u32 second = e->reverse ? a->group * 2U : a->group * 2U + 1U;

            (void)emit(e, RE_SAVE, 0U, 0U, first);
            gen(e, a->a);
            (void)emit(e, RE_SAVE, 0U, 0U, second);
        } else {
            gen(e, a->a);
        }
        break;
    case RE_A_BOL:
        (void)emit(e, e->reverse ? RE_EOL : RE_BOL, 0U, 0U, 0U);
        break;
    case RE_A_EOL:
        (void)emit(e, e->reverse ? RE_BOL : RE_EOL, 0U, 0U, 0U);
        break;
    case RE_A_BOT:
        (void)emit(e, e->reverse ? RE_EOT : RE_BOT, 0U, 0U, 0U);
        break;
    case RE_A_EOT:
        (void)emit(e, e->reverse ? RE_BOT : RE_EOT, 0U, 0U, 0U);
        break;
    case RE_A_WORDB:
        /* \b is symmetric: it looks at both sides either way. */
        (void)emit(e, RE_WORDB, 0U, 0U, 0U);
        break;
    case RE_A_NWORDB:
        (void)emit(e, RE_NWORDB, 0U, 0U, 0U);
        break;
    default:
        YEW_BUG("regex compile: unknown AST node %u", (unsigned)a->kind);
    }
}

/* Minimum codepoints any match consumes.  Sprint 21 uses it to skip
 * hopeless windows; the VM uses it for nothing, so an underestimate is
 * safe and an overestimate is a correctness bug. */
static u32 min_len(const ReAst *a)
{
    if (a == NULL)
        return 0U;
    switch ((ReAstKind)a->kind) {
    case RE_A_CHAR:
    case RE_A_CLASS:
    case RE_A_ANY:
        return 1U;
    case RE_A_CAT: {
        u32 l = min_len(a->a);
        u32 r = min_len(a->b);

        return l + r < l ? UINT32_MAX : l + r;
    }
    case RE_A_ALT: {
        u32 l = min_len(a->a);
        u32 r = min_len(a->b);

        return l < r ? l : r;
    }
    case RE_A_PLUS:
        return min_len(a->a);
    case RE_A_REPEAT:
        return a->min == 0U ? 0U : min_len(a->a) * a->min;
    case RE_A_GROUP:
        return min_len(a->a);
    default:
        break;
    }
    return 0U;
}

/*
 * Maximum codepoints any match can consume; UINT32_MAX means unbounded.
 * The mirror of min_len, and its safety runs the other way: an
 * OVERestimate is safe (a wider window still contains the match), an
 * underestimate is a correctness bug, so every saturating case returns
 * UINT32_MAX rather than a wrapped total.
 */
static u32 max_len(const ReAst *a)
{
    if (a == NULL)
        return 0U;
    switch ((ReAstKind)a->kind) {
    case RE_A_CHAR:
    case RE_A_CLASS:
    case RE_A_ANY:
        return 1U;
    case RE_A_CAT: {
        u32 l = max_len(a->a);
        u32 r = max_len(a->b);

        if (l == UINT32_MAX || r == UINT32_MAX || l + r < l)
            return UINT32_MAX;
        return l + r;
    }
    case RE_A_ALT: {
        u32 l = max_len(a->a);
        u32 r = max_len(a->b);

        return l > r ? l : r;
    }
    case RE_A_STAR:
    case RE_A_PLUS:
        return UINT32_MAX;
    case RE_A_QUEST:
        return max_len(a->a);
    case RE_A_REPEAT: {
        u32 inner;

        if (a->max == UINT32_MAX)
            return UINT32_MAX;
        inner = max_len(a->a);
        if (inner == UINT32_MAX || a->max == 0U)
            return inner == UINT32_MAX ? UINT32_MAX : 0U;
        if (inner != 0U && a->max > UINT32_MAX / inner)
            return UINT32_MAX;
        return inner * a->max;
    }
    case RE_A_GROUP:
        return max_len(a->a);
    default:
        break;
    }
    return 0U;
}

static bool build_program(Emit *e, const ReAst *root, bool reverse)
{
    e->reverse = reverse;
    e->n = 0U;
    e->cap = 0U;
    e->prog = NULL;
    e->overflow = false;
    (void)emit(e, RE_SAVE, 0U, 0U, reverse ? 1U : 0U);
    gen(e, root);
    (void)emit(e, RE_SAVE, 0U, 0U, reverse ? 0U : 1U);
    (void)emit(e, RE_MATCH, 0U, 0U, 0U);
    return !e->overflow;
}

/* Literal prefix extraction (§7): walk the leading concatenation while
 * the nodes are plain literals.  Anything else — a class, a quantifier
 * that can match zero, an alternation — ends the prefix. */
static void collect_literal(const ReAst *a, Bytebuf *out, bool *done)
{
    if (a == NULL || *done)
        return;
    switch ((ReAstKind)a->kind) {
    case RE_A_CHAR: {
        u8 buf[YEW_UTF8_MAX];
        size_t n = yew_utf8_encode(a->cp, buf);

        if (n == 0U || out->len + n > 32U) {
            *done = true;
            return;
        }
        bytebuf_append(out, buf, n);
        return;
    }
    case RE_A_CAT:
        collect_literal(a->a, out, done);
        collect_literal(a->b, out, done);
        return;
    case RE_A_GROUP:
        /* A capturing group would make the prefix's offset ambiguous for
         * the anchored re-match, so stop rather than reason about it. */
        *done = true;
        return;
    default:
        break;
    }
    *done = true;
}

static void build_literal(YewRe *re, const ReAst *root, u32 flags)
{
    Bytebuf buf;
    bool done = false;
    bool whole;

    (void)memset(&re->lit, 0, sizeof(re->lit));
    re->lit.kind = RE_LIT_NONE;
    /*
     * Under ICASE only an ASCII prefix is safe to prefilter, and this
     * implementation folds literals into classes at parse time, so an
     * ICASE pattern has no RE_A_CHAR prefix to extract at all.  A
     * prefilter that can miss a match is a correctness bug, not a perf
     * regression, so we simply decline.
     */
    if ((flags & YEW_RE_ICASE) != 0U)
        return;
    bytebuf_init(&buf);
    collect_literal(root, &buf, &done);
    if (buf.len == 0U) {
        bytebuf_free(&buf);
        return;
    }
    /* Whole-pattern-literal means the VM is never entered at all. */
    whole = (flags & YEW_RE_LITERAL) != 0U ||
            (!done && buf.len != 0U);
    (void)memcpy(re->lit.s, buf.data, buf.len);
    re->lit.n = (u32)buf.len;
    if (buf.len == 1U)
        re->lit.kind = RE_LIT_BYTE;
    else
        re->lit.kind = RE_LIT_BMH;
    if (whole)
        re->lit.kind = RE_LIT_WHOLE;
    yew_bmh_build(&re->lit);
    bytebuf_free(&buf);
}

YewRe *yew_re_compile(Arena *a, const char *pat, size_t len, u32 flags,
                      YewReErr *err)
{
    ReParse p;
    ReAst *root;
    Emit e;
    YewRe *re;

    if (err != NULL) {
        err->off = 0U;
        err->msg = NULL;
    }
    if (a == NULL || pat == NULL) {
        if (err != NULL)
            err->msg = "no pattern";
        return NULL;
    }
    if (len > YEW_RE_MAX_PATTERN) {
        if (err != NULL) {
            err->off = YEW_RE_MAX_PATTERN;
            err->msg = "pattern too long (max 64 KiB)";
        }
        return NULL;
    }
    (void)memset(&p, 0, sizeof(p));
    p.arena = a;
    p.pat = (const u8 *)pat;
    p.len = len;
    p.flags = flags;
    p.base_flags = flags;
    p.ngroups = 1U; /* group 0 */
    p.err = err;
    root = yew_re_parse(&p);
    if (root == NULL || p.failed)
        return NULL;

    (void)memset(&e, 0, sizeof(e));
    e.arena = a;
    re = arena_alloc(a, sizeof(*re), sizeof(void *));
    (void)memset(re, 0, sizeof(*re));
    if (!build_program(&e, root, false)) {
        if (err != NULL) {
            err->off = (u32)len;
            err->msg = "pattern too complex (program limit 4096)";
        }
        return NULL;
    }
    re->prog = e.prog;
    re->nprog = e.n;
    if (!build_program(&e, root, true)) {
        if (err != NULL) {
            err->off = (u32)len;
            err->msg = "pattern too complex (program limit 4096)";
        }
        return NULL;
    }
    re->rprog = e.prog;
    re->nrprog = e.n;
    re->classes = p.classes;
    re->nclasses = p.nclasses;
    re->ngroups = p.ngroups;
    re->flags = flags;
    re->saw_upper_literal = p.saw_upper_literal;
    re->force_icase = p.force_icase;
    re->force_case = p.force_case;
    re->min_len = min_len(root);
    re->max_len = max_len(root);
    build_literal(re, root, flags);
    return re;
}

u32 yew_re_group_count(const YewRe *re)
{
    return re == NULL ? 0U : re->ngroups;
}

bool yew_re_has_upper_literal(const YewRe *re)
{
    return re != NULL && re->saw_upper_literal;
}

bool yew_re_forces_icase(const YewRe *re)
{
    return re != NULL && re->force_icase;
}

bool yew_re_forces_case(const YewRe *re)
{
    return re != NULL && re->force_case;
}

u32 yew_re_min_len(const YewRe *re)
{
    return re == NULL ? 0U : re->min_len;
}

static bool is_class_or_any(const ReInst *ins)
{
    return ins->op == RE_CLASS || ins->op == RE_ANY;
}

bool yew_re_is_simple_catch_all(const YewRe *re)
{
    const ReInst *p;

    if (re == NULL || re->prog == NULL || re->min_len > 1U)
        return false;
    p = re->prog;
    /* SAVE, atom, SAVE, MATCH: `.` or one character class. */
    if (re->nprog == 4U && p[0].op == RE_SAVE &&
        is_class_or_any(&p[1]) && p[2].op == RE_SAVE &&
        p[3].op == RE_MATCH)
        return true;
    /* SAVE, atom, SPLIT(atom, exit), SAVE, MATCH: `.+` / `class+`. */
    if (re->nprog == 5U && p[0].op == RE_SAVE &&
        is_class_or_any(&p[1]) && p[2].op == RE_SPLIT &&
        ((p[2].x == 1U && p[2].y == 3U) ||
         (p[2].x == 3U && p[2].y == 1U)) &&
        p[3].op == RE_SAVE && p[4].op == RE_MATCH)
        return true;
    /* SAVE, SPLIT(atom, exit), atom, JMP(split), SAVE, MATCH: `.*`. */
    return re->nprog == 6U && p[0].op == RE_SAVE &&
           p[1].op == RE_SPLIT &&
           ((p[1].x == 2U && p[1].y == 4U) ||
            (p[1].x == 4U && p[1].y == 2U)) &&
           is_class_or_any(&p[2]) && p[3].op == RE_JMP &&
           p[3].x == 1U && p[4].op == RE_SAVE && p[5].op == RE_MATCH;
}

static void first_add(u8 first[32], u8 byte)
{
    first[byte >> 3U] |= (u8)(1U << (byte & 7U));
}

static void first_char(u8 first[32], u32 cp)
{
    u8 encoded[YEW_UTF8_MAX];
    size_t n = yew_utf8_encode(cp, encoded);

    if (n == 0U)
        (void)memset(first, 0xff, 32U);
    else
        first_add(first, encoded[0]);
}

static void first_class(u8 first[32], const ReClass *cls)
{
    u32 byte;

    /* Syntax scans bytes, while regex classes describe codepoints.  Testing
     * all single-byte codepoints gives the exact ASCII portion; any class
     * containing a non-ASCII codepoint conservatively admits every UTF-8
     * lead byte. */
    for (byte = 0U; byte < 128U; byte++) {
        if (yew_re_class_has(cls, byte))
            first_add(first, (u8)byte);
    }
    /* Invalid UTF-8 is decoded losslessly into the surrogate escape band.
     * A negated class such as \S therefore matches those raw bytes too. */
    for (byte = 128U; byte < 256U; byte++) {
        if (yew_re_class_has(cls, yew_utf8_escape_of((u8)byte)))
            first_add(first, (u8)byte);
    }
    for (byte = 0U; byte < cls->n; byte++) {
        u32 lead;

        if (cls->r[byte].hi < 128U)
            continue;
        for (lead = 0xc2U; lead <= 0xf4U; lead++)
            first_add(first, (u8)lead);
        break;
    }
}

void yew_re_first_bytes(const YewRe *re, u8 first[32])
{
    u32 stack[YEW_RE_MAX_PROG];
    u8 seen[YEW_RE_MAX_PROG];
    u32 nstack = 0U;

    if (first == NULL)
        return;
    (void)memset(first, 0, 32U);
    if (re == NULL || re->prog == NULL || re->nprog == 0U ||
        re->nprog > YEW_RE_MAX_PROG) {
        (void)memset(first, 0xff, 32U);
        return;
    }
    (void)memset(seen, 0, sizeof(seen));
    stack[nstack++] = 0U;
    while (nstack != 0U) {
        u32 pc = stack[--nstack];
        const ReInst *ins;

        if (pc >= re->nprog || seen[pc] != 0U)
            continue;
        seen[pc] = 1U;
        ins = &re->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_CHAR:
            first_char(first, ins->arg);
            break;
        case RE_CLASS:
            if (ins->arg >= re->nclasses) {
                (void)memset(first, 0xff, 32U);
                return;
            }
            first_class(first, &re->classes[ins->arg]);
            break;
        case RE_ANY:
            (void)memset(first, 0xff, 32U);
            return;
        case RE_SPLIT:
            if (nstack + 2U > YEW_RE_MAX_PROG) {
                (void)memset(first, 0xff, 32U);
                return;
            }
            stack[nstack++] = ins->x;
            stack[nstack++] = ins->y;
            break;
        case RE_JMP:
            if (nstack == YEW_RE_MAX_PROG) {
                (void)memset(first, 0xff, 32U);
                return;
            }
            stack[nstack++] = ins->x;
            break;
        case RE_SAVE:
            if (nstack == YEW_RE_MAX_PROG) {
                (void)memset(first, 0xff, 32U);
                return;
            }
            stack[nstack++] = pc + 1U;
            break;
        case RE_BOL:
        case RE_BOT:
        case RE_WORDB:
        case RE_NWORDB:
        case RE_EOL:
        case RE_EOT:
        case RE_MATCH:
            /* Empty matches are tried only by the engine's guarded BOL path.
             * All bits keeps the set conservative for other callers. */
            (void)memset(first, 0xff, 32U);
            return;
        }
    }
}

YewReInput yew_re_input_bytes(const u8 *bytes, u64 len)
{
    YewReInput in;

    (void)memset(&in, 0, sizeof(in));
    in.bytes = bytes;
    in.len = len;
    in.window.lo = 0U;
    in.window.hi = len;
    return in;
}

YewReInput yew_re_input_textbuf(const TextBuf *tb)
{
    YewReInput in;

    (void)memset(&in, 0, sizeof(in));
    in.tb = tb;
    in.len = yew_textbuf_len(tb);
    in.window.lo = 0U;
    in.window.hi = in.len;
    return in;
}
