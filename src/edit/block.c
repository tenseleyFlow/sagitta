#include "edit/block.h"

#include <limits.h>
#include <stdlib.h>

#include "edit/ed.h"
#include "unicode/coords.h"
#include "unicode/wordbreak.h"
#include "util/log.h"

enum { SAG_BLOCK_PROVIDERS_MAX = 16 };

typedef struct {
    u8 byte;
    u64 off;
} ScopeOpen;

typedef struct {
    ScopeOpen *data;
    size_t len;
    size_t cap;
} ScopeStack;

typedef struct {
    Span span;
    u64 indent;
    bool blank;
} LineInfo;

static BlockProvider providers[SAG_BLOCK_PROVIDERS_MAX];
static u8 provider_count;
static bool providers_ready;

static bool scope_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                            Span *out);
static bool indent_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                             Span *out);
static bool paragraph_enclosing(void *ctx, UnitCtx *u, ByteOff p,
                                Span inner, Span *out);

static bool span_contains(Span outer, Span inner)
{
    return outer.lo <= inner.lo && outer.hi >= inner.hi;
}

static bool span_strictly_contains(Span outer, Span inner)
{
    return span_contains(outer, inner) &&
           (outer.lo < inner.lo || outer.hi > inner.hi);
}

static u64 span_size(Span span)
{
    return span.hi - span.lo;
}

static void provider_append(BlockProvider provider)
{
    if (provider.name == NULL)
        SAG_BUG("block provider has no name");
    if (provider_count == SAG_BLOCK_PROVIDERS_MAX)
        SAG_BUG("block provider registry overflow");
    providers[provider_count++] = provider;
}

static void ensure_providers(void)
{
    if (providers_ready)
        return;
    providers_ready = true;
    provider_append((BlockProvider){"syntax", 40, NULL, NULL});
    provider_append((BlockProvider){"scope", 30, scope_enclosing, NULL});
    provider_append((BlockProvider){"indent", 20, indent_enclosing, NULL});
    provider_append(
        (BlockProvider){"paragraph", 10, paragraph_enclosing, NULL});
}

void sag_block_register(BlockProvider provider)
{
    ensure_providers();
    provider_append(provider);
}

void sag_block_provider_syntax_install(BlockProvider provider)
{
    (void)provider;
    SAG_BUG("syntax block provider installation lands in Sprint 40");
}

static bool read_span(const TextBuf *tb, Span span, u8 *dst)
{
    TextIter it;
    u64 copied = 0U;

    if (span.lo == span.hi)
        return true;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        return false;
    while (copied < span.hi - span.lo) {
        const u8 *chunk;
        u64 chunk_len;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &chunk, &chunk_len))
            return false;
        take = span.hi - span.lo - copied;
        if (take > chunk_len)
            take = chunk_len;
        for (u64 i = 0U; i < take; i++)
            dst[copied + i] = chunk[i];
        copied += take;
        if (copied != span.hi - span.lo && !sag_textiter_advance(&it, tb))
            return false;
    }
    return true;
}

static u64 line_content_hi(const u8 *bytes, Span span)
{
    u64 len = span.hi - span.lo;

    if (len != 0U && bytes[len - 1U] == (u8)'\n') {
        len--;
        if (len != 0U && bytes[len - 1U] == (u8)'\r')
            len--;
    }
    return len;
}

static bool ascii_white(u8 byte)
{
    return byte == (u8)' ' || byte == (u8)'\t' || byte == (u8)'\n' ||
           byte == (u8)'\r' || byte == (u8)'\v' || byte == (u8)'\f';
}

/* Most source and prose lines reveal their indentation entirely in ASCII.
 * Keep column calculation in unicode/coords.c, but avoid constructing a
 * grapheme reader for every unindented ASCII line in a large paragraph. */
static bool line_ascii_first_nonwhite(const TextBuf *tb, Span span,
                                      ByteOff *first, bool *blank)
{
    TextIter it;
    u64 consumed = 0U;

    *first = BYTEOFF(span.lo);
    *blank = true;
    if (span.lo == span.hi)
        return true;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        SAG_BUG("block line classifier cannot start iterator");
    while (consumed < span.hi - span.lo) {
        const u8 *chunk;
        u64 chunk_len;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &chunk, &chunk_len))
            SAG_BUG("block line classifier cannot read iterator");
        take = span.hi - span.lo - consumed;
        if (take > chunk_len)
            take = chunk_len;
        for (u64 i = 0U; i < take; i++) {
            if (chunk[i] >= 0x80U)
                return false;
            if (!ascii_white(chunk[i])) {
                *first = BYTEOFF(span.lo + consumed + i);
                *blank = false;
                return true;
            }
        }
        consumed += take;
        if (consumed != span.hi - span.lo &&
            !sag_textiter_advance(&it, tb))
            SAG_BUG("block line classifier iterator ended early");
    }
    return true;
}

static bool line_info(UnitCtx *u, LineNo line, LineInfo *out)
{
    Span span = sag_textbuf_line_span(u->tb, line);
    ByteOff at = BYTEOFF(span.lo);
    ByteOff first = at;
    u32 tabwidth = u->buf != NULL && u->buf->tabwidth != 0U
                       ? u->buf->tabwidth
                       : 4U;

    out->span = span;
    out->blank = true;
    out->indent = 0U;
    if (line_ascii_first_nonwhite(u->tb, span, &first, &out->blank)) {
        if (!out->blank && first.v != span.lo)
            out->indent =
                sag_off_to_ccol(u->tb, span, first, tabwidth).v;
        return true;
    }
    while (at.v < span.hi) {
        SagTextCluster cluster;

        if (!sag_text_cluster_next(u->tb, span, at, &cluster))
            break;
        if (!sag_unicode_is_white_space(cluster.base_cp)) {
            out->blank = false;
            first = at;
            break;
        }
        at = BYTEOFF(cluster.bytes.hi);
    }
    if (!out->blank)
        out->indent = sag_off_to_ccol(u->tb, span, first, tabwidth).v;
    return true;
}

static bool line_blank(UnitCtx *u, u64 line)
{
    LineInfo info;

    (void)line_info(u, LINENO(line), &info);
    return info.blank;
}

static bool better_span(Span candidate, int priority, Span best,
                        int best_priority, bool have_best)
{
    u64 candidate_size;
    u64 best_size;

    if (!have_best)
        return true;
    candidate_size = span_size(candidate);
    best_size = span_size(best);
    return candidate_size < best_size ||
           (candidate_size == best_size && priority > best_priority);
}

bool sag_block_level(UnitCtx *u, ByteOff p, u32 level, Span *out)
{
    Span inner;
    u64 len;

    if (u == NULL || u->tb == NULL || out == NULL)
        SAG_BUG("sag_block_level: incomplete context");
    ensure_providers();
    len = sag_textbuf_len(u->tb);
    if (p.v > len)
        p = BYTEOFF(len);
    inner = (Span){p.v, p.v};
    for (u32 depth = 0U; depth <= level; depth++) {
        Span best = {0U, 0U};
        int best_priority = INT_MIN;
        bool have_best = false;

        for (u8 i = 0U; i < provider_count; i++) {
            Span candidate;
            BlockProvider *provider = &providers[i];

            if (provider->enclosing == NULL ||
                !provider->enclosing(provider->ctx, u, p, inner,
                                     &candidate))
                continue;
            if (candidate.lo > candidate.hi || candidate.hi > len ||
                !span_strictly_contains(candidate, inner))
                continue;
            if (better_span(candidate, provider->priority, best,
                            best_priority, have_best)) {
                best = candidate;
                best_priority = provider->priority;
                have_best = true;
            }
        }
        if (!have_best) {
            Span whole = {0U, len};

            if (span_strictly_contains(whole, inner))
                best = whole;
            else {
                *out = whole;
                return true;
            }
        }
        inner = best;
    }
    *out = inner;
    return true;
}

static bool paragraph_run(UnitCtx *u, u64 line, Span *out,
                          u64 *first_line, u64 *last_line)
{
    u64 count = sag_textbuf_line_count(u->tb);
    bool blank;
    u64 lo;
    u64 hi;

    if (count == 0U)
        return false;
    if (line >= count)
        line = count - 1U;
    blank = line_blank(u, line);
    lo = line;
    while (lo != 0U && line_blank(u, lo - 1U) == blank)
        lo--;
    hi = line;
    while (hi + 1U < count && line_blank(u, hi + 1U) == blank)
        hi++;
    *out = (Span){sag_textbuf_line_span(u->tb, LINENO(lo)).lo,
                  sag_textbuf_line_span(u->tb, LINENO(hi)).hi};
    if (first_line != NULL)
        *first_line = lo;
    if (last_line != NULL)
        *last_line = hi;
    return true;
}

static bool paragraph_section(UnitCtx *u, u64 line, Span *out)
{
    u64 count = sag_textbuf_line_count(u->tb);
    u64 lo = 0U;
    u64 hi = count == 0U ? 0U : count - 1U;
    u64 cursor = 0U;

    if (count == 0U || line_blank(u, line))
        return false;
    while (cursor < line) {
        if (line_blank(u, cursor)) {
            u64 end = cursor;

            while (end + 1U < count && line_blank(u, end + 1U))
                end++;
            if (end > cursor && end < line)
                lo = end + 1U;
            cursor = end + 1U;
        } else {
            cursor++;
        }
    }
    cursor = line + 1U;
    while (cursor < count) {
        if (line_blank(u, cursor)) {
            u64 end = cursor;

            while (end + 1U < count && line_blank(u, end + 1U))
                end++;
            if (end > cursor) {
                hi = cursor - 1U;
                break;
            }
            cursor = end + 1U;
        } else {
            cursor++;
        }
    }
    if (lo > hi)
        return false;
    *out = (Span){sag_textbuf_line_span(u->tb, LINENO(lo)).lo,
                  sag_textbuf_line_span(u->tb, LINENO(hi)).hi};
    return true;
}

static bool paragraph_enclosing(void *ctx, UnitCtx *u, ByteOff p,
                                Span inner, Span *out)
{
    LineNo line;
    Span run;
    Span section;

    (void)ctx;
    line = sag_textbuf_line_of(u->tb, p);
    if (!paragraph_run(u, line.v, &run, NULL, NULL))
        return false;
    if (span_strictly_contains(run, inner)) {
        *out = run;
        return true;
    }
    if (paragraph_section(u, line.v, &section) &&
        span_strictly_contains(section, inner)) {
        *out = section;
        return true;
    }
    return false;
}

static bool indent_candidate(UnitCtx *u, u64 target, u64 threshold,
                             bool include_equal, Span *out)
{
    u64 count = sag_textbuf_line_count(u->tb);
    u64 lo = target;
    u64 hi = target;
    u64 committed_hi = target;
    LineInfo info;

    if (include_equal && threshold == 0U) {
        u64 first = 0U;
        u64 last = count - 1U;

        while (first < count) {
            (void)line_info(u, LINENO(first), &info);
            if (!info.blank)
                break;
            first++;
        }
        while (last > first) {
            (void)line_info(u, LINENO(last), &info);
            if (!info.blank)
                break;
            last--;
        }
        if (first < count) {
            *out = (Span){sag_textbuf_line_span(u->tb, LINENO(first)).lo,
                          sag_textbuf_line_span(u->tb, LINENO(last)).hi};
            return true;
        }
    }

    while (include_equal && lo != 0U) {
        (void)line_info(u, LINENO(lo - 1U), &info);
        if (!info.blank && info.indent < threshold)
            break;
        lo--;
    }
    for (u64 at = target; at < count; at++) {
        (void)line_info(u, LINENO(at), &info);
        if (!info.blank &&
            (info.indent < threshold ||
             (!include_equal && at != target && info.indent == threshold)))
            break;
        hi = at;
        if (!info.blank)
            committed_hi = at;
    }
    while (lo < target) {
        (void)line_info(u, LINENO(lo), &info);
        if (!info.blank)
            break;
        lo++;
    }
    hi = committed_hi;
    *out = (Span){sag_textbuf_line_span(u->tb, LINENO(lo)).lo,
                  sag_textbuf_line_span(u->tb, LINENO(hi)).hi};
    return true;
}

static bool indent_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                             Span *out)
{
    u64 count = sag_textbuf_line_count(u->tb);
    u64 line = sag_textbuf_line_of(u->tb, p).v;
    u64 target = line;
    LineInfo target_info;
    Span best = {0U, 0U};
    bool have_best = false;

    (void)ctx;
    if (count == 0U)
        return false;
    (void)line_info(u, LINENO(target), &target_info);
    if (target_info.blank) {
        bool found = false;

        for (u64 at = target; at != 0U; at--) {
            (void)line_info(u, LINENO(at - 1U), &target_info);
            if (!target_info.blank) {
                target = at - 1U;
                found = true;
                break;
            }
        }
        if (!found) {
            for (u64 at = target + 1U; at < count; at++) {
                (void)line_info(u, LINENO(at), &target_info);
                if (!target_info.blank) {
                    target = at;
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            return false;
    }
    /* At level zero an unindented line cannot yield a useful indentation
     * unit smaller than its paragraph.  Avoid walking to both file ends on
     * large flat files; paragraph/section and the buffer fallback own those
     * outer levels. */
    if (target_info.indent == 0U && inner.lo == inner.hi)
        return false;
    {
        Span candidate;

        (void)indent_candidate(u, target, target_info.indent, true,
                               &candidate);
        if (span_strictly_contains(candidate, inner)) {
            best = candidate;
            have_best = true;
        }
    }
    {
        u64 search = target;
        u64 child_indent = target_info.indent;

        while (search != 0U) {
            LineInfo header;
            bool found = false;

            for (u64 at = search; at != 0U; at--) {
                (void)line_info(u, LINENO(at - 1U), &header);
                if (!header.blank && header.indent < child_indent) {
                    search = at - 1U;
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
            {
                Span candidate;

                (void)indent_candidate(u, search, header.indent, false,
                                       &candidate);
                if (span_strictly_contains(candidate, inner) &&
                    (!have_best || span_size(candidate) < span_size(best))) {
                    best = candidate;
                    have_best = true;
                }
            }
            child_indent = header.indent;
        }
    }
    if (!have_best)
        return false;
    *out = best;
    return true;
}

static bool escaped_at(const u8 *line, u64 pos)
{
    u64 slashes = 0U;

    while (pos != 0U && line[--pos] == (u8)'\\')
        slashes++;
    return (slashes & 1U) != 0U;
}

static u64 quote_count(const u8 *line, u64 len, u8 quote)
{
    u64 count = 0U;

    for (u64 i = 0U; i < len; i++) {
        if (line[i] == quote && !escaped_at(line, i))
            count++;
    }
    return count;
}

static bool opener_for(u8 close, u8 *open)
{
    static const struct {
        u8 open;
        u8 close;
    } pairs[] = {{'(', ')'}, {'[', ']'}, {'{', '}'}};

    for (size_t i = 0U; i < SAG_ARRAY_LEN(pairs); i++) {
        if (pairs[i].close == close) {
            *open = pairs[i].open;
            return true;
        }
    }
    return false;
}

static bool is_open(u8 byte)
{
    return byte == (u8)'(' || byte == (u8)'[' || byte == (u8)'{';
}

static bool scope_window_has_delimiter(const u8 *bytes, u64 len)
{
    for (u64 i = 0U; i < len; i++) {
        u8 byte = bytes[i];

        if (is_open(byte) || byte == (u8)')' || byte == (u8)']' ||
            byte == (u8)'}')
            return true;
    }
    return false;
}

static void scope_push(ScopeStack *stack, ScopeOpen open)
{
    if (stack->len == stack->cap) {
        size_t cap = stack->cap == 0U ? 64U : stack->cap * 2U;

        if (cap < stack->cap)
            SAG_BUG("scope stack capacity overflow");
        stack->data = sag_xreallocarray(stack->data, cap,
                                        sizeof(*stack->data));
        stack->cap = cap;
    }
    stack->data[stack->len++] = open;
}

static bool scope_pair(UnitCtx *u, ByteOff p, Span inner, Span *out,
                       u64 *open_off, u64 *close_off)
{
    u64 count = sag_textbuf_line_count(u->tb);
    u64 center = sag_textbuf_line_of(u->tb, p).v;
    u64 first = center > SAG_BLOCK_SCAN_LINES
                    ? center - SAG_BLOCK_SCAN_LINES
                    : 0U;
    u64 last = center + SAG_BLOCK_SCAN_LINES;
    ScopeStack stack = {0};
    Span best = {0U, 0U};
    u64 best_open = 0U;
    u64 best_close = 0U;
    bool have_best = false;
    bool block_comment = false;
    Span window;
    u64 window_len;
    u8 *bytes;
    u64 cursor = 0U;

    if (last >= count || last < center)
        last = count - 1U;
    window.lo = sag_textbuf_line_span(u->tb, LINENO(first)).lo;
    window.hi = sag_textbuf_line_span(u->tb, LINENO(last)).hi;
    window_len = window.hi - window.lo;
    bytes = sag_xmalloc((size_t)(window_len == 0U ? 1U : window_len));
    if (!read_span(u->tb, window, bytes))
        SAG_BUG("scope provider cannot read scan window");
    if (!scope_window_has_delimiter(bytes, window_len)) {
        free(bytes);
        return false;
    }
    while (cursor < window_len) {
        u64 raw_len = 0U;
        u64 absolute = window.lo + cursor;
        u8 *line = bytes + cursor;
        Span line_span;
        u64 len;
        u64 singles;
        u64 doubles;
        u8 quote = 0U;

        while (cursor + raw_len < window_len) {
            raw_len++;
            if (line[raw_len - 1U] == (u8)'\n')
                break;
        }
        line_span = (Span){absolute, absolute + raw_len};
        len = line_content_hi(line, line_span);
        singles = quote_count(line, len, (u8)'\'');
        doubles = quote_count(line, len, (u8)'"');
        for (u64 i = 0U; i < len; i++) {
            u8 byte = line[i];

            if (block_comment) {
                if (byte == (u8)'*' && i + 1U < len &&
                    line[i + 1U] == (u8)'/') {
                    block_comment = false;
                    i++;
                }
                continue;
            }
            if (quote != 0U) {
                if (byte == (u8)'\\' && i + 1U < len) {
                    i++;
                    continue;
                }
                if (byte == quote)
                    quote = 0U;
                continue;
            }
            if (byte == (u8)'/' && i + 1U < len &&
                line[i + 1U] == (u8)'*') {
                block_comment = true;
                i++;
                continue;
            }
            if ((byte == (u8)'/' && i + 1U < len &&
                 line[i + 1U] == (u8)'/') ||
                (byte == (u8)'-' && i + 1U < len &&
                 line[i + 1U] == (u8)'-') ||
                byte == (u8)'#' || byte == (u8)';')
                break;
            if ((byte == (u8)'\'' && singles != 0U &&
                 (singles & 1U) == 0U) ||
                (byte == (u8)'"' && doubles != 0U &&
                 (doubles & 1U) == 0U)) {
                quote = byte;
                continue;
            }
            if (is_open(byte)) {
                scope_push(&stack,
                           (ScopeOpen){byte, line_span.lo + i});
            } else {
                u8 wanted;

                if (opener_for(byte, &wanted) && stack.len != 0U &&
                    stack.data[stack.len - 1U].byte == wanted) {
                    ScopeOpen open = stack.data[--stack.len];
                    Span candidate = {open.off, line_span.lo + i + 1U};

                    if (candidate.lo <= p.v && candidate.hi >= p.v &&
                        span_strictly_contains(candidate, inner) &&
                        (!have_best ||
                         span_size(candidate) < span_size(best))) {
                        best = candidate;
                        best_open = open.off;
                        best_close = line_span.lo + i;
                        have_best = true;
                    }
                }
            }
        }
        cursor += raw_len;
    }
    free(bytes);
    free(stack.data);
    if (!have_best)
        return false;
    *out = best;
    if (open_off != NULL)
        *open_off = best_open;
    if (close_off != NULL)
        *close_off = best_close;
    return true;
}

static bool scope_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                            Span *out)
{
    (void)ctx;
    return scope_pair(u, p, inner, out, NULL, NULL);
}

bool sag_block_match(UnitCtx *u, ByteOff p, bool next, ByteOff *out)
{
    Span pair;
    u64 open_off;
    u64 close_off;

    if (u == NULL || u->tb == NULL || out == NULL)
        SAG_BUG("sag_block_match: incomplete context");
    if (p.v > sag_textbuf_len(u->tb))
        p = BYTEOFF(sag_textbuf_len(u->tb));
    if (!scope_pair(u, p, (Span){p.v, p.v}, &pair, &open_off, &close_off))
        return false;
    (void)pair;
    *out = BYTEOFF(next ? close_off : open_off);
    return true;
}

static bool cluster_white_at(const TextBuf *tb, ByteOff at)
{
    Span line = sag_textbuf_line_span(tb, sag_textbuf_line_of(tb, at));
    SagTextCluster cluster;

    return sag_text_cluster_next(tb, line, at, &cluster) &&
           sag_unicode_is_white_space(cluster.base_cp);
}

static ByteOff skip_white_next(const TextBuf *tb, ByteOff at)
{
    u64 len = sag_textbuf_len(tb);

    while (at.v < len && cluster_white_at(tb, at))
        at = sag_grapheme_next_boundary(tb, at);
    return at;
}

static ByteOff skip_white_prev(const TextBuf *tb, ByteOff at)
{
    while (at.v != 0U) {
        ByteOff prev = sag_grapheme_prev_boundary(tb, at);

        if (!cluster_white_at(tb, prev))
            break;
        at = prev;
    }
    return at;
}

static Span block_span(UnitCtx *u, ByteOff p, bool alt)
{
    Span span;

    (void)alt;
    (void)sag_block_level(u, p, 0U, &span);
    return span;
}

static ByteOff block_home(UnitCtx *u, ByteOff p, bool alt)
{
    return BYTEOFF(block_span(u, p, alt).lo);
}

static ByteOff block_end(UnitCtx *u, ByteOff p, bool alt)
{
    return BYTEOFF(block_span(u, p, alt).hi);
}

static ByteOff block_next(UnitCtx *u, ByteOff p, bool alt)
{
    u64 len = sag_textbuf_len(u->tb);
    Span current;
    ByteOff probe;
    Span sibling;
    Span parent;

    (void)alt;
    if (p.v >= len)
        return BYTEOFF(len);
    current = block_span(u, p, false);
    if (current.lo == 0U && current.hi == len)
        return BYTEOFF(len);
    probe = skip_white_next(u->tb, BYTEOFF(current.hi));
    if (probe.v < len && sag_block_level(u, probe, 0U, &sibling) &&
        sibling.lo > p.v)
        return BYTEOFF(sibling.lo);
    (void)sag_block_level(u, p, 1U, &parent);
    if (parent.hi > p.v)
        return BYTEOFF(parent.hi);
    return BYTEOFF(len);
}

static ByteOff block_prev(UnitCtx *u, ByteOff p, bool alt)
{
    Span current;
    ByteOff probe;
    ByteOff best = BYTEOFF(UINT64_MAX);
    Span parent;

    (void)alt;
    if (p.v == 0U)
        return BYTEOFF(0U);
    current = block_span(u, p, false);
    probe = skip_white_prev(u->tb, BYTEOFF(current.lo));
    if (probe.v != 0U) {
        probe = sag_grapheme_prev_boundary(u->tb, probe);
        for (u32 level = 0U; level < SAG_SEL_DEPTH; level++) {
            Span sibling;
            ByteOff after;

            if (!sag_block_level(u, probe, level, &sibling) ||
                sibling.lo >= p.v)
                break;
            after = skip_white_next(u->tb, BYTEOFF(sibling.hi));
            if (after.v >= current.lo)
                best = BYTEOFF(sibling.lo);
            if (sibling.lo == 0U || sibling.hi >= current.hi)
                break;
        }
        if (best.v != UINT64_MAX)
            return best;
    }
    (void)sag_block_level(u, p, 1U, &parent);
    if (parent.lo < p.v)
        return BYTEOFF(parent.lo);
    return BYTEOFF(0U);
}

const UnitOps sag_unit_block = {
    "block", block_next, block_prev, block_home, block_end, block_span,
};
