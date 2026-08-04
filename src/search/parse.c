/* Sprint 20 §2/§8: pattern text -> AST, with precise error offsets. */
#include "search/regex_internal.h"

#include <stdio.h>
#include <string.h>

#include "unicode/case.h"
#include "unicode/utf8.h"
#include "unicode/wordbreak.h"
#include "util/log.h"
#include "util/sort.h"

/* ---------------------------------------------------------------- */
/* Character properties (§3)                                        */
/* ---------------------------------------------------------------- */

bool sag_re_is_word(u32 cp)
{
    switch (sag_wb_prop(cp)) {
    case SAG_WB_ALETTER:
    case SAG_WB_HEBREW_LETTER:
    case SAG_WB_NUMERIC:
    case SAG_WB_KATAKANA:
    /* EXTENDNUMLET is the Pc category, which is where '_' lives. */
    case SAG_WB_EXTENDNUMLET:
        return true;
    default:
        break;
    }
    return false;
}

bool sag_re_is_digit(u32 cp)
{
    return sag_wb_prop(cp) == SAG_WB_NUMERIC;
}

bool sag_re_is_space(u32 cp)
{
    return sag_unicode_is_white_space(cp);
}

/* ---------------------------------------------------------------- */
/* Errors                                                           */
/* ---------------------------------------------------------------- */

void sag_re_fail(ReParse *p, size_t off, const char *msg)
{
    if (p->failed)
        return;
    p->failed = true;
    if (p->err != NULL) {
        p->err->off = (u32)(off > p->len ? p->len : off);
        p->err->msg = msg;
    }
}

/* ---------------------------------------------------------------- */
/* Class construction                                               */
/* ---------------------------------------------------------------- */

static int range_cmp(const void *a, const void *b, void *ctx)
{
    const ReRange *ra = a;
    const ReRange *rb = b;

    (void)ctx;
    if (ra->lo != rb->lo)
        return ra->lo < rb->lo ? -1 : 1;
    if (ra->hi != rb->hi)
        return ra->hi < rb->hi ? -1 : 1;
    return 0;
}

/* Sorts, merges overlaps, and optionally complements over the Unicode
 * scalar space extended to cover the invalid-byte escapes: a negated
 * class must match U+DC80..U+DCFF so `[^x]` still matches a raw byte in
 * a binary file rather than silently failing (§3). */
static u32 normalize(ReParse *p, ReRange *in, u32 n, bool negate,
                     ReRange **out)
{
    ReRange *tmp;
    u32 w = 0U;
    u32 i;

    if (n != 0U)
        sag_sort_stable(in, n, sizeof(*in), range_cmp, NULL);
    tmp = arena_alloc(p->arena, (size_t)(n + 2U) * sizeof(*tmp),
                      sizeof(u32));
    for (i = 0U; i < n; i++) {
        if (w != 0U && in[i].lo <= tmp[w - 1U].hi + 1U) {
            if (in[i].hi > tmp[w - 1U].hi)
                tmp[w - 1U].hi = in[i].hi;
            continue;
        }
        tmp[w++] = in[i];
    }
    if (!negate) {
        *out = tmp;
        return w;
    }
    {
        ReRange *neg = arena_alloc(p->arena,
                                   (size_t)(w + 1U) * sizeof(*neg),
                                   sizeof(u32));
        u32 nn = 0U;
        u32 next = 0U;

        for (i = 0U; i < w; i++) {
            if (tmp[i].lo > next) {
                neg[nn].lo = next;
                neg[nn].hi = tmp[i].lo - 1U;
                nn++;
            }
            if (tmp[i].hi + 1U > next)
                next = tmp[i].hi + 1U;
        }
        if (next <= 0x10FFFFU) {
            neg[nn].lo = next;
            neg[nn].hi = 0x10FFFFU;
            nn++;
        }
        *out = neg;
        return nn;
    }
}

u32 sag_re_class_intern(ReParse *p, ReRange *ranges, u32 n, bool negate)
{
    ReRange *norm = NULL;
    u32 count;

    if (p->nclasses >= SAG_RE_MAX_CLASSES) {
        sag_re_fail(p, p->at, "too many character classes (max 256)");
        return UINT32_MAX;
    }
    count = normalize(p, ranges, n, negate, &norm);
    if (p->nclasses == p->ncap_classes) {
        u32 cap = p->ncap_classes == 0U ? 8U : p->ncap_classes * 2U;
        ReClass *grown = arena_alloc(p->arena,
                                     (size_t)cap * sizeof(*grown),
                                     sizeof(void *));

        if (p->nclasses != 0U)
            (void)memcpy(grown, p->classes,
                         (size_t)p->nclasses * sizeof(*grown));
        p->classes = grown;
        p->ncap_classes = cap;
    }
    p->classes[p->nclasses].r = norm;
    p->classes[p->nclasses].n = count;
    return p->nclasses++;
}

bool sag_re_class_has(const ReClass *c, u32 cp)
{
    u32 lo = 0U;
    u32 hi = c->n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;

        if (cp < c->r[mid].lo)
            hi = mid;
        else if (cp > c->r[mid].hi)
            lo = mid + 1U;
        else
            return true;
    }
    return false;
}

/* Materializes a Perl shorthand as explicit ranges.  Enumerating the
 * whole scalar space would be 1.1M probes per class, so the sets are
 * built by scanning for property runs once at compile time. */
static u32 build_prop_ranges(ReParse *p, bool (*pred)(u32), ReRange **out)
{
    /* Property runs are sparse; 4096 is far above what \w/\d/\s need and
     * the count is asserted rather than assumed. */
    enum { MAX_RUNS = 4096 };
    ReRange *r = arena_alloc(p->arena, MAX_RUNS * sizeof(*r), sizeof(u32));
    u32 n = 0U;
    u32 cp = 0U;

    while (cp <= 0x10FFFFU) {
        if (!pred(cp)) {
            cp++;
            continue;
        }
        if (n == MAX_RUNS)
            SAG_BUG("regex: property run table overflow");
        r[n].lo = cp;
        while (cp + 1U <= 0x10FFFFU && pred(cp + 1U))
            cp++;
        r[n].hi = cp;
        n++;
        cp++;
    }
    *out = r;
    return n;
}

u32 sag_re_class_perl(ReParse *p, char which)
{
    ReRange *r = NULL;
    u32 n;
    bool negate = which == 'W' || which == 'D' || which == 'S';
    char base = negate ? (char)(which + 32) : which;

    switch (base) {
    case 'w':
        n = build_prop_ranges(p, sag_re_is_word, &r);
        break;
    case 'd':
        n = build_prop_ranges(p, sag_re_is_digit, &r);
        break;
    default:
        n = build_prop_ranges(p, sag_re_is_space, &r);
        break;
    }
    return sag_re_class_intern(p, r, n, negate);
}

/* Simple folding only: a length-changing fold (ss -> ß) would break the
 * one-codepoint-per-step invariant the linear-time VM depends on. */
u32 sag_re_fold_partners(u32 cp, u32 out[4])
{
    u32 n = 0U;
    u32 mapped[SAG_CASE_MAX_CODEPOINTS];
    u32 probe;
    u8 got;

    out[n++] = cp;
    got = sag_case_map(cp, SAG_CASE_LOWER, mapped);
    if (got == 1U && mapped[0] != cp)
        out[n++] = mapped[0];
    got = sag_case_map(cp, SAG_CASE_UPPER, mapped);
    if (got == 1U && mapped[0] != cp) {
        u32 i;
        bool dup = false;

        for (i = 0U; i < n; i++)
            dup = dup || out[i] == mapped[0];
        if (!dup)
            out[n++] = mapped[0];
    }
    /* Kelvin sign and the long s fold onto ASCII letters; the simple
     * maps above do not round-trip them, so probe the two known singles
     * rather than pretend they do not exist. */
    probe = cp == 'k' || cp == 'K' ? 0x212AU :
            (cp == 's' || cp == 'S' ? 0x017FU : 0U);
    if (probe != 0U && n < 4U)
        out[n++] = probe;
    return n;
}

/* ---------------------------------------------------------------- */
/* Lexing helpers                                                   */
/* ---------------------------------------------------------------- */

static bool at_end(const ReParse *p)
{
    return p->at >= p->len;
}

static u8 peek(const ReParse *p)
{
    return p->at < p->len ? p->pat[p->at] : 0U;
}

static u8 peek_at(const ReParse *p, size_t ahead)
{
    return p->at + ahead < p->len ? p->pat[p->at + ahead] : 0U;
}

/* Decodes one codepoint, advancing.  Invalid bytes become escapes so a
 * pattern typed with a stray byte still compiles to something exact. */
static u32 take_cp(ReParse *p)
{
    u32 cp = 0U;
    size_t n = sag_utf8_decode(p->pat + p->at, p->len - p->at, &cp);

    p->at += n == 0U ? 1U : n;
    return cp;
}

static ReAst *node(ReParse *p, ReAstKind kind)
{
    ReAst *a = arena_alloc(p->arena, sizeof(*a), sizeof(void *));

    (void)memset(a, 0, sizeof(*a));
    a->kind = (u8)kind;
    a->greedy = true;
    return a;
}

static bool parse_hex_digits(ReParse *p, size_t count, u32 *out)
{
    size_t i;
    u32 value = 0U;

    for (i = 0U; i < count; i++) {
        u8 c = peek(p);
        u32 digit;

        if (c >= '0' && c <= '9')
            digit = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (u32)(c - 'a') + 10U;
        else if (c >= 'A' && c <= 'F')
            digit = (u32)(c - 'A') + 10U;
        else
            return false;
        value = value * 16U + digit;
        p->at++;
    }
    *out = value;
    return true;
}

/* \xHH and \x{H..}.  Returns false with the error already recorded. */
static bool parse_hex_escape(ReParse *p, size_t start, u32 *out)
{
    if (peek(p) == '{') {
        u32 value = 0U;
        size_t digits = 0U;

        p->at++;
        while (!at_end(p) && peek(p) != '}') {
            u32 one;

            if (!parse_hex_digits(p, 1U, &one) || digits >= 6U) {
                sag_re_fail(p, start, "invalid codepoint escape");
                return false;
            }
            value = value * 16U + one;
            digits++;
        }
        if (digits == 0U || at_end(p) || peek(p) != '}' ||
            value > 0x10FFFFU) {
            sag_re_fail(p, start, "invalid codepoint escape");
            return false;
        }
        p->at++;
        *out = value;
        return true;
    }
    if (!parse_hex_digits(p, 2U, out)) {
        sag_re_fail(p, start, "invalid codepoint escape");
        return false;
    }
    return true;
}

/* Shared by pattern-level and class-level escapes.  `cls_out` receives a
 * class index when the escape denotes a set; otherwise `cp_out`. */
static bool parse_escape(ReParse *p, u32 *cp_out, u32 *cls_out)
{
    size_t start = p->at - 1U; /* the backslash */
    u8 c;

    *cls_out = UINT32_MAX;
    if (at_end(p)) {
        sag_re_fail(p, start, "trailing backslash");
        return false;
    }
    c = peek(p);
    switch (c) {
    case 'n': p->at++; *cp_out = '\n'; return true;
    case 't': p->at++; *cp_out = '\t'; return true;
    case 'r': p->at++; *cp_out = '\r'; return true;
    case 'f': p->at++; *cp_out = '\f'; return true;
    case 'v': p->at++; *cp_out = 0x0BU; return true;
    case 'a': p->at++; *cp_out = 0x07U; return true;
    case 'e': p->at++; *cp_out = 0x1BU; return true;
    case '0': p->at++; *cp_out = 0U; return true;
    case 'x':
        p->at++;
        return parse_hex_escape(p, start, cp_out);
    case 'w': case 'W': case 'd': case 'D': case 's': case 'S':
        p->at++;
        *cls_out = sag_re_class_perl(p, (char)c);
        return !p->failed;
    default:
        break;
    }
    /* Metacharacters and punctuation escape to themselves. */
    if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
        (c >= '[' && c <= '`') || (c >= '{' && c <= '~')) {
        p->at++;
        *cp_out = c;
        return true;
    }
    {
        static char msg[32];

        (void)snprintf(msg, sizeof(msg), "unknown escape '\\%c'",
                       (char)c);
        sag_re_fail(p, start, msg);
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* Character classes                                                */
/* ---------------------------------------------------------------- */

typedef struct ClassBuf {
    ReRange *r;
    u32 n;
    u32 cap;
} ClassBuf;

static void cbuf_add(ReParse *p, ClassBuf *b, u32 lo, u32 hi)
{
    if (b->n == b->cap) {
        u32 cap = b->cap == 0U ? 16U : b->cap * 2U;
        ReRange *grown = arena_alloc(p->arena,
                                     (size_t)cap * sizeof(*grown),
                                     sizeof(u32));

        if (b->n != 0U)
            (void)memcpy(grown, b->r, (size_t)b->n * sizeof(*grown));
        b->r = grown;
        b->cap = cap;
    }
    b->r[b->n].lo = lo;
    b->r[b->n].hi = hi;
    b->n++;
}

static void cbuf_add_class(ReParse *p, ClassBuf *b, u32 idx)
{
    const ReClass *c;
    u32 i;

    if (idx == UINT32_MAX || idx >= p->nclasses)
        return;
    c = &p->classes[idx];
    for (i = 0U; i < c->n; i++)
        cbuf_add(p, b, c->r[i].lo, c->r[i].hi);
}

static void cbuf_add_folded(ReParse *p, ClassBuf *b, u32 cp)
{
    if ((p->flags & SAG_RE_ICASE) != 0U) {
        u32 partners[4];
        u32 n = sag_re_fold_partners(cp, partners);
        u32 i;

        for (i = 0U; i < n; i++)
            cbuf_add(p, b, partners[i], partners[i]);
        return;
    }
    cbuf_add(p, b, cp, cp);
}

/* [[:name:]] — only valid inside a bracket expression. */
static bool parse_posix_class(ReParse *p, ClassBuf *b)
{
    static const struct {
        const char *name;
        char perl;
    } simple[] = {
        {"digit", 'd'}, {"space", 's'}, {"word", 'w'}
    };
    size_t start = p->at;
    size_t end;
    bool negate = false;
    size_t name_at;
    size_t name_len;
    size_t i;

    /* p->at points at ':' of "[:" */
    p->at++;
    if (peek(p) == '^') {
        negate = true;
        p->at++;
    }
    name_at = p->at;
    while (!at_end(p) && peek(p) != ':')
        p->at++;
    if (at_end(p) || peek_at(p, 1U) != ']') {
        sag_re_fail(p, start - 1U, "unterminated character class");
        return false;
    }
    name_len = p->at - name_at;
    end = p->at + 2U;
    for (i = 0U; i < SAG_ARRAY_LEN(simple); i++) {
        if (strlen(simple[i].name) == name_len &&
            memcmp(p->pat + name_at, simple[i].name, name_len) == 0) {
            u32 idx = sag_re_class_perl(p, negate ?
                                        (char)(simple[i].perl - 32) :
                                        simple[i].perl);

            cbuf_add_class(p, b, idx);
            p->at = end;
            return !p->failed;
        }
    }
    /*
     * The remaining nine POSIX classes need general-category data
     * (Alphabetic, Uppercase, punctuation and symbol categories,
     * Cc, assigned).  Sprint 2's tables
     * carry grapheme, width and word-break properties but not category,
     * so those classes cannot be answered correctly yet — and answering
     * them with an ASCII approximation would be a silent wrong answer in
     * exactly the scripts a Unicode-correct editor exists to serve.
     * They error honestly until the generator emits the category field.
     */
    {
        static char msg[160];
        int shown = (int)(name_len > 24U ? 24U : name_len);

        (void)snprintf(msg, sizeof(msg),
                       "POSIX class '[:%.*s:]' needs Unicode general "
                       "categories, which the Sprint 2 tables do not "
                       "emit yet",
                       shown, (const char *)(p->pat + name_at));
        sag_re_fail(p, start - 1U, msg);
    }
    return false;
}

static u32 parse_class(ReParse *p)
{
    ClassBuf b = {0};
    size_t open = p->at - 1U; /* the '[' */
    bool negate = false;
    bool first = true;

    if (peek(p) == '^') {
        negate = true;
        p->at++;
    }
    for (;;) {
        u32 lo = 0U;
        u32 lo_cls = UINT32_MAX;

        if (at_end(p)) {
            sag_re_fail(p, open, "unterminated character class");
            return UINT32_MAX;
        }
        /* ']' first is a literal, per the syntax table. */
        if (peek(p) == ']' && !first) {
            p->at++;
            break;
        }
        first = false;
        if (peek(p) == '[' && peek_at(p, 1U) == ':') {
            p->at++;
            if (!parse_posix_class(p, &b))
                return UINT32_MAX;
            continue;
        }
        if (peek(p) == '\\') {
            p->at++;
            if (!parse_escape(p, &lo, &lo_cls))
                return UINT32_MAX;
            if (lo_cls != UINT32_MAX) {
                cbuf_add_class(p, &b, lo_cls);
                continue;
            }
        } else {
            lo = take_cp(p);
        }
        /* A '-' at the end is a literal, not a range opener. */
        if (peek(p) == '-' && peek_at(p, 1U) != ']' &&
            p->at + 1U < p->len) {
            u32 hi = 0U;
            u32 hi_cls = UINT32_MAX;

            p->at++;
            if (peek(p) == '\\') {
                p->at++;
                if (!parse_escape(p, &hi, &hi_cls))
                    return UINT32_MAX;
                if (hi_cls != UINT32_MAX) {
                    /* [a-\d] is nonsense; treat '-' as literal. */
                    cbuf_add_folded(p, &b, lo);
                    cbuf_add(p, &b, '-', '-');
                    cbuf_add_class(p, &b, hi_cls);
                    continue;
                }
            } else {
                hi = take_cp(p);
            }
            if (hi < lo) {
                sag_re_fail(p, open, "invalid range in character class");
                return UINT32_MAX;
            }
            if ((p->flags & SAG_RE_ICASE) != 0U && lo < 0x80U &&
                hi < 0x80U) {
                u32 c;

                /* Fold ASCII ranges elementwise; folding a huge range
                 * elementwise would be gratuitous, and non-ASCII ranges
                 * keep their literal meaning. */
                for (c = lo; c <= hi; c++)
                    cbuf_add_folded(p, &b, c);
            } else {
                cbuf_add(p, &b, lo, hi);
            }
            continue;
        }
        cbuf_add_folded(p, &b, lo);
    }
    if (b.n == 0U && !negate) {
        sag_re_fail(p, open, "empty character class");
        return UINT32_MAX;
    }
    return sag_re_class_intern(p, b.r, b.n, negate);
}

/* ---------------------------------------------------------------- */
/* Grammar                                                          */
/* ---------------------------------------------------------------- */

static ReAst *parse_alt(ReParse *p);

static ReAst *literal_node(ReParse *p, u32 cp)
{
    /* Under ICASE a literal becomes the class of its folded partners, so
     * the VM never does case work per input codepoint. */
    if ((p->flags & SAG_RE_ICASE) != 0U) {
        u32 partners[4];
        u32 n = sag_re_fold_partners(cp, partners);

        if (n > 1U) {
            ClassBuf b = {0};
            ReAst *a;
            u32 i;

            for (i = 0U; i < n; i++)
                cbuf_add(p, &b, partners[i], partners[i]);
            a = node(p, RE_A_CLASS);
            a->cls = sag_re_class_intern(p, b.r, b.n, false);
            return a;
        }
    }
    {
        ReAst *a = node(p, RE_A_CHAR);

        a->cp = cp;
        return a;
    }
}

/* (?i) (?-i) (?s) (?-s) and their scoped (?i:...) forms. */
static bool parse_flags(ReParse *p, u32 *scoped_out, bool *is_scoped)
{
    size_t start = p->at;
    bool negate = false;
    u32 flags = p->flags;

    for (;;) {
        u8 c = peek(p);

        if (c == '-') {
            negate = true;
            p->at++;
            continue;
        }
        if (c == 'i' || c == 's') {
            u32 bit = c == 'i' ? (u32)SAG_RE_ICASE : (u32)SAG_RE_DOTALL;

            if (negate)
                flags &= ~bit;
            else
                flags |= bit;
            p->at++;
            continue;
        }
        break;
    }
    if (peek(p) == ':') {
        p->at++;
        *scoped_out = flags;
        *is_scoped = true;
        return true;
    }
    if (peek(p) == ')') {
        p->at++;
        /* Applies from here to the end of the enclosing group. */
        p->flags = flags;
        *is_scoped = false;
        return true;
    }
    sag_re_fail(p, start - 2U, "unknown group flags");
    return false;
}

static ReAst *parse_group(ReParse *p)
{
    size_t open = p->at - 1U; /* the '(' */
    u32 saved_flags = p->flags;
    u32 group = 0U;
    bool scoped = false;
    ReAst *body;
    ReAst *g;

    if (peek(p) == '?') {
        u8 k = peek_at(p, 1U);

        /*
         * The permanent non-goals.  The message says so plainly: these
         * are not waiting on a sprint, they are incompatible with the
         * linear-time guarantee that keeps a typed pattern from freezing
         * the editor.
         */
        if (k == '=' || k == '!' || k == '<' || k == '>') {
            sag_re_fail(p, open,
                        "lookaround is not supported (sagitta's regex "
                        "engine is linear-time by design; see man "
                        "sagitta-regex)");
            return NULL;
        }
        if (k == 'P' || k == '\'') {
            sag_re_fail(p, open,
                        "named groups are not supported in 1.0");
            return NULL;
        }
        p->at += 2U;
        if (k == ':') {
            body = parse_alt(p);
        } else {
            u32 scoped_flags = p->flags;

            p->at--; /* re-read the flag letters */
            if (!parse_flags(p, &scoped_flags, &scoped))
                return NULL;
            if (!scoped) {
                /*
                 * `(?i)` is a directive, not a group: parse_flags already
                 * consumed its ')' and applied the flags to the rest of
                 * the enclosing group.  There is no body and no closing
                 * paren left to demand — expecting one reports every
                 * inline-flag pattern as an unterminated group.
                 */
                return node(p, RE_A_EMPTY);
            }
            p->flags = scoped_flags;
            body = parse_alt(p);
        }
    } else {
        if (p->ngroups >= SAG_RE_MAX_GROUPS) {
            sag_re_fail(p, open, "too many capture groups (max 32)");
            return NULL;
        }
        group = p->ngroups++;
        body = parse_alt(p);
    }
    if (p->failed)
        return NULL;
    if (at_end(p) || peek(p) != ')') {
        sag_re_fail(p, open, "unterminated group");
        return NULL;
    }
    p->at++;
    /* Inline flags die with their enclosing group. */
    p->flags = saved_flags;
    g = node(p, RE_A_GROUP);
    g->group = group;
    g->a = body;
    return g;
}

static ReAst *parse_atom(ReParse *p)
{
    u8 c = peek(p);
    size_t here = p->at;

    switch (c) {
    case '(':
        p->at++;
        return parse_group(p);
    case '[': {
        ReAst *a;
        u32 idx;

        p->at++;
        idx = parse_class(p);
        if (idx == UINT32_MAX)
            return NULL;
        a = node(p, RE_A_CLASS);
        a->cls = idx;
        return a;
    }
    case '.': {
        ReAst *a;

        p->at++;
        a = node(p, RE_A_ANY);
        a->dotall = (p->flags & SAG_RE_DOTALL) != 0U;
        return a;
    }
    case '^':
        p->at++;
        return node(p, RE_A_BOL);
    case '$':
        p->at++;
        return node(p, RE_A_EOL);
    case ')':
        sag_re_fail(p, here, "unmatched )");
        return NULL;
    case '*': case '+': case '?':
        sag_re_fail(p, here, "nothing to repeat");
        return NULL;
    case '\\': {
        u32 cp = 0U;
        u32 cls = UINT32_MAX;

        p->at++;
        /* Anchors and boundaries that are not classes. */
        switch (peek(p)) {
        case 'A': p->at++; return node(p, RE_A_BOT);
        case 'z': p->at++; return node(p, RE_A_EOT);
        case 'b': p->at++; return node(p, RE_A_WORDB);
        case 'B': p->at++; return node(p, RE_A_NWORDB);
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            sag_re_fail(p, here,
                        "backreferences are not supported (sagitta's "
                        "regex engine is linear-time by design; see man "
                        "sagitta-regex)");
            return NULL;
        default:
            break;
        }
        if (!parse_escape(p, &cp, &cls))
            return NULL;
        if (cls != UINT32_MAX) {
            ReAst *a = node(p, RE_A_CLASS);

            a->cls = cls;
            return a;
        }
        return literal_node(p, cp);
    }
    default:
        break;
    }
    return literal_node(p, take_cp(p));
}

static bool parse_repeat_bounds(ReParse *p, u32 *min, u32 *max)
{
    size_t open = p->at; /* the '{' */
    size_t scan = p->at + 1U;
    u32 lo = 0U;
    u32 hi;
    bool have_lo = false;
    bool comma = false;
    bool have_hi = false;
    u32 hi_val = 0U;

    while (scan < p->len && p->pat[scan] >= '0' && p->pat[scan] <= '9') {
        lo = lo * 10U + (u32)(p->pat[scan] - '0');
        have_lo = true;
        scan++;
        if (lo > 1000000U)
            break;
    }
    if (scan < p->len && p->pat[scan] == ',') {
        comma = true;
        scan++;
        while (scan < p->len && p->pat[scan] >= '0' &&
               p->pat[scan] <= '9') {
            hi_val = hi_val * 10U + (u32)(p->pat[scan] - '0');
            have_hi = true;
            scan++;
            if (hi_val > 1000000U)
                break;
        }
    }
    /* Not a quantifier at all: '{' is an ordinary literal. */
    if (scan >= p->len || p->pat[scan] != '}' || !have_lo)
        return false;
    hi = comma ? (have_hi ? hi_val : UINT32_MAX) : lo;
    if (lo > SAG_RE_MAX_REPEAT ||
        (hi != UINT32_MAX && hi > SAG_RE_MAX_REPEAT)) {
        static char msg[64];
        u32 shown = lo > SAG_RE_MAX_REPEAT ? lo : hi;

        (void)snprintf(msg, sizeof(msg),
                       "repeat count %u exceeds %d", (unsigned)shown,
                       SAG_RE_MAX_REPEAT);
        sag_re_fail(p, open, msg);
        return true;
    }
    if (hi != UINT32_MAX && lo > hi) {
        static char msg[64];

        (void)snprintf(msg, sizeof(msg),
                       "invalid repeat: min %u > max %u", (unsigned)lo,
                       (unsigned)hi);
        sag_re_fail(p, open, msg);
        return true;
    }
    p->at = scan + 1U;
    *min = lo;
    *max = hi;
    return true;
}

static ReAst *parse_piece(ReParse *p)
{
    ReAst *a = parse_atom(p);

    if (a == NULL || p->failed)
        return NULL;
    for (;;) {
        u8 c = peek(p);
        ReAst *q;

        if (c == '*' || c == '+' || c == '?') {
            p->at++;
            q = node(p, c == '*' ? RE_A_STAR :
                        (c == '+' ? RE_A_PLUS : RE_A_QUEST));
            q->a = a;
        } else if (c == '{') {
            u32 min = 0U;
            u32 max = 0U;

            if (!parse_repeat_bounds(p, &min, &max)) {
                /* A bare '{' is a literal; it is not a repeat. */
                break;
            }
            if (p->failed)
                return NULL;
            q = node(p, RE_A_REPEAT);
            q->a = a;
            q->min = min;
            q->max = max;
        } else {
            break;
        }
        if (peek(p) == '?') {
            p->at++;
            q->greedy = false;
        }
        a = q;
        /* `a**` is nothing to repeat, not a second quantifier. */
        if (peek(p) == '*' || peek(p) == '+') {
            sag_re_fail(p, p->at, "nothing to repeat");
            return NULL;
        }
    }
    return a;
}

static ReAst *parse_concat(ReParse *p)
{
    ReAst *left = NULL;

    while (!at_end(p) && peek(p) != '|' && peek(p) != ')') {
        ReAst *piece = parse_piece(p);
        ReAst *cat;

        if (piece == NULL || p->failed)
            return NULL;
        if (left == NULL) {
            left = piece;
            continue;
        }
        cat = node(p, RE_A_CAT);
        cat->a = left;
        cat->b = piece;
        left = cat;
    }
    return left == NULL ? node(p, RE_A_EMPTY) : left;
}

static ReAst *parse_alt(ReParse *p)
{
    ReAst *left = parse_concat(p);

    if (left == NULL || p->failed)
        return NULL;
    while (peek(p) == '|') {
        ReAst *right;
        ReAst *alt;

        p->at++;
        right = parse_concat(p);
        if (right == NULL || p->failed)
            return NULL;
        alt = node(p, RE_A_ALT);
        alt->a = left;
        alt->b = right;
        left = alt;
    }
    return left;
}

ReAst *sag_re_parse(ReParse *p)
{
    ReAst *root;

    /* SAG_RE_LITERAL: every byte is itself, metacharacters included. */
    if ((p->base_flags & SAG_RE_LITERAL) != 0U) {
        ReAst *left = NULL;

        while (!at_end(p)) {
            ReAst *lit = literal_node(p, take_cp(p));
            ReAst *cat;

            if (p->failed)
                return NULL;
            if (left == NULL) {
                left = lit;
                continue;
            }
            cat = node(p, RE_A_CAT);
            cat->a = left;
            cat->b = lit;
            left = cat;
        }
        return left == NULL ? node(p, RE_A_EMPTY) : left;
    }
    root = parse_alt(p);
    if (root == NULL || p->failed)
        return NULL;
    if (!at_end(p)) {
        /* parse_alt stops at ')' it did not open. */
        sag_re_fail(p, p->at, "unmatched )");
        return NULL;
    }
    return root;
}
