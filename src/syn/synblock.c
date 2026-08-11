#include "edit/block.h"

#include <stdlib.h>
#include <string.h>

#include "edit/buf.h"
#include "syn/engine.h"
#include "text/piece.h"

enum { YEW_SYN_UNIT_SCAN_LINES = 100000 };

typedef struct SynBlockLine {
    u8 *bytes;
    u32 len;
    Span span;
} SynBlockLine;

static bool syn_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                          Span *out);

static bool textbuf_byte(const TextBuf *tb, u64 off, u8 *byte)
{
    TextIter it;
    const u8 *bytes;
    u64 n;

    return yew_textiter_begin(&it, tb, BYTEOFF(off)) &&
           yew_textiter_chunk(&it, tb, &bytes, &n) && n != 0U &&
           (*byte = bytes[0], true);
}

static bool line_read(const TextBuf *tb, u64 line, SynBlockLine *out)
{
    TextIter it;
    u64 need;
    u64 copied = 0U;
    u8 byte;

    out->span = yew_textbuf_line_span(tb, LINENO(line));
    need = out->span.hi - out->span.lo;
    if (need != 0U && textbuf_byte(tb, out->span.hi - 1U, &byte) &&
        byte == (u8)'\n')
        need--;
    if (need != 0U && textbuf_byte(tb, out->span.lo + need - 1U, &byte) &&
        byte == (u8)'\r')
        need--;
    if (need > YEW_SYN_LINE_BYTE_CAP || need > UINT32_MAX)
        return false;
    out->bytes = malloc((size_t)(need == 0U ? 1U : need));
    if (out->bytes == NULL)
        return false;
    if (need != 0U && !yew_textiter_begin(&it, tb, BYTEOFF(out->span.lo))) {
        free(out->bytes);
        out->bytes = NULL;
        return false;
    }
    while (copied < need) {
        const u8 *bytes;
        u64 n;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &n))
            break;
        take = n < need - copied ? n : need - copied;
        (void)memcpy(out->bytes + copied, bytes, (size_t)take);
        copied += take;
        if (copied < need && !yew_textiter_advance(&it, tb))
            break;
    }
    if (copied != need) {
        free(out->bytes);
        out->bytes = NULL;
        return false;
    }
    out->len = (u32)need;
    return true;
}

static bool frame_is(const SynState *state, u8 depth, u16 ctx)
{
    return state != NULL && depth < state->depth &&
           state->f[depth].ctx == ctx;
}

static bool state_at_line(const Buffer *buf, u64 line, u32 at,
                          SynState *out)
{
    SynBlockLine text = {0};
    u32 entry;
    bool ok;

    if (line >= buf->syn.entry.len)
        return false;
    entry = buf->syn.entry.data[line];
    if (entry == YEW_SYN_STATE_UNKNOWN || !line_read(buf->tb, line, &text))
        return false;
    ok = yew_syn_stack_at(buf->syn.engine, entry, text.bytes, text.len, at,
                          out);
    free(text.bytes);
    return ok;
}

static bool line_first_frame(const Buffer *buf, u64 line, u8 depth,
                             u16 ctx, u32 from, bool want, u32 *at,
                             bool *found)
{
    SynBlockLine text = {0};
    SynState *trace;
    u32 entry;
    u32 p;

    *found = false;
    if (line >= buf->syn.entry.len)
        return false;
    entry = buf->syn.entry.data[line];
    if (entry == YEW_SYN_STATE_UNKNOWN || !line_read(buf->tb, line, &text))
        return false;
    trace = malloc(((size_t)text.len + 1U) * sizeof(*trace));
    if (trace == NULL ||
        !yew_syn_stack_trace(buf->syn.engine, entry, text.bytes, text.len,
                             trace, (size_t)text.len + 1U)) {
        free(trace);
        free(text.bytes);
        return false;
    }
    if (from > text.len)
        from = text.len;
    for (p = from; p <= text.len; p++) {
        if (frame_is(&trace[p], depth, ctx) == want) {
            *at = p;
            *found = true;
            free(trace);
            free(text.bytes);
            return true;
        }
    }
    free(trace);
    free(text.bytes);
    return true;
}

static bool line_last_frame_start(const Buffer *buf, u64 line, u8 depth,
                                  u16 ctx, u32 through, u32 *at)
{
    SynBlockLine text = {0};
    SynState *trace;
    u32 entry;
    bool previous = false;
    bool found = false;

    if (line >= buf->syn.entry.len)
        return false;
    entry = buf->syn.entry.data[line];
    if (entry == YEW_SYN_STATE_UNKNOWN || !line_read(buf->tb, line, &text))
        return false;
    trace = malloc(((size_t)text.len + 1U) * sizeof(*trace));
    if (trace == NULL ||
        !yew_syn_stack_trace(buf->syn.engine, entry, text.bytes, text.len,
                             trace, (size_t)text.len + 1U)) {
        free(trace);
        free(text.bytes);
        return false;
    }
    if (through > text.len)
        through = text.len;
    for (u32 p = 0U; p <= through; p++) {
        bool present;
        present = frame_is(&trace[p], depth, ctx);
        if (present && !previous) {
            *at = p;
            found = true;
        }
        previous = present;
    }
    free(trace);
    free(text.bytes);
    return found;
}

static bool entry_has(const Buffer *buf, u64 line, u8 depth, u16 ctx,
                      u32 *matching_entry)
{
    const SynState *state;
    u32 id;

    if (line >= buf->syn.entry.len)
        return false;
    id = buf->syn.entry.data[line];
    if (id != YEW_SYN_STATE_UNKNOWN && id == *matching_entry)
        return true;
    state = yew_syn_state_get(yew_syn_engine_states(buf->syn.engine), id);
    if (!frame_is(state, depth, ctx))
        return false;
    *matching_entry = id;
    return true;
}

static bool syntax_ready_at(const Buffer *buf, u64 line)
{
    u64 lines;

    if (buf == NULL || buf->tb == NULL || buf->syn.lang == YEW_LANG_NONE ||
        buf->syn.engine == NULL || buf->syn.degraded)
        return false;
    lines = yew_textbuf_line_count(buf->tb);
    return buf->syn.entry.len == lines && line < lines &&
           line < buf->syn.settled_to.v &&
           buf->syn.entry.data[line] != YEW_SYN_STATE_UNKNOWN;
}

static bool frame_bounds(const Buffer *buf, u64 center, u32 local,
                         u8 depth, u16 ctx, Span *out)
{
    u64 lo_line = center;
    u64 hi_line = center;
    u64 scanned = 0U;
    u32 matching_entry = YEW_SYN_STATE_UNKNOWN;
    u32 local_at;
    bool found;
    bool tail_unsettled = false;

    while (lo_line != 0U &&
           entry_has(buf, lo_line, depth, ctx, &matching_entry)) {
        if (++scanned > YEW_SYN_UNIT_SCAN_LINES)
            return false;
        lo_line--;
    }
    if (!line_last_frame_start(buf, lo_line, depth, ctx,
                               lo_line == center ? local : UINT32_MAX,
                               &local_at)) {
        if (lo_line == center)
            return false;
        lo_line++;
        local_at = 0U;
    }
    out->lo = yew_textbuf_line_span(buf->tb, LINENO(lo_line)).lo + local_at;

    scanned = 0U;
    while (hi_line + 1U < buf->syn.entry.len) {
        if (hi_line + 1U >= buf->syn.settled_to.v ||
            buf->syn.entry.data[hi_line + 1U] == YEW_SYN_STATE_UNKNOWN) {
            tail_unsettled = true;
            break;
        }
        if (!entry_has(buf, hi_line + 1U, depth, ctx, &matching_entry))
            break;
        if (++scanned > YEW_SYN_UNIT_SCAN_LINES)
            return false;
        hi_line++;
    }
    /* entry[] identifies the last line whose entry stack still contains
     * this frame.  Interior lines need only sequential cached-state reads;
     * replay the last such line once to locate its exact closing byte. */
    if (!line_first_frame(buf, hi_line, depth, ctx,
                          hi_line == center ? local : 0U, false,
                          &local_at, &found))
        return false;
    if (found) {
        out->hi = yew_textbuf_line_span(buf->tb, LINENO(hi_line)).lo +
                  local_at;
        return true;
    }
    if (tail_unsettled)
        return false;
    {
        SynBlockLine text = {0};
        if (!line_read(buf->tb, hi_line, &text))
            return false;
        out->hi = text.span.lo + text.len;
        free(text.bytes);
    }
    return true;
}

static bool span_strictly_contains(Span outer, Span inner)
{
    return outer.lo <= inner.lo && outer.hi >= inner.hi &&
           (outer.lo < inner.lo || outer.hi > inner.hi);
}

static bool syn_enclosing(void *ctx, UnitCtx *u, ByteOff p, Span inner,
                          Span *out)
{
    const Buffer *buf;
    const SynDef *def;
    SynState state;
    Span line_span;
    u64 line;
    u32 local;
    int atom = -1;
    int depth;

    (void)ctx;
    if (u == NULL || out == NULL || u->buf == NULL || u->buf->tb == NULL)
        return false;
    buf = u->buf;
    line = yew_textbuf_line_of(buf->tb, p).v;
    if (!syntax_ready_at(buf, line))
        return false;
    def = yew_syn_engine_def(buf->syn.engine);
    if (def == NULL || def->ctxs == NULL)
        return false;
    line_span = yew_textbuf_line_span(buf->tb, LINENO(line));
    local = p.v <= line_span.lo ? 0U :
            p.v - line_span.lo > UINT32_MAX ? UINT32_MAX :
            (u32)(p.v - line_span.lo);
    if (!state_at_line(buf, line, local, &state) || state.lost != 0U)
        return false;
    for (depth = 0; depth < state.depth; depth++) {
        u16 id = state.f[depth].ctx;
        if (id >= def->nctxs)
            return false;
        if ((def->ctxs[id].flags & YEW_SYN_CTX_UNIT_ATOM) != 0U) {
            atom = depth;
            break;
        }
    }
    for (depth = atom >= 0 ? atom : (int)state.depth - 1;
         depth >= 0; depth--) {
        u16 id = state.f[depth].ctx;
        u8 flags = def->ctxs[id].flags;
        Span candidate;

        if ((flags & (YEW_SYN_CTX_UNIT_SPAN | YEW_SYN_CTX_UNIT_ATOM)) == 0U)
            continue;
        if (!frame_bounds(buf, line, local, (u8)depth, id, &candidate))
            return false;
        if (span_strictly_contains(candidate, inner)) {
            *out = candidate;
            return true;
        }
    }
    return false;
}

void yew_block_provider_syntax_install(bool enabled)
{
    BlockProvider provider = {"syntax", 40,
                              enabled ? syn_enclosing : NULL, NULL};

    yew_block_register(provider);
}

bool yew_syn_in_string_or_comment(const Buffer *buf, ByteOff off)
{
    SynBlockLine line = {0};
    SynSpan *spans;
    SynLineOut out;
    u64 line_no;
    u64 local;
    u8 attr = YEW_ATTR_TEXT;

    if (buf == NULL || buf->tb == NULL || buf->syn.lang == YEW_LANG_NONE ||
        buf->syn.engine == NULL || buf->syn.degraded)
        return false;
    line_no = yew_textbuf_line_of(buf->tb, off).v;
    if (line_no >= buf->syn.entry.len || buf->syn.settled_to.v <= line_no ||
        buf->syn.entry.data[line_no] == YEW_SYN_STATE_UNKNOWN ||
        !line_read(buf->tb, line_no, &line))
        return false;
    local = off.v <= line.span.lo ? 0U : off.v - line.span.lo;
    if (local >= line.len && line.len != 0U)
        local = line.len - 1U;
    spans = malloc(sizeof(*spans) * YEW_SYN_MAX_SPANS);
    if (spans == NULL) {
        free(line.bytes);
        return false;
    }
    out = (SynLineOut){spans, 0U, YEW_SYN_MAX_SPANS,
                       YEW_SYN_STATE_UNKNOWN, YEW_SYN_STOP_OK};
    yew_syn_line(buf->syn.engine, buf->syn.entry.data[line_no], line.bytes,
                 line.len, &out);
    if (out.stop == YEW_SYN_STOP_OK) {
        for (u32 i = 0U; i < out.n; i++) {
            u32 hi = out.spans[i].start + out.spans[i].len;
            if (local >= out.spans[i].start && local < hi) {
                attr = out.spans[i].attr;
                break;
            }
        }
    }
    free(spans);
    free(line.bytes);
    return (attr >= YEW_ATTR_STRING && attr <= YEW_ATTR_STRING_SPECIAL) ||
           (attr >= YEW_ATTR_COMMENT && attr <= YEW_ATTR_COMMENT_TODO);
}
