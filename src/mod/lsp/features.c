/*
 * Sprint 47 snippet downgrade policy.
 *
 * yew 1.0 does not expand snippets: there is no tab-stop mode, placeholder
 * navigation, or mirrored-field machinery.  The client advertises
 * snippetSupport:false, but servers sometimes ignore it, so every
 * insertTextFormat==2 item is downgraded here to deterministic plain text.
 * Defaults are retained, choices select their first value, variables and
 * bare tab stops disappear, and $0 records the final cursor position.
 * Snippet expansion is a post-1.0 feature, not a stub or TODO.
 */
#include "mod/lsp/features.h"

#include <stdbool.h>
#include <stddef.h>

enum { SNIPPET_DEPTH_MAX = 8 };

typedef struct SnippetStrip {
    const u8 *in;
    Bytebuf *out;
    size_t base;
    u32 cursor;
    bool have_cursor;
} SnippetStrip;

static bool snippet_ident_start(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool snippet_ident_continue(u8 c)
{
    return snippet_ident_start(c) || (c >= '0' && c <= '9');
}

static bool snippet_escaped(u8 c)
{
    return c == '$' || c == '\\' || c == '}' || c == ',' || c == '|';
}

static void snippet_cursor(SnippetStrip *s)
{
    size_t offset;

    if (s->have_cursor)
        return;
    offset = s->out->len - s->base;
    s->cursor = offset > UINT32_MAX ? UINT32_MAX : (u32)offset;
    s->have_cursor = true;
}

static bool snippet_is_zero(const u8 *in, u32 lo, u32 hi)
{
    u32 at;

    if (lo == hi)
        return false;
    for (at = lo; at < hi; at++)
        if (in[at] != '0')
            return false;
    return true;
}

static bool snippet_brace_end(const u8 *in, u32 n, u32 open, u32 *close)
{
    u32 at = open + 2U;
    u32 depth = 1U;

    while (at < n) {
        if (in[at] == '\\' && at + 1U < n) {
            at += 2U;
            continue;
        }
        if (in[at] == '$' && at + 1U < n && in[at + 1U] == '{') {
            depth++;
            at += 2U;
            continue;
        }
        if (in[at] == '}') {
            depth--;
            if (depth == 0U) {
                *close = at;
                return true;
            }
        }
        at++;
    }
    return false;
}

static void snippet_range(SnippetStrip *s, u32 lo, u32 hi, u8 depth);

static bool snippet_braced(SnippetStrip *s, u32 at, u32 hi, u8 depth,
                           u32 *after)
{
    u32 close;
    u32 id_lo;
    u32 id_hi;
    bool numeric;

    if (!snippet_brace_end(s->in, hi, at, &close))
        return false;
    id_lo = at + 2U;
    id_hi = id_lo;
    numeric = id_hi < close && s->in[id_hi] >= '0' && s->in[id_hi] <= '9';
    if (numeric) {
        while (id_hi < close && s->in[id_hi] >= '0' &&
               s->in[id_hi] <= '9')
            id_hi++;
    } else if (id_hi < close && snippet_ident_start(s->in[id_hi])) {
        while (id_hi < close && snippet_ident_continue(s->in[id_hi]))
            id_hi++;
    } else {
        return false;
    }
    *after = close + 1U;
    if (numeric && snippet_is_zero(s->in, id_lo, id_hi))
        snippet_cursor(s);
    if (id_hi == close)
        return true;
    if (depth >= SNIPPET_DEPTH_MAX)
        return true;
    if (s->in[id_hi] == ':') {
        snippet_range(s, id_hi + 1U, close, (u8)(depth + 1U));
        return true;
    }
    if (s->in[id_hi] == '|' && close > id_hi + 1U &&
        s->in[close - 1U] == '|') {
        u32 choice_hi = id_hi + 1U;

        while (choice_hi < close - 1U) {
            if (s->in[choice_hi] == '\\' && choice_hi + 1U < close - 1U) {
                choice_hi += 2U;
                continue;
            }
            if (s->in[choice_hi] == ',')
                break;
            choice_hi++;
        }
        snippet_range(s, id_hi + 1U, choice_hi, (u8)(depth + 1U));
        return true;
    }
    return false;
}

static bool snippet_dollar(SnippetStrip *s, u32 at, u32 hi, u8 depth,
                           u32 *after)
{
    u32 end;

    if (at + 1U >= hi)
        return false;
    if (s->in[at + 1U] == '{')
        return snippet_braced(s, at, hi, depth, after);
    end = at + 1U;
    if (s->in[end] >= '0' && s->in[end] <= '9') {
        while (end < hi && s->in[end] >= '0' && s->in[end] <= '9')
            end++;
        if (snippet_is_zero(s->in, at + 1U, end))
            snippet_cursor(s);
        *after = end;
        return true;
    }
    if (snippet_ident_start(s->in[end])) {
        while (end < hi && snippet_ident_continue(s->in[end]))
            end++;
        *after = end;
        return true;
    }
    return false;
}

static void snippet_range(SnippetStrip *s, u32 lo, u32 hi, u8 depth)
{
    u32 at = lo;

    while (at < hi) {
        u32 after;

        if (s->in[at] == '\\' && at + 1U < hi &&
            snippet_escaped(s->in[at + 1U])) {
            bytebuf_push_u8(s->out, s->in[at + 1U]);
            at += 2U;
        } else if (s->in[at] == '$' &&
                   snippet_dollar(s, at, hi, depth, &after)) {
            at = after;
        } else {
            bytebuf_push_u8(s->out, s->in[at]);
            at++;
        }
    }
}

u32 yew_lsp_snippet_strip(const u8 *in, u32 n, Bytebuf *out)
{
    SnippetStrip strip;
    size_t emitted;

    if (out == NULL || (n != 0U && in == NULL))
        return 0U;
    strip = (SnippetStrip){in, out, out->len, 0U, false};
    snippet_range(&strip, 0U, n, 0U);
    emitted = out->len - strip.base;
    return strip.have_cursor ? strip.cursor :
           (emitted > UINT32_MAX ? UINT32_MAX : (u32)emitted);
}
