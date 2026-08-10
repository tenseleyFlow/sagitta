/* Sprint 29: the Fletch parser.  Spec §2's grammar, §1.2's terminator
 * rule, §1.7's `{` rule, and §12's pure-literal entry point. */
#include "fl/parse.h"

#include <string.h>

#include "fl/lex.h"
#include "util/log.h"

typedef struct Parser {
    FlLexer lx;
    FlTok cur;
    FlTokKind prev_kind;
    Arena *arena;
    DiagCtx *dc;
    Interner *in;
    u32 depth;        /* open ( [ { @[ -- §1.2's "syntactically incomplete" */
    u32 rdepth;       /* recursion guard, FL_PARSE_MAX_DEPTH */
    u32 nerrors;
    u64 ntok;         /* tokens consumed; the loops' progress check */
    bool panicking;   /* suppress cascades until the next sync point */
    bool gave_up;     /* hit FL_PARSE_MAX_ERRORS */
    bool incomplete;
    bool had_error;
} Parser;

/* ---------------------------------------------------------------- */
/* Token plumbing and §1.2's terminator rule                        */
/* ---------------------------------------------------------------- */

/* Tokens after which a newline CONTINUES the statement (§1.2): a
 * trailing binary operator, `,` or `=`. */
static bool continues_line(FlTokKind k)
{
    switch (k) {
    case FL_T_PLUS: case FL_T_MINUS: case FL_T_STAR: case FL_T_SLASH:
    case FL_T_PERCENT: case FL_T_EQ: case FL_T_EQEQ: case FL_T_BANGEQ:
    case FL_T_LT: case FL_T_LE: case FL_T_GT: case FL_T_GE:
    case FL_T_COMMA: case FL_T_AND: case FL_T_OR: case FL_T_NOT:
        return true;
    default:
        return false;
    }
}

/*
 * THE one place that decides whether a newline ends a statement.
 *
 * Spec §1.2 makes this rule shared with the Sprint 32 REPL's
 * continuation prompt, and says why in as many words: two
 * implementations of "is this statement finished" drift, and the drift
 * shows up as a REPL that hangs on input the file parser accepts.  So
 * the REPL will call the parser rather than re-derive this, and the
 * logic lives here once.
 */
static bool nl_terminates(const Parser *p)
{
    return p->depth == 0U && !continues_line(p->prev_kind);
}

/* Bracket depth is tracked from the TOKEN STREAM rather than from the
 * parse functions, so a construct that bails out early cannot leave the
 * count wrong. */
static void track_depth(Parser *p, FlTokKind k)
{
    switch (k) {
    /*
     * `{` IS ABSENT ON PURPOSE, and the map literal adds itself.
     *
     * §1.2 lists `{` among the openers that continue a line, but that
     * can only mean a MAP LITERAL: statements inside a block are
     * newline-terminated, as §14's own `fn counter` demonstrates two
     * lines running.  Counting a block's brace here made every
     * statement in every body a continuation of the one before it, and
     * the normative example failed on `let n = start` / `return`.
     * §1.7 is what keeps the two braces distinguishable at all, and the
     * parser is the only thing that knows which one it consumed -- so
     * the depth for a map is taken there rather than from the token.
     */
    case FL_T_LPAREN: case FL_T_LBRACKET:
    case FL_T_ATBRACKET: case FL_M_LPAREN:
        p->depth++;
        break;
    case FL_T_RPAREN: case FL_T_RBRACKET:
    case FL_M_END: case FL_M_RPAREN:
        if (p->depth > 0U)
            p->depth--;
        break;
    default:
        break;
    }
}

static void advance(Parser *p)
{
    p->prev_kind = p->cur.kind;
    track_depth(p, p->cur.kind);
    p->ntok++;
    for (;;) {
        p->cur = fl_lex_next(&p->lx);
        if (p->cur.kind == FL_T_ERROR) {
            /* The lexer already emitted the diagnostic; counting it here
             * keeps the 20-error cap honest without re-reporting. */
            p->had_error = true;
            p->nerrors++;
            if (p->nerrors >= FL_PARSE_MAX_ERRORS) {
                p->gave_up = true;
                /*
                 * Mute HERE too, not only in verror.
                 *
                 * These diagnostics come from the lexer, which reports
                 * through the context directly, and this loop drains a
                 * whole run of them in one call -- so a burst of bad
                 * bytes sailed past the cap that the parser thought it
                 * was enforcing.  fuzz_fl_parse counted 23 against a
                 * cap of 20.
                 */
                if (p->dc != NULL && !p->dc->muted) {
                    p->dc->muted = true;
                    /* Unmute for one message so the reason is visible,
                     * then close the sink for good. */
                    p->dc->muted = false;
                    fl_diag_emit(p->dc, FL_DIAG_ERROR, p->cur.sp,
                                 "too many errors, giving up");
                    p->dc->muted = true;
                }
            }
            continue;
        }
        if (p->cur.kind == FL_T_NEWLINE && !nl_terminates(p))
            continue;
        break;
    }
}

static bool check(const Parser *p, FlTokKind k) { return p->cur.kind == k; }

static bool match(Parser *p, FlTokKind k)
{
    if (!check(p, k))
        return false;
    advance(p);
    return true;
}

/* ---------------------------------------------------------------- */
/* Diagnostics (deliverable 4)                                      */
/* ---------------------------------------------------------------- */

static void verror(Parser *p, FlSpan sp, const char *fmt, va_list ap)
{
    if (p->panicking || p->gave_up)
        return;
    p->had_error = true;
    /*
     * An error and `incomplete` are mutually exclusive.  The REPL reads
     * `incomplete` as "ask for another line"; a real syntax error inside
     * an open bracket that also set it would make the REPL wait forever
     * for a closing token that cannot fix the mistake.
     */
    p->incomplete = false;
    p->nerrors++;
    if (p->nerrors >= FL_PARSE_MAX_ERRORS) {
        p->gave_up = true;
        fl_diag_vemit(p->dc, FL_DIAG_ERROR, sp, fmt, ap);
        fl_diag_emit(p->dc, FL_DIAG_ERROR, sp, "too many errors, giving up");
        /* Past here the lexer would keep reporting through the same
         * context; the cap is only a cap once the sink is closed. */
        if (p->dc != NULL)
            p->dc->muted = true;
        return;
    }
    fl_diag_vemit(p->dc, FL_DIAG_ERROR, sp, fmt, ap);
    p->panicking = true;
}

static void error_at(Parser *p, FlSpan sp, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    verror(p, sp, fmt, ap);
    va_end(ap);
}

static void error_here(Parser *p, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    verror(p, p->cur.sp, fmt, ap);
    va_end(ap);
}

/*
 * The `expected X, found Y` contract, in one place so every message
 * words it the same way.  Goldens pin the phrasing.
 */
static void expected(Parser *p, const char *what)
{
    error_here(p, "expected %s, found '%s'", what,
               fl_tok_spelling(p->cur.kind));
}

/*
 * A missing closer at EOF is the REPL asking for another line, not a
 * mistake.
 *
 * `fn f(` is unfinished; `fn f()}` is wrong.  The difference is only
 * whether the input RAN OUT or something else turned up, so the check
 * is on EOF -- and on nothing having gone wrong yet, because after a
 * real error the same shape means the user needs a message rather than
 * a continuation prompt (spec §1.2's rule, shared with Sprint 32).
 */
static void expect_closer(Parser *p, FlTokKind k, const char *what)
{
    if (match(p, k))
        return;
    if (check(p, FL_T_EOF) && !p->had_error) {
        p->incomplete = true;
        return;
    }
    /*
     * A further OPENER where a closer was due means the user is still
     * nesting rather than mistaken -- `fn f((`.  Run that group out; if
     * it too reaches the end of the input the whole thing is
     * unfinished, and if it closes we are back to a real missing
     * closer and say so.  Bounded by the group, so recovery elsewhere
     * (a `)` that never arrives before the next statement) is
     * untouched: that stops on a keyword, not an opener.
     */
    if (check(p, FL_T_LPAREN) || check(p, FL_T_LBRACKET) ||
        check(p, FL_T_LBRACE) || check(p, FL_T_ATBRACKET)) {
        u32 d0 = p->depth;

        advance(p);
        while (!check(p, FL_T_EOF) && p->depth > d0)
            advance(p);
        if (check(p, FL_T_EOF) && !p->had_error) {
            p->incomplete = true;
            return;
        }
        if (match(p, k))
            return;
    }
    expected(p, what);
}

static bool is_stmt_keyword(FlTokKind k)
{
    switch (k) {
    case FL_T_LET: case FL_T_FN: case FL_T_IF: case FL_T_WHILE:
    case FL_T_FOR: case FL_T_RETURN: case FL_T_BREAK: case FL_T_CONTINUE:
    case FL_T_IMPORT: case FL_T_MACRO: case FL_T_EDIT: case FL_T_TRY:
    case FL_T_CATCH: case FL_T_ELSE:
        return true;
    default:
        return false;
    }
}

/*
 * Panic-mode recovery: discard until something that plausibly starts a
 * new statement, so ONE mistake yields ONE diagnostic.
 *
 * The lexer may still be inside a motion block when a statement-level
 * error is raised, in which case the tokens arriving here are motion
 * tokens; syncing has to run out that block too or every one of them
 * would be reported as a stray motion word.
 */
static void sync(Parser *p, bool in_block)
{
    p->panicking = false;
    while (!check(p, FL_T_EOF)) {
        if (p->prev_kind == FL_T_SEMI)
            return;
        if (check(p, FL_T_NEWLINE)) {
            advance(p);
            return;
        }
        if (check(p, FL_T_SEMI)) {
            advance(p);
            return;
        }
        if (is_stmt_keyword(p->cur.kind))
            return;
        /*
         * A closer stops the sync only when a construct is waiting for
         * it.  Inside a block, eating the `}` would put every later
         * statement in the wrong scope; at the top level nothing owns
         * it, so it is skipped in silence -- reporting a stray closer
         * would be a second diagnostic for one mistake.
         */
        if (in_block &&
            (check(p, FL_T_RBRACE) || check(p, FL_T_RBRACKET) ||
             check(p, FL_T_RPAREN) || check(p, FL_M_END)))
            return;
        advance(p);
    }
}

/* ---------------------------------------------------------------- */
/* Node construction                                                */
/* ---------------------------------------------------------------- */

static FlNode *node(Parser *p, FlAstKind kind, FlSpan sp)
{
    FlNode *n = arena_alloc(p->arena, sizeof(*n), _Alignof(FlNode));

    (void)memset(n, 0, sizeof(*n));
    n->kind = (u8)kind;
    n->sp = sp;
    return n;
}

/*
 * A staging list that becomes a right-sized arena array.
 *
 * ARENA ONLY, in chunks, and never resized in place.  DoD 4 requires
 * the
 * parser to allocate from the caller's arena and nowhere else, and a
 * heap-backed staging vector would satisfy the letter of DoD 4's
 * allocator grep while breaking exactly the property it exists to
 * check.  Chunking is what makes that affordable: growth by
 * doubling would have to copy, and copying into a bump allocator leaks
 * every intermediate size.
 *
 * The FINAL array is a separate, exactly-sized allocation, because
 * ast.h's rule is that a parent holds interior pointers into its child
 * array and nothing may ever move it.
 */
enum { NL_CHUNK = 32 };

typedef struct NodeChunk {
    struct NodeChunk *next;
    u32 n;
    FlNode *v[NL_CHUNK];
} NodeChunk;

typedef struct NodeList {
    NodeChunk *head;
    NodeChunk *tail;
    size_t n;
} NodeList;

static void nl_push(Parser *p, NodeList *l, FlNode *n)
{
    if (l->tail == NULL || l->tail->n == (u32)NL_CHUNK) {
        NodeChunk *c = arena_alloc(p->arena, sizeof(*c), _Alignof(NodeChunk));

        c->next = NULL;
        c->n = 0U;
        if (l->tail == NULL)
            l->head = c;
        else
            l->tail->next = c;
        l->tail = c;
    }
    l->tail->v[l->tail->n++] = n;
    l->n++;
}

static FlNode **nl_freeze(Parser *p, NodeList *l, u32 *out_n)
{
    FlNode **out = NULL;
    size_t at = 0U;
    const NodeChunk *c;

    *out_n = (u32)l->n;
    if (l->n != 0U) {
        out = arena_alloc(p->arena, l->n * sizeof(*out), _Alignof(FlNode *));
        for (c = l->head; c != NULL; c = c->next) {
            (void)memcpy(out + at, c->v, (size_t)c->n * sizeof(*out));
            at += c->n;
        }
    }
    l->head = NULL;
    l->tail = NULL;
    l->n = 0U;
    return out;
}

/* ---------------------------------------------------------------- */
/* Expressions (spec §2's ladder)                                   */
/* ---------------------------------------------------------------- */

static FlNode *parse_expr(Parser *p);
static FlNode *parse_stmt(Parser *p);
static FlNode *parse_block(Parser *p);
static void sync(Parser *p, bool in_block);
static FlNode *parse_motion_block(Parser *p);

/* Binding power for the binary operators, 0 for everything else.  The
 * cascade is spec §2's, and every operator is left-associative. */
static int binding_power(FlTokKind k)
{
    switch (k) {
    case FL_T_OR: return 1;
    case FL_T_AND: return 2;
    case FL_T_EQEQ: case FL_T_BANGEQ: return 3;
    case FL_T_LT: case FL_T_LE: case FL_T_GT: case FL_T_GE: return 4;
    case FL_T_PLUS: case FL_T_MINUS: return 5;
    case FL_T_STAR: case FL_T_SLASH: case FL_T_PERCENT: return 6;
    default: return 0;
    }
}

static bool enter(Parser *p)
{
    if (p->rdepth >= FL_PARSE_MAX_DEPTH) {
        error_here(p, "nesting too deep");
        return false;
    }
    p->rdepth++;
    return true;
}

static void leave(Parser *p) { p->rdepth--; }

/*
 * Bracket depth must be RESTORED by whatever opened it, matched or not.
 *
 * depth is read by nl_terminates: while it is non-zero a newline is
 * layout rather than a terminator.  A construct that reports a missing
 * `)` and gives up therefore leaves the count permanently high, and the
 * rest of the file parses as one endless continued line -- which shows
 * up as a second, baffling diagnostic on a statement that is perfectly
 * well formed.  Each opener snapshots the depth it started at and puts
 * it back on the way out.
 */
static FlNode *parse_list_literal(Parser *p)
{
    FlSpan sp = p->cur.sp;
    u32 depth0 = p->depth;
    NodeList items = {0};
    FlNode *n;

    advance(p); /* [ */
    while (!check(p, FL_T_RBRACKET) && !check(p, FL_T_EOF)) {
        nl_push(p, &items, parse_expr(p));
        if (!match(p, FL_T_COMMA))
            break;
    }
    expect_closer(p, FL_T_RBRACKET, "']'");
    p->depth = depth0;
    n = node(p, FL_A_LIST, sp);
    n->as.list.items = nl_freeze(p, &items, &n->as.list.n);
    return n;
}

static FlNode *parse_map_literal(Parser *p)
{
    FlSpan sp = p->cur.sp;
    u32 depth0 = p->depth;
    NodeList keys = {0};
    NodeList vals = {0};
    FlNode *n;

    /* Raised BEFORE the brace is consumed, for the same reason it is
     * lowered before the closing one: advance() fetches the following
     * token in this very call and decides there whether a newline is
     * layout.  Raising it afterwards delivered the newline that opens a
     * multi-line map and the parse stopped on it. */
    p->depth++;
    advance(p); /* { */
    while (!check(p, FL_T_RBRACE) && !check(p, FL_T_EOF)) {
        FlNode *k;

        /* §2: entry = ( IDENT | STRING | INT ) ":" expr.  A bare word
         * key is the common case and is NOT an identifier reference. */
        if (check(p, FL_T_IDENT) || check(p, FL_T_STRING) ||
            check(p, FL_T_INT)) {
            k = node(p, FL_A_LIT, p->cur.sp);
            if (p->cur.kind == FL_T_INT) {
                k->as.lit.lit = (u8)FL_L_INT;
                k->as.lit.v.i = p->cur.v.i;
            } else {
                k->as.lit.lit = (u8)FL_L_STR;
                k->as.lit.v.str_id = p->cur.v.str_id;
            }
            advance(p);
        } else {
            expected(p, "a map key (identifier, string or integer)");
            break;
        }
        if (!match(p, FL_T_COLON))
            expected(p, "':'");
        nl_push(p, &keys, k);
        nl_push(p, &vals, parse_expr(p));
        if (!match(p, FL_T_COMMA))
            break;
    }
    /*
     * Restored BEFORE the brace is consumed, not after.
     *
     * advance() fetches the next token in the same call that closes the
     * construct, and decides there and then whether a newline is layout
     * -- using the depth as it stands.  Putting the brace back
     * afterwards left that one lookahead reading a depth of one, so the
     * newline ending `let cfg = { ... }` was swallowed and the next
     * statement ran on.  Bracket closers avoid this only because
     * track_depth has already run on them by that point.
     */
    p->depth = depth0;
    expect_closer(p, FL_T_RBRACE, "'}'");
    n = node(p, FL_A_MAP, sp);
    n->as.map.keys = nl_freeze(p, &keys, &n->as.map.n);
    {
        u32 ignored = 0U;

        n->as.map.vals = nl_freeze(p, &vals, &ignored);
    }
    return n;
}

static u32 *parse_params(Parser *p, u32 *nparams)
{
    u32 *out = NULL;
    u32 buf[64];
    u32 n = 0U;

    while (check(p, FL_T_IDENT)) {
        if (n < (u32)YEW_ARRAY_LEN(buf))
            buf[n] = p->cur.v.str_id;
        n++;
        advance(p);
        if (!match(p, FL_T_COMMA))
            break;
    }
    if (n > (u32)YEW_ARRAY_LEN(buf)) {
        error_here(p, "too many parameters (max %zu)", YEW_ARRAY_LEN(buf));
        n = (u32)YEW_ARRAY_LEN(buf);
    }
    if (n != 0U) {
        out = arena_alloc(p->arena, (size_t)n * sizeof(*out),
                          _Alignof(u32));
        (void)memcpy(out, buf, (size_t)n * sizeof(*out));
    }
    *nparams = n;
    return out;
}

static FlNode *parse_fn_expr(Parser *p)
{
    FlSpan sp = p->cur.sp;
    FlNode *n = node(p, FL_A_FN_EXPR, sp);

    advance(p); /* fn */
    if (!match(p, FL_T_LPAREN))
        expected(p, "'('");
    n->as.fn.params = parse_params(p, &n->as.fn.nparams);
    expect_closer(p, FL_T_RPAREN, "')'");
    /* §2: fn_expr = "fn" "(" [params] ")" ( block | expr ) */
    n->as.fn.body = check(p, FL_T_LBRACE) ? parse_block(p) : parse_expr(p);
    return n;
}

static FlNode *parse_primary(Parser *p)
{
    FlSpan sp = p->cur.sp;
    FlNode *n;

    switch (p->cur.kind) {
    case FL_T_NIL:
        n = node(p, FL_A_LIT, sp); n->as.lit.lit = (u8)FL_L_NIL;
        advance(p); return n;
    case FL_T_TRUE:
    case FL_T_FALSE:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_BOOL;
        n->as.lit.v.b = p->cur.kind == FL_T_TRUE;
        advance(p); return n;
    case FL_T_INT:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_INT; n->as.lit.v.i = p->cur.v.i;
        advance(p); return n;
    case FL_T_FLOAT:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_FLOAT; n->as.lit.v.f = p->cur.v.f;
        advance(p); return n;
    case FL_T_STRING:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_STR; n->as.lit.v.str_id = p->cur.v.str_id;
        advance(p); return n;
    case FL_T_IDENT:
        n = node(p, FL_A_IDENT, sp);
        n->as.ident.name = p->cur.v.str_id;
        advance(p); return n;
    case FL_T_LPAREN: {
        u32 depth0 = p->depth;

        advance(p);
        n = parse_expr(p);
        expect_closer(p, FL_T_RPAREN, "')'");
        p->depth = depth0;
        return n;
    }
    case FL_T_LBRACKET:
        return parse_list_literal(p);
    case FL_T_LBRACE:
        return parse_map_literal(p);
    case FL_T_FN:
        return parse_fn_expr(p);
    case FL_T_ATBRACKET:
        return parse_motion_block(p);
    case FL_T_EQ:
        /* Special-cased: the single most common typo in a condition. */
        error_here(p, "'=' is a statement; did you mean '=='?");
        advance(p);
        return node(p, FL_A_LIT, sp);
    default:
        break;
    }
    if (p->cur.kind >= FL_M_COUNT && p->cur.kind <= FL_M_END) {
        /*
         * Reachable after recovery: a statement-level error can leave
         * the LEXER inside a motion block, and its tokens then arrive
         * where an expression was expected.  Naming the block is more
         * use than "unexpected token".
         */
        error_here(p, "motion word outside '@[ ... ]'");
        advance(p);
        return node(p, FL_A_LIT, sp);
    }
    if (check(p, FL_T_EOF) && !p->had_error) {
        /* `let a = 1 +` -- the operator wanted a right-hand side and the
         * input ended.  Unfinished, not wrong. */
        p->incomplete = true;
        return node(p, FL_A_LIT, sp);
    }
    expected(p, "an expression");
    /* Do not advance: the caller's sync decides where to resume, and
     * eating a token here would skip the statement keyword it needs. */
    return node(p, FL_A_LIT, sp);
}

static FlNode *parse_postfix(Parser *p)
{
    FlNode *n = parse_primary(p);

    for (;;) {
        FlSpan sp = p->cur.sp;

        if (check(p, FL_T_LPAREN)) {
            NodeList args = {0};
            FlNode *call;
            u32 depth0 = p->depth;

            advance(p);
            call = node(p, FL_A_CALL, sp);

            while (!check(p, FL_T_RPAREN) && !check(p, FL_T_EOF)) {
                nl_push(p, &args, parse_expr(p));
                if (!match(p, FL_T_COMMA))
                    break;
            }
            expect_closer(p, FL_T_RPAREN, "')'");
            p->depth = depth0;
            call->as.call.callee = n;
            call->as.call.args = nl_freeze(p, &args, &call->as.call.nargs);
            n = call;
            continue;
        }
        if (check(p, FL_T_LBRACKET)) {
            FlNode *idx;
            u32 depth0 = p->depth;

            advance(p);
            idx = node(p, FL_A_INDEX, sp);
            idx->as.index.obj = n;
            idx->as.index.idx = parse_expr(p);
            expect_closer(p, FL_T_RBRACKET, "']'");
            p->depth = depth0;
            n = idx;
            continue;
        }
        if (match(p, FL_T_DOT)) {
            FlNode *fld = node(p, FL_A_FIELD, sp);

            fld->as.field.obj = n;
            if (check(p, FL_T_IDENT)) {
                fld->as.field.name = p->cur.v.str_id;
                advance(p);
            } else {
                expected(p, "a field name");
            }
            n = fld;
            continue;
        }
        break;
    }
    return n;
}

static FlNode *parse_unary(Parser *p)
{
    if (check(p, FL_T_NOT) || check(p, FL_T_MINUS)) {
        FlSpan sp = p->cur.sp;
        FlTokKind op = p->cur.kind;
        FlNode *n;

        advance(p);
        n = node(p, FL_A_UNOP, sp);
        n->as.un.op = (u8)op;
        n->as.un.operand = parse_unary(p); /* right-associative */
        return n;
    }
    return parse_postfix(p);
}

/* Precedence climbing.  Left-associative because the recursive call
 * uses `bp + 1` -- with `bp` it would fold to the right and `1 - 2 - 3`
 * would evaluate as `1 - (2 - 3)`. */
static FlNode *parse_binary(Parser *p, int min_bp)
{
    FlNode *lhs;

    if (!enter(p))
        return node(p, FL_A_LIT, p->cur.sp);
    lhs = parse_unary(p);
    for (;;) {
        int bp = binding_power(p->cur.kind);
        FlTokKind op;
        FlSpan sp;
        FlNode *n;

        if (bp == 0 || bp < min_bp)
            break;
        op = p->cur.kind;
        sp = p->cur.sp;
        advance(p);
        n = node(p, FL_A_BINOP, sp);
        n->as.bin.op = (u8)op;
        n->as.bin.l = lhs;
        n->as.bin.r = parse_binary(p, bp + 1);
        lhs = n;
    }
    leave(p);
    return lhs;
}

static FlNode *parse_expr(Parser *p) { return parse_binary(p, 1); }

/* ---------------------------------------------------------------- */
/* Motion blocks (spec §2's motion productions)                     */
/* ---------------------------------------------------------------- */

static FlNode *parse_motion_seq(Parser *p, NodeList *out, bool nested);

static FlNode *parse_one_motion(Parser *p)
{
    FlSpan sp = p->cur.sp;
    u32 count = 0U;
    FlNode *n;

    if (check(p, FL_M_COUNT)) {
        count = p->cur.v.count;
        advance(p);
    }
    n = node(p, FL_A_MOTION, sp);
    n->as.motion.count = count == 0U ? 1U : count;
    n->as.motion.count_given = count != 0U;
    switch (p->cur.kind) {
    case FL_M_UNIT:
        n->as.motion.mkind = (u8)FL_MK_UNIT;
        n->as.motion.ch = p->cur.v.m.ch;
        advance(p);
        return n;
    case FL_M_ARROW:
        n->as.motion.mkind = (u8)FL_MK_ARROW;
        n->as.motion.ch = p->cur.v.m.ch;
        n->as.motion.alt = p->cur.v.m.alt;
        advance(p);
        return n;
    case FL_M_INSERT:
        n->as.motion.mkind = (u8)FL_MK_INSERT;
        n->as.motion.payload = p->cur.v.str_id;
        advance(p);
        return n;
    case FL_M_DEL:
        n->as.motion.mkind = (u8)FL_MK_DEL;
        advance(p);
        return n;
    case FL_M_ESC:
        n->as.motion.mkind = (u8)FL_MK_ESC;
        advance(p);
        return n;
    case FL_M_WORD:
        n->as.motion.mkind = (u8)FL_MK_WORD;
        n->as.motion.payload = p->cur.v.str_id;
        advance(p);
        return n;
    case FL_M_H: {
        NodeList inner = {0};

        n->as.motion.mkind = (u8)FL_MK_HIGHLIGHT;
        advance(p);
        if (!match(p, FL_M_LPAREN))
            expected(p, "'(' after 'H'");
        (void)parse_motion_seq(p, &inner, true);
        if (!match(p, FL_M_RPAREN))
            expected(p, "')'");
        n->as.motion.inner = nl_freeze(p, &inner, &n->as.motion.ninner);
        return n;
    }
    default:
        expected(p, "a motion word");
        return n;
    }
}

static FlNode *parse_motion_seq(Parser *p, NodeList *out, bool nested)
{
    FlTokKind stop = nested ? FL_M_RPAREN : FL_M_END;

    while (!check(p, stop) && !check(p, FL_T_EOF)) {
        if (!enter(p))
            break;
        nl_push(p, out, parse_one_motion(p));
        leave(p);
        if (p->panicking || p->gave_up)
            break;
    }
    return NULL;
}

static FlNode *parse_motion_block(Parser *p)
{
    FlSpan sp = p->cur.sp;
    NodeList items = {0};
    FlNode *n;
    u32 depth0 = p->depth;

    advance(p); /* @[ */
    (void)parse_motion_seq(p, &items, false);
    expect_closer(p, FL_M_END, "']'");
    p->depth = depth0;
    n = node(p, FL_A_MOTION_BLOCK, sp);
    n->as.list.items = nl_freeze(p, &items, &n->as.list.n);
    return n;
}

/* ---------------------------------------------------------------- */
/* Statements                                                       */
/* ---------------------------------------------------------------- */

/* §1.2: TERM = NEWLINE | ";" | EOF.  Consumes one when present. */
static void expect_term(Parser *p, const char *what)
{
    if (check(p, FL_T_EOF))
        return;
    if (match(p, FL_T_NEWLINE) || match(p, FL_T_SEMI))
        return;
    /* A `}` ends the enclosing block and is a perfectly good place for
     * the last statement of that block to stop. */
    if (check(p, FL_T_RBRACE))
        return;
    error_here(p, "expected end of statement after %s, found '%s'", what,
               fl_tok_spelling(p->cur.kind));
}

static FlNode *parse_block(Parser *p)
{
    FlSpan sp = p->cur.sp;
    NodeList items = {0};
    FlNode *n;
    u64 before;
    u32 depth0 = p->depth;

    if (!match(p, FL_T_LBRACE)) {
        /* Input that simply ran out is unfinished, not wrong: `fn f(`
         * has already flagged its missing `)` as a continuation, and
         * complaining about the absent body as well would turn one
         * unfinished line into a diagnostic the REPL must not show. */
        if (check(p, FL_T_EOF) && !p->had_error)
            p->incomplete = true;
        /* `if x = 1 { }` reaches here with `=` current: the condition
         * swallowed `x` and the assignment is the actual mistake, so
         * say so rather than complain about the missing brace. */
        else if (check(p, FL_T_EQ))
            error_here(p, "'=' is a statement; did you mean '=='?");
        else
            expected(p, "'{'");
        return node(p, FL_A_BLOCK, sp);
    }
    while (!check(p, FL_T_RBRACE) && !check(p, FL_T_EOF)) {
        FlNode *s;

        if (match(p, FL_T_NEWLINE) || match(p, FL_T_SEMI))
            continue;
        if (!enter(p))
            break;
        before = p->ntok;
        s = parse_stmt(p);
        leave(p);
        if (s != NULL)
            nl_push(p, &items, s);
        if (p->panicking)
            sync(p, true);
        if (p->gave_up)
            break;
        /*
         * FORWARD PROGRESS, unconditionally.
         *
         * A statement that reports an error without consuming anything
         * -- `}` where a block was expected, say -- would otherwise be
         * re-parsed forever, and what the user would see is the same
         * message twenty times followed by "too many errors".  Skipping
         * the token silently keeps one mistake to one diagnostic.
         */
        if (p->ntok == before)
            advance(p);
    }
    expect_closer(p, FL_T_RBRACE, "'}'");
    p->depth = depth0;
    n = node(p, FL_A_BLOCK, sp);
    n->as.list.items = nl_freeze(p, &items, &n->as.list.n);
    return n;
}

/*
 * A block is self-terminating, so a newline after its `}` is layout.
 *
 * §2 puts no TERM between a block and the `else` or `catch` that
 * continues it, and §14 writes both on the following line.  Consuming
 * the newline is safe even when the next token turns out to start a
 * fresh statement: the block already ended the previous one.
 */
static void skip_block_newlines(Parser *p)
{
    while (check(p, FL_T_NEWLINE))
        advance(p);
}

static bool is_assign_target(const FlNode *n)
{
    return n->kind == (u8)FL_A_IDENT || n->kind == (u8)FL_A_INDEX ||
           n->kind == (u8)FL_A_FIELD;
}

static FlNode *parse_stmt(Parser *p)
{
    FlSpan sp = p->cur.sp;
    FlNode *n;

    switch (p->cur.kind) {
    case FL_T_LET:
        advance(p);
        n = node(p, FL_A_LET, sp);
        if (check(p, FL_T_IDENT)) {
            n->as.let.name = p->cur.v.str_id;
            advance(p);
        } else {
            expected(p, "a name after 'let'");
        }
        if (match(p, FL_T_EQ))
            n->as.let.init = parse_expr(p);
        expect_term(p, "'let'");
        return n;
    case FL_T_FN:
        advance(p);
        n = node(p, FL_A_FN, sp);
        if (check(p, FL_T_IDENT)) {
            n->as.fn.name = p->cur.v.str_id;
            advance(p);
        } else {
            expected(p, "a function name");
        }
        if (!match(p, FL_T_LPAREN))
            expected(p, "'('");
        n->as.fn.params = parse_params(p, &n->as.fn.nparams);
        expect_closer(p, FL_T_RPAREN, "')'");
        n->as.fn.body = parse_block(p);
        return n;
    case FL_T_MACRO:
        advance(p);
        n = node(p, FL_A_MACRO, sp);
        if (check(p, FL_T_IDENT)) {
            n->as.macro.name = p->cur.v.str_id;
            advance(p);
        } else {
            expected(p, "a macro name");
        }
        if (!match(p, FL_T_EQ))
            expected(p, "'='");
        if (check(p, FL_T_ATBRACKET))
            n->as.macro.body = parse_motion_block(p);
        else
            expected(p, "a motion block");
        expect_term(p, "'macro'");
        return n;
    case FL_T_IMPORT:
        advance(p);
        n = node(p, FL_A_IMPORT, sp);
        if (check(p, FL_T_IDENT)) {
            n->as.import.name = p->cur.v.str_id;
            advance(p);
        } else if (check(p, FL_T_STRING)) {
            n->as.import.path = p->cur.v.str_id;
            n->as.import.is_string = true;
            advance(p);
            if (!match(p, FL_T_AS))
                expected(p, "'as'");
            if (check(p, FL_T_IDENT)) {
                n->as.import.name = p->cur.v.str_id;
                advance(p);
            } else {
                expected(p, "a name after 'as'");
            }
        } else {
            expected(p, "a module name or a quoted path");
        }
        expect_term(p, "'import'");
        return n;
    case FL_T_IF:
        advance(p);
        n = node(p, FL_A_IF, sp);
        n->as.ifs.cond = parse_expr(p);
        n->as.ifs.then = parse_block(p);
        skip_block_newlines(p);
        if (match(p, FL_T_ELSE)) {
            /* `else if` chains by nesting, which is what §2's
             * `{ "else" "if" expr block }` describes. */
            n->as.ifs.els = check(p, FL_T_IF) ? parse_stmt(p)
                                              : parse_block(p);
        }
        return n;
    case FL_T_WHILE:
        advance(p);
        n = node(p, FL_A_WHILE, sp);
        n->as.whiles.cond = parse_expr(p);
        n->as.whiles.body = parse_block(p);
        return n;
    case FL_T_FOR:
        advance(p);
        n = node(p, FL_A_FOR, sp);
        if (check(p, FL_T_IDENT)) {
            n->as.fors.var = p->cur.v.str_id;
            advance(p);
        } else {
            expected(p, "a loop variable");
        }
        if (match(p, FL_T_COMMA)) {
            if (check(p, FL_T_IDENT)) {
                n->as.fors.var2 = p->cur.v.str_id;
                advance(p);
            } else {
                expected(p, "a second loop variable");
            }
        }
        if (!match(p, FL_T_IN))
            expected(p, "'in'");
        n->as.fors.iter = parse_expr(p);
        n->as.fors.body = parse_block(p);
        return n;
    case FL_T_RETURN:
        advance(p);
        n = node(p, FL_A_RETURN, sp);
        if (!check(p, FL_T_NEWLINE) && !check(p, FL_T_SEMI) &&
            !check(p, FL_T_EOF) && !check(p, FL_T_RBRACE))
            n->as.ret.value = parse_expr(p);
        expect_term(p, "'return'");
        return n;
    case FL_T_BREAK:
        advance(p);
        expect_term(p, "'break'");
        return node(p, FL_A_BREAK, sp);
    case FL_T_CONTINUE:
        advance(p);
        expect_term(p, "'continue'");
        return node(p, FL_A_CONTINUE, sp);
    case FL_T_EDIT:
        advance(p);
        n = node(p, FL_A_EDIT, sp);
        n->as.edit.body = parse_block(p);
        return n;
    case FL_T_TRY:
        advance(p);
        n = node(p, FL_A_TRY, sp);
        n->as.trys.body = parse_block(p);
        skip_block_newlines(p);
        if (!match(p, FL_T_CATCH))
            expected(p, "'catch'");
        if (check(p, FL_T_IDENT)) {
            n->as.trys.var = p->cur.v.str_id;
            advance(p);
        } else {
            expected(p, "an error variable after 'catch'");
        }
        n->as.trys.handler = parse_block(p);
        return n;
    case FL_T_LBRACE:
        /*
         * Spec §1.7: a `{` in statement position is a hard error, and
         * the message carries the fix.  Blocks only ever follow a
         * keyword, so a bare `{` is always a mistake.
         */
        error_here(p, "map literal cannot start a statement; parenthesize");
        return node(p, FL_A_EXPR_STMT, sp);
    default:
        break;
    }

    /* assign_stmt and expr_stmt share a prefix: parse the expression,
     * then let the `=` decide which it was. */
    {
        FlNode *lhs = parse_expr(p);

        if (check(p, FL_T_EQ)) {
            FlSpan eq = p->cur.sp;

            advance(p);
            if (!is_assign_target(lhs))
                error_at(p, eq, "cannot assign to this expression");
            n = node(p, FL_A_ASSIGN, sp);
            n->as.assign.tgt = lhs;
            n->as.assign.val = parse_expr(p);
            expect_term(p, "assignment");
            return n;
        }
        n = node(p, FL_A_EXPR_STMT, sp);
        n->as.expr_stmt.expr = lhs;
        expect_term(p, "expression");
        return n;
    }
}

/* ---------------------------------------------------------------- */
/* Entry points                                                     */
/* ---------------------------------------------------------------- */

static void parser_init(Parser *p, Arena *a, DiagCtx *dc, Interner *in,
                        const char *src, size_t len, u32 file_id)
{
    (void)memset(p, 0, sizeof(*p));
    p->arena = a;
    p->dc = dc;
    p->in = in;
    p->prev_kind = FL_T_NEWLINE;
    /* Muting is per-parse: a context reused for a second entry point
     * must not inherit the first one's silence. */
    if (dc != NULL)
        dc->muted = false;
    fl_lex_init(&p->lx, a, dc, in, src, len, file_id);
    advance(p);
}

FlProgram fl_parse(Arena *a, DiagCtx *dc, Interner *in,
                   const char *src, size_t len, u32 file_id)
{
    Parser p;
    NodeList stmts = {0};
    FlProgram prog;
    u64 before;

    (void)memset(&prog, 0, sizeof(prog));
    parser_init(&p, a, dc, in, src, len, file_id);
    while (!check(&p, FL_T_EOF) && !p.gave_up) {
        FlNode *s;

        if (match(&p, FL_T_NEWLINE) || match(&p, FL_T_SEMI))
            continue;
        before = p.ntok;
        s = parse_stmt(&p);
        if (s != NULL)
            nl_push(&p, &stmts, s);
        if (p.panicking)
            sync(&p, false);
        if (p.ntok == before)
            advance(&p);          /* see parse_block: progress or spin */
    }
    /*
     * EOF with brackets still open, and nothing wrong so far, is the
     * REPL's continuation signal -- not an error.  The order matters:
     * verror() clears `incomplete`, so a run that reported anything can
     * never end up claiming to be merely unfinished.
     */
    if (p.depth > 0U && !p.had_error)
        prog.incomplete = true;
    if (continues_line(p.prev_kind) && !p.had_error)
        prog.incomplete = true;
    if (p.incomplete && !p.had_error)
        prog.incomplete = true;
    prog.had_error = p.had_error;
    if (prog.had_error)
        prog.incomplete = false;
    prog.stmts = nl_freeze(&p, &stmts, &prog.n);
    return prog;
}

/* ---------------------------------------------------------------- */
/* §12 pure-literal mode                                            */
/* ---------------------------------------------------------------- */

static FlNode *parse_pl_value(Parser *p);

static FlNode *pl_reject(Parser *p)
{
    error_here(p, "pure-literal mode: '%s' not allowed",
               fl_tok_spelling(p->cur.kind));
    advance(p);
    return NULL;
}

static FlNode *parse_pl_list(Parser *p)
{
    FlSpan sp = p->cur.sp;
    NodeList items = {0};
    FlNode *n;

    advance(p); /* [ */
    while (!check(p, FL_T_RBRACKET) && !check(p, FL_T_EOF)) {
        FlNode *v = parse_pl_value(p);

        if (v == NULL)
            break;
        nl_push(p, &items, v);
        if (!match(p, FL_T_COMMA))
            break;                       /* §12: trailing comma allowed */
    }
    expect_closer(p, FL_T_RBRACKET, "']'");
    n = node(p, FL_A_LIST, sp);
    n->as.list.items = nl_freeze(p, &items, &n->as.list.n);
    return n;
}

static FlNode *parse_pl_map(Parser *p)
{
    FlSpan sp = p->cur.sp;
    u32 depth0 = p->depth;
    NodeList keys = {0};
    NodeList vals = {0};
    FlNode *n;

    /* Raised BEFORE the brace is consumed, for the same reason it is
     * lowered before the closing one: advance() fetches the following
     * token in this very call and decides there whether a newline is
     * layout.  Raising it afterwards delivered the newline that opens a
     * multi-line map and the parse stopped on it. */
    p->depth++;
    advance(p); /* { */
    while (!check(p, FL_T_RBRACE) && !check(p, FL_T_EOF)) {
        FlNode *k;
        FlNode *v;

        /*
         * IDENT appears HERE and nowhere else in §12: as a bare map
         * key, never as a value.  That single restriction is most of
         * what makes the mode grant nothing -- there is no production
         * by which a name can be looked up.
         */
        if (check(p, FL_T_IDENT) || check(p, FL_T_STRING) ||
            check(p, FL_T_INT)) {
            k = node(p, FL_A_LIT, p->cur.sp);
            if (p->cur.kind == FL_T_INT) {
                k->as.lit.lit = (u8)FL_L_INT;
                k->as.lit.v.i = p->cur.v.i;
            } else {
                k->as.lit.lit = (u8)FL_L_STR;
                k->as.lit.v.str_id = p->cur.v.str_id;
            }
            advance(p);
        } else {
            (void)pl_reject(p);
            break;
        }
        if (!match(p, FL_T_COLON))
            expected(p, "':'");
        v = parse_pl_value(p);
        if (v == NULL)
            break;
        nl_push(p, &keys, k);
        nl_push(p, &vals, v);
        if (!match(p, FL_T_COMMA))
            break;
    }
    /*
     * Restored BEFORE the brace is consumed, not after.
     *
     * advance() fetches the next token in the same call that closes the
     * construct, and decides there and then whether a newline is layout
     * -- using the depth as it stands.  Putting the brace back
     * afterwards left that one lookahead reading a depth of one, so the
     * newline ending `let cfg = { ... }` was swallowed and the next
     * statement ran on.  Bracket closers avoid this only because
     * track_depth has already run on them by that point.
     */
    p->depth = depth0;
    expect_closer(p, FL_T_RBRACE, "'}'");
    n = node(p, FL_A_MAP, sp);
    n->as.map.keys = nl_freeze(p, &keys, &n->as.map.n);
    {
        u32 ignored = 0U;

        n->as.map.vals = nl_freeze(p, &vals, &ignored);
    }
    return n;
}

static FlNode *parse_pl_value(Parser *p)
{
    FlSpan sp = p->cur.sp;
    FlNode *n;
    bool negate = false;

    if (!enter(p))
        return NULL;
    /* §12: pl_value = ... | [ "-" ] INT | [ "-" ] FLOAT | ...  The sign
     * is part of the LITERAL, not a unary operator -- §12.1 says no
     * operator is reachable, and folding it here is what keeps that
     * true rather than merely claimed. */
    if (check(p, FL_T_MINUS)) {
        negate = true;
        advance(p);
        if (!check(p, FL_T_INT) && !check(p, FL_T_FLOAT)) {
            leave(p);
            expected(p, "a number after '-'");
            return NULL;
        }
    }
    switch (p->cur.kind) {
    case FL_T_NIL:
        n = node(p, FL_A_LIT, sp); n->as.lit.lit = (u8)FL_L_NIL;
        advance(p); break;
    case FL_T_TRUE:
    case FL_T_FALSE:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_BOOL;
        n->as.lit.v.b = p->cur.kind == FL_T_TRUE;
        advance(p); break;
    case FL_T_INT:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_INT;
        n->as.lit.v.i = negate ? -p->cur.v.i : p->cur.v.i;
        advance(p); break;
    case FL_T_FLOAT:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_FLOAT;
        n->as.lit.v.f = negate ? -p->cur.v.f : p->cur.v.f;
        advance(p); break;
    case FL_T_STRING:
        n = node(p, FL_A_LIT, sp);
        n->as.lit.lit = (u8)FL_L_STR;
        n->as.lit.v.str_id = p->cur.v.str_id;
        advance(p); break;
    case FL_T_LBRACKET:
        n = parse_pl_list(p); break;
    case FL_T_LBRACE:
        n = parse_pl_map(p); break;
    default:
        leave(p);
        return pl_reject(p);
    }
    leave(p);
    return n;
}

FlNode *fl_parse_literal(Arena *a, DiagCtx *dc, Interner *in,
                         const char *src, size_t len, u32 file_id)
{
    Parser p;
    FlNode *v;

    parser_init(&p, a, dc, in, src, len, file_id);
    while (match(&p, FL_T_NEWLINE))
        ;
    v = parse_pl_value(&p);
    while (match(&p, FL_T_NEWLINE))
        ;
    if (v != NULL && !check(&p, FL_T_EOF)) {
        error_here(&p, "pure-literal mode: '%s' not allowed",
                   fl_tok_spelling(p.cur.kind));
        v = NULL;
    }
    return p.had_error ? NULL : v;
}
