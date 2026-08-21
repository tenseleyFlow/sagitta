#include "mod/ai/context.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "text/piece.h"
#include "ui/message.h"
#include "ui/win.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"

enum {
    AI_CONTEXT_BYTES_DEFAULT = 4096,
    AI_CONTEXT_PREFIX_PCT_DEFAULT = 75,
    /* Enough look-behind for ordinary extended clusters while preserving
     * the contract that work is bounded independently of file size. */
    AI_CONTEXT_GRAPHEME_MARGIN = 256
};

static AiRedactCheck redact_check;
static AiContextBuildTestFn context_build_observe;
static void *context_build_observe_opaque;

void yew_ai_redact_hook_set(AiRedactCheck check)
{
    redact_check = check;
}

bool yew_ai_redact_check(Ed *ed, const AiCtx *ctx, RedactHit *hit)
{
    if (hit != NULL)
        (void)memset(hit, 0, sizeof(*hit));
    return redact_check != NULL && redact_check(ed, ctx, hit);
}

void yew_ai_context_build_test_set(AiContextBuildTestFn observe,
                                   void *opaque)
{
    context_build_observe = observe;
    context_build_observe_opaque = opaque;
}

static void context_error(AiErr *err, AiErrKind kind, const char *message)
{
    if (err == NULL)
        return;
    (void)memset(err, 0, sizeof(*err));
    err->kind = (u8)kind;
    if (message != NULL)
        (void)snprintf(err->msg, sizeof(err->msg), "%s", message);
}

static u32 context_option(Ed *ed, Buffer *buf, Win *win,
                          const char *name, u32 fallback)
{
    OptVal value;

    if (yew_opt_get(ed, buf, win, name, (u32)strlen(name), &value) &&
        value.type == (u8)YEW_OPT_INT && value.as.i >= 0 &&
        (u64)value.as.i <= UINT_MAX)
        return (u32)value.as.i;
    return fallback;
}

static bool context_enabled(Ed *ed)
{
    OptVal value;

    return yew_opt_get(ed, NULL, NULL, "ai.enable", 9U, &value) &&
           value.type == (u8)YEW_OPT_BOOL && value.as.b;
}

static const char *context_redact_mode(Ed *ed)
{
    OptVal value;

    if (yew_opt_get(ed, NULL, NULL, "ai.on_redact", 12U, &value) &&
        value.type == (u8)YEW_OPT_ENUM && value.as.str.s != NULL)
        return value.as.str.s;
    return "block";
}

static bool context_elide(Arena *arena, const char *rule, ByteOff off,
                          u32 hit_len, const u8 **bytes, u32 *len)
{
    char replacement[96];
    int printed;
    size_t replacement_len;
    size_t before;
    size_t after;
    size_t total;
    u8 *copy;

    if (arena == NULL || bytes == NULL || len == NULL || *bytes == NULL ||
        off.v > *len || hit_len > *len - off.v)
        return false;
    printed = snprintf(replacement, sizeof(replacement), "<redacted:%s>",
                       rule == NULL ? "secret" : rule);
    if (printed < 0)
        return false;
    replacement_len = (size_t)printed < sizeof(replacement) ?
                      (size_t)printed : sizeof(replacement) - 1U;
    before = (size_t)off.v;
    after = (size_t)*len - before - hit_len;
    if (before > UINT32_MAX - replacement_len ||
        before + replacement_len > UINT32_MAX - after)
        return false;
    total = before + replacement_len + after;
    copy = arena_alloc(arena, total == 0U ? 1U : total, 1U);
    if (before != 0U)
        (void)memcpy(copy, *bytes, before);
    (void)memcpy(copy + before, replacement, replacement_len);
    if (after != 0U)
        (void)memcpy(copy + before + replacement_len,
                     *bytes + before + hit_len, after);
    *bytes = copy;
    *len = (u32)total;
    return true;
}

static bool context_apply_redaction(Ed *ed, Arena *arena, AiCtx *context,
                                    const RedactHit *hit, AiErr *err)
{
    const AiBackendEntry *entry = yew_ai_backend_selected(ed);
    const char *mode = context_redact_mode(ed);
    bool remote = entry == NULL || !entry->backend.url.loopback;
    const char *host = entry == NULL || entry->backend.url.host == NULL ?
                       "the selected backend" : entry->backend.url.host;
    const char *rule = hit->rule == NULL ? "secret" : hit->rule;

    if (strcmp(mode, "off") == 0)
        return true;
    if (remote && strcmp(mode, "elide") != 0) {
        context_error(err, YEW_AI_ERR_PROTOCOL, "");
        yew_ai_block_offer(ed,
                           ed->win == NULL || ed->win->buf == NULL ? 0U :
                               ed->win->buf->id,
                           hit->line_1based);
        (void)snprintf(
            err->msg, sizeof(err->msg),
            "AI request blocked: line %u matches '%s'. Nothing was sent to "
            "%s. [g] go to line %u [i] ignore this file for the session "
            "[p] :ai privacy",
            hit->line_1based, rule, host, hit->line_1based);
        yew_ai_status_note(ed, YEW_AI_ERR_PROTOCOL);
        return false;
    }
    if (!context_elide(arena, rule, hit->off, hit->len,
                       hit->in_prefix ? &context->prefix : &context->suffix,
                       hit->in_prefix ? &context->plen : &context->slen)) {
        context_error(err, YEW_AI_ERR_PROTOCOL,
                      "AI redaction produced an invalid context span");
        yew_ai_status_note(ed, YEW_AI_ERR_PROTOCOL);
        return false;
    }
    if (ed->ai != NULL && !ed->ai->redact_elide_messaged) {
        yew_msg(ed, YEW_MSG_INFO,
                "AI elided text matching '%s' before sending locally", rule);
        ed->ai->redact_elide_messaged = true;
    }
    return true;
}

static bool text_copy(const TextBuf *tb, u64 lo, u64 hi, u8 *dst)
{
    TextIter iter;
    u64 copied = 0U;
    u64 need = hi - lo;

    if (need == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(lo)))
        return false;
    while (copied < need) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available))
            return false;
        take = available < need - copied ? available : need - copied;
        if (take == 0U)
            return false;
        (void)memcpy(dst + (size_t)copied, bytes, (size_t)take);
        copied += take;
        if (copied < need && !yew_textiter_advance(&iter, tb))
            return false;
    }
    return true;
}

static size_t boundary_at_or_after(const u8 *bytes, size_t len, size_t at)
{
    size_t pos = 0U;

    while (pos < at && pos < len) {
        size_t next = yew_gb_next_bytes(bytes, len, pos);

        if (next <= pos)
            return len;
        pos = next;
    }
    return pos;
}

static size_t boundary_at_or_before(const u8 *bytes, size_t len, size_t at)
{
    size_t pos = 0U;

    while (pos < at && pos < len) {
        size_t next = yew_gb_next_bytes(bytes, len, pos);

        if (next <= pos || next > at)
            return pos;
        pos = next;
    }
    return pos;
}

static bool build_prefix(const TextBuf *tb, u64 cursor, u32 budget,
                         Arena *arena, const u8 **out, u32 *out_len)
{
    u64 raw = cursor > budget ? cursor - budget : 0U;
    u64 margin = raw > AI_CONTEXT_GRAPHEME_MARGIN ?
                 raw - AI_CONTEXT_GRAPHEME_MARGIN : 0U;
    size_t n = (size_t)(cursor - margin);
    u8 *copy = arena_alloc(arena, n == 0U ? 1U : n, 1U);
    size_t start = (size_t)(raw - margin);
    size_t quarter = budget / 4U;
    size_t i;

    if (!text_copy(tb, margin, cursor, copy))
        return false;
    if (raw != 0U) {
        for (i = start; i < n && i - start <= quarter; i++) {
            if (copy[i] == (u8)'\n') {
                start = i + 1U;
                break;
            }
        }
    }
    start = boundary_at_or_after(copy, n, start);
    if (yew_utf8_validate(copy + start, n - start) != n - start)
        return false;
    *out_len = (u32)(n - start);
    *out = arena_alloc(arena, *out_len == 0U ? 1U : *out_len, 1U);
    if (*out_len != 0U)
        (void)memcpy((u8 *)*out, copy + start, *out_len);
    return true;
}

static bool build_suffix(const TextBuf *tb, u64 cursor, u64 total,
                         u32 budget, Arena *arena,
                         const u8 **out, u32 *out_len)
{
    u64 raw = total - cursor < budget ? total : cursor + budget;
    u64 margin = total - raw < AI_CONTEXT_GRAPHEME_MARGIN ?
                 total : raw + AI_CONTEXT_GRAPHEME_MARGIN;
    size_t n = (size_t)(margin - cursor);
    u8 *copy = arena_alloc(arena, n == 0U ? 1U : n, 1U);
    size_t end = (size_t)(raw - cursor);
    size_t quarter = budget / 4U;
    size_t floor = end > quarter ? end - quarter : 0U;
    size_t i;

    if (!text_copy(tb, cursor, margin, copy))
        return false;
    if (raw != total) {
        for (i = end; i > floor; i--) {
            if (copy[i - 1U] == (u8)'\n') {
                end = i;
                break;
            }
        }
    }
    end = boundary_at_or_before(copy, n, end);
    if (yew_utf8_validate(copy, end) != end)
        return false;
    *out_len = (u32)end;
    *out = arena_alloc(arena, end == 0U ? 1U : end, 1U);
    if (end != 0U)
        (void)memcpy((u8 *)*out, copy, end);
    return true;
}

static const char *context_path(Ed *ed, const Buffer *buf, Arena *arena)
{
    const char *root = yew_ws_root(ed);
    const char *path = buf->path != NULL ? buf->path : buf->meta.realpath;
    const char *relative;
    size_t root_len;
    size_t len;
    char *copy;
    size_t i;

    if (path == NULL || path[0] == '\0')
        return "";
    root_len = strlen(root);
    if (path[0] == '/') {
        if (strncmp(path, root, root_len) != 0 ||
            (root_len != 1U && path[root_len] != '/'))
            return "";
        relative = path + root_len;
        if (*relative == '/')
            relative++;
    } else {
        relative = path;
        while (relative[0] == '.' && relative[1] == '/')
            relative += 2;
        if (strcmp(relative, "..") == 0 || strncmp(relative, "../", 3U) == 0)
            return "";
    }
    len = strlen(relative);
    copy = arena_alloc(arena, len + 1U, 1U);
    for (i = 0U; i < len; i++)
        copy[i] = relative[i] == '\\' ? '/' : relative[i];
    copy[len] = '\0';
    return copy;
}

bool yew_ai_context_build(Ed *ed, Win *win, const ShadowReq *request,
                          Arena *arena, AiCtx *out, AiErr *err)
{
    Buffer *buf;
    u64 total;
    u64 cursor;
    u32 budget;
    u32 prefix_pct;
    u32 prefix_budget;
    u32 suffix_budget;
    RedactHit hit;

    if (context_build_observe != NULL)
        context_build_observe(context_build_observe_opaque);

    if (out != NULL)
        (void)memset(out, 0, sizeof(*out));
    if (ed == NULL || win == NULL || request == NULL || arena == NULL ||
        out == NULL || (buf = win->buf) == NULL || buf->tb == NULL) {
        context_error(err, YEW_AI_ERR_PROTOCOL, "invalid AI context request");
        return false;
    }
    /* Independent backend gate: configuration may enable AI before a
     * backend has been selected; never build or redact context in that
     * state.  This is intentionally redundant with shadow_ai's request
     * gate so new callers cannot accidentally construct outbound context. */
    if (context_enabled(ed) &&
        yew_ai_backend_selected(ed) == NULL) {
        context_error(err, YEW_AI_ERR_CANCELLED,
                      "AI context declined: no backend selected");
        return false;
    }
    total = yew_textbuf_len(buf->tb);
    cursor = request->pos.v;
    if (request->buf_id != buf->id || cursor > total) {
        context_error(err, YEW_AI_ERR_PROTOCOL, "stale AI context request");
        return false;
    }
    budget = context_option(ed, buf, win, "ai.context_bytes",
                            AI_CONTEXT_BYTES_DEFAULT);
    prefix_pct = context_option(ed, buf, win, "ai.context_prefix_pct",
                                AI_CONTEXT_PREFIX_PCT_DEFAULT);
    if (prefix_pct > 100U)
        prefix_pct = AI_CONTEXT_PREFIX_PCT_DEFAULT;
    prefix_budget = (u32)(((u64)budget * prefix_pct) / 100U);
    suffix_budget = budget - prefix_budget;
    if (!build_prefix(buf->tb, cursor, prefix_budget, arena,
                      &out->prefix, &out->plen) ||
        !build_suffix(buf->tb, cursor, total, suffix_budget, arena,
                      &out->suffix, &out->slen)) {
        context_error(err, YEW_AI_ERR_PROTOCOL,
                      "AI context is not valid UTF-8");
        return false;
    }
    out->path = context_path(ed, buf, arena);
    out->lang = buf->lang == NULL ? "" : buf->lang;
    out->line_1based = (u32)(yew_textbuf_line_of(buf->tb,
                                                 request->pos).v + 1U);
    out->truncated_head = cursor > out->plen;
    out->truncated_tail = total - cursor > out->slen;

    /* Sprint 50 owns the patterns and block/elide policy.  This remains the
     * only redaction call site, before prompt construction can begin. */
    if (yew_ai_redact_check(ed, out, &hit) &&
        !context_apply_redaction(ed, arena, out, &hit, err))
        return false;
    context_error(err, YEW_AI_OK, "");
    return true;
}
