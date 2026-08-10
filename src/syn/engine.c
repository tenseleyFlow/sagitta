#define _POSIX_C_SOURCE 200809L

#include "syn/engine.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "syn/theme.h"
#include "text/piece.h"
#include "util/log.h"

typedef struct SynCacheEnt {
    u64 line;
    u64 gen;
    u32 entry_state;
    u32 exit_state;
    u32 span_off;
    u32 n;
    u8 stop;
    bool valid;
} SynCacheEnt;

typedef struct SynCache {
    SynCacheEnt slots[YEW_SYN_SPAN_CACHE];
    u32 hand;
    u32 slab_hand;
    SynSpan *slab;
    u32 slab_cap;
    SynSpan scratch[YEW_SYN_MAX_SPANS];
    u8 *line;
    size_t line_cap;
} SynCache;

struct SynStateTab {
    SynState *states;
    u32 len;
    u32 cap;
    u32 *slots;
    u32 slots_cap;
    bool exhausted;
};

struct SynEngine {
    SynDef *def;
    SynStateTab *states;
    u8 *ctx_aux;
    u64 line_calls;
    u64 generation;
};

static void engine_index_aux(SynEngine *engine)
{
    u32 i;
    free(engine->ctx_aux);
    engine->ctx_aux = NULL;
    if (engine->def == NULL || engine->def->nctxs == 0U)
        return;
    if (engine->def->ctxs == NULL ||
        (engine->def->nrules != 0U && engine->def->rules == NULL))
        YEW_BUG("syntax: malformed compiled definition");
    engine->ctx_aux = yew_xcalloc(engine->def->nctxs,
                                  sizeof(*engine->ctx_aux));
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 j;
        for (j = 0U; j < ctx->nrules; j++) {
            u32 rule = ctx->first_rule + j;
            if (rule < engine->def->nrules &&
                engine->def->rules[rule].aux_match != SYN_AUXM_NONE) {
                engine->ctx_aux[i] = 1U;
                break;
            }
        }
    }
}

static u64 state_hash(const SynState *state)
{
    const u8 *p = (const u8 *)state;
    u64 h = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0U; i < sizeof(*state); i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void state_validate(const SynState *state)
{
    if (state == NULL)
        YEW_BUG("syntax: NULL state");
    if (state->def != 0U)
        YEW_BUG("syntax: embedded definitions are outside the 1.0 core");
    if (state->depth == 0U || state->depth > YEW_SYN_DEPTH_MAX)
        YEW_BUG("syntax: invalid state depth %u", (unsigned)state->depth);
}

static void state_rehash(SynStateTab *tab, u32 cap)
{
    u32 *slots = yew_xcalloc(cap, sizeof(*slots));
    u32 i;

    for (i = 1U; i < tab->len; i++) {
        u32 at = (u32)state_hash(&tab->states[i]) & (cap - 1U);
        while (slots[at] != 0U)
            at = (at + 1U) & (cap - 1U);
        slots[at] = i;
    }
    free(tab->slots);
    tab->slots = slots;
    tab->slots_cap = cap;
}

SynStateTab *yew_syn_state_tab_new(u16 root_ctx)
{
    SynStateTab *tab = yew_xcalloc(1U, sizeof(*tab));
    SynState root;

    tab->cap = 16U;
    tab->states = yew_xcalloc(tab->cap, sizeof(*tab->states));
    tab->len = 2U;
    (void)memset(&root, 0, sizeof(root));
    root.ctx[0] = root_ctx;
    root.depth = 1U;
    tab->states[YEW_SYN_STATE_ROOT] = root;
    state_rehash(tab, 32U);
    return tab;
}

void yew_syn_state_tab_free(SynStateTab *tab)
{
    if (tab == NULL)
        return;
    free(tab->slots);
    free(tab->states);
    free(tab);
}

u32 yew_syn_state_intern(SynStateTab *tab, const SynState *state)
{
    u32 at;

    if (tab == NULL)
        YEW_BUG("syntax: NULL state table");
    state_validate(state);
    at = (u32)state_hash(state) & (tab->slots_cap - 1U);
    while (tab->slots[at] != 0U) {
        u32 id = tab->slots[at];
        if (memcmp(&tab->states[id], state, sizeof(*state)) == 0)
            return id;
        at = (at + 1U) & (tab->slots_cap - 1U);
    }
    if (tab->len >= YEW_SYN_MAX_STATES) {
        if (!tab->exhausted)
            yew_log(YEW_LOG_WARN,
                    "syntax state table exhausted; reusing root state");
        tab->exhausted = true;
        return YEW_SYN_STATE_ROOT;
    }
    if (tab->len == tab->cap) {
        u32 cap = tab->cap * 2U;
        if (cap > YEW_SYN_MAX_STATES)
            cap = YEW_SYN_MAX_STATES;
        tab->states = yew_xreallocarray(tab->states, cap,
                                        sizeof(*tab->states));
        tab->cap = cap;
    }
    if ((u64)(tab->len + 1U) * 10U >= (u64)tab->slots_cap * 7U) {
        u32 cap = tab->slots_cap * 2U;
        if (cap < tab->slots_cap)
            YEW_BUG("syntax: state hash capacity overflow");
        state_rehash(tab, cap);
        at = (u32)state_hash(state) & (tab->slots_cap - 1U);
        while (tab->slots[at] != 0U)
            at = (at + 1U) & (tab->slots_cap - 1U);
    }
    tab->states[tab->len] = *state;
    tab->slots[at] = tab->len;
    return tab->len++;
}

const SynState *yew_syn_state_get(const SynStateTab *tab, u32 id)
{
    if (tab == NULL || id == YEW_SYN_STATE_UNKNOWN || id >= tab->len)
        return NULL;
    return &tab->states[id];
}

u32 yew_syn_state_count(const SynStateTab *tab)
{
    /* Includes the UNKNOWN and ROOT reservations, matching the id domain. */
    return tab == NULL ? 0U : tab->len;
}

bool yew_syn_state_exhausted(const SynStateTab *tab)
{
    return tab != NULL && tab->exhausted;
}

void yew_syn_state_push(SynState *state, u16 ctx)
{
    state_validate(state);
    if (state->depth < YEW_SYN_DEPTH_MAX) {
        state->ctx[state->depth++] = ctx;
        return;
    }
    if (state->lost < YEW_SYN_LOST_MAX)
        state->lost++;
    if (state->lost == YEW_SYN_LOST_MAX)
        state->flags |= YEW_SYN_F_DEGRADED;
}

void yew_syn_state_pop(SynState *state, u8 count)
{
    state_validate(state);
    while (count-- != 0U) {
        if (state->lost != 0U) {
            state->lost--;
        } else if (state->depth > 1U) {
            state->ctx[--state->depth] = 0U;
        }
    }
}

void yew_syn_state_set(SynState *state, u16 ctx)
{
    state_validate(state);
    state->ctx[state->depth - 1U] = ctx;
}

SynEngine *yew_syn_engine_new(SynDef *def)
{
    SynEngine *engine = yew_xcalloc(1U, sizeof(*engine));
    u16 root = def == NULL ? 0U : def->root;

    engine->def = def;
    engine->states = yew_syn_state_tab_new(root);
    engine->generation = 1U;
    engine_index_aux(engine);
    return engine;
}

void yew_syn_engine_free(SynEngine *engine)
{
    if (engine == NULL)
        return;
    yew_syn_state_tab_free(engine->states);
    free(engine->ctx_aux);
    free(engine);
}

void yew_syn_engine_set_def(SynEngine *engine, SynDef *def)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    yew_syn_state_tab_free(engine->states);
    engine->def = def;
    engine->states = yew_syn_state_tab_new(def == NULL ? 0U : def->root);
    engine_index_aux(engine);
    engine->line_calls = 0U;
    engine->generation++;
    if (engine->generation == 0U)
        engine->generation = 1U;
}

SynStateTab *yew_syn_engine_states(SynEngine *engine)
{
    return engine == NULL ? NULL : engine->states;
}

const SynDef *yew_syn_engine_def(const SynEngine *engine)
{
    return engine == NULL ? NULL : engine->def;
}

u64 yew_syn_engine_line_calls(const SynEngine *engine)
{
    return engine == NULL ? 0U : engine->line_calls;
}

void yew_syn_engine_reset_counters(SynEngine *engine)
{
    if (engine != NULL)
        engine->line_calls = 0U;
}

static bool bitset_has(const u8 bits[32], u8 byte)
{
    return (bits[byte >> 3U] & (u8)(1U << (byte & 7U))) != 0U;
}

static void emit_span(SynLineOut *out, u32 start, u32 len, u8 attr,
                      u8 flags)
{
    if (out->spans == NULL)
        return;
    while (len != 0U) {
        u16 take = len > UINT16_MAX ? UINT16_MAX : (u16)len;
        if (out->n != 0U) {
            SynSpan *last = &out->spans[out->n - 1U];
            if (last->attr == attr && last->flags == flags &&
                last->start + last->len == start &&
                (u32)last->len + take <= UINT16_MAX) {
                last->len = (u16)(last->len + take);
                start += take;
                len -= take;
                continue;
            }
        }
        if (out->n >= out->cap || out->n >= YEW_SYN_MAX_SPANS) {
            if (out->n != 0U)
                out->spans[out->n - 1U].flags |= YEW_SPAN_TRUNCATED;
            if (out->stop == YEW_SYN_STOP_OK)
                out->stop = YEW_SYN_STOP_SPANS;
            return;
        }
        out->spans[out->n++] = (SynSpan){start, take, attr, flags};
        start += take;
        len -= take;
    }
}

static u32 next_boundary(const u8 *line, u32 len, u32 at)
{
    u32 next = at < len ? at + 1U : at;
    while (next < len && (line[next] & 0xC0U) == 0x80U)
        next++;
    return next;
}

static bool bytes_equal(const u8 *a, size_t an, const char *b, size_t bn)
{
    return an == bn && (an == 0U || memcmp(a, b, an) == 0);
}

static bool aux_match(const SynEngine *engine, const SynState *state,
                      const SynRule *rule, const u8 *line, u32 len, u32 at,
                      YewReMatch *match)
{
    static const u8 empty[] = "";
    const char *aux;
    size_t aux_len;
    u32 lo = 0U;

    if (rule->aux_match == SYN_AUXM_INDENT_LT) {
        u64 p = 0U;
        u64 indent = 0U;
        if (at != 0U)
            return false;
        while (p < len) {
            if (line[p] == ' ') {
                indent++;
            } else if (line[p] == '\t') {
                indent = (indent + 8U) & ~UINT64_C(7);
            } else {
                break;
            }
            p++;
        }
        (void)memset(match, 0, sizeof(*match));
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return indent < state->aux;
    }
    if (engine->def->aux == NULL || state->aux == 0U)
        return false;
    aux = yew_intern_str(engine->def->aux, state->aux);
    aux_len = yew_intern_len(engine->def->aux, state->aux);
    if (aux == NULL)
        return false;
    (void)memset(match, 0, sizeof(*match));
    match->ngroups = 1U;
    switch ((SynAuxMatch)rule->aux_match) {
    case SYN_AUXM_LINE_EQ: {
        const u8 *text;
        if ((state->flags & YEW_SYN_F_STRIP) != 0U) {
            while (lo < len && line[lo] == '\t')
                lo++;
        }
        text = lo == 0U ? (line == NULL ? empty : line) : line + lo;
        if (at != 0U || !bytes_equal(text, len - lo, aux, aux_len))
            return false;
        match->g[0] = (Span){0U, len};
        return true;
    }
    case SYN_AUXM_LITERAL: {
        const char *pre = yew_intern_str(engine->def->aux, rule->aux_pre);
        const char *post = yew_intern_str(engine->def->aux, rule->aux_post);
        size_t pre_len = yew_intern_len(engine->def->aux, rule->aux_pre);
        size_t post_len = yew_intern_len(engine->def->aux, rule->aux_post);
        u64 total = (u64)pre_len + aux_len + post_len;
        if (total > len - at ||
            (pre_len != 0U && memcmp(line + at, pre, pre_len) != 0) ||
            (aux_len != 0U &&
             memcmp(line + at + pre_len, aux, aux_len) != 0) ||
            (post_len != 0U &&
             memcmp(line + at + pre_len + aux_len, post, post_len) != 0))
            return false;
        match->g[0] = (Span){at, at + total};
        return true;
    }
    case SYN_AUXM_FENCE_CLOSE: {
        u32 p = 0U;
        u32 markers = 0U;
        while (p < len && p < 4U && line[p] == ' ')
            p++;
        if (p > 3U || at != 0U || aux_len == 0U)
            return false;
        while (p + markers < len && line[p + markers] == (u8)aux[0])
            markers++;
        if (markers < aux_len)
            return false;
        while (p + markers < len &&
               (line[p + markers] == ' ' || line[p + markers] == '\t'))
            markers++;
        if (p + markers != len)
            return false;
        match->g[0] = (Span){0U, len};
        return true;
    }
    case SYN_AUXM_INDENT_LT:
        return false;
    case SYN_AUXM_NONE:
        break;
    }
    return false;
}

static bool rule_match(SynEngine *engine, const SynState *state,
                       const SynRule *rule, const u8 *line, u32 len, u32 at,
                       YewReMatch *match)
{
    if (rule->aux_match != SYN_AUXM_NONE)
        return aux_match(engine, state, rule, line, len, at, match);
    if (rule->re == NULL)
        return false;
    return yew_re_match_at(rule->re, &(YewReInput){NULL, line, len,
                                                   {0U, len}},
                           BYTEOFF(at), match);
}

static void emit_match(SynLineOut *out, const SynRule *rule,
                       const YewReMatch *match, u32 consume_hi)
{
    u32 p = (u32)match->g[0].lo;
    u32 hi = consume_hi;

    while (p < hi) {
        u8 attr = rule->caps[0] == UINT8_MAX ? rule->attr : rule->caps[0];
        u32 end = hi;
        u32 g;

        for (g = 1U; g < 8U && g < match->ngroups; g++) {
            Span cap = match->g[g];
            if (rule->caps[g] == UINT8_MAX || cap.lo == UINT64_MAX ||
                cap.hi < cap.lo)
                continue;
            if (cap.lo <= p && p < cap.hi) {
                attr = rule->caps[g];
                if (cap.hi < end)
                    end = (u32)cap.hi;
            } else if (p < cap.lo && cap.lo < end) {
                end = (u32)cap.lo;
            }
        }
        if (end <= p)
            end = p + 1U;
        emit_span(out, p, end - p, attr, 0U);
        p = end;
    }
}

static bool set_aux(SynEngine *engine, SynState *state, const SynRule *rule,
                    const u8 *line, u32 len, const YewReMatch *match)
{
    Span cap;
    u64 lo;
    u64 hi;

    if ((rule->flags & YEW_SYN_RULE_SET_AUX) == 0U)
        return true;
    if (((rule->flags & YEW_SYN_RULE_AUX_INT) == 0U &&
         engine->def->aux == NULL) || rule->aux_group >= match->ngroups)
        YEW_BUG("syntax: SET_AUX rule has no usable capture/interner");
    cap = match->g[rule->aux_group];
    if (cap.lo == UINT64_MAX || cap.hi == UINT64_MAX)
        return false;
    lo = cap.lo;
    hi = cap.hi;
    if (hi < lo || hi > len || hi - lo > SIZE_MAX)
        YEW_BUG("syntax: invalid aux capture");
    if ((rule->flags & YEW_SYN_RULE_STRIP) != 0U) {
        while (lo < hi && (line[lo] == ' ' || line[lo] == '\t'))
            lo++;
        while (hi > lo && (line[hi - 1U] == ' ' || line[hi - 1U] == '\t'))
            hi--;
    }
    if ((rule->flags & YEW_SYN_RULE_AUX_INT) != 0U) {
        u32 value = 0U;
        u64 p;
        bool decimal = lo < hi;
        for (p = lo; p < hi; p++) {
            if (line[p] < '0' || line[p] > '9') {
                decimal = false;
                break;
            }
            if (value <= (UINT32_MAX - 9U) / 10U)
                value = value * 10U + (u32)(line[p] - '0');
        }
        if (!decimal) {
            value = 0U;
            for (p = lo; p < hi; p++) {
                if (line[p] == ' ')
                    value++;
                else if (line[p] == '\t')
                    value = (value + 8U) & ~7U;
            }
        }
        state->aux = value;
    } else {
        static const char empty[] = "";
        const char *bytes = hi == lo && line == NULL
                                ? empty
                                : (const char *)line + lo;
        state->aux = yew_intern(engine->def->aux, bytes, (size_t)(hi - lo));
    }
    return true;
}

static void apply_op(SynState *state, u8 op, u8 nop, u16 target)
{
    switch ((SynOp)op) {
    case SYN_OP_STAY:
        break;
    case SYN_OP_PUSH:
        yew_syn_state_push(state, target);
        break;
    case SYN_OP_POP:
        yew_syn_state_pop(state, nop == 0U ? 1U : nop);
        break;
    case SYN_OP_SET:
        yew_syn_state_set(state, target);
        break;
    default:
        YEW_BUG("syntax: invalid rule operation %u", (unsigned)op);
    }
}

static void apply_rule_op(SynState *state, const SynRule *rule)
{
    u8 depth = state->depth;
    if (rule->op == SYN_OP_PUSH && rule->npush != 0U) {
        u8 i;
        if (rule->npush > 4U)
            YEW_BUG("syntax: rule push list exceeds four contexts");
        for (i = 0U; i < rule->npush; i++)
            yew_syn_state_push(state, rule->push[i]);
    } else {
        apply_op(state, rule->op, rule->nop, rule->target);
    }
    /* A refused over-depth push is repaid through lost before the real
     * frame pops.  Its delimiter belongs to the still-live outer frame,
     * so clearing aux during that repayment would corrupt the matcher. */
    if ((rule->flags & YEW_SYN_RULE_CLR_AUX) != 0U &&
        state->depth < depth) {
        state->aux = 0U;
        state->flags &= (u8)~YEW_SYN_F_STRIP;
    }
}

static void apply_empty_bol(SynEngine *engine, SynState *state,
                            const u8 *line)
{
    u32 guard = 0U;
    while (guard++ <= YEW_SYN_DEPTH_MAX) {
        u16 ctx_id = state->ctx[state->depth - 1U];
        const SynCtx *ctx = &engine->def->ctxs[ctx_id];
        const SynRule *matched = NULL;
        YewReMatch match;
        u32 i;
        if (engine->ctx_aux == NULL || engine->ctx_aux[ctx_id] == 0U)
            return;
        for (i = 0U; i < ctx->nrules; i++) {
            u32 index = ctx->first_rule + i;
            const SynRule *rule;
            if (index >= engine->def->nrules)
                return;
            rule = &engine->def->rules[index];
            if (rule_match(engine, state, rule, line, 0U, 0U, &match) &&
                match.g[0].hi == 0U) {
                matched = rule;
                break;
            }
        }
        if (matched == NULL)
            return;
        (void)set_aux(engine, state, matched, line, 0U, &match);
        apply_rule_op(state, matched);
        if ((matched->flags & YEW_SYN_RULE_ZERO_POP) == 0U ||
            matched->op != SYN_OP_POP)
            return;
    }
}

static void syn_line_run(SynEngine *engine, u32 entry_state,
                         const u8 *line, u32 len, SynLineOut *out,
                         bool apply_eol, SynState *trace)
{
    SynState state;
    const SynState *entry;
    u64 steps = 0U;
    u64 step_cap;
    u32 p = 0U;
    u32 zero_pops = 0U;

    if (engine == NULL || out == NULL || (line == NULL && len != 0U))
        YEW_BUG("syntax: invalid line arguments");
    engine->line_calls++;
    out->n = 0U;
    out->stop = YEW_SYN_STOP_OK;
    entry = yew_syn_state_get(engine->states, entry_state);
    if (entry == NULL) {
        entry_state = YEW_SYN_STATE_ROOT;
        entry = yew_syn_state_get(engine->states, entry_state);
    }
    state = *entry;
    if (trace != NULL)
        trace[0] = state;
    if (len > YEW_SYN_LINE_BYTE_CAP) {
        emit_span(out, 0U, len, YEW_ATTR_TEXT, YEW_SPAN_TRUNCATED);
        out->exit_state = entry_state;
        out->stop = YEW_SYN_STOP_BYTES;
        return;
    }
    if (engine->def == NULL || engine->def->ctxs == NULL ||
        state.ctx[state.depth - 1U] >= engine->def->nctxs) {
        if (trace != NULL) {
            u32 t;
            for (t = 1U; t <= len; t++)
                trace[t] = state;
        }
        emit_span(out, 0U, len, YEW_ATTR_TEXT, 0U);
        out->exit_state = entry_state;
        return;
    }
    step_cap = YEW_SYN_LINE_STEPS(len);
    if (len == 0U) {
        apply_empty_bol(engine, &state, line);
        if (trace != NULL)
            trace[0] = state;
    }
    while (p < len) {
        const SynCtx *ctx = &engine->def->ctxs[state.ctx[state.depth - 1U]];
        const SynRule *matched = NULL;
        SynState before;
        YewReMatch match;
        u32 ri;

        if (!bitset_has(ctx->first, line[p]) &&
            (engine->ctx_aux == NULL ||
             engine->ctx_aux[state.ctx[state.depth - 1U]] == 0U)) {
            u32 q = p + 1U;
            while (q < len && !bitset_has(ctx->first, line[q]))
                q++;
            emit_span(out, p, q - p, ctx->dflt_attr, 0U);
            if (trace != NULL) {
                u32 t;
                for (t = p + 1U; t <= q; t++)
                    trace[t] = state;
            }
            p = q;
            continue;
        }
        for (ri = 0U; ri < ctx->nrules; ri++) {
            u32 index = ctx->first_rule + ri;
            const SynRule *rule;
            if (++steps > step_cap || index >= engine->def->nrules) {
                emit_span(out, p, len - p, YEW_ATTR_TEXT,
                          YEW_SPAN_TRUNCATED);
                out->exit_state = entry_state;
                out->stop = YEW_SYN_STOP_STEPS;
                return;
            }
            rule = &engine->def->rules[index];
            if (!bitset_has(rule->first, line[p]) &&
                rule->aux_match == SYN_AUXM_NONE)
                continue;
            if (rule_match(engine, &state, rule, line, len, p, &match)) {
                matched = rule;
                break;
            }
        }
        if (matched == NULL) {
            u32 q = next_boundary(line, len, p);
            emit_span(out, p, q - p, ctx->dflt_attr, 0U);
            if (trace != NULL) {
                u32 t;
                for (t = p + 1U; t <= q; t++)
                    trace[t] = state;
            }
            p = q;
            continue;
        }
        before = state;
        {
            u32 end = (u32)match.g[0].hi;
            if (matched->consume != 0U &&
                matched->consume < match.ngroups &&
                match.g[matched->consume].hi != UINT64_MAX)
                end = (u32)match.g[matched->consume].hi;
            if (end < p || end > match.g[0].hi)
                YEW_BUG("syntax: consume group ends outside whole match");
            emit_match(out, matched, &match, end);
        }
        {
            bool aux_set = set_aux(engine, &state, matched, line, len,
                                   &match);
            if ((matched->flags & YEW_SYN_RULE_STRIP) != 0U && aux_set)
                state.flags |= YEW_SYN_F_STRIP;
        }
        if ((matched->flags & YEW_SYN_RULE_SET_VALUE) != 0U)
            state.flags |= YEW_SYN_F_VALUE;
        if ((matched->flags & YEW_SYN_RULE_CLR_VALUE) != 0U)
            state.flags &= (u8)~YEW_SYN_F_VALUE;
        apply_rule_op(&state, matched);
        {
            u32 end = (u32)match.g[0].hi;
            if (matched->consume != 0U &&
                matched->consume < match.ngroups &&
                match.g[matched->consume].hi != UINT64_MAX)
                end = (u32)match.g[matched->consume].hi;
            if (end > p) {
                if (trace != NULL) {
                    u32 t;
                    for (t = p + 1U; t < end; t++)
                        trace[t] = before;
                    trace[end] = state;
                }
                p = end;
            } else if ((matched->flags & YEW_SYN_RULE_ZERO_POP) != 0U &&
                       matched->op == SYN_OP_POP && p == 0U &&
                       zero_pops++ < YEW_SYN_DEPTH_MAX) {
                if (trace != NULL)
                    trace[p] = state;
                continue;
            } else {
                const SynCtx *after =
                    &engine->def->ctxs[state.ctx[state.depth - 1U]];
                u32 q = next_boundary(line, len, p);
                emit_span(out, p, q - p, after->dflt_attr, 0U);
                if (trace != NULL) {
                    u32 t;
                    for (t = p + 1U; t <= q; t++)
                        trace[t] = state;
                }
                p = q;
            }
        }
    }
    if (apply_eol) {
        const SynCtx *ctx = &engine->def->ctxs[state.ctx[state.depth - 1U]];
        apply_op(&state, ctx->at_eol, ctx->eol_nop, ctx->eol_target);
    }
    out->exit_state = yew_syn_state_intern(engine->states, &state);
}

void yew_syn_line(SynEngine *engine, u32 entry_state, const u8 *line,
                  u32 len, SynLineOut *out)
{
    syn_line_run(engine, entry_state, line, len, out, true, NULL);
}

bool yew_syn_stack_trace(SynEngine *engine, u32 entry_state, const u8 *line,
                         u32 len, SynState *trace, size_t trace_cap)
{
    SynLineOut line_out = {NULL, 0U, 0U, YEW_SYN_STATE_UNKNOWN,
                           YEW_SYN_STOP_OK};

    if (engine == NULL || trace == NULL || len > YEW_SYN_LINE_BYTE_CAP ||
        (line == NULL && len != 0U) || trace_cap < (size_t)len + 1U)
        return false;
    syn_line_run(engine, entry_state, line, len, &line_out, false, trace);
    return line_out.stop == YEW_SYN_STOP_OK;
}

bool yew_syn_stack_at(SynEngine *engine, u32 entry_state, const u8 *line,
                      u32 len, u32 p, SynState *out)
{
    SynState *trace;
    bool ok;

    if (engine == NULL || out == NULL || len > YEW_SYN_LINE_BYTE_CAP ||
        (line == NULL && len != 0U))
        return false;
    if (p > len)
        p = len;
    trace = malloc(((size_t)len + 1U) * sizeof(*trace));
    if (trace == NULL)
        return false;
    ok = yew_syn_stack_trace(engine, entry_state, line, len, trace,
                             (size_t)len + 1U);
    if (ok)
        *out = trace[p];
    free(trace);
    return ok;
}

static i64 real_now_us(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000;
}

static i64 syn_now(const SynBuf *syn)
{
    return syn->clock == NULL ? real_now_us(NULL) : syn->clock(syn->clock_ctx);
}

static void vec_reserve(SynU32Vec *vec, size_t need)
{
    size_t cap;
    if (vec->cap >= need)
        return;
    cap = vec->cap == 0U ? 8U : vec->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    vec->data = yew_xreallocarray(vec->data, cap, sizeof(*vec->data));
    vec->cap = cap;
}

static void vec_splice(SynU32Vec *vec, size_t at, size_t removed,
                       size_t inserted)
{
    size_t tail;
    size_t new_len;
    if (at > vec->len)
        at = vec->len;
    if (removed > vec->len - at)
        removed = vec->len - at;
    if (inserted > SIZE_MAX - (vec->len - removed))
        YEW_BUG("syntax: line-state vector overflow");
    new_len = vec->len - removed + inserted;
    vec_reserve(vec, new_len);
    tail = vec->len - at - removed;
    (void)memmove(vec->data + at + inserted, vec->data + at + removed,
                  tail * sizeof(*vec->data));
    if (inserted != 0U)
        (void)memset(vec->data + at, 0, inserted * sizeof(*vec->data));
    vec->len = new_len;
}

void yew_syn_buf_init(SynBuf *syn)
{
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    (void)memset(syn, 0, sizeof(*syn));
    syn->clock = real_now_us;
}

static void cache_prepare(SynBuf *syn)
{
    SynCache *cache;
    if (syn->private_cache != NULL)
        return;
    cache = yew_xcalloc(1U, sizeof(*cache));
    cache->slab_cap = YEW_SYN_SPAN_CACHE * YEW_SYN_MAX_SPANS;
    cache->slab = yew_xcalloc(cache->slab_cap, sizeof(*cache->slab));
    cache->line_cap = YEW_SYN_LINE_BYTE_CAP + 1U;
    cache->line = yew_xmalloc(cache->line_cap);
    syn->private_cache = cache;
}

static void cache_invalidate(SynBuf *syn)
{
    SynCache *cache = syn->private_cache;
    u32 i;
    if (cache == NULL)
        return;
    cache->hand = 0U;
    cache->slab_hand = 0U;
    for (i = 0U; i < YEW_SYN_SPAN_CACHE; i++)
        cache->slots[i].valid = false;
}

static u32 cache_span_alloc(SynCache *cache, u32 n)
{
    u32 off = cache->slab_hand;
    u32 i;

    if (n > cache->slab_cap)
        YEW_BUG("syntax: span slab request exceeds capacity");
    if (off + n > cache->slab_cap)
        off = 0U;
    for (i = 0U; i < YEW_SYN_SPAN_CACHE; i++) {
        SynCacheEnt *ent = &cache->slots[i];
        if (ent->valid && ent->n != 0U && n != 0U &&
            off < ent->span_off + ent->n && ent->span_off < off + n)
            ent->valid = false;
    }
    cache->slab_hand = off + n;
    if (cache->slab_hand == cache->slab_cap)
        cache->slab_hand = 0U;
    return off;
}

void yew_syn_attach(SynBuf *syn, u32 lang, const TextBuf *tb)
{
    size_t n;
    if (syn == NULL || tb == NULL)
        YEW_BUG("syntax: invalid attach arguments");
    free(syn->entry.data);
    syn->entry = (SynU32Vec){0};
    n = (size_t)yew_textbuf_line_count(tb);
    vec_reserve(&syn->entry, n);
    syn->entry.len = n;
    if (n != 0U)
        (void)memset(syn->entry.data, 0, n * sizeof(*syn->entry.data));
    if (n != 0U)
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    syn->lang = lang;
    syn->settled_to = LINENO(n == 0U ? 0U : 1U);
    syn->wave = LINENO(0U);
    syn->buf_gen = tb->gen;
    syn->settling = n != 0U && lang != YEW_LANG_NONE && syn->engine != NULL;
    if (!syn->settling) {
        syn->settled_to = LINENO(n);
        syn->wave = LINENO(n);
    }
    syn->degraded = false;
    syn->spec_valid = false;
    syn->spec_from = LINENO(0U);
    syn->must_reach = LINENO(syn->settling ? n : 0U);
    syn->engine_gen = syn->engine == NULL ? 0U : syn->engine->generation;
    if (syn->engine != NULL)
        cache_prepare(syn);
    cache_invalidate(syn);
    syn->edit_us = syn_now(syn);
    syn->splice_count = 0U;
    syn->provisional_corrections = 0U;
}

void yew_syn_detach(SynBuf *syn)
{
    SynCache *cache;
    if (syn == NULL)
        return;
    cache = syn->private_cache;
    if (cache != NULL) {
        free(cache->line);
        free(cache->slab);
        free(cache);
    }
    free(syn->entry.data);
    yew_syn_buf_init(syn);
}

void yew_syn_buf_bind(SynBuf *syn, SynEngine *engine)
{
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    syn->engine = engine;
    if (engine != NULL && syn->lang != YEW_LANG_NONE && syn->entry.len != 0U) {
        cache_prepare(syn);
        cache_invalidate(syn);
        syn->engine_gen = engine->generation;
        (void)memset(syn->entry.data, 0,
                     syn->entry.len * sizeof(*syn->entry.data));
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
        syn->wave = LINENO(0U);
        syn->settled_to = LINENO(1U);
        syn->must_reach = LINENO(syn->entry.len);
        syn->settling = true;
    } else {
        syn->engine_gen = engine == NULL ? 0U : engine->generation;
    }
}

void yew_syn_buf_set_clock(SynBuf *syn, SynClockFn clock, void *ctx)
{
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    syn->clock = clock == NULL ? real_now_us : clock;
    syn->clock_ctx = ctx;
}

void yew_syn_edit(SynBuf *syn, LineNo lo, u64 removed, u64 inserted)
{
    size_t at;
    u64 frontier;
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    if (lo.v >= syn->entry.len || lo.v >= SIZE_MAX ||
        removed > SIZE_MAX || inserted > SIZE_MAX)
        YEW_BUG("syntax: edit line count exceeds address space");
    at = (size_t)lo.v + 1U;
    if (removed > syn->entry.len - at ||
        inserted > SIZE_MAX - (syn->entry.len - (size_t)removed))
        YEW_BUG("syntax: edit splice is outside the line-state array");
    frontier = syn->must_reach.v;
    if (syn->settling && syn->wave.v < UINT64_MAX &&
        frontier < syn->wave.v + 1U)
        frontier = syn->wave.v + 1U;
    if (frontier > at) {
        if (frontier <= at + removed)
            frontier = at + inserted;
        else
            frontier = frontier - removed + inserted;
    }
    vec_splice(&syn->entry, at, (size_t)removed, (size_t)inserted);
    if (syn->entry.len != 0U)
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    if (syn->settled_to.v > lo.v + 1U)
        syn->settled_to = LINENO(lo.v + 1U);
    if (syn->wave.v > lo.v)
        syn->wave = lo;
    syn->settling = true;
    syn->spec_valid = false;
    if (frontier < at + inserted)
        frontier = at + inserted;
    if (frontier > syn->entry.len)
        frontier = syn->entry.len;
    syn->must_reach = LINENO(frontier);
    syn->edit_us = syn_now(syn);
    syn->edit_spliced = true;
    syn->splice_count++;
    if (syn->lang == YEW_LANG_NONE || syn->engine == NULL) {
        syn->settling = false;
        syn->settled_to = LINENO(syn->entry.len);
        syn->wave = LINENO(syn->entry.len);
    }
}

static SynCache *cache_get(SynBuf *syn)
{
    if (syn->private_cache == NULL)
        YEW_BUG("syntax: span cache was not prepared before drawing");
    return syn->private_cache;
}

static u32 line_content_len(const TextBuf *tb, LineNo line);

static u8 *line_copy(SynBuf *syn, const TextBuf *tb, LineNo line, u32 *len)
{
    SynCache *cache = cache_get(syn);
    Span span = yew_textbuf_line_span(tb, line);
    u64 need = line_content_len(tb, line);
    TextIter it;
    u64 copied = 0U;

    if (need > YEW_SYN_LINE_BYTE_CAP + 1U)
        need = YEW_SYN_LINE_BYTE_CAP + 1U;
    if (need != 0U && yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
        while (copied < need) {
            const u8 *bytes;
            u64 n;
            u64 take;
            if (!yew_textiter_chunk(&it, tb, &bytes, &n))
                YEW_BUG("syntax: failed to read line bytes");
            take = n < need - copied ? n : need - copied;
            (void)memcpy(cache->line + copied, bytes, (size_t)take);
            copied += take;
            if (copied < need && !yew_textiter_advance(&it, tb))
                YEW_BUG("syntax: truncated line iterator");
        }
    }
    if (copied != need)
        YEW_BUG("syntax: incomplete line copy");
    *len = (u32)need;
    return cache->line;
}

static bool textbuf_byte(const TextBuf *tb, u64 off, u8 *byte)
{
    TextIter it;
    const u8 *bytes;
    u64 n;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&it, tb, &bytes, &n) || n == 0U)
        return false;
    *byte = bytes[0];
    return true;
}

static u32 line_content_len(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    u64 len = span.hi - span.lo;
    u8 byte;
    if (len != 0U && textbuf_byte(tb, span.hi - 1U, &byte) && byte == '\n')
        len--;
    if (len != 0U && textbuf_byte(tb, span.lo + len - 1U, &byte) &&
        byte == '\r')
        len--;
    return len > UINT32_MAX ? UINT32_MAX : (u32)len;
}

static void reconcile_generation(SynBuf *syn, const TextBuf *tb)
{
    size_t n = (size_t)yew_textbuf_line_count(tb);
    if (syn->buf_gen == tb->gen && syn->entry.len == n)
        return;
    if (syn->edit_spliced && syn->entry.len == n) {
        syn->buf_gen = tb->gen;
        syn->edit_spliced = false;
        return;
    }
    if (syn->entry.len != n) {
        vec_reserve(&syn->entry, n);
        syn->entry.len = n;
        if (n != 0U)
            (void)memset(syn->entry.data, 0, n * sizeof(*syn->entry.data));
        if (n != 0U)
            syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    }
    syn->wave = LINENO(0U);
    syn->settled_to = LINENO(n == 0U ? 0U : 1U);
    syn->settling = n != 0U;
    syn->buf_gen = tb->gen;
    syn->edit_spliced = false;
    syn->must_reach = LINENO(n);
    cache_invalidate(syn);
}

static void reconcile_engine(SynBuf *syn)
{
    if (syn->engine == NULL || syn->engine_gen == syn->engine->generation)
        return;
    if (syn->entry.len != 0U) {
        (void)memset(syn->entry.data, 0,
                     syn->entry.len * sizeof(*syn->entry.data));
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    }
    syn->wave = LINENO(0U);
    syn->settled_to = LINENO(syn->entry.len == 0U ? 0U : 1U);
    syn->must_reach = LINENO(syn->entry.len);
    syn->settling = syn->lang != YEW_LANG_NONE && syn->entry.len != 0U;
    syn->spec_valid = false;
    syn->engine_gen = syn->engine->generation;
    cache_invalidate(syn);
}

void yew_syn_settle(SynBuf *syn, const TextBuf *tb, LineNo view_lo,
                    LineNo view_hi, i64 budget_us, SynSettleReport *report)
{
    SynSettleReport local;
    i64 started;
    i64 elapsed = 0;
    u64 nlines;
    u64 i;
    SynSpan spans[YEW_SYN_MAX_SPANS];

    if (syn == NULL || tb == NULL)
        YEW_BUG("syntax: invalid settle arguments");
    if (report == NULL)
        report = &local;
    (void)memset(report, 0, sizeof(*report));
    report->damage_lo = LINENO(UINT64_MAX);
    started = syn_now(syn);
    reconcile_engine(syn);
    reconcile_generation(syn, tb);
    nlines = syn->entry.len;
    if (syn->engine == NULL || syn->lang == YEW_LANG_NONE || nlines == 0U) {
        syn->settling = false;
        syn->wave = LINENO(nlines);
        syn->settled_to = LINENO(nlines);
        report->fixpoint = true;
        report->damage_lo = LINENO(0U);
        report->damage_hi = LINENO(0U);
        return;
    }
    i = syn->wave.v;
    if (i >= nlines)
        i = nlines - 1U;
    for (; i < nlines; i++) {
        SynLineOut out = {spans, 0U, YEW_SYN_MAX_SPANS, 0U,
                          YEW_SYN_STOP_OK};
        u32 len;
        const u8 *bytes;
        u32 held;
        u32 next;
        u32 entry = syn->entry.data[i];
        if (entry == YEW_SYN_STATE_UNKNOWN)
            entry = YEW_SYN_STATE_ROOT;
        bytes = line_copy(syn, tb, LINENO(i), &len);
        yew_syn_line(syn->engine, entry, bytes, len, &out);
        report->lines++;
        {
            const SynState *exit =
                yew_syn_state_get(yew_syn_engine_states(syn->engine),
                                  out.exit_state);
            if (out.stop != YEW_SYN_STOP_OK ||
                yew_syn_state_exhausted(
                    yew_syn_engine_states(syn->engine)) ||
                (exit != NULL &&
                 (exit->flags & YEW_SYN_F_DEGRADED) != 0U)) {
                if (!syn->degraded)
                    yew_log(YEW_LOG_WARN,
                            "syntax highlighting degraded for buffer");
                syn->degraded = true;
            }
        }
        if (report->damage_lo.v == UINT64_MAX)
            report->damage_lo = LINENO(i);
        report->damage_hi = LINENO(i + 1U);
        if (i >= view_lo.v && i < view_hi.v)
            report->hit_view = true;
        next = out.exit_state;
        if (i + 1U >= nlines) {
            syn->wave = LINENO(nlines);
            syn->settled_to = LINENO(nlines);
            syn->must_reach = LINENO(0U);
            syn->settling = false;
            syn->spec_valid = false;
            report->fixpoint = true;
            break;
        }
        held = syn->entry.data[i + 1U];
        if (held != YEW_SYN_STATE_UNKNOWN && held == next &&
            i + 1U >= syn->must_reach.v) {
            syn->wave = LINENO(nlines);
            syn->settled_to = LINENO(nlines);
            syn->must_reach = LINENO(0U);
            syn->settling = false;
            if (syn->spec_valid && i + 1U >= syn->spec_from.v) {
                syn->provisional_corrections++;
                report->provisional = true;
            }
            syn->spec_valid = false;
            report->fixpoint = true;
            break;
        }
        syn->entry.data[i + 1U] = next;
        if (syn->spec_valid && i + 1U >= syn->spec_from.v) {
            syn->provisional_corrections++;
            report->provisional = true;
            syn->spec_valid = false;
        }
        syn->settled_to = LINENO(i + 2U);
        syn->wave = LINENO(i + 1U);
        if (i + 1U >= view_lo.v && i + 1U < view_hi.v) {
            report->hit_view = true;
            report->damage_hi = LINENO(i + 2U);
        }
        if ((i & (YEW_SYN_CLOCK_EVERY - 1U)) == 0U && budget_us > 0) {
            elapsed = syn_now(syn) - started;
        }
        if (elapsed >= budget_us && budget_us > 0) {
            syn->settling = true;
            if (syn->wave.v < view_lo.v) {
                syn->spec_from = view_lo;
                syn->spec_valid = true;
                report->provisional = true;
            }
            break;
        }
    }
    if (report->fixpoint) {
        i64 ended = syn_now(syn);
        elapsed = ended >= started ? ended - started : 0;
    }
    report->us = (u64)(elapsed > 0 ? elapsed : 0);
    if (report->damage_lo.v == UINT64_MAX) {
        report->damage_lo = LINENO(0U);
        report->damage_hi = LINENO(0U);
    }
    syn->buf_gen = tb->gen;
}

static SynCacheEnt *cache_find(SynCache *cache, u64 line, u64 gen, u32 state)
{
    u32 i;
    for (i = 0U; i < YEW_SYN_SPAN_CACHE; i++) {
        SynCacheEnt *ent = &cache->slots[i];
        if (ent->valid && ent->line == line && ent->gen == gen &&
            ent->entry_state == state)
            return ent;
    }
    return NULL;
}

static void note_span_stop(SynBuf *syn, u8 stop)
{
    if (stop == YEW_SYN_STOP_OK)
        return;
    if (!syn->degraded)
        yew_log(YEW_LOG_WARN, "syntax highlighting degraded for buffer");
    syn->degraded = true;
}

void yew_syn_spans(SynBuf *syn, const TextBuf *tb, LineNo line,
                   SynLineOut *out)
{
    SynCache *cache;
    SynCacheEnt *ent;
    u32 state;
    u32 copy;
    u32 len;
    const u8 *bytes;

    if (syn == NULL || tb == NULL || out == NULL || line.v >= syn->entry.len)
        YEW_BUG("syntax: invalid span request");
    reconcile_engine(syn);
    out->n = 0U;
    out->stop = YEW_SYN_STOP_OK;
    state = syn->entry.data[line.v];
    if (state == YEW_SYN_STATE_UNKNOWN)
        state = YEW_SYN_STATE_ROOT;
    if (syn->engine == NULL || syn->lang == YEW_LANG_NONE) {
        len = line_content_len(tb, line);
        emit_span(out, 0U, len, YEW_ATTR_TEXT, 0U);
        out->exit_state = state;
        return;
    }
    cache = cache_get(syn);
    ent = cache_find(cache, line.v, tb->gen, state);
    if (ent == NULL) {
        ent = &cache->slots[cache->hand++ % YEW_SYN_SPAN_CACHE];
        ent->valid = false;
        bytes = line_copy(syn, tb, line, &len);
        {
            SynLineOut evaluated = {cache->scratch, 0U, YEW_SYN_MAX_SPANS, 0U,
                                    YEW_SYN_STOP_OK};
            yew_syn_line(syn->engine, state, bytes, len, &evaluated);
            ent->span_off = cache_span_alloc(cache, evaluated.n);
            if (evaluated.n != 0U)
                (void)memcpy(cache->slab + ent->span_off, evaluated.spans,
                             evaluated.n * sizeof(*cache->slab));
            ent->n = evaluated.n;
            ent->exit_state = evaluated.exit_state;
            ent->stop = evaluated.stop;
        }
        ent->line = line.v;
        ent->gen = tb->gen;
        ent->entry_state = state;
        ent->valid = true;
    }
    copy = ent->n < out->cap ? ent->n : out->cap;
    if (copy != 0U)
        (void)memcpy(out->spans, cache->slab + ent->span_off,
                     copy * sizeof(*out->spans));
    out->n = copy;
    out->exit_state = ent->exit_state;
    out->stop = copy == ent->n ? ent->stop : YEW_SYN_STOP_SPANS;
    if (copy != ent->n && copy != 0U)
        out->spans[copy - 1U].flags |= YEW_SPAN_TRUNCATED;
    note_span_stop(syn, out->stop);
}

bool yew_syn_status_visible(const SynBuf *syn)
{
    if (syn == NULL)
        return false;
    if (syn->degraded)
        return true;
    return syn->settling && syn_now(syn) - syn->edit_us >=
                                (i64)YEW_SYN_SETTLING_MS * 1000;
}

void yew_syn_status(const SynBuf *syn, u64 line_count, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0U)
        return;
    if (syn == NULL) {
        (void)snprintf(dst, cap, "syntax unavailable");
        return;
    }
    (void)snprintf(dst, cap,
                   "settled %llu/%llu lines, wave at %llu, degraded=%s",
                   (unsigned long long)syn->settled_to.v,
                   (unsigned long long)line_count,
                   (unsigned long long)syn->wave.v,
                   syn->degraded ? "yes" : "no");
}

void yew_theme_load(const char *path)
{
    (void)path;
    YEW_BUG("theme loading lands in Sprint 41");
}

const ThemeEnt *yew_theme_table(void)
{
    static ThemeEnt table[64];
    static bool initialized;
    if (!initialized) {
        table[YEW_ATTR_COMMENT].attrs = YEW_ATTR_DIM;
        initialized = true;
    }
    return table;
}
