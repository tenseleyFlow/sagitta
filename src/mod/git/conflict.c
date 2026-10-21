#include "mod/git/conflict.h"

#include <string.h>

static bool oid_valid(const u8 *s, size_t n)
{
    size_t i;
    bool nonzero = false;

    if (n != 40U && n != 64U)
        return false;
    for (i = 0U; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f')))
            return false;
        nonzero = nonzero || s[i] != '0';
    }
    return nonzero;
}

bool yew_git_conflict_index(GitConflictIndex *out, const u8 *bytes,
                             size_t len, const char *path)
{
    GitConflictIndex next = {0};
    size_t at = 0U, path_len;

    if (out == NULL || path == NULL || path[0] == '\0' ||
        (len != 0U && bytes == NULL) || len > YEW_CONFLICT_BYTES_MAX)
        return false;
    path_len = strlen(path);
    while (at < len) {
        size_t start = at, oid_at, oid_len;
        u32 mode = 0U, stage;
        const u8 *end;

        while (at < len && bytes[at] >= '0' && bytes[at] <= '7' &&
               at - start < 6U)
            mode = mode * 8U + (u32)(bytes[at++] - '0');
        if (at - start != 6U || at == len || bytes[at++] != ' ')
            return false;
        oid_at = at;
        while (at < len && bytes[at] != ' ' && at - oid_at <= 64U)
            at++;
        oid_len = at - oid_at;
        if (at == len || !oid_valid(bytes + oid_at, oid_len))
            return false;
        at++;
        if (len - at < 2U || bytes[at] < '1' || bytes[at] > '3' ||
            bytes[at + 1U] != '\t')
            return false;
        stage = (u32)(bytes[at] - '1');
        at += 2U;
        end = memchr(bytes + at, 0, len - at);
        if (end == NULL || (size_t)(end - (bytes + at)) != path_len ||
            memcmp(bytes + at, path, path_len) != 0 ||
            next.stage[stage].present ||
            (mode != 0100644U && mode != 0100755U &&
             mode != 0120000U && mode != 0160000U))
            return false;
        next.stage[stage].present = true;
        next.stage[stage].mode = mode;
        (void)memcpy(next.stage[stage].oid, bytes + oid_at, oid_len);
        at += path_len + 1U;
    }
    *out = next;
    return true;
}

bool yew_git_conflict_index_equal(const GitConflictIndex *a,
                                  const GitConflictIndex *b)
{
    u32 i;

    for (i = 0U; i < 3U; i++)
        if (a->stage[i].present != b->stage[i].present ||
            a->stage[i].mode != b->stage[i].mode ||
            strcmp(a->stage[i].oid, b->stage[i].oid) != 0)
            return false;
    return true;
}

void yew_git_conflict_parse_drop(GitConflictParse *p)
{
    GitConflictRangeVec_free(&p->ranges);
    (void)memset(p, 0, sizeof(*p));
}

void yew_git_conflict_parse_start(GitConflictParse *p, const u8 *bytes,
                                  size_t len, u32 width)
{
    yew_git_conflict_parse_drop(p);
    p->bytes = bytes;
    p->len = len;
    p->width = width;
    if (len > YEW_CONFLICT_BYTES_MAX || width > YEW_CONFLICT_BYTES_MAX) {
        p->result = YEW_CONFLICT_LIMIT;
        p->error = "merge input exceeds 16 MiB";
    } else if (bytes == NULL && len != 0U) {
        p->result = YEW_CONFLICT_INVALID;
        p->error = "missing merge input";
    }
}

static void parse_fail(GitConflictParse *p, const char *why)
{
    p->result = YEW_CONFLICT_INVALID;
    p->error = why;
}

static void parse_line(GitConflictParse *p, size_t end, size_t next)
{
    size_t at = p->line_at, run = p->marker_run, n;
    u8 c;

    if (++p->line > YEW_CONFLICT_LINES_MAX) {
        p->result = YEW_CONFLICT_LIMIT;
        p->error = "merge input exceeds 200000 lines";
        return;
    }
    if (at == 0U && end >= 3U && memcmp(p->bytes, "\xef\xbb\xbf", 3U) == 0)
        at = 3U;
    if (end > at && p->bytes[end - 1U] == '\r')
        end--;
    if (end == at)
        return;
    c = p->bytes[at];
    if (c != '<' && c != '|' && c != '=' && c != '>')
        return;
    n = end - at;
    if (run < (p->width == 0U ? 7U : p->width))
        return;
    /* A marker's suffix is a label separated by a space, never arbitrary
     * code following a run of operators. Separator labels are forbidden. */
    if ((c == '=' && run != n) ||
        (c != '=' && run < n && p->bytes[at + run] != ' '))
        return;
    if (p->width != 0U && run != p->width) {
        parse_fail(p, "inconsistent conflict marker width");
        return;
    }
    if (c == '<' && p->state == 0U) {
        p->width = (u32)run;
        (void)memset(&p->current, 0, sizeof(p->current));
        p->current.envelope.lo = at;
        p->current.current.lo = next;
        p->current.line = p->line - 1U;
        p->current.width = (u32)run;
        p->state = 1U;
    } else if (c == '|' && p->state == 1U) {
        p->current.current.hi = at;
        p->current.base.lo = next;
        p->current.has_base = true;
        p->state = 2U;
    } else if (c == '=' && (p->state == 1U || p->state == 2U)) {
        if (p->state == 1U)
            p->current.current.hi = at;
        else
            p->current.base.hi = at;
        p->current.incoming.lo = next;
        p->state = 3U;
    } else if (c == '>' && p->state == 3U) {
        p->current.incoming.hi = at;
        p->current.envelope.hi = next;
        GitConflictRangeVec_push(&p->ranges, p->current);
        p->state = 0U;
    } else {
        parse_fail(p, "nested, unmatched or ambiguous conflict markers");
    }
}

GitConflictParseResult yew_git_conflict_parse_step(GitConflictParse *p,
                                                   size_t byte_budget)
{
    size_t stop = p->len - p->at < byte_budget ? p->len :
                  p->at + byte_budget;

    if (p->result != YEW_CONFLICT_PENDING)
        return p->result;
    while (p->at < stop && p->result == YEW_CONFLICT_PENDING) {
        size_t marker_at = p->line_at;
        if (marker_at == 0U && p->len >= 3U &&
            memcmp(p->bytes, "\xef\xbb\xbf", 3U) == 0)
            marker_at = 3U;
        if (p->at == marker_at + p->marker_run &&
            p->bytes[p->at] == p->bytes[marker_at])
            p->marker_run++;
        u8 c = p->bytes[p->at++];
        if (c == 0U) {
            p->result = YEW_CONFLICT_BINARY;
            p->error = "binary merge result; edit manually";
        } else if (c == '\n') {
            parse_line(p, p->at - 1U, p->at);
            p->line_at = p->at;
            p->marker_run = 0U;
        }
    }
    if (p->at == p->len && p->result == YEW_CONFLICT_PENDING) {
        if (p->line_at < p->len)
            parse_line(p, p->len, p->len);
        if (p->result == YEW_CONFLICT_PENDING) {
            if (p->state != 0U)
                parse_fail(p, "unterminated conflict markers");
            else
                p->result = YEW_CONFLICT_OK;
        }
    }
    return p->result;
}

bool yew_git_conflict_choice(Bytebuf *out, const u8 *bytes, size_t len,
                              const GitConflictRange *range,
                              GitConflictChoice choice, bool crlf)
{
    Bytebuf next = {0};
    Span a, b;

    if (out == NULL || range == NULL)
        return false;
    a = range->current;
    b = range->incoming;

    if (choice < YEW_CONFLICT_CURRENT || choice > YEW_CONFLICT_BOTH ||
        len > YEW_CONFLICT_BYTES_MAX || (len != 0U && bytes == NULL) ||
        range->envelope.lo > a.lo || a.lo > a.hi || a.hi > b.lo ||
        b.lo > b.hi || b.hi > range->envelope.hi ||
        range->envelope.hi > len)
        return false;
    if (choice != YEW_CONFLICT_INCOMING && a.hi != a.lo)
        bytebuf_append(&next, bytes + a.lo, (size_t)(a.hi - a.lo));
    if (choice == YEW_CONFLICT_BOTH && next.len != 0U && b.hi != b.lo &&
        next.data[next.len - 1U] != '\n')
        bytebuf_append(&next, crlf ? "\r\n" : "\n", crlf ? 2U : 1U);
    if (choice != YEW_CONFLICT_CURRENT && b.hi != b.lo)
        bytebuf_append(&next, bytes + b.lo, (size_t)(b.hi - b.lo));
    bytebuf_free(out);
    *out = next;
    return true;
}

typedef struct ConflictLine {
    u32 lo, hi, end;
    u64 hash;
} ConflictLine;
VEC_DECL(ConflictLineVec, ConflictLine);

typedef struct ConflictLines {
    const u8 *bytes;
    size_t len, at, start;
    u64 hash, before_last;
    ConflictLineVec lines;
    bool done;
} ConflictLines;

struct GitConflictMatch {
    ConflictLines source, arm;
    u32 *prefix;
    size_t built, scan, matched, equal_at;
    u32 hits;
    Span hit;
    bool missing_lf;
    GitConflictParseResult result;
};

#define CONFLICT_HASH_INIT UINT64_C(14695981039346656037)

static bool lines_step(ConflictLines *l, size_t budget)
{
    size_t stop = l->len - l->at < budget ? l->len : l->at + budget;
    while (l->at < stop) {
        u8 c = l->bytes[l->at++];
        if (c == 0U)
            return false;
        if (c == '\n' || l->at == l->len) {
            ConflictLine line;
            size_t hi = c == '\n' ? l->at - 1U : l->at;
            if (c != '\n') {
                l->before_last = l->hash;
                l->hash = (l->hash ^ c) * UINT64_C(1099511628211);
            }
            if (c == '\n' && hi > l->start && l->bytes[hi - 1U] == '\r') {
                hi--;
                l->hash = l->before_last;
            }
            if (l->lines.len >= YEW_CONFLICT_LINES_MAX)
                return false;
            line = (ConflictLine){(u32)l->start, (u32)hi, (u32)l->at, l->hash};
            ConflictLineVec_push(&l->lines, line);
            l->start = l->at;
            l->hash = CONFLICT_HASH_INIT;
        } else {
            l->before_last = l->hash;
            l->hash = (l->hash ^ c) * UINT64_C(1099511628211);
        }
    }
    l->done = l->at == l->len;
    return true;
}

/* Hashes only reject; byte equality remains authoritative. */
static bool line_equal_step(GitConflictMatch *w,
                             const ConflictLines *a, size_t ai,
                             const ConflictLines *b, size_t bi,
                             size_t *budget, bool *equal)
{
    const ConflictLine *x = &a->lines.data[ai], *y = &b->lines.data[bi];
    size_t n = x->hi - x->lo, take;

    if (*budget == 0U)
        return false;
    *equal = false;
    if (x->hash != y->hash || n != y->hi - y->lo) {
        --*budget;
        w->equal_at = 0U;
        return true;
    }
    if (n == 0U) {
        --*budget;
        *equal = true;
        return true;
    }
    take = n - w->equal_at;
    if (take > *budget)
        take = *budget;
    *budget -= take;
    if (take != 0U && memcmp(a->bytes + x->lo + w->equal_at,
                            b->bytes + y->lo + w->equal_at, take) != 0) {
        w->equal_at = 0U;
        return true;
    }
    w->equal_at += take;
    if (w->equal_at != n)
        return false;
    w->equal_at = 0U;
    *equal = true;
    return true;
}

GitConflictMatch *yew_git_conflict_match_new(const u8 *source, size_t len,
                                            const u8 *arm, size_t arm_len)
{
    GitConflictMatch *w;
    if (len > YEW_CONFLICT_BYTES_MAX || arm_len > YEW_CONFLICT_BYTES_MAX ||
        (len != 0U && source == NULL) || (arm_len != 0U && arm == NULL))
        return NULL;
    w = yew_xcalloc(1U, sizeof(*w));
    w->source.bytes = source; w->source.len = len;
    w->arm.bytes = arm; w->arm.len = arm_len;
    w->source.hash = w->arm.hash = CONFLICT_HASH_INIT;
    return w;
}

GitConflictParseResult yew_git_conflict_match_step(GitConflictMatch *w,
                                                   size_t budget)
{
    if (w == NULL)
        return YEW_CONFLICT_LIMIT;
    if (w->result != YEW_CONFLICT_PENDING || budget == 0U)
        return w->result;
    if (!w->source.done) {
        if (!lines_step(&w->source, budget))
            w->result = YEW_CONFLICT_INVALID;
        return w->result;
    }
    if (!w->arm.done) {
        if (!lines_step(&w->arm, budget))
            w->result = YEW_CONFLICT_INVALID;
        return w->result;
    }
    if (w->arm.lines.len == 0U) {
        w->hits = 1U;
        return w->result = YEW_CONFLICT_OK;
    }
    if (w->prefix == NULL) {
        w->prefix = yew_xcalloc(w->arm.lines.len, sizeof(*w->prefix));
        w->built = 1U;
    }
    /* Both equality checks and KMP fallback steps yield. A long line or
     * repetitive prefix must not hide an unbounded inner loop. */
    while (w->built < w->arm.lines.len) {
        bool equal;
        if (!line_equal_step(w, &w->arm, w->built, &w->arm, w->matched,
                             &budget, &equal))
            return YEW_CONFLICT_PENDING;
        if (!equal && w->matched != 0U) {
            w->matched = w->prefix[w->matched - 1U];
            continue;
        }
        if (equal)
            w->matched++;
        w->prefix[w->built++] = (u32)w->matched;
        if (w->built == w->arm.lines.len)
            w->matched = 0U;
    }
    while (w->scan < w->source.lines.len) {
        size_t i = w->scan;
        bool equal;
        if (!line_equal_step(w, &w->source, i, &w->arm, w->matched,
                             &budget, &equal))
            return YEW_CONFLICT_PENDING;
        if (!equal && w->matched != 0U) {
            w->matched = w->prefix[w->matched - 1U];
            continue;
        }
        if (equal)
            w->matched++;
        if (w->matched == w->arm.lines.len) {
            w->hit.lo = w->source.lines.data[i + 1U - w->matched].lo;
            w->hit.hi = w->source.lines.data[i].end;
            w->missing_lf = w->hit.hi == w->source.len &&
                w->source.len != 0U &&
                w->source.bytes[w->source.len - 1U] != '\n';
            if (++w->hits > 1U)
                return w->result = YEW_CONFLICT_INVALID;
            w->matched = w->prefix[w->matched - 1U];
        }
        w->scan++;
    }
    if (w->built == w->arm.lines.len && w->scan == w->source.lines.len)
        w->result = w->hits == 1U ? YEW_CONFLICT_OK : YEW_CONFLICT_INVALID;
    return w->result;
}

bool yew_git_conflict_match_span(const GitConflictMatch *w, Span *span,
                                  bool *missing_lf)
{
    if (w == NULL || w->result != YEW_CONFLICT_OK)
        return false;
    *span = w->hit;
    *missing_lf = w->missing_lf;
    return true;
}

void yew_git_conflict_match_free(GitConflictMatch *w)
{
    if (w == NULL)
        return;
    ConflictLineVec_free(&w->source.lines);
    ConflictLineVec_free(&w->arm.lines);
    yew_xfree(w->prefix);
    yew_xfree(w);
}
