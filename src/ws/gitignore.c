/*
 * Sprint 26 §4.  See gitignore.h for the supported/unsupported table
 * and why this is bespoke.
 */
#define _POSIX_C_SOURCE 200809L

#include "ws/gitignore.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

enum {
    /* A pattern longer than this is not one a human wrote. */
    SAG_GI_MAX_PATTERN = 1024,
    /* An ignore file larger than this is not one either; the rest is
     * dropped with a log line rather than silently. */
    SAG_GI_MAX_BYTES = 1U * 1024U * 1024U
};

/*
 * One compiled rule.
 *
 * `pat` is the pattern with its `!`, anchoring slash and trailing slash
 * already stripped — those became flags at compile time so matching
 * never re-parses.  `base` is the directory the rule was written in,
 * workspace-relative, so a nested .gitignore anchors to itself.
 */
typedef struct GiRule {
    const char *pat;
    const char *base; /* "" at the workspace root */
    u32 base_len;
    bool negate;
    bool dir_only;
    /* The pattern was anchored: a leading `/`, or a `/` anywhere but at
     * the end.  Unanchored patterns match any path COMPONENT. */
    bool anchored;
} GiRule;

struct GiSet {
    GiRule *rule;
    u32 n;
    /* The parent chain: a nested .gitignore is consulted after the sets
     * above it, so the deepest matching rule wins. */
    const GiSet *parent;
};

/* ---------------------------------------------------------------- */
/* Matching one pattern against one name                            */
/* ---------------------------------------------------------------- */

/*
 * A bracket set: `[abc]`, `[a-z]`, `[!a-z]`, `[^a-z]`.
 *
 * `*pp` points just past the `[` and is advanced past the `]`.  A set
 * with no closing bracket is not a set — the `[` is then a literal,
 * which is what git does with an unterminated one.
 */
static bool bracket_match(const char **pp, const char *end, u8 c,
                          bool *ok)
{
    const char *p = *pp;
    bool negate = false;
    bool hit = false;
    bool first = true;

    *ok = true;
    if (p < end && (*p == '!' || *p == '^')) {
        negate = true;
        p++;
    }
    while (p < end) {
        u8 lo;

        if (*p == ']' && !first) {
            *pp = p + 1;
            return negate ? !hit : hit;
        }
        first = false;
        lo = (u8)*p;
        if (lo == (u8)'\\' && p + 1 < end) {
            p++;
            lo = (u8)*p;
        }
        /* `a-z`, but a `-` just before the `]` is a literal. */
        if (p + 2 < end && p[1] == '-' && p[2] != ']') {
            u8 hi = (u8)p[2];

            if (c >= lo && c <= hi)
                hit = true;
            p += 3;
            continue;
        }
        if (c == lo)
            hit = true;
        p++;
    }
    /* Unterminated: the caller treats the `[` as a literal byte. */
    *ok = false;
    return false;
}

/*
 * Glob one path SEGMENT-aware pattern against `text`.
 *
 * `*` does not cross `/`; `?` is one non-`/` byte; `**` crosses
 * anything.  Backtracking is iterative — a recursive matcher on a
 * hostile pattern is a stack hazard, and this runs on 200 000 paths.
 */
static bool glob_match(const char *pat, const char *text)
{
    const char *p = pat;
    const char *t = text;
    const char *star_p = NULL;
    const char *star_t = NULL;
    bool star_crosses = false;

    for (;;) {
        if (*p == '*') {
            bool dstar = p[1] == '*';

            p += dstar ? 2 : 1;
            /* `**\/` consumes the slash it is followed by, so `**\/b`
             * matches `b` as well as `x/b`. */
            if (dstar && *p == '/') {
                const char *after = p + 1;

                if (glob_match(after, t))
                    return true;
                p = after;
            }
            star_p = p;
            star_t = t;
            star_crosses = dstar;
            continue;
        }
        if (*p == '?' && *t != '\0' && *t != '/') {
            p++;
            t++;
            continue;
        }
        if (*p == '[' && *t != '\0') {
            const char *q = p + 1;
            bool ok = false;
            bool hit = bracket_match(&q, q + strlen(q), (u8)*t, &ok);

            if (ok) {
                if (hit) {
                    p = q;
                    t++;
                    continue;
                }
                goto backtrack;
            }
            /* Unterminated `[`: fall through to the literal compare. */
        }
        if (*p != '\0' && *p == *t) {
            p++;
            t++;
            continue;
        }
        if (*p == '\0' && *t == '\0')
            return true;
    backtrack:
        if (star_p == NULL)
            return false;
        /* A single `*` may not cross a separator. */
        if (*star_t == '\0' || (!star_crosses && *star_t == '/'))
            return false;
        star_t++;
        t = star_t;
        p = star_p;
    }
}

/* ---------------------------------------------------------------- */
/* Matching a rule against a path                                   */
/* ---------------------------------------------------------------- */

/* The part of `rel` below the rule's own directory, or NULL when `rel`
 * is not under it at all. */
static const char *below_base(const GiRule *r, const char *rel)
{
    if (r->base_len == 0U)
        return rel;
    if (strncmp(rel, r->base, r->base_len) != 0)
        return NULL;
    if (rel[r->base_len] != '/')
        return NULL;
    return rel + r->base_len + 1U;
}

static bool rule_match(const GiRule *r, const char *rel, bool is_dir)
{
    const char *sub = below_base(r, rel);

    if (sub == NULL || sub[0] == '\0')
        return false;
    if (r->dir_only && !is_dir)
        return false;
    if (r->anchored)
        return glob_match(r->pat, sub);
    /*
     * Unanchored patterns match any COMPONENT: `*.o` ignores
     * `a/b/c.o`, and `build` ignores `x/build`.  Walking the suffixes
     * is how git expresses "at any depth".
     */
    for (;;) {
        const char *slash;

        if (glob_match(r->pat, sub))
            return true;
        slash = strchr(sub, '/');
        if (slash == NULL)
            return false;
        sub = slash + 1;
    }
}

bool sag_gi_match(const GiSet *g, const char *rel, bool is_dir)
{
    const GiSet *chain[64];
    u32 depth = 0U;
    bool ignored = false;
    u32 i;

    if (g == NULL || rel == NULL || rel[0] == '\0')
        return false;
    /*
     * Root-first order.  The sets chain child -> parent, but the LAST
     * matching rule must win overall and a nested .gitignore is deeper
     * than the one above it — so the chain is reversed before scanning.
     */
    while (g != NULL && depth < 64U) {
        chain[depth++] = g;
        g = g->parent;
    }
    while (depth > 0U) {
        const GiSet *s = chain[--depth];

        for (i = 0U; i < s->n; i++) {
            if (rule_match(&s->rule[i], rel, is_dir))
                ignored = !s->rule[i].negate;
        }
    }
    return ignored;
}

/* ---------------------------------------------------------------- */
/* §4.1: pruning                                                    */
/* ---------------------------------------------------------------- */

/* The pattern text up to its first wildcard — the part that is a plain
 * path prefix and can therefore be compared. */
static u32 literal_prefix_len(const char *pat)
{
    u32 i = 0U;

    while (pat[i] != '\0' && pat[i] != '*' && pat[i] != '?' &&
           pat[i] != '[')
        i++;
    return i;
}

bool sag_gi_prunable(const GiSet *g, const char *rel)
{
    const GiSet *s;

    if (g == NULL || rel == NULL || rel[0] == '\0')
        return false;
    if (!sag_gi_match(g, rel, true))
        return false;
    /*
     * CONSERVATIVE.  Any negation whose literal prefix could name
     * something inside `rel` blocks the prune, even if it would not
     * actually re-include anything — deciding that exactly would mean
     * evaluating the pattern against files we have deliberately not
     * listed.  A slow correct walk beats a fast one that hides a file
     * the user can see in `git status`.
     */
    for (s = g; s != NULL; s = s->parent) {
        u32 i;

        for (i = 0U; i < s->n; i++) {
            const GiRule *r = &s->rule[i];
            const char *sub;
            u32 lit;
            u64 rel_len;

            if (!r->negate)
                continue;
            sub = below_base(r, rel);
            /* A negation written above this directory, whose own base
             * does not contain it, cannot reach inside it. */
            if (sub == NULL && r->base_len != 0U)
                continue;
            lit = literal_prefix_len(r->pat);
            /* A negation that begins with a wildcard could match
             * anything, anywhere. */
            if (lit == 0U)
                return false;
            rel_len = (u64)strlen(rel);
            /*
             * Does the negation's literal prefix lie inside `rel`?
             * Compared against the path relative to the RULE's base, so
             * `!node_modules/keep.js` at the root blocks pruning
             * `node_modules`.
             */
            if (r->anchored) {
                const char *tail = below_base(r, "");
                u64 n = (u64)lit < rel_len ? (u64)lit : rel_len;

                (void)tail;
                if (r->base_len == 0U && strncmp(r->pat, rel, (size_t)n) == 0)
                    return false;
                if (r->base_len != 0U)
                    return false;
            } else {
                /* Unanchored negations match at any depth, so any of
                 * them could name something below here. */
                return false;
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Compiling                                                        */
/* ---------------------------------------------------------------- */

/*
 * One line into one rule.  False when the line contributes nothing —
 * blank, or a comment.
 */
static bool compile_line(Arena *a, const char *base, u32 base_len,
                         const char *line, u64 len, GiRule *out)
{
    char buf[SAG_GI_MAX_PATTERN];
    u64 at = 0U;
    u64 i = 0U;
    bool escaped_hash = false;

    (void)memset(out, 0, sizeof(*out));
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= len)
        return false;
    /* `\#` is a literal `#`; a bare `#` starts a comment. */
    if (line[i] == '#')
        return false;
    if (line[i] == '\\' && i + 1U < len && line[i + 1U] == '#') {
        escaped_hash = true;
        i++;
    }
    if (!escaped_hash && line[i] == '!') {
        out->negate = true;
        i++;
        if (i >= len)
            return false;
    }
    /*
     * Trailing whitespace is stripped unless escaped.  Scanned from the
     * end so `a\ ` keeps its space while `a   ` does not.
     */
    while (len > i && (line[len - 1U] == ' ' || line[len - 1U] == '\t')) {
        if (len - 1U > i && line[len - 2U] == '\\')
            break;
        len--;
    }
    if (len <= i)
        return false;
    if (line[len - 1U] == '/') {
        out->dir_only = true;
        len--;
        if (len <= i)
            return false;
    }
    if (line[i] == '/') {
        out->anchored = true;
        i++;
        if (i >= len)
            return false;
    }
    /* A `/` anywhere but the end also anchors — git's rule. */
    {
        u64 k;

        for (k = i; k + 1U < len; k++) {
            if (line[k] == '/')
                out->anchored = true;
        }
    }
    for (; i < len && at + 1U < sizeof(buf); i++) {
        /* `\ ` and `\#` lose their backslash; every other escape is
         * kept for the matcher's bracket handling. */
        if (line[i] == '\\' && i + 1U < len &&
            (line[i + 1U] == ' ' || line[i + 1U] == '#')) {
            i++;
        }
        buf[at++] = line[i];
    }
    buf[at] = '\0';
    if (at == 0U)
        return false;
    out->pat = arena_strndup(a, buf, (size_t)at);
    out->base = base;
    out->base_len = base_len;
    return true;
}

GiSet *sag_gi_compile(Arena *a, const char *base, const char *text, u64 len,
                      const GiSet *parent)
{
    GiSet *g;
    const char *p = text;
    u64 at = 0U;
    u32 cap = 16U;
    u32 n = 0U;
    GiRule *rules;

    if (a == NULL || text == NULL)
        return (GiSet *)parent;
    g = arena_alloc(a, sizeof(*g), sizeof(void *));
    rules = arena_alloc(a, (size_t)cap * sizeof(*rules), sizeof(void *));
    while (at < len) {
        u64 eol = at;
        GiRule r;

        while (eol < len && text[eol] != '\n')
            eol++;
        {
            u64 line_len = eol - at;

            /* A CRLF file is not a broken file. */
            if (line_len > 0U && text[at + line_len - 1U] == '\r')
                line_len--;
            if (line_len > 0U &&
                compile_line(a, base,
                             base == NULL ? 0U : (u32)strlen(base),
                             p + at, line_len, &r)) {
                if (n == cap) {
                    GiRule *bigger = arena_alloc(
                        a, (size_t)cap * 2U * sizeof(*rules), sizeof(void *));

                    (void)memcpy(bigger, rules, (size_t)n * sizeof(*rules));
                    rules = bigger;
                    cap *= 2U;
                }
                rules[n++] = r;
            }
        }
        at = eol + 1U;
    }
    g->rule = rules;
    g->n = n;
    g->parent = parent;
    /* An ignore file with no usable rules is not a set — returning the
     * parent keeps the chain short on a deep tree. */
    if (n == 0U)
        return (GiSet *)parent;
    return g;
}

GiSet *sag_gi_load(Arena *a, const char *dir, const GiSet *parent)
{
    char path[PATH_MAX];
    char *text;
    FILE *fp;
    long size;
    size_t got;
    GiSet *g;

    if (a == NULL || dir == NULL)
        return (GiSet *)parent;
    if (snprintf(path, sizeof(path), "%s/.gitignore", dir) >=
        (int)sizeof(path))
        return (GiSet *)parent;
    fp = fopen(path, "rb");
    if (fp == NULL)
        return (GiSet *)parent;
    if (fseek(fp, 0L, SEEK_END) != 0) {
        (void)fclose(fp);
        return (GiSet *)parent;
    }
    size = ftell(fp);
    if (size < 0 || (u64)size > (u64)SAG_GI_MAX_BYTES) {
        if (size > 0)
            sag_log(SAG_LOG_WARN, "%s is too large to read as ignore rules",
                    path);
        (void)fclose(fp);
        return (GiSet *)parent;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        (void)fclose(fp);
        return (GiSet *)parent;
    }
    text = arena_alloc(a, (size_t)size + 1U, 1U);
    got = fread(text, 1U, (size_t)size, fp);
    (void)fclose(fp);
    text[got] = '\0';
    g = sag_gi_compile(a, NULL, text, (u64)got, parent);
    return g;
}

bool sag_gi_negated(const GiSet *g, const char *rel)
{
    const GiSet *chain[64];
    u32 depth = 0U;
    bool negated = false;
    u32 i;

    /*
     * "Did a `!` rule name this path?", asked separately from
     * sag_gi_match because inside an ignored directory the default is
     * reversed: everything is out unless something re-includes it, and
     * sag_gi_match alone cannot express that — a file under
     * `node_modules/` matches no rule at all, since a directory-only
     * rule does not match files.
     */
    if (g == NULL || rel == NULL || rel[0] == '\0')
        return false;
    while (g != NULL && depth < 64U) {
        chain[depth++] = g;
        g = g->parent;
    }
    while (depth > 0U) {
        const GiSet *s = chain[--depth];

        for (i = 0U; i < s->n; i++) {
            if (rule_match(&s->rule[i], rel, false))
                negated = s->rule[i].negate;
        }
    }
    return negated;
}
