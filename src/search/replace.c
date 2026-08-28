/*
 * Sprint 21 §4.  See replace.h for the plan/apply split and why it
 * exists.
 */
#include "search/replace.h"

#include <stdlib.h>
#include <string.h>

#include "text/edit.h"
#include "text/piece.h"
#include "unicode/case.h"
#include "unicode/category.h"
#include "unicode/coords.h"
#include "unicode/utf8.h"
#include "util/log.h"

/* ---------------------------------------------------------------- */
/* Template expansion                                               */
/* ---------------------------------------------------------------- */

typedef enum {
    CASE_NONE = 0,
    CASE_UPPER, /* \U ... \E */
    CASE_LOWER  /* \L ... \E */
} CaseMode;

typedef struct Emitter {
    Bytebuf *out;
    CaseMode mode;   /* the \U/\L span we are inside */
    CaseMode one;    /* a pending \u/\l, consumed by the next character */
    CaseMode forced; /* the p flag's inferred shape, applied under all */
    bool force_first_only; /* p flag, Titlecase: only the first cased */
    bool seen_cased;
} Emitter;

static u32 case_one(u32 cp, YewCaseKind kind)
{
    u32 mapped[YEW_CASE_MAX_CODEPOINTS];
    u8 n = yew_case_map(cp, kind, mapped);

    /*
     * Simple mappings only: a one-to-many expansion (German ß, the
     * ligatures) would change the length of a replacement in ways the
     * user did not write, so it stays as typed.  Same rationale as
     * Sprint 20 §3.
     */
    return n == 1U ? mapped[0] : cp;
}

/* Appends one codepoint with whatever case state is in effect. */
static void emit_cp(Emitter *e, u32 cp)
{
    CaseMode apply = CASE_NONE;
    u8 buf[YEW_UTF8_MAX];
    size_t n;
    bool cased = (yew_cat_rec(cp) &
                  (YEW_CAT_UPPER | YEW_CAT_LOWER | YEW_CAT_ALPHA)) != 0U;

    if (e->one != CASE_NONE) {
        apply = e->one;
        e->one = CASE_NONE;
    } else if (e->mode != CASE_NONE) {
        apply = e->mode;
    } else if (e->forced != CASE_NONE) {
        /* The p flag never overrides an explicit \u/\U in the template:
         * the user who wrote one was more specific than the heuristic. */
        if (!e->force_first_only)
            apply = e->forced;
        else if (cased && !e->seen_cased)
            apply = e->forced;
    }
    if (cased)
        e->seen_cased = true;
    if (apply == CASE_UPPER)
        cp = case_one(cp, YEW_CASE_UPPER);
    else if (apply == CASE_LOWER)
        cp = case_one(cp, YEW_CASE_LOWER);
    n = yew_utf8_encode(cp, buf);
    bytebuf_append(e->out, buf, n);
}

/* Appends raw bytes from the subject, decoding so case state applies
 * per codepoint rather than per byte. */
static void emit_span(Emitter *e, const YewReInput *in, Span span)
{
    u64 at = span.lo;

    while (at < span.hi) {
        u8 raw[YEW_UTF8_MAX];
        u32 have = 0U;
        u32 cp = 0U;
        size_t used;

        while (have < (u32)YEW_UTF8_MAX && at + have < span.hi) {
            if (!yew_re_input_byte(in, at + have, &raw[have]))
                break;
            have++;
        }
        if (have == 0U)
            break;
        used = yew_utf8_decode(raw, have, &cp);
        if (used == 0U)
            used = 1U;
        emit_cp(e, cp);
        at += used;
    }
}

static bool repl_fail(YewReplErr *err, size_t off, const char *msg)
{
    if (err != NULL) {
        err->off = (u32)off;
        err->msg = msg;
    }
    return false;
}

/*
 * The shape of the matched text, for the p flag.  "All uppercase"
 * requires two or more cased characters, so a single capital does not
 * turn a replacement into a shout.
 */
static void infer_case(Emitter *e, const YewReInput *in, Span m)
{
    u64 at = m.lo;
    u32 cased = 0U;
    u32 upper = 0U;
    u32 lower_after_first = 0U;
    bool first_is_upper = false;

    while (at < m.hi) {
        u8 raw[YEW_UTF8_MAX];
        u32 have = 0U;
        u32 cp = 0U;
        size_t used;
        u16 cat;

        while (have < (u32)YEW_UTF8_MAX && at + have < m.hi) {
            if (!yew_re_input_byte(in, at + have, &raw[have]))
                break;
            have++;
        }
        if (have == 0U)
            break;
        used = yew_utf8_decode(raw, have, &cp);
        if (used == 0U)
            used = 1U;
        cat = yew_cat_rec(cp);
        if ((cat & (YEW_CAT_UPPER | YEW_CAT_LOWER)) != 0U) {
            bool is_upper = (cat & YEW_CAT_UPPER) != 0U;

            if (cased == 0U)
                first_is_upper = is_upper;
            else if (!is_upper)
                lower_after_first++;
            cased++;
            if (is_upper)
                upper++;
        }
        at += used;
    }
    if (cased >= 2U && upper == cased) {
        e->forced = CASE_UPPER;
        return;
    }
    if (first_is_upper && cased >= 1U && upper == 1U &&
        lower_after_first == cased - 1U) {
        e->forced = CASE_UPPER;
        e->force_first_only = true;
        return;
    }
    e->forced = CASE_NONE;
}

/*
 * The one template walker.  `in`/`m` are NULL when only validating, so
 * the grammar cannot drift between the check the command line runs and
 * the expansion the buffer gets.
 */
static bool repl_walk(Bytebuf *out, const char *tpl, size_t tlen,
                      const YewReInput *in, const YewReMatch *m,
                      u32 ngroups, bool preserve_case, YewReplErr *err)
{
    Emitter e;
    size_t i = 0U;

    (void)memset(&e, 0, sizeof(e));
    e.out = out;
    if (preserve_case && in != NULL && m != NULL)
        infer_case(&e, in, m->g[0]);
    while (i < tlen) {
        u8 c = (u8)tpl[i];
        size_t esc = i;

        if (c != '\\') {
            /*
             * `&` lands here: a literal ampersand.  This is the
             * divergence from vim/sed, and it is silent by design —
             * there is no way to warn about text that means exactly
             * what it says.
             */
            if (out != NULL) {
                u32 cp = 0U;
                size_t used = yew_utf8_decode((const u8 *)tpl + i,
                                              tlen - i, &cp);

                emit_cp(&e, cp);
                i += used == 0U ? 1U : used;
            } else {
                i++;
            }
            continue;
        }
        i++;
        if (i >= tlen)
            return repl_fail(err, esc, "trailing backslash in replacement");
        c = (u8)tpl[i];
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            u32 g = (u32)(c - '0');

            i++;
            if (g >= ngroups)
                return repl_fail(err, esc,
                                 "replacement refers to a group the "
                                 "pattern does not have");
            if (out != NULL && m != NULL && in != NULL &&
                m->g[g].lo != UINT64_MAX &&
                m->g[g].hi >= m->g[g].lo)
                emit_span(&e, in, m->g[g]);
            break;
        }
        case '{': {
            u32 g = 0U;
            size_t start = i;
            bool any = false;

            i++;
            while (i < tlen && tpl[i] >= '0' && tpl[i] <= '9') {
                g = g * 10U + (u32)(tpl[i] - '0');
                any = true;
                i++;
                if (g > YEW_RE_MAX_GROUPS)
                    break;
            }
            if (!any || i >= tlen || tpl[i] != '}')
                return repl_fail(err, start,
                                 "expected \\{N} group reference");
            i++;
            if (g >= ngroups)
                return repl_fail(err, esc,
                                 "replacement refers to a group the "
                                 "pattern does not have");
            if (out != NULL && m != NULL && in != NULL &&
                m->g[g].lo != UINT64_MAX &&
                m->g[g].hi >= m->g[g].lo)
                emit_span(&e, in, m->g[g]);
            break;
        }
        case 'n': i++; if (out != NULL) emit_cp(&e, (u32)'\n'); break;
        case 't': i++; if (out != NULL) emit_cp(&e, (u32)'\t'); break;
        case 'r': i++; if (out != NULL) emit_cp(&e, (u32)'\r'); break;
        case '\\': i++; if (out != NULL) emit_cp(&e, (u32)'\\'); break;
        case 'u': i++; e.one = CASE_UPPER; break;
        case 'l': i++; e.one = CASE_LOWER; break;
        case 'U': i++; e.mode = CASE_UPPER; break;
        case 'L': i++; e.mode = CASE_LOWER; break;
        case 'E': i++; e.mode = CASE_NONE; break;
        default: {
            static char msg[48];

            (void)snprintf(msg, sizeof(msg),
                           "unknown replacement escape '\\%c'", (char)c);
            return repl_fail(err, esc, msg);
        }
        }
    }
    return true;
}

bool yew_repl_expand(Bytebuf *out, const char *tpl, size_t tlen,
                     const YewReInput *in, const YewReMatch *m,
                     bool preserve_case, YewReplErr *err)
{
    if (out == NULL || tpl == NULL || m == NULL)
        return false;
    return repl_walk(out, tpl, tlen, in, m, m->ngroups, preserve_case, err);
}

bool yew_repl_check(const char *tpl, size_t tlen, u32 ngroups,
                    YewReplErr *err)
{
    if (tpl == NULL)
        return false;
    return repl_walk(NULL, tpl, tlen, NULL, NULL, ngroups, false, err);
}

/* ---------------------------------------------------------------- */
/* Plan and apply                                                   */
/* ---------------------------------------------------------------- */

void yew_repl_plan_init(YewReplPlan *p)
{
    if (p != NULL)
        (void)memset(p, 0, sizeof(*p));
}

void yew_repl_plan_free(YewReplPlan *p)
{
    u32 i;

    if (p == NULL)
        return;
    for (i = 0U; i < p->len; i++)
        bytebuf_free(&p->v[i].text);
    yew_xfree(p->v);
    (void)memset(p, 0, sizeof(*p));
}

static YewReplEdit *plan_push(YewReplPlan *p)
{
    if (p->len == p->cap) {
        u32 cap = p->cap == 0U ? 16U : p->cap * 2U;

        p->v = yew_xreallocarray(p->v, cap, sizeof(*p->v));
        p->cap = cap;
    }
    (void)memset(&p->v[p->len], 0, sizeof(p->v[p->len]));
    bytebuf_init(&p->v[p->len].text);
    return &p->v[p->len++];
}

bool yew_repl_plan_build(YewReplPlan *p, const YewRe *re, const TextBuf *tb,
                         LineNo lo, LineNo hi, const char *tpl,
                         size_t tlen, u32 flags, YewReplErr *err)
{
    YewReInput in;
    u64 at;
    u64 stop;
    u64 nlines;
    bool have_last_line = false;
    LineNo last_line = LINENO(0U);

    if (p == NULL || re == NULL || tb == NULL || tpl == NULL)
        return false;
    nlines = yew_textbuf_line_count(tb);
    if (nlines == 0U)
        return true;
    if (hi.v >= nlines)
        hi = LINENO(nlines - 1U);
    if (lo.v > hi.v)
        return true;
    /*
     * The input window stays the WHOLE buffer while the loop is bounded
     * by the range.  Narrowing the window to the range instead would
     * redefine what `^`, `\A` and `\b` are measured against, so
     * `:5,10s/\A/x/` would fire at line 5 — the same trap Sprint 20's
     * dispatcher hit.
     */
    in = yew_re_input_textbuf(tb);
    at = yew_textbuf_line_start(tb, lo).v;
    stop = hi.v + 1U < nlines ? yew_textbuf_line_start(tb,
                                                       LINENO(hi.v + 1U)).v
                              : yew_textbuf_len(tb);
    for (;;) {
        YewReMatch m;
        LineNo line;

        (void)memset(&m, 0, sizeof(m));
        if (at > yew_textbuf_len(tb))
            break;
        if (!yew_re_search(re, &in, BYTEOFF(at), &m))
            break;
        if (m.g[0].lo >= stop)
            break;
        line = yew_textbuf_line_of(tb, BYTEOFF(m.g[0].lo));
        if ((flags & YEW_SUB_GLOBAL) == 0U && have_last_line &&
            line.v == last_line.v) {
            /*
             * Without `g` only the first match on a line is replaced,
             * so skip to the next line rather than to the next match —
             * scanning match by match over a line dense with them is
             * work whose result is thrown away.
             */
            if (line.v + 1U >= nlines)
                break;
            at = yew_textbuf_line_start(tb, LINENO(line.v + 1U)).v;
            continue;
        }
        {
            YewReplEdit *ed = plan_push(p);

            ed->span = m.g[0];
            ed->line = line;
            ed->accepted = true;
            if (!yew_repl_expand(&ed->text, tpl, tlen, &in, &m,
                                 (flags & YEW_SUB_PRESERVE) != 0U, err)) {
                yew_repl_plan_free(p);
                return false;
            }
        }
        if (!have_last_line || last_line.v != line.v)
            p->lines++;
        have_last_line = true;
        last_line = line;
        /*
         * A zero-width match must fire — `:%s/^/> /` is the canonical
         * case — but searching again from the same offset would find it
         * forever, so step one grapheme.  One grapheme and not one byte:
         * restarting mid-cluster would let the next match split a
         * combining sequence.
         */
        if (m.g[0].hi == m.g[0].lo) {
            ByteOff next = yew_grapheme_next(tb, BYTEOFF(m.g[0].hi));

            if (next.v <= m.g[0].hi)
                break;
            at = next.v;
        } else {
            at = m.g[0].hi;
        }
    }
    return true;
}

u32 yew_repl_plan_apply(YewReplPlan *p, EditCtx *ec)
{
    u32 applied = 0U;
    u32 i;
    bool own_transaction;

    if (p == NULL || ec == NULL || p->len == 0U)
        return 0U;
    own_transaction = ec->undo->depth == 0U;
    if (own_transaction)
        yew_undo_begin(ec, YEW_TXN_REPLACE);
    /*
     * Back to front: every remaining edit is at a lower offset than the
     * one just applied, so no offset needs adjusting.  Marks and
     * cursors still ride the Sprint 9 choke point either way; this
     * simply removes a whole class of off-by-N bugs from the loop.
     */
    for (i = p->len; i > 0U; i--) {
        YewReplEdit *ed = &p->v[i - 1U];

        if (!ed->accepted)
            continue;
        /*
         * A zero-width match with an empty template changes nothing.
         * Counting it would report replacements that did not happen —
         * `:s/^//` would claim one per line — and, worse, would let a
         * run of only such entries open and close an EMPTY transaction,
         * so the next undo would reach past it to the user's previous
         * edit.
         */
        if (ed->span.hi == ed->span.lo && ed->text.len == 0U)
            continue;
        if (ed->span.hi > ed->span.lo &&
            !yew_edit_delete(ec, ed->span))
            break;
        if (ed->text.len > 0U &&
            !yew_edit_insert(ec, BYTEOFF(ed->span.lo), ed->text.data,
                             ed->text.len))
            break;
        applied++;
    }
    if (own_transaction)
        yew_undo_end(ec);
    return applied;
}

/* ---------------------------------------------------------------- */
/* Confirm                                                          */
/* ---------------------------------------------------------------- */

void yew_repl_confirm_begin(YewReplConfirm *c, YewReplPlan *p)
{
    u32 i;

    if (c == NULL || p == NULL)
        return;
    (void)memset(c, 0, sizeof(*c));
    c->plan = p;
    /* Nothing is approved until asked: the plan arrives all-accepted so
     * that a non-confirm run needs no second pass. */
    for (i = 0U; i < p->len; i++)
        p->v[i].accepted = false;
    c->done = p->len == 0U;
}

bool yew_repl_confirm_pending(const YewReplConfirm *c)
{
    return c != NULL && !c->done && c->plan != NULL &&
           c->at < c->plan->len;
}

const YewReplEdit *yew_repl_confirm_current(const YewReplConfirm *c)
{
    if (!yew_repl_confirm_pending(c))
        return NULL;
    return &c->plan->v[c->at];
}

bool yew_repl_confirm_answer(YewReplConfirm *c, u8 key)
{
    if (!yew_repl_confirm_pending(c))
        return false;
    switch (key) {
    case 'y':
        c->plan->v[c->at].accepted = true;
        c->at++;
        break;
    case 'n':
        c->at++;
        break;
    case 'a': {
        u32 i;

        for (i = c->at; i < c->plan->len; i++)
            c->plan->v[i].accepted = true;
        c->at = c->plan->len;
        break;
    }
    case 'l':
        c->plan->v[c->at].accepted = true;
        c->at = c->plan->len;
        break;
    case 'q':
    case 0x1BU: /* Esc */
        /* Stop, keeping what was already approved.  Aborting the
         * interaction commits the user\'s answers as one step; it does
         * not throw them away. */
        c->at = c->plan->len;
        break;
    default:
        /* ^E / ^Y scroll the view while deciding and are not answers. */
        return false;
    }
    if (c->at >= c->plan->len)
        c->done = true;
    return true;
}
