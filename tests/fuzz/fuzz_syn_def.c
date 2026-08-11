/* Sprint 40: syntax-definition compiler and compiled-table fuzzer. */
#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "fl/diag.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "util/arena.h"

enum {
    SYN_DEF_FUZZ_ARENA_LIMIT = 64U * 1024U * 1024U,
    SYN_DEF_FUZZ_ERROR_LIMIT = 256U,
    SYN_DEF_FUZZ_LINE_LIMIT = 64U * 1024U
};

typedef struct DiagCount {
    u32 errors;
    u32 warnings;
} DiagCount;

static void count_diag(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    DiagCount *count = ctx;

    (void)sp;
    (void)msg;
    (void)rendered;
    if (level == FL_DIAG_ERROR)
        count->errors++;
    else if (level == FL_DIAG_WARNING)
        count->warnings++;
}

static bool check_tables(const SynDef *def, char *why, size_t why_cap)
{
    u32 bad_rule;
    u8 bad_byte;

    if (def->name == NULL || def->nctxs == 0U || def->root >= def->nctxs ||
        def->ctxs == NULL || (def->nrules != 0U && def->rules == NULL) ||
        def->aux == NULL) {
        (void)snprintf(why, why_cap, "invalid definition header");
        return false;
    }
    for (u32 i = 0U; i < def->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];

        if (ctx->first_rule > def->nrules ||
            ctx->nrules > def->nrules - ctx->first_rule ||
            ctx->dflt_attr >= YEW_ATTR__COUNT || ctx->at_eol > SYN_OP_SET ||
            ctx->eol_nop > 4U ||
            (ctx->at_eol == SYN_OP_SET && ctx->eol_target >= def->nctxs)) {
            (void)snprintf(why, why_cap, "invalid context %u",
                           (unsigned)i);
            return false;
        }
    }
    for (u32 i = 0U; i < def->nrules; i++) {
        const SynRule *rule = &def->rules[i];

        if (rule->attr >= YEW_ATTR__COUNT || rule->op > SYN_OP_SET ||
            rule->nop > 4U || rule->npush > 4U ||
            rule->aux_match > SYN_AUXM_LINE_START ||
            rule->value_pred > SYN_VALUE_SET ||
            ((rule->op == SYN_OP_PUSH || rule->op == SYN_OP_SET) &&
             rule->target >= def->nctxs)) {
            (void)snprintf(why, why_cap, "invalid rule %u",
                           (unsigned)i);
            return false;
        }
        for (u8 j = 0U; j < rule->npush; j++) {
            if (rule->push[j] >= def->nctxs) {
                (void)snprintf(why, why_cap,
                               "rule %u push target is out of range",
                               (unsigned)i);
                return false;
            }
        }
    }
    if (!yew_syn_def_firstbyte_check(def, &bad_rule, &bad_byte)) {
        (void)snprintf(why, why_cap,
                       "rule %u omits matching first byte 0x%02x",
                       (unsigned)bad_rule, (unsigned)bad_byte);
        return false;
    }
    return true;
}

static bool check_engine(SynDef *def, const u8 *data, size_t len,
                         char *why, size_t why_cap)
{
    SynSpan spans[YEW_SYN_MAX_SPANS];
    SynLineOut out = {spans, 0U, YEW_SYN_MAX_SPANS, 0U, 0U};
    SynEngine *engine;
    const SynState *exit;
    u64 prior_end = 0U;

    if (len > SYN_DEF_FUZZ_LINE_LIMIT)
        len = SYN_DEF_FUZZ_LINE_LIMIT;
    engine = yew_syn_engine_new(def);
    if (engine == NULL) {
        (void)snprintf(why, why_cap, "engine rejected compiled definition");
        return false;
    }
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, data, (u32)len, &out);
    if (out.n > out.cap) {
        (void)snprintf(why, why_cap, "span count exceeds output capacity");
        yew_syn_engine_free(engine);
        return false;
    }
    for (u32 i = 0U; i < out.n; i++) {
        u64 end = (u64)out.spans[i].start + out.spans[i].len;

        if (out.spans[i].len == 0U || out.spans[i].start < prior_end ||
            end > len || out.spans[i].attr >= YEW_ATTR__COUNT) {
            (void)snprintf(why, why_cap, "invalid emitted span %u",
                           (unsigned)i);
            yew_syn_engine_free(engine);
            return false;
        }
        prior_end = end;
    }
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    if (exit == NULL || exit->depth == 0U ||
        exit->depth > YEW_SYN_DEPTH_MAX) {
        (void)snprintf(why, why_cap, "invalid emitted exit state");
        yew_syn_engine_free(engine);
        return false;
    }
    yew_syn_engine_free(engine);
    return true;
}

static bool compile_one(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Arena arena;
    DiagCtx dc;
    DiagCount count = {0U, 0U};
    SynDef *def;
    u32 nerr = 0U;
    u32 nwarn = 0U;
    bool ok = true;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    (void)fl_diag_add_file(&dc, "fuzz-syntax.fl", (const char *)data, len);
    fl_diag_set_sink(&dc, count_diag, &count);
    def = yew_syn_def_compile(&arena, &dc, data, len, 0U, &nerr, &nwarn);
    if (nerr > SYN_DEF_FUZZ_ERROR_LIMIT ||
        count.errors > SYN_DEF_FUZZ_ERROR_LIMIT) {
        (void)snprintf(why, why_cap, "diagnostic count is unbounded: %u/%u",
                       (unsigned)nerr, (unsigned)count.errors);
        ok = false;
    } else if ((def == NULL) != (nerr != 0U)) {
        (void)snprintf(why, why_cap,
                       "compiler result disagrees with error count");
        ok = false;
    } else if (def != NULL && !check_tables(def, why, why_cap)) {
        ok = false;
    } else if (def != NULL && !check_engine(def, data, len, why, why_cap)) {
        ok = false;
    } else if (arena.next_block_size > SYN_DEF_FUZZ_ARENA_LIMIT) {
        (void)snprintf(why, why_cap,
                       "arena grew past 64 MiB on %zu bytes", len);
        ok = false;
    }
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&arena);
    (void)nwarn;
    (void)count.warnings;
    return ok;
}

static bool check_syn_def(const u8 *data, size_t len, char *why,
                          size_t why_cap)
{
    static const char *const patterns[] = {
        "[A-Za-z_][A-Za-z0-9_]*", "[0-9]+", "\\\\.",
        "[;#].*$", "[:=]", "\\S+"
    };
    static const char *const attrs[] = {
        "text", "keyword", "number", "string.escape", "comment", "error"
    };
    char plausible[1024];
    unsigned selector = len == 0U ? 0U : data[0];
    int wrote;

    if (!compile_one(data, len, why, why_cap))
        return false;
    wrote = snprintf(plausible, sizeof(plausible),
        "{ syntax: 1, language: { name: \"fuzz-%u\", extensions: "
        "[\"fz\"], priority: %u }, contexts: { main: { default: \"text\", "
        "rules: [ { match: \"%s\", attr: \"%s\" }, "
        "{ match: \"\\\"\", attr: \"string\", push: \"quoted\" } ] }, "
        "quoted: { default: \"string\", at_eol: \"pop\", unit: \"atom\", "
        "rules: [ { match: \"\\\\.\", attr: \"string.escape\" }, "
        "{ match: \"\\\"\", attr: \"string\", pop: 1 } ] } } }",
        selector, selector & 7U,
        patterns[selector % YEW_ARRAY_LEN(patterns)],
        attrs[(selector / 7U) % YEW_ARRAY_LEN(attrs)]);
    if (wrote < 0 || (size_t)wrote >= sizeof(plausible)) {
        (void)snprintf(why, why_cap, "plausible fixture overflow");
        return false;
    }
    return compile_one((const u8 *)plausible, (size_t)wrote, why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_syn_def",
                         "tests/fuzz/corpus/syn_def", check_syn_def);
}
