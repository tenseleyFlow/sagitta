#define _POSIX_C_SOURCE 200809L

#include "syn/engine.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "search/regex_internal.h"
#include "syn/theme.h"
#include "text/piece.h"
#include "unicode/utf8.h"
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

enum {
    SYN_CANDIDATE_BYTES = 256,
    SYN_CANDIDATE_STRIDE = SYN_CANDIDATE_BYTES + 1,
    SYN_CANDIDATE_RULE_BYTES_MAX = 8 * 1024 * 1024
};

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
    u8 *rule_bol;
    u8 *rule_wordb;
    u8 *rule_word_literal;
    u8 *rule_identifier_suffix;
    u8 (*rule_first)[32];
    u8 (*ctx_first_nonbol)[32];
    u32 *candidate_offsets;
    u32 *candidate_rules;
    YewReWorkspace re_workspace;
    bool has_first_line;
    bool identifier_fast_enabled;
    SynCoverage *coverage;
    u64 line_calls;
    u64 generation;
};

static bool ascii_identifier_start(u8 byte)
{
    return byte == (u8)'_' || (byte >= (u8)'A' && byte <= (u8)'Z') ||
           (byte >= (u8)'a' && byte <= (u8)'z');
}

static bool ascii_identifier_continue(u8 byte)
{
    return ascii_identifier_start(byte) ||
           (byte >= (u8)'0' && byte <= (u8)'9');
}

static bool ascii_space(u8 byte)
{
    return byte == (u8)' ' || byte == (u8)'\t' || byte == (u8)'\n' ||
           byte == (u8)'\v' || byte == (u8)'\f' || byte == (u8)'\r';
}

typedef bool (*AsciiPred)(u8 byte);

static bool bitset_has(const u8 bits[32], u8 byte);

static bool class_ascii_equals(const YewRe *re, u32 index, AsciiPred pred)
{
    u32 byte;

    if (index >= re->nclasses)
        return false;
    for (byte = 0U; byte < 128U; byte++) {
        if (yew_re_class_has(&re->classes[index], byte) != pred((u8)byte))
            return false;
    }
    return true;
}

static u8 regex_identifier_suffix(const YewRe *re)
{
    const ReInst *p;

    if (re == NULL || re->nprog != 13U || re->ngroups != 2U ||
        (re->flags & YEW_RE_ICASE) != 0U)
        return 0U;
    p = re->prog;
    if ((ReOp)p[0].op != RE_SAVE || p[0].arg != 0U ||
        (ReOp)p[1].op != RE_SAVE || p[1].arg != 2U ||
        (ReOp)p[2].op != RE_CLASS ||
        (ReOp)p[3].op != RE_SPLIT || p[3].x != 4U || p[3].y != 6U ||
        (ReOp)p[4].op != RE_CLASS ||
        (ReOp)p[5].op != RE_JMP || p[5].x != 3U ||
        (ReOp)p[6].op != RE_SAVE || p[6].arg != 3U ||
        (ReOp)p[7].op != RE_SPLIT || p[7].x != 8U || p[7].y != 10U ||
        (ReOp)p[8].op != RE_CLASS ||
        (ReOp)p[9].op != RE_JMP || p[9].x != 7U ||
        (ReOp)p[10].op != RE_CHAR ||
        (p[10].arg != (u32)'(' && p[10].arg != (u32)':') ||
        (ReOp)p[11].op != RE_SAVE || p[11].arg != 1U ||
        (ReOp)p[12].op != RE_MATCH ||
        !class_ascii_equals(re, p[2].arg, ascii_identifier_start) ||
        !class_ascii_equals(re, p[4].arg, ascii_identifier_continue) ||
        !class_ascii_equals(re, p[8].arg, ascii_space))
        return 0U;
    return (u8)p[10].arg;
}

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

static bool regex_requires_bol(const YewRe *re)
{
    u32 pc = 0U;
    u32 guard = 0U;

    if (re == NULL)
        return false;
    while (pc < re->nprog && guard++ <= re->nprog) {
        const ReInst *ins = &re->prog[pc];

        switch ((ReOp)ins->op) {
        case RE_BOL:
            return true;
        case RE_JMP:
            pc = ins->x;
            break;
        case RE_SAVE:
        case RE_BOT:
        case RE_EOT:
        case RE_WORDB:
        case RE_NWORDB:
            pc++;
            break;
        case RE_CHAR:
        case RE_CLASS:
        case RE_ANY:
        case RE_SPLIT:
        case RE_EOL:
        case RE_MATCH:
            return false;
        }
    }
    return false;
}

static bool regex_requires_wordb(const YewRe *re)
{
    u32 pc = 0U;
    u32 guard = 0U;

    if (re == NULL)
        return false;
    while (pc < re->nprog && guard++ <= re->nprog) {
        const ReInst *ins = &re->prog[pc];

        switch ((ReOp)ins->op) {
        case RE_WORDB:
            return true;
        case RE_JMP:
            pc = ins->x;
            break;
        case RE_SAVE:
        case RE_BOL:
        case RE_BOT:
        case RE_EOT:
        case RE_NWORDB:
            pc++;
            break;
        case RE_CHAR:
        case RE_CLASS:
        case RE_ANY:
        case RE_SPLIT:
        case RE_EOL:
        case RE_MATCH:
            return false;
        }
    }
    return false;
}

static bool regex_is_word_literal(const YewRe *re)
{
    u32 suffix;
    u32 stack[256];
    u8 seen[256];
    u32 nstack = 0U;

    if (re == NULL || re->nprog < 5U || re->nprog > 256U ||
        re->ngroups > 2U || (ReOp)re->prog[0].op != RE_SAVE ||
        re->prog[0].arg != 0U || (ReOp)re->prog[1].op != RE_WORDB)
        return false;
    suffix = re->nprog - 3U;
    if (re->ngroups == 2U) {
        if (re->nprog < 7U || (ReOp)re->prog[2].op != RE_SAVE ||
            re->prog[2].arg != 2U ||
            (ReOp)re->prog[re->nprog - 4U].op != RE_SAVE ||
            re->prog[re->nprog - 4U].arg != 3U)
            return false;
        suffix--;
    }
    if ((ReOp)re->prog[re->nprog - 3U].op != RE_WORDB ||
        (ReOp)re->prog[re->nprog - 2U].op != RE_SAVE ||
        re->prog[re->nprog - 2U].arg != 1U ||
        (ReOp)re->prog[re->nprog - 1U].op != RE_MATCH)
        return false;
    (void)memset(seen, 0, sizeof(seen));
    stack[nstack++] = re->ngroups == 2U ? 3U : 2U;
    while (nstack != 0U) {
        u32 pc = stack[--nstack];
        const ReInst *ins;

        if (pc == suffix)
            continue;
        if (pc >= suffix)
            return false;
        if (seen[pc] != 0U)
            continue;
        seen[pc] = 1U;
        ins = &re->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_CHAR:
            if (ins->arg >= 0x80U)
                return false;
            if (nstack == YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = pc + 1U;
            break;
        case RE_JMP:
            if (ins->x <= pc)
                return false;
            if (nstack == YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = ins->x;
            break;
        case RE_SPLIT:
            if (ins->x <= pc || ins->y <= pc)
                return false;
            if (nstack + 2U > YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = ins->x;
            stack[nstack++] = ins->y;
            break;
        default:
            return false;
        }
    }
    return true;
}

static void first_add(u8 first[32], u8 byte)
{
    first[byte >> 3U] |= (u8)(1U << (byte & 7U));
}

static void regex_effective_first(const YewRe *re, u8 first[32])
{
    u32 *stack;
    u8 *seen;
    u32 nstack = 0U;

    (void)memset(first, 0, 32U);
    if (re == NULL || re->nprog == 0U) {
        (void)memset(first, 0xff, 32U);
        return;
    }
    stack = yew_xcalloc((size_t)re->nprog * 2U + 1U, sizeof(*stack));
    seen = yew_xcalloc(re->nprog, sizeof(*seen));
    stack[nstack++] = 0U;
    while (nstack != 0U) {
        u32 pc = stack[--nstack];
        const ReInst *ins;

        if (pc >= re->nprog || seen[pc] != 0U)
            continue;
        seen[pc] = 1U;
        ins = &re->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_CHAR: {
            u8 encoded[YEW_UTF8_MAX];
            size_t n = yew_utf8_encode(ins->arg, encoded);

            if (n == 0U)
                (void)memset(first, 0xff, 32U);
            else
                first_add(first, encoded[0]);
            break;
        }
        case RE_CLASS: {
            u32 byte;
            const ReClass *cls;

            if (ins->arg >= re->nclasses) {
                (void)memset(first, 0xff, 32U);
                break;
            }
            cls = &re->classes[ins->arg];
            for (byte = 0U; byte < 128U; byte++) {
                if (yew_re_class_has(cls, byte))
                    first_add(first, (u8)byte);
            }
            for (byte = 128U; byte < 256U; byte++) {
                if (yew_re_class_has(cls, yew_utf8_escape_of((u8)byte)))
                    first_add(first, (u8)byte);
            }
            for (byte = 0U; byte < cls->n; byte++) {
                u32 lead;

                if (cls->r[byte].hi < 128U)
                    continue;
                for (lead = 0xc2U; lead <= 0xf4U; lead++)
                    first_add(first, (u8)lead);
                break;
            }
            break;
        }
        case RE_ANY:
            (void)memset(first, 0xff, 32U);
            if (ins->arg == 0U)
                first[(u8)'\n' >> 3U] &=
                    (u8)~(1U << ((u8)'\n' & 7U));
            break;
        case RE_SPLIT:
            stack[nstack++] = ins->x;
            stack[nstack++] = ins->y;
            break;
        case RE_JMP:
            stack[nstack++] = ins->x;
            break;
        case RE_SAVE:
        case RE_BOL:
        case RE_EOL:
        case RE_BOT:
        case RE_EOT:
        case RE_WORDB:
        case RE_NWORDB:
            stack[nstack++] = pc + 1U;
            break;
        case RE_MATCH:
            (void)memset(first, 0xff, 32U);
            break;
        }
    }
    free(seen);
    free(stack);
}

static void engine_index_bol(SynEngine *engine)
{
    u32 i;

    free(engine->rule_bol);
    engine->rule_bol = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_bol = yew_xcalloc(engine->def->nrules,
                                   sizeof(*engine->rule_bol));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (regex_requires_bol(engine->def->rules[i].re))
            engine->rule_bol[i] = 1U;
    }
}

static void engine_index_first_line(SynEngine *engine)
{
    u32 i;

    engine->has_first_line = false;
    if (engine->def == NULL)
        return;
    for (i = 0U; i < engine->def->nrules; i++) {
        if ((engine->def->rules[i].flags & YEW_SYN_RULE_FIRST_LINE) != 0U) {
            engine->has_first_line = true;
            return;
        }
    }
}

static void engine_index_wordb(SynEngine *engine)
{
    u32 i;

    free(engine->rule_wordb);
    engine->rule_wordb = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_wordb = yew_xcalloc(engine->def->nrules,
                                     sizeof(*engine->rule_wordb));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (regex_requires_wordb(engine->def->rules[i].re))
            engine->rule_wordb[i] = 1U;
    }
}

static void engine_index_word_literals(SynEngine *engine)
{
    u32 i;

    free(engine->rule_word_literal);
    engine->rule_word_literal = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_word_literal = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_word_literal));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (regex_is_word_literal(engine->def->rules[i].re))
            engine->rule_word_literal[i] = 1U;
    }
}

static void engine_index_identifier_suffixes(SynEngine *engine)
{
    u32 i;

    free(engine->rule_identifier_suffix);
    engine->rule_identifier_suffix = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_identifier_suffix = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_identifier_suffix));
    for (i = 0U; i < engine->def->nrules; i++)
        engine->rule_identifier_suffix[i] =
            regex_identifier_suffix(engine->def->rules[i].re);
}

static void engine_index_first(SynEngine *engine)
{
    u32 i;

    free(engine->rule_first);
    engine->rule_first = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_first = yew_xcalloc(engine->def->nrules,
                                     sizeof(*engine->rule_first));
    for (i = 0U; i < engine->def->nrules; i++)
        regex_effective_first(engine->def->rules[i].re,
                              engine->rule_first[i]);
}

static void engine_index_ctx_first_nonbol(SynEngine *engine)
{
    u32 i;

    free(engine->ctx_first_nonbol);
    engine->ctx_first_nonbol = NULL;
    if (engine->def == NULL || engine->def->nctxs == 0U)
        return;
    engine->ctx_first_nonbol = yew_xcalloc(
        engine->def->nctxs, sizeof(*engine->ctx_first_nonbol));
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 j;

        for (j = 0U; j < ctx->nrules; j++) {
            u32 index = ctx->first_rule + j;
            u32 byte;

            if (index >= engine->def->nrules)
                YEW_BUG("syntax: context rule range exceeds definition");
            if (engine->rule_bol != NULL &&
                engine->rule_bol[index] != 0U)
                continue;
            for (byte = 0U; byte < 32U; byte++)
                engine->ctx_first_nonbol[i][byte] |=
                    engine->rule_first[index][byte];
        }
    }
}

static void engine_index_candidates(SynEngine *engine)
{
    u32 *offsets;
    u64 total = 0U;
    size_t noffsets;
    u32 out = 0U;
    u32 i;

    free(engine->candidate_offsets);
    free(engine->candidate_rules);
    engine->candidate_offsets = NULL;
    engine->candidate_rules = NULL;
    if (engine->def == NULL || engine->def->nctxs == 0U)
        return;
    noffsets = (size_t)engine->def->nctxs * SYN_CANDIDATE_STRIDE;
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 j;

        for (j = 0U; j < ctx->nrules; j++) {
            u32 index = ctx->first_rule + j;
            u32 byte;

            if (index >= engine->def->nrules)
                YEW_BUG("syntax: context rule range exceeds definition");
            for (byte = 0U; byte < SYN_CANDIDATE_BYTES; byte++) {
                if (engine->def->rules[index].aux_match != SYN_AUXM_NONE ||
                    bitset_has(engine->rule_first[index], (u8)byte)) {
                    total++;
                    /* The index is an optional accelerator.  A broad valid
                     * definition must fall back to the filtered rule scan,
                     * not turn its rule/byte cross-product into a fatal
                     * allocation. */
                    if (total >
                        SYN_CANDIDATE_RULE_BYTES_MAX / sizeof(u32))
                        return;
                }
            }
        }
    }
    offsets = yew_xcalloc(noffsets, sizeof(*offsets));
    engine->candidate_rules = yew_xcalloc((size_t)total,
                                           sizeof(*engine->candidate_rules));
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 byte;

        for (byte = 0U; byte < SYN_CANDIDATE_BYTES; byte++) {
            u32 j;

            offsets[(size_t)i * SYN_CANDIDATE_STRIDE + byte] = out;
            for (j = 0U; j < ctx->nrules; j++) {
                u32 index = ctx->first_rule + j;

                if (engine->def->rules[index].aux_match != SYN_AUXM_NONE ||
                    bitset_has(engine->rule_first[index], (u8)byte))
                    engine->candidate_rules[out++] = index;
            }
        }
        offsets[(size_t)i * SYN_CANDIDATE_STRIDE +
                SYN_CANDIDATE_BYTES] = out;
    }
    engine->candidate_offsets = offsets;
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
    engine->identifier_fast_enabled = true;
    yew_re_workspace_init(&engine->re_workspace);
    engine_index_aux(engine);
    engine_index_bol(engine);
    engine_index_first_line(engine);
    engine_index_wordb(engine);
    engine_index_word_literals(engine);
    engine_index_identifier_suffixes(engine);
    engine_index_first(engine);
    engine_index_ctx_first_nonbol(engine);
    engine_index_candidates(engine);
    return engine;
}

void yew_syn_engine_free(SynEngine *engine)
{
    if (engine == NULL)
        return;
    yew_syn_state_tab_free(engine->states);
    free(engine->ctx_aux);
    free(engine->rule_bol);
    free(engine->rule_wordb);
    free(engine->rule_word_literal);
    free(engine->rule_identifier_suffix);
    free(engine->rule_first);
    free(engine->ctx_first_nonbol);
    free(engine->candidate_offsets);
    free(engine->candidate_rules);
    yew_re_workspace_free(&engine->re_workspace);
    free(engine);
}

void yew_syn_engine_set_def(SynEngine *engine, SynDef *def)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    yew_syn_state_tab_free(engine->states);
    engine->def = def;
    engine->coverage = NULL;
    engine->states = yew_syn_state_tab_new(def == NULL ? 0U : def->root);
    engine_index_aux(engine);
    engine_index_bol(engine);
    engine_index_first_line(engine);
    engine_index_wordb(engine);
    engine_index_word_literals(engine);
    engine_index_identifier_suffixes(engine);
    engine_index_first(engine);
    engine_index_ctx_first_nonbol(engine);
    engine_index_candidates(engine);
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

void yew_syn_engine_set_identifier_fast_path(SynEngine *engine,
                                             bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->identifier_fast_enabled = enabled;
}

u32 yew_syn_engine_identifier_fast_rules(const SynEngine *engine)
{
    u32 count = 0U;
    u32 i;

    if (engine == NULL || engine->def == NULL ||
        engine->rule_identifier_suffix == NULL)
        return 0U;
    for (i = 0U; i < engine->def->nrules; i++) {
        if (engine->rule_identifier_suffix[i] != 0U)
            count++;
    }
    return count;
}

bool yew_syn_coverage_init(SynCoverage *coverage, const SynDef *def)
{
    if (coverage == NULL || def == NULL)
        return false;
    (void)memset(coverage, 0, sizeof(*coverage));
    coverage->nctxs = def->nctxs;
    coverage->nrules = def->nrules;
    if (coverage->nctxs != 0U)
        coverage->contexts = yew_xcalloc(coverage->nctxs,
                                         sizeof(*coverage->contexts));
    if (coverage->nrules != 0U)
        coverage->rules = yew_xcalloc(coverage->nrules,
                                      sizeof(*coverage->rules));
    return true;
}

void yew_syn_coverage_clear(SynCoverage *coverage)
{
    if (coverage == NULL)
        return;
    if (coverage->contexts != NULL)
        (void)memset(coverage->contexts, 0,
                     coverage->nctxs * sizeof(*coverage->contexts));
    if (coverage->rules != NULL)
        (void)memset(coverage->rules, 0,
                     coverage->nrules * sizeof(*coverage->rules));
}

void yew_syn_coverage_free(SynCoverage *coverage)
{
    if (coverage == NULL)
        return;
    free(coverage->contexts);
    free(coverage->rules);
    (void)memset(coverage, 0, sizeof(*coverage));
}

void yew_syn_engine_set_coverage(SynEngine *engine, SynCoverage *coverage)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    if (coverage != NULL &&
        (engine->def == NULL || coverage->nctxs != engine->def->nctxs ||
         coverage->nrules != engine->def->nrules))
        YEW_BUG("syntax: coverage table does not match definition");
    engine->coverage = coverage;
}

static void coverage_context(SynEngine *engine, u16 ctx)
{
    if (engine->coverage != NULL && ctx < engine->coverage->nctxs)
        engine->coverage->contexts[ctx]++;
}

static void coverage_rule(SynEngine *engine, u32 rule)
{
    if (engine->coverage != NULL && rule < engine->coverage->nrules)
        engine->coverage->rules[rule]++;
}

static void coverage_transition(SynEngine *engine, const SynState *before,
                                const SynState *after)
{
    u8 i;

    if (engine->coverage == NULL || before == NULL || after == NULL ||
        after->depth == 0U)
        return;
    if (after->depth > before->depth) {
        for (i = before->depth; i < after->depth; i++)
            coverage_context(engine, after->ctx[i]);
    } else if (after->depth != before->depth ||
               after->ctx[after->depth - 1U] !=
                   before->ctx[before->depth - 1U]) {
        coverage_context(engine, after->ctx[after->depth - 1U]);
    }
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
        /* Blank lines are content in indentation-delimited constructs such
         * as YAML block scalars; only a nonblank dedent closes the context. */
        if (p == len)
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return indent < state->aux;
    }
    if (rule->aux_match == SYN_AUXM_LINE_EMPTY) {
        if (at != 0U || len != 0U)
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return true;
    }
    if (rule->aux_match == SYN_AUXM_LINE_START) {
        if (at != 0U)
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return true;
    }
    if (engine->def->aux == NULL || state->aux == 0U)
        return false;
    aux = yew_intern_str(engine->def->aux, state->aux);
    aux_len = yew_intern_len(engine->def->aux, state->aux);
    if (aux == NULL)
        return false;
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
    case SYN_AUXM_LINE_EMPTY:
    case SYN_AUXM_LINE_START:
        return false;
    case SYN_AUXM_NONE:
        break;
    }
    return false;
}

static bool ascii_word(u8 byte)
{
    return byte == (u8)'_' || (byte >= (u8)'0' && byte <= (u8)'9') ||
           (byte >= (u8)'A' && byte <= (u8)'Z') ||
           (byte >= (u8)'a' && byte <= (u8)'z');
}

static int word_literal_match(const YewRe *re, const u8 *line, u32 len,
                              u32 at, YewReMatch *match)
{
    typedef struct WordFrame {
        u32 pc;
        u32 pos;
    } WordFrame;
    WordFrame stack[256];
    u32 nstack = 0U;
    u32 suffix = re->nprog - (re->ngroups == 2U ? 4U : 3U);

    if (at >= len)
        return 0;
    if ((at != 0U && line[at - 1U] >= 0x80U) || line[at] >= 0x80U)
        return -1;
    stack[nstack++] = (WordFrame){re->ngroups == 2U ? 3U : 2U, at};
    while (nstack != 0U) {
        WordFrame frame = stack[--nstack];
        const ReInst *ins;

        if (frame.pc == suffix) {
            bool before_word;
            bool after_word;
            u32 group;

            if (frame.pos != len && line[frame.pos] >= 0x80U)
                return -1;
            before_word = frame.pos != 0U &&
                          ascii_word(line[frame.pos - 1U]);
            after_word = frame.pos != len && ascii_word(line[frame.pos]);
            if (before_word == after_word)
                continue;
            match->ngroups = re->ngroups;
            for (group = 0U; group < re->ngroups; group++)
                match->g[group] = (Span){at, frame.pos};
            return 1;
        }
        ins = &re->prog[frame.pc];
        switch ((ReOp)ins->op) {
        case RE_CHAR:
            if (frame.pos < len && line[frame.pos] == (u8)ins->arg) {
                if (nstack == YEW_ARRAY_LEN(stack))
                    return -1;
                stack[nstack++] =
                    (WordFrame){frame.pc + 1U, frame.pos + 1U};
            }
            break;
        case RE_JMP:
            if (nstack == YEW_ARRAY_LEN(stack))
                return -1;
            stack[nstack++] = (WordFrame){ins->x, frame.pos};
            break;
        case RE_SPLIT:
            if (nstack + 2U > YEW_ARRAY_LEN(stack))
                return -1;
            stack[nstack++] = (WordFrame){ins->y, frame.pos};
            stack[nstack++] = (WordFrame){ins->x, frame.pos};
            break;
        default:
            YEW_BUG("syntax: invalid word-literal fast path");
        }
    }
    return 0;
}

static int identifier_suffix_match(const YewRe *re, u8 suffix,
                                   const u8 *line, u32 len, u32 at,
                                   YewReMatch *match)
{
    u32 p;
    u32 identifier_hi;

    if (at >= len)
        return 0;
    if (line[at] >= 0x80U)
        return -1;
    if (!ascii_identifier_start(line[at]))
        return 0;
    p = at + 1U;
    while (p < len && line[p] < 0x80U &&
           ascii_identifier_continue(line[p]))
        p++;
    identifier_hi = p;
    if (p < len && line[p] >= 0x80U)
        return -1;
    while (p < len && ascii_space(line[p]))
        p++;
    if (p < len && line[p] >= 0x80U)
        return -1;
    if (p >= len || line[p] != suffix)
        return 0;
    match->ngroups = re->ngroups;
    match->g[0] = (Span){at, p + 1U};
    match->g[1] = (Span){at, identifier_hi};
    return 1;
}

static bool rule_match(SynEngine *engine, const SynState *state,
                       const SynRule *rule, u32 rule_index, const u8 *line,
                       u32 len, u32 at, YewReMatch *match)
{
    if (rule->aux_match != SYN_AUXM_NONE)
        return aux_match(engine, state, rule, line, len, at, match);
    if (rule->re == NULL)
        return false;
    if (engine->identifier_fast_enabled &&
        engine->rule_identifier_suffix != NULL &&
        engine->rule_identifier_suffix[rule_index] != 0U) {
        int fast = identifier_suffix_match(
            rule->re, engine->rule_identifier_suffix[rule_index], line,
            len, at, match);

        if (fast >= 0)
            return fast != 0;
    }
    if (engine->rule_word_literal != NULL &&
        engine->rule_word_literal[rule_index] != 0U) {
        int fast = word_literal_match(rule->re, line, len, at, match);

        if (fast >= 0)
            return fast != 0;
    }
    if (rule->re->lit.kind == RE_LIT_WHOLE &&
        rule->re->ngroups == 1U &&
        (rule->re->flags & YEW_RE_ICASE) == 0U) {
        if (rule->re->lit.n > len - at ||
            (rule->re->lit.n != 0U &&
             memcmp(line + at, rule->re->lit.s, rule->re->lit.n) != 0))
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){at, at + rule->re->lit.n};
        return true;
    }
    if (rule->re->lit.kind != RE_LIT_NONE &&
        (rule->re->flags & YEW_RE_ICASE) == 0U &&
        (rule->re->lit.n > len - at ||
         memcmp(line + at, rule->re->lit.s, rule->re->lit.n) != 0))
        return false;
    return yew_re_match_at_ws(&engine->re_workspace, rule->re,
                              &(YewReInput){NULL, line, len, {0U, len}},
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
        if (value > UINT32_MAX - rule->aux_add)
            value = UINT32_MAX;
        else
            value += rule->aux_add;
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
                            const u8 *line, bool instrument)
{
    u32 guard = 0U;
    while (guard++ <= YEW_SYN_DEPTH_MAX) {
        u16 ctx_id = state->ctx[state->depth - 1U];
        const SynCtx *ctx = &engine->def->ctxs[ctx_id];
        const SynRule *matched = NULL;
        u32 matched_index = UINT32_MAX;
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
            if ((state->flags & YEW_SYN_F_PAST_FIRST) != 0U &&
                (rule->flags & YEW_SYN_RULE_FIRST_LINE) != 0U)
                continue;
            if ((rule->value_pred == SYN_VALUE_SET &&
                 (state->flags & YEW_SYN_F_VALUE) == 0U) ||
                (rule->value_pred == SYN_VALUE_CLEAR &&
                 (state->flags & YEW_SYN_F_VALUE) != 0U))
                continue;
            if (rule_match(engine, state, rule, index, line, 0U, 0U,
                           &match) &&
                match.g[0].hi == 0U) {
                matched = rule;
                matched_index = index;
                break;
            }
        }
        if (matched == NULL)
            return;
        {
            SynState before = *state;

            if (instrument)
                coverage_rule(engine, matched_index);
            (void)set_aux(engine, state, matched, line, 0U, &match);
            apply_rule_op(state, matched);
            if (instrument)
                coverage_transition(engine, &before, state);
        }
        if ((matched->flags & YEW_SYN_RULE_ZERO_TRANSITION) == 0U ||
            matched->op == SYN_OP_STAY)
            return;
    }
}

static u32 truncated_exit_state(SynEngine *engine, u32 entry_state,
                                const SynState *entry, bool apply_eol)
{
    SynState exit = *entry;

    /* A truncated line deliberately suppresses grammar transitions, but
     * it is still a physical line.  Losing this bit lets line-two-only
     * content satisfy `first_line` rules after an oversized or hostile
     * first line. */
    if (apply_eol) {
        exit.flags &= (u8)~YEW_SYN_F_VALUE;
        if (engine->has_first_line)
            exit.flags |= YEW_SYN_F_PAST_FIRST;
        return yew_syn_state_intern(engine->states, &exit);
    }
    return entry_state;
}

static void syn_line_run(SynEngine *engine, u32 entry_state,
                         const u8 *line, u32 len, SynLineOut *out,
                         bool apply_eol, bool instrument, SynState *trace)
{
    SynState state;
    const SynState *entry;
    u64 steps = 0U;
    u64 step_cap;
    u32 p = 0U;
    u32 zero_transitions = 0U;

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
    if (instrument && state.depth != 0U)
        coverage_context(engine, state.ctx[state.depth - 1U]);
    if (trace != NULL)
        trace[0] = state;
    if (len > YEW_SYN_LINE_BYTE_CAP) {
        emit_span(out, 0U, len, YEW_ATTR_TEXT, YEW_SPAN_TRUNCATED);
        out->exit_state = truncated_exit_state(engine, entry_state, entry,
                                               apply_eol);
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
        apply_empty_bol(engine, &state, line, instrument);
        if (trace != NULL)
            trace[0] = state;
    }
    while (p < len) {
        const SynCtx *ctx = &engine->def->ctxs[state.ctx[state.depth - 1U]];
        const u32 *candidate_offsets = engine->candidate_offsets;
        size_t candidate_slot =
            (size_t)state.ctx[state.depth - 1U] * SYN_CANDIDATE_STRIDE +
            line[p];
        u32 candidate_off = candidate_offsets == NULL ? 0U :
            candidate_offsets[candidate_slot];
        u32 candidate_len = candidate_offsets == NULL ? 0U :
            candidate_offsets[candidate_slot + 1U] - candidate_off;
        const SynRule *matched = NULL;
        u32 matched_index = UINT32_MAX;
        SynState before;
        YewReMatch match;
        u32 ri;

        if (!bitset_has(p == 0U || engine->ctx_first_nonbol == NULL ?
                        ctx->first :
                        engine->ctx_first_nonbol[
                            state.ctx[state.depth - 1U]], line[p]) &&
            (engine->ctx_aux == NULL ||
             engine->ctx_aux[state.ctx[state.depth - 1U]] == 0U)) {
            u32 q = p + 1U;
            const u8 *next_first = engine->ctx_first_nonbol == NULL ?
                ctx->first :
                engine->ctx_first_nonbol[state.ctx[state.depth - 1U]];

            while (q < len && !bitset_has(next_first, line[q]))
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
        for (ri = 0U; ri < (candidate_offsets == NULL ? ctx->nrules :
                            candidate_len); ri++) {
            u32 index = candidate_offsets == NULL ? ctx->first_rule + ri :
                engine->candidate_rules[candidate_off + ri];
            const SynRule *rule;
            if (++steps > step_cap || index >= engine->def->nrules) {
                emit_span(out, p, len - p, YEW_ATTR_TEXT,
                          YEW_SPAN_TRUNCATED);
                out->exit_state = truncated_exit_state(
                    engine, entry_state, entry, apply_eol);
                out->stop = YEW_SYN_STOP_STEPS;
                return;
            }
            rule = &engine->def->rules[index];
            if ((state.flags & YEW_SYN_F_PAST_FIRST) != 0U &&
                (rule->flags & YEW_SYN_RULE_FIRST_LINE) != 0U)
                continue;
            if ((rule->value_pred == SYN_VALUE_SET &&
                 (state.flags & YEW_SYN_F_VALUE) == 0U) ||
                (rule->value_pred == SYN_VALUE_CLEAR &&
                 (state.flags & YEW_SYN_F_VALUE) != 0U))
                continue;
            if (p != 0U && engine->rule_bol != NULL &&
                engine->rule_bol[index] != 0U)
                continue;
            if (engine->rule_wordb != NULL &&
                engine->rule_wordb[index] != 0U && line[p] < 0x80U) {
                bool after_word = line[p] == (u8)'_' ||
                                  (line[p] >= (u8)'0' &&
                                   line[p] <= (u8)'9') ||
                                  (line[p] >= (u8)'A' &&
                                   line[p] <= (u8)'Z') ||
                                  (line[p] >= (u8)'a' &&
                                   line[p] <= (u8)'z');
                bool before_word = false;

                if (p != 0U && line[p - 1U] < 0x80U)
                    before_word = line[p - 1U] == (u8)'_' ||
                        (line[p - 1U] >= (u8)'0' &&
                         line[p - 1U] <= (u8)'9') ||
                        (line[p - 1U] >= (u8)'A' &&
                         line[p - 1U] <= (u8)'Z') ||
                        (line[p - 1U] >= (u8)'a' &&
                         line[p - 1U] <= (u8)'z');
                else if (p != 0U)
                    before_word = !after_word;
                if (before_word == after_word)
                    continue;
            }
            if (candidate_offsets == NULL &&
                !bitset_has(engine->rule_first == NULL ? rule->first :
                            engine->rule_first[index], line[p]) &&
                rule->aux_match == SYN_AUXM_NONE)
                continue;
            if (rule_match(engine, &state, rule, index, line, len, p,
                           &match)) {
                matched = rule;
                matched_index = index;
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
        if (instrument || trace != NULL)
            before = state;
        if (instrument)
            coverage_rule(engine, matched_index);
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
        if (instrument)
            coverage_transition(engine, &before, &state);
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
            } else if ((matched->flags & YEW_SYN_RULE_ZERO_TRANSITION) != 0U &&
                       matched->op != SYN_OP_STAY && p == 0U &&
                       zero_transitions++ < YEW_SYN_DEPTH_MAX) {
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
        SynState before;

        if (instrument)
            before = state;
        apply_op(&state, ctx->at_eol, ctx->eol_nop, ctx->eol_target);
        state.flags &= (u8)~YEW_SYN_F_VALUE;
        if (engine->has_first_line)
            state.flags |= YEW_SYN_F_PAST_FIRST;
        if (instrument)
            coverage_transition(engine, &before, &state);
    }
    out->exit_state = yew_syn_state_intern(engine->states, &state);
}

void yew_syn_line(SynEngine *engine, u32 entry_state, const u8 *line,
                  u32 len, SynLineOut *out)
{
    syn_line_run(engine, entry_state, line, len, out, true,
                 engine != NULL && engine->coverage != NULL, NULL);
}

bool yew_syn_stack_trace(SynEngine *engine, u32 entry_state, const u8 *line,
                         u32 len, SynState *trace, size_t trace_cap)
{
    SynLineOut line_out = {NULL, 0U, 0U, YEW_SYN_STATE_UNKNOWN,
                           YEW_SYN_STOP_OK};

    if (engine == NULL || trace == NULL || len > YEW_SYN_LINE_BYTE_CAP ||
        (line == NULL && len != 0U) || trace_cap < (size_t)len + 1U)
        return false;
    syn_line_run(engine, entry_state, line, len, &line_out, false, false,
                 trace);
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
    cap = vec->cap == 0U ? (need > 1024U ? need : 8U) : vec->cap;
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
    u64 clock_every;
    i64 deadline_us;
    SynSpan spans[YEW_SYN_MAX_SPANS];

    if (syn == NULL || tb == NULL)
        YEW_BUG("syntax: invalid settle arguments");
    if (report == NULL)
        report = &local;
    (void)memset(report, 0, sizeof(*report));
    report->damage_lo = LINENO(UINT64_MAX);
    started = syn_now(syn);
    /* Injected deterministic clocks count observations, so preserve their
     * established 256-line quantum.  The real monotonic clock samples
     * more often to keep a 1 ms frame from overshooting its deadline. */
    clock_every = syn->clock == real_now_us ? YEW_SYN_CLOCK_EVERY :
                  YEW_SYN_INJECTED_CLOCK_EVERY;
    deadline_us = budget_us;
    if (syn->clock == real_now_us &&
        deadline_us > YEW_SYN_CLOCK_HEADROOM_US)
        deadline_us -= YEW_SYN_CLOCK_HEADROOM_US;
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
        if ((i & (clock_every - 1U)) == 0U && budget_us > 0) {
            elapsed = syn_now(syn) - started;
        }
        if (elapsed >= deadline_us && budget_us > 0) {
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
