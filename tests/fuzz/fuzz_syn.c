#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search/regex.h"
#include "syn/engine.h"
#include "text/piece.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    SYN_FUZZ_CTX_ROOT = 0,
    SYN_FUZZ_CTX_STRING,
    SYN_FUZZ_CTX_COMMENT,
    SYN_FUZZ_CTX_COUNT,
    SYN_FUZZ_RULE_COUNT = 9,
    SYN_FUZZ_EDIT_BYTES = 4096,
    SYN_FUZZ_DEFAULT_EDITS = 4
};

typedef struct SynFixture {
    Arena arena;
    Interner aux;
    SynCtx ctx[SYN_FUZZ_CTX_COUNT];
    SynRule rule[SYN_FUZZ_RULE_COUNT];
    SynDef def;
    SynEngine *engine;
} SynFixture;

typedef struct Rng {
    u64 state;
} Rng;

static u64 rng_next(Rng *rng)
{
    u64 x = rng->state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    rng->state = x;
    return x * UINT64_C(2685821657736338717);
}

static size_t choose(Rng *rng, size_t limit)
{
    return limit == 0U ? 0U : (size_t)(rng_next(rng) % (u64)limit);
}

static void first_add(u8 first[32], u8 byte)
{
    first[byte >> 3U] |= (u8)(1U << (byte & 7U));
}

static bool rule_init(SynFixture *fx, u32 at, const char *pattern,
                      u32 flags, u8 first, u8 attr, u8 op, u16 target)
{
    SynRule *rule = &fx->rule[at];

    (void)memset(rule, 0, sizeof(*rule));
    (void)memset(rule->caps, 0xff, sizeof(rule->caps));
    rule->re = yew_re_compile(&fx->arena, pattern, strlen(pattern), flags,
                              NULL);
    if (rule->re == NULL)
        return false;
    rule->attr = attr;
    rule->op = op;
    rule->nop = 1U;
    rule->target = target;
    first_add(rule->first, first);
    return true;
}

static bool fixture_init(SynFixture *fx)
{
    (void)memset(fx, 0, sizeof(*fx));
    arena_init(&fx->arena);
    interner_init(&fx->aux, &fx->arena);
    if (!rule_init(fx, 0U, "\"", YEW_RE_LITERAL, (u8)'\"',
                   YEW_ATTR_STRING, SYN_OP_PUSH, SYN_FUZZ_CTX_STRING) ||
        !rule_init(fx, 1U, "/*", YEW_RE_LITERAL, (u8)'/',
                   YEW_ATTR_COMMENT, SYN_OP_PUSH, SYN_FUZZ_CTX_COMMENT) ||
        !rule_init(fx, 2U, "if", YEW_RE_LITERAL, (u8)'i',
                   YEW_ATTR_KEYWORD_CONTROL, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 3U, "[0-9]+", 0U, (u8)'0',
                   YEW_ATTR_NUMBER, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 4U, "(z)?q", 0U, (u8)'q',
                   YEW_ATTR_TEXT, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 5U, "\\\\.", 0U, (u8)'\\',
                   YEW_ATTR_STRING_ESCAPE, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 6U, "\"", YEW_RE_LITERAL, (u8)'\"',
                   YEW_ATTR_STRING, SYN_OP_POP, 0U) ||
        !rule_init(fx, 7U, "a*", 0U, (u8)'a',
                   YEW_ATTR_STRING_SPECIAL, SYN_OP_STAY, 0U) ||
        !rule_init(fx, 8U, "*/", YEW_RE_LITERAL, (u8)'*',
                   YEW_ATTR_COMMENT, SYN_OP_POP, 0U)) {
        interner_free(&fx->aux);
        arena_free_all(&fx->arena);
        return false;
    }

    fx->ctx[SYN_FUZZ_CTX_ROOT].first_rule = 0U;
    fx->rule[4U].flags = YEW_SYN_RULE_SET_AUX | YEW_SYN_RULE_STRIP;
    fx->rule[4U].aux_group = 1U;
    fx->ctx[SYN_FUZZ_CTX_ROOT].nrules = 5U;
    fx->ctx[SYN_FUZZ_CTX_ROOT].dflt_attr = YEW_ATTR_TEXT;
    first_add(fx->ctx[SYN_FUZZ_CTX_ROOT].first, (u8)'\"');
    first_add(fx->ctx[SYN_FUZZ_CTX_ROOT].first, (u8)'/');
    first_add(fx->ctx[SYN_FUZZ_CTX_ROOT].first, (u8)'i');
    first_add(fx->ctx[SYN_FUZZ_CTX_ROOT].first, (u8)'q');
    for (u8 digit = (u8)'0'; digit <= (u8)'9'; digit++) {
        first_add(fx->rule[3].first, digit);
        first_add(fx->ctx[SYN_FUZZ_CTX_ROOT].first, digit);
    }
    fx->ctx[SYN_FUZZ_CTX_STRING].first_rule = 5U;
    fx->ctx[SYN_FUZZ_CTX_STRING].nrules = 3U;
    fx->ctx[SYN_FUZZ_CTX_STRING].dflt_attr = YEW_ATTR_STRING;
    fx->ctx[SYN_FUZZ_CTX_STRING].at_eol = SYN_OP_POP;
    fx->ctx[SYN_FUZZ_CTX_STRING].eol_nop = 1U;
    first_add(fx->ctx[SYN_FUZZ_CTX_STRING].first, (u8)'\\');
    first_add(fx->ctx[SYN_FUZZ_CTX_STRING].first, (u8)'\"');
    first_add(fx->ctx[SYN_FUZZ_CTX_STRING].first, (u8)'a');
    fx->ctx[SYN_FUZZ_CTX_COMMENT].first_rule = 8U;
    fx->ctx[SYN_FUZZ_CTX_COMMENT].nrules = 1U;
    fx->ctx[SYN_FUZZ_CTX_COMMENT].dflt_attr = YEW_ATTR_COMMENT;
    first_add(fx->ctx[SYN_FUZZ_CTX_COMMENT].first, (u8)'*');

    fx->def.name = "fuzz-toy";
    fx->def.root = SYN_FUZZ_CTX_ROOT;
    fx->def.nctxs = SYN_FUZZ_CTX_COUNT;
    fx->def.nrules = SYN_FUZZ_RULE_COUNT;
    fx->def.ctxs = fx->ctx;
    fx->def.rules = fx->rule;
    fx->def.aux = &fx->aux;
    fx->engine = yew_syn_engine_new(&fx->def);
    if (fx->engine == NULL) {
        interner_free(&fx->aux);
        arena_free_all(&fx->arena);
        return false;
    }
    return true;
}

static void fixture_free(SynFixture *fx)
{
    yew_syn_engine_free(fx->engine);
    interner_free(&fx->aux);
    arena_free_all(&fx->arena);
}

static u64 seed_of(const u8 *data, size_t len)
{
    u64 hash = UINT64_C(1469598103934665603);

    for (size_t i = 0U; i < len; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0U ? 1U : hash;
}

static void mutate_definition(SynFixture *fx, Rng *rng)
{
    size_t a = choose(rng, 5U);
    size_t b = choose(rng, 5U);
    SynRule tmp = fx->rule[a];
    SynRule *rule;

    fx->rule[a] = fx->rule[b];
    fx->rule[b] = tmp;
    rule = &fx->rule[choose(rng, SYN_FUZZ_RULE_COUNT)];
    rule->op = (u8)choose(rng, 4U);
    rule->target = (u16)choose(rng, SYN_FUZZ_CTX_COUNT);
    rule->nop = (u8)(1U + choose(rng, 4U));
}

static u32 random_entry(SynFixture *fx, Rng *rng)
{
    SynState state;
    u8 depth = (u8)(1U + choose(rng, YEW_SYN_DEPTH_MAX));

    (void)memset(&state, 0, sizeof(state));
    state.depth = depth;
    state.lost = (u8)choose(rng, 8U);
    state.ctx[0] = SYN_FUZZ_CTX_ROOT;
    for (u8 i = 1U; i < depth; i++)
        state.ctx[i] = (u16)choose(rng, SYN_FUZZ_CTX_COUNT);
    return yew_syn_state_intern(yew_syn_engine_states(fx->engine), &state);
}

static bool check_line(SynFixture *fx, Rng *rng, const u8 *data, size_t len,
                       char *why, size_t why_cap)
{
    SynSpan spans[YEW_SYN_MAX_SPANS];
    SynLineOut out;
    const SynState *exit;
    u32 entry;
    u64 end = 0U;

    mutate_definition(fx, rng);
    entry = random_entry(fx, rng);
    (void)memset(&out, 0, sizeof(out));
    out.spans = spans;
    out.cap = YEW_SYN_MAX_SPANS;
    yew_syn_line(fx->engine, entry, data, (u32)len, &out);
    if (out.n > out.cap) {
        (void)snprintf(why, why_cap, "span count %u exceeds cap %u",
                       (unsigned)out.n, (unsigned)out.cap);
        return false;
    }
    for (u32 i = 0U; i < out.n; i++) {
        const SynSpan *span = &out.spans[i];
        u64 span_end = (u64)span->start + span->len;

        if (span->len == 0U || span->start < end || span_end > len) {
            (void)snprintf(why, why_cap,
                           "invalid span %u: %u+%u after %llu in %zu",
                           (unsigned)i, (unsigned)span->start,
                           (unsigned)span->len, (unsigned long long)end, len);
            return false;
        }
        if (span->attr >= YEW_ATTR__COUNT) {
            (void)snprintf(why, why_cap, "span %u has attr %u",
                           (unsigned)i, (unsigned)span->attr);
            return false;
        }
        end = span_end;
    }
    exit = yew_syn_state_get(yew_syn_engine_states(fx->engine),
                             out.exit_state);
    if (exit == NULL || exit->depth < 1U ||
        exit->depth > YEW_SYN_DEPTH_MAX || exit->def != 0U) {
        (void)snprintf(why, why_cap, "invalid exit state id %u",
                       (unsigned)out.exit_state);
        return false;
    }
    for (u8 i = 0U; i < exit->depth; i++) {
        if (exit->ctx[i] >= SYN_FUZZ_CTX_COUNT) {
            (void)snprintf(why, why_cap, "exit context %u out of range",
                           (unsigned)exit->ctx[i]);
            return false;
        }
    }
    return true;
}

static bool settle_all(SynBuf *syn, const TextBuf *tb, char *why,
                       size_t why_cap)
{
    u64 limit = yew_textbuf_line_count(tb) + 16U;

    for (u64 i = 0U; i < limit; i++) {
        SynSettleReport report;

        yew_syn_settle(syn, tb, LINENO(0U),
                       LINENO(yew_textbuf_line_count(tb)),
                       INT64_C(1000000000), &report);
        if (report.fixpoint)
            return true;
    }
    (void)snprintf(why, why_cap, "settle did not reach a fixpoint");
    return false;
}

static bool compare_spans(SynBuf *a, SynBuf *b, const TextBuf *tb,
                          char *why, size_t why_cap)
{
    SynSpan as[YEW_SYN_MAX_SPANS];
    SynSpan bs[YEW_SYN_MAX_SPANS];
    u64 lines = yew_textbuf_line_count(tb);

    for (u64 line = 0U; line < lines; line++) {
        SynLineOut ao = {as, 0U, YEW_SYN_MAX_SPANS, 0U, 0U};
        SynLineOut bo = {bs, 0U, YEW_SYN_MAX_SPANS, 0U, 0U};

        yew_syn_spans(a, tb, LINENO(line), &ao);
        yew_syn_spans(b, tb, LINENO(line), &bo);
        if (ao.n != bo.n || ao.exit_state != bo.exit_state ||
            ao.stop != bo.stop ||
            (ao.n != 0U && memcmp(ao.spans, bo.spans,
                                  ao.n * sizeof(*ao.spans)) != 0)) {
            (void)snprintf(why, why_cap,
                           "incremental spans differ on line %llu",
                           (unsigned long long)line);
            return false;
        }
    }
    return true;
}

static bool compare_fresh(SynBuf *incremental, SynBuf *fresh,
                          const TextBuf *tb, char *why, size_t why_cap)
{
    bool ok;

    yew_syn_attach(fresh, 1U, tb);
    if (!settle_all(fresh, tb, why, why_cap))
        return false;
    ok = incremental->entry.len == fresh->entry.len &&
         (fresh->entry.len == 0U ||
          memcmp(incremental->entry.data, fresh->entry.data,
                 fresh->entry.len * sizeof(*fresh->entry.data)) == 0);
    if (!ok) {
        size_t at = 0U;

        while (at < incremental->entry.len && at < fresh->entry.len &&
               incremental->entry.data[at] == fresh->entry.data[at])
            at++;
        (void)snprintf(why, why_cap,
                       "incremental entry differs at %zu (%u != %u)", at,
                       at < incremental->entry.len
                           ? (unsigned)incremental->entry.data[at] : 0U,
                       at < fresh->entry.len
                           ? (unsigned)fresh->entry.data[at] : 0U);
    } else {
        ok = compare_spans(incremental, fresh, tb, why, why_cap);
    }
    return ok;
}

static size_t line_of_bytes(const u8 *data, size_t at)
{
    size_t line = 0U;

    for (size_t i = 0U; i < at; i++)
        line += data[i] == (u8)'\n';
    return line;
}

static size_t count_lfs(const u8 *data, size_t len)
{
    size_t lines = 0U;

    for (size_t i = 0U; i < len; i++)
        lines += data[i] == (u8)'\n';
    return lines;
}

static size_t configured_edits(void)
{
    const char *text = getenv("YEW_SYN_FUZZ_EDIT_OPS");
    char *end;
    unsigned long value;

    if (text == NULL || *text == '\0')
        return SYN_FUZZ_DEFAULT_EDITS;
    value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0UL)
        return SYN_FUZZ_DEFAULT_EDITS;
    return value > 100000UL ? 100000U : (size_t)value;
}

static bool check_edits(SynFixture *fx, Rng *rng, const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    u8 bytes[SYN_FUZZ_EDIT_BYTES];
    size_t used = len < sizeof(bytes) ? len : sizeof(bytes);
    TextBuf *tb;
    SynBuf incremental;
    SynBuf fresh;
    size_t edits = configured_edits();
    bool ok = true;

    if (used != 0U)
        (void)memcpy(bytes, data, used);
    tb = yew_textbuf_from_bytes(bytes, used);
    if (tb == NULL) {
        (void)snprintf(why, why_cap, "cannot allocate edit buffer");
        return false;
    }
    yew_syn_buf_init(&incremental);
    yew_syn_buf_bind(&incremental, fx->engine);
    yew_syn_attach(&incremental, 1U, tb);
    yew_syn_buf_init(&fresh);
    yew_syn_buf_bind(&fresh, fx->engine);
    ok = settle_all(&incremental, tb, why, why_cap);
    for (size_t op = 0U; ok && op < edits; op++) {
        bool insert = used == 0U ||
                      (used < sizeof(bytes) && choose(rng, 2U) == 0U);

        if (insert) {
            u8 add[8];
            size_t at = choose(rng, used + 1U);
            size_t n = 1U + choose(rng, sizeof(add));
            size_t line = line_of_bytes(bytes, at);

            if (n > sizeof(bytes) - used)
                n = sizeof(bytes) - used;
            for (size_t i = 0U; i < n; i++)
                add[i] = (u8)rng_next(rng);
            yew_textbuf_insert(tb, BYTEOFF(at), add, n);
            (void)memmove(bytes + at + n, bytes + at, used - at);
            (void)memcpy(bytes + at, add, n);
            used += n;
            yew_syn_edit(&incremental, LINENO(line), 0U,
                         count_lfs(add, n));
        } else {
            size_t at = choose(rng, used);
            size_t n = 1U + choose(rng, used - at);
            size_t line = line_of_bytes(bytes, at);
            size_t removed = count_lfs(bytes + at, n);

            yew_textbuf_delete(tb, (Span){at, at + n});
            (void)memmove(bytes + at, bytes + at + n, used - at - n);
            used -= n;
            yew_syn_edit(&incremental, LINENO(line), removed, 0U);
        }
        ok = incremental.entry.len == yew_textbuf_line_count(tb);
        if (!ok) {
            (void)snprintf(why, why_cap,
                           "entry count %zu differs from line count %llu",
                           incremental.entry.len,
                           (unsigned long long)yew_textbuf_line_count(tb));
            break;
        }
        ok = settle_all(&incremental, tb, why, why_cap) &&
             compare_fresh(&incremental, &fresh, tb, why, why_cap);
        if (!ok) {
            char detail[192];

            (void)snprintf(detail, sizeof(detail), "%s", why);
            (void)snprintf(why, why_cap, "edit %zu: %s", op, detail);
        }
    }
    yew_syn_detach(&fresh);
    yew_syn_detach(&incremental);
    yew_textbuf_free(tb);
    return ok;
}

static bool check_syn(const u8 *data, size_t len, char *why, size_t why_cap)
{
    SynFixture fx;
    Rng rng = {seed_of(data, len)};
    bool forced_edits = getenv("YEW_SYN_FUZZ_EDIT_OPS") != NULL;
    bool ok;

    if (!fixture_init(&fx)) {
        (void)snprintf(why, why_cap, "cannot initialize toy definition");
        return false;
    }
    ok = check_line(&fx, &rng, data, len, why, why_cap);
    /* One input in 32 also drives the expensive incremental oracle.  A
     * normal replay stays quick; long campaigns still accumulate far more
     * than the Sprint 39 requirement's 100k random edit operations. */
    if (ok && (forced_edits || (rng.state & 31U) == 0U))
        ok = check_edits(&fx, &rng, data, len, why, why_cap);
    fixture_free(&fx);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_syn",
                         "tests/fuzz/corpus/syn", check_syn);
}
