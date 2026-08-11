#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syn/defs.h"
#include "syn/engine.h"
#include "syn/langs_gen.h"
#include "text/piece.h"

typedef struct RuntimeFix {
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    SynEngine *engine;
} RuntimeFix;

static void runtime_open(RuntimeFix *fix, const char *src)
{
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 file;

    (void)memset(fix, 0, sizeof(*fix));
    arena_init(&fix->arena);
    fl_diag_init(&fix->dc, &fix->arena);
    file = fl_diag_add_file(&fix->dc, "embed-runtime.fl", src, strlen(src));
    fix->def = yew_syn_def_compile(&fix->arena, &fix->dc,
                                   (const u8 *)src, strlen(src), file,
                                   &errors, &warnings);
    YEW_ASSERT_NOT_NULL(fix->def);
    YEW_ASSERT_EQ_U64(errors, 0U);
    YEW_ASSERT_EQ_U64(warnings, 0U);
    fix->engine = yew_syn_engine_new(fix->def);
}

static void runtime_close(RuntimeFix *fix)
{
    yew_syn_engine_free(fix->engine);
    yew_syn_def_dispose(fix->def);
    arena_free_all(&fix->arena);
}

static u8 runtime_attr_at(const SynLineOut *out, u32 at)
{
    u32 i;

    for (i = 0U; i < out->n; i++) {
        if (out->spans[i].start <= at &&
            at < out->spans[i].start + out->spans[i].len)
            return out->spans[i].attr;
    }
    return UINT8_MAX;
}

void test_syn_embed_runtime_self_inline_exit_precedes_guest(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-self\"},contexts:{"
        "main:{rules:[{match:\"BEGIN\",push:\"bridge\",embed:{lang:\"@self\",end:\"inline\",fallback:\"code\"}},{match:\"x\",attr:\"keyword\"}]},"
        "bridge:{default:\"comment\",rules:[{match:\"END\",attr:\"punct\",pop:1,end:true}]}}}";
    RuntimeFix fix;
    SynSpan spans[32];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *exit;

    runtime_open(&fix, src);
    yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"BEGINxENDz", 10U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                             out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(exit->ndef, 1U);
    YEW_ASSERT_EQ_U64(runtime_attr_at(&out, 5U), YEW_ATTR_KEYWORD);
    YEW_ASSERT_EQ_U64(runtime_attr_at(&out, 6U), YEW_ATTR_PUNCT);
    runtime_close(&fix);
}

void test_syn_embed_runtime_deferred_self_enters_at_eol(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-defer\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"@self\",end:\"line\",defer:true,fallback:\"code\"}},{match:\"x\",attr:\"keyword\"}]},"
        "bridge:{default:\"comment\",rules:[{match:\"END\",attr:\"punct\",pop:1,end:true}]}}}";
    RuntimeFix fix;
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *state;

    runtime_open(&fix, src);
    yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT, (const u8 *)"OPEN", 4U,
                 &out);
    state = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                              out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 3U);
    YEW_ASSERT_EQ_U64(state->ndef, 2U);
    YEW_ASSERT_EQ_U64(state->f[1].fl, YEW_SYN_FR_BRIDGE);
    out.n = 0U;
    yew_syn_line(fix.engine, out.exit_state, (const u8 *)"END", 3U, &out);
    state = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                              out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 1U);
    YEW_ASSERT_EQ_U64(state->ndef, 1U);
    YEW_ASSERT_EQ_U64(state->aux[1], 0U);
    runtime_close(&fix);
}

void test_syn_embed_runtime_unknown_uses_fallback_and_balances(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-unknown\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"not-installed\",end:\"inline\",fallback:\"code\"}}]},"
        "bridge:{default:\"comment\",rules:[{match:\"END\",attr:\"punct\",pop:1,end:true}]}}}";
    RuntimeFix fix;
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *exit;
    SynBuf syn;
    TextBuf *tb;
    SynSettleReport report;

    runtime_open(&fix, src);
    yew_test_capture_log();
    yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"OPENbodyEND", 11U, &out);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 0U);
    exit = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                             out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(runtime_attr_at(&out, 4U), YEW_ATTR_CODE);
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(exit->ndef, 1U);
    YEW_ASSERT((exit->flags & YEW_SYN_F_EMBED_PEND) == 0U);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fix.engine);
    tb = yew_textbuf_from_bytes(
        (const u8 *)"OPENbodyEND\nOPENbodyEND\n", 24U);
    YEW_ASSERT_NOT_NULL(tb);
    yew_syn_attach(&syn, 1U, tb);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(2U), INT64_MAX, &report);
    YEW_ASSERT(report.fixpoint);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "not-installed' is unavailable"));
    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    runtime_close(&fix);
}

void test_syn_embed_runtime_line_host_eol_returns_after_guest(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-line\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"@self\",end:\"line\",fallback:\"code\"}}]},"
        "bridge:{at_eol:\"pop\",rules:[]}}}";
    RuntimeFix fix;
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *exit;

    runtime_open(&fix, src);
    yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"OPENrecipe", 10U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                             out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(exit->ndef, 1U);
    runtime_close(&fix);
}

void test_syn_embed_runtime_zero_headroom_records_protected_lost_debt(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-full\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"@self\",end:\"inline\",fallback:\"code\"}},"
        "{match:\"POP\",pop:1}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    RuntimeFix fix;
    SynState full = {0};
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynState trace[8];
    const SynState *exit;
    SynBuf syn;
    TextBuf *tb;
    SynSettleReport report;
    char status[256];
    u32 entry;
    u8 i;

    runtime_open(&fix, src);
    full.depth = YEW_SYN_DEPTH_MAX;
    full.ndef = 1U;
    for (i = 0U; i < full.depth; i++)
        full.f[i] = (SynFrame){0U, 0U, 0U};
    entry = yew_syn_state_intern(yew_syn_engine_states(fix.engine), &full);
    YEW_ASSERT(yew_syn_stack_trace(fix.engine, entry,
                                   (const u8 *)"OPENPOP", 7U, trace,
                                   YEW_ARRAY_LEN(trace)));
    YEW_ASSERT_EQ_U64(trace[4].depth, YEW_SYN_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(trace[4].lost, 2U);
    YEW_ASSERT((trace[4].flags & YEW_SYN_F_EMBED_ORPHAN) != 0U);
    YEW_ASSERT_EQ_U64(trace[7].depth, YEW_SYN_DEPTH_MAX - 1U);
    YEW_ASSERT_EQ_U64(trace[7].lost, 2U);
    yew_syn_line(fix.engine, entry, (const u8 *)"OPENPOP", 7U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                             out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(exit->depth, YEW_SYN_DEPTH_MAX - 1U);
    YEW_ASSERT_EQ_U64(exit->lost, 0U);
    YEW_ASSERT((exit->flags & (YEW_SYN_F_EMBED_LOST |
                              YEW_SYN_F_EMBED_ORPHAN)) == 0U);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fix.engine);
    tb = yew_textbuf_from_bytes((const u8 *)"OPEN\n", 5U);
    YEW_ASSERT_NOT_NULL(tb);
    yew_syn_attach(&syn, 1U, tb);
    syn.entry.data[0] = entry;
    syn.settled_to = LINENO(0U);
    syn.wave = LINENO(0U);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(1U), INT64_MAX, &report);
    YEW_ASSERT(report.fixpoint);
    YEW_ASSERT(syn.degraded);
    YEW_ASSERT(syn.embed_refused != YEW_LANG_NONE);
    yew_syn_status(&syn, 1U, status, sizeof(status));
    YEW_ASSERT(strstr(status, "degraded=yes") != NULL);
    YEW_ASSERT(strstr(status, "embed_refused=runtime-full") != NULL);
    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    runtime_close(&fix);
}

void test_syn_embed_runtime_merged_first_mask_covers_all_bytes(void)
{
    static const struct {
        const char *name;
        const char *path;
        u32 sites;
        u32 compiled;
    } inventory[] = {
        {"css", "runtime/syntax/css.fl", 3U, 5U},
        {"html", "runtime/syntax/html.fl", 11U, 11U},
        {"javascript", "runtime/syntax/javascript.fl", 4U, 16U},
        {"make", "runtime/syntax/make.fl", 2U, 2U},
        {"markdown", "runtime/syntax/markdown.fl", 1U, 2U},
        {"sh", "runtime/syntax/sh.fl", 4U, 10U},
        {"typescript", "runtime/syntax/typescript.fl", 4U, 16U}
    };
    u32 compiled_sites = 0U;
    u32 source_sites = 0U;
    u32 checked_sites = 0U;
    bool mutate = getenv("YEW_SYN_TEST_NARROW_EMBED_MASK") != NULL;
    bool mutated = false;

    /* The explicit source inventory is the grep gate: adding an embed site
     * to any shipped definition changes the source count and fails here
     * until that site joins the 256-byte matrix below. */
    YEW_ASSERT_EQ_U64(yew_syn_builtin_langs_len, 19U);
    for (u32 file_index = 0U; file_index < YEW_ARRAY_LEN(inventory);
         file_index++) {
        const SynDef *host = yew_syn_def_for(
            yew_syn_lang_named(inventory[file_index].name));
        FILE *file = fopen(inventory[file_index].path, "rb");
        char source[32768];
        size_t nread;
        u32 in_source = 0U;
        u32 before = compiled_sites;
        const char *at;

        YEW_ASSERT_NOT_NULL(host);
        YEW_ASSERT_NOT_NULL(file);
        nread = fread(source, 1U, sizeof(source) - 1U, file);
        YEW_ASSERT(!ferror(file));
        YEW_ASSERT_EQ_I64(fclose(file), 0);
        source[nread] = '\0';
        at = source;
        while ((at = strstr(at, "embed:")) != NULL) {
            in_source++;
            at += sizeof("embed:") - 1U;
        }
        YEW_ASSERT_EQ_U64(in_source, inventory[file_index].sites);
        source_sites += in_source;

        for (u32 rule_index = 0U; rule_index < host->nrules;
             rule_index++) {
            const SynRule *opener = &host->rules[rule_index];
            const SynCtx *bridge;
            const SynDef *guest[20];
            u32 nguests = 0U;

            if (opener->op != SYN_OP_EMBED)
                continue;
            compiled_sites++;
            YEW_ASSERT(opener->target < host->nctxs);
            bridge = &host->ctxs[opener->target];
            YEW_ASSERT((bridge->flags & YEW_SYN_CTX_EMBED_BRIDGE) != 0U);
            if (opener->embed.lang_kind == SYN_EMBED_LANG_SELF) {
                guest[nguests++] = host;
            } else if (opener->embed.lang_kind == SYN_EMBED_LANG_LITERAL) {
                const char *name = yew_intern_str(host->aux,
                                                  opener->embed.lang);
                u32 lang = name == NULL ? YEW_LANG_NONE :
                           yew_syn_lang_named(name);

                /* Unknown literals use the bridge fallback as the active
                 * context; model that path with no separate guest. */
                guest[nguests++] = lang == YEW_LANG_NONE ? NULL :
                                    yew_syn_def_for(lang);
            } else {
                YEW_ASSERT_EQ_U64(opener->embed.lang_kind,
                                  SYN_EMBED_LANG_CAPTURE);
                for (size_t lang_index = 0U;
                     lang_index < yew_syn_builtin_langs_len; lang_index++)
                    guest[nguests++] = yew_syn_def_for(
                        yew_syn_builtin_langs[lang_index].id);
            }

            for (u32 guest_index = 0U; guest_index < nguests;
                 guest_index++) {
                const SynDef *active = guest[guest_index];
                const SynDef *active_def = active == NULL ? host : active;
                const SynCtx *active_ctx = bridge;
                SynEngine *master = yew_syn_engine_new((SynDef *)host);
                SynEngine *runtime = master;
                SynState state = {0};
                u8 expected_active[32] = {0};
                u8 expected_bridge[32] = {0};
                u8 merged_bol[32];
                u8 bridge_bol[32];
                u8 merged_nonbol[32];
                u8 bridge_nonbol[32];
                u8 active_slot = 0U;

                YEW_ASSERT_NOT_NULL(master);
                if (active != NULL) {
                    u16 ctx_id = active->root;

                    if (opener->embed.ctx != 0U) {
                        const char *want = yew_intern_str(host->aux,
                                                         opener->embed.ctx);

                        for (u16 candidate = 0U; candidate < active->nctxs;
                             candidate++) {
                            const char *name = yew_syn_ctx_name(active,
                                                                candidate);

                            if (name != NULL && want != NULL &&
                                strcmp(name, want) == 0) {
                                ctx_id = candidate;
                                break;
                            }
                        }
                    }
                    active_ctx = &active->ctxs[ctx_id];
                    if (active != host) {
                        u32 lang = yew_syn_lang_named(active->name);

                        runtime = yew_syn_engine_new((SynDef *)active);
                        YEW_ASSERT_NOT_NULL(runtime);
                        YEW_ASSERT(yew_syn_engine_test_install_resident(
                            master, lang, runtime));
                        active_slot = 1U;
                    }
                }
                for (u32 i = 0U; i < active_ctx->nrules; i++) {
                    u8 first[32];
                    const SynRule *rule = &active_def->rules[
                        active_ctx->first_rule + i];

                    yew_re_first_bytes(rule->re, first);
                    for (u32 octet = 0U;
                         octet < sizeof(expected_active); octet++)
                        expected_active[octet] |= first[octet];
                }
                for (u32 i = 0U; i < bridge->nrules; i++) {
                    const SynRule *rule = &host->rules[
                        bridge->first_rule + i];
                    u8 first[32];

                    if (rule->end == 0U)
                        continue;
                    if (rule->aux_match != SYN_AUXM_NONE)
                        (void)memset(first, 0xff, sizeof(first));
                    else
                        yew_re_first_bytes(rule->re, first);
                    for (u32 octet = 0U;
                         octet < sizeof(expected_bridge); octet++)
                        expected_bridge[octet] |= first[octet];
                }
                state.depth = active == NULL ? 1U : 2U;
                state.ndef = active == NULL ? 1U : 2U;
                state.f[0] = (SynFrame){opener->target, 0U,
                                        YEW_SYN_FR_BRIDGE};
                if (active != NULL) {
                    state.f[1] = (SynFrame){
                        (u16)(active_ctx - active_def->ctxs), active_slot, 0U
                    };
                }
                YEW_ASSERT(yew_syn_engine_test_masks(
                    master, &state, true, merged_bol, bridge_bol));
                YEW_ASSERT(yew_syn_engine_test_masks(
                    master, &state, false, merged_nonbol,
                    bridge_nonbol));
                if (mutate && !mutated) {
                    for (u32 byte = 0U; byte <= UINT8_MAX; byte++) {
                        u8 bit = (u8)(1U << (byte & 7U));

                        if ((expected_bridge[byte >> 3U] & bit) == 0U)
                            continue;
                        YEW_ASSERT(yew_syn_engine_test_narrow_mask(
                            master, &state, true, (u8)byte));
                        YEW_ASSERT(yew_syn_engine_test_masks(
                            master, &state, true, merged_bol,
                            bridge_bol));
                        mutated = true;
                        break;
                    }
                }
                for (u32 byte = 0U; byte <= UINT8_MAX; byte++) {
                    u8 bit = (u8)(1U << (byte & 7U));
                    bool want_active =
                        (expected_active[byte >> 3U] & bit) != 0U;
                    bool want_bridge =
                        (expected_bridge[byte >> 3U] & bit) != 0U;
                    bool inline_end =
                        bridge->embed.end == SYN_EMBED_END_INLINE ||
                        bridge->embed.end == SYN_EMBED_END_INLINE_ROOT;

                    if (want_active)
                        YEW_ASSERT((merged_bol[byte >> 3U] & bit) != 0U);
                    if (want_bridge) {
                        YEW_ASSERT((bridge_bol[byte >> 3U] & bit) != 0U);
                        if (inline_end) {
                            YEW_ASSERT((merged_bol[byte >> 3U] & bit) !=
                                       0U);
                            YEW_ASSERT((merged_nonbol[byte >> 3U] & bit) !=
                                       0U);
                            YEW_ASSERT((bridge_nonbol[byte >> 3U] & bit) !=
                                       0U);
                        } else {
                            YEW_ASSERT((bridge_nonbol[byte >> 3U] & bit) ==
                                       0U);
                        }
                    }
                }
                if (active != NULL && state.depth < YEW_SYN_DEPTH_MAX) {
                    SynState nested = state;
                    u8 nested_merged[32];
                    u8 nested_bridge[32];

                    nested.f[nested.depth] = nested.f[nested.depth - 1U];
                    nested.depth++;
                    YEW_ASSERT(yew_syn_engine_test_masks(
                        master, &nested, true, nested_merged,
                        nested_bridge));
                    for (u32 byte = 0U; byte <= UINT8_MAX; byte++) {
                        u8 bit = (u8)(1U << (byte & 7U));
                        bool want_bridge =
                            (expected_bridge[byte >> 3U] & bit) != 0U;

                        if (!want_bridge)
                            continue;
                        if (bridge->embed.end == SYN_EMBED_END_INLINE_ROOT)
                            YEW_ASSERT((nested_bridge[byte >> 3U] & bit) ==
                                       0U);
                        else
                            YEW_ASSERT((nested_bridge[byte >> 3U] & bit) !=
                                       0U);
                    }
                }
                yew_syn_engine_free(master);
                if (runtime != master)
                    yew_syn_engine_free(runtime);
            }
            checked_sites++;
        }
        YEW_ASSERT_EQ_U64(compiled_sites - before,
                          inventory[file_index].compiled);
        YEW_ASSERT_EQ_U64(checked_sites, compiled_sites);
    }
    YEW_ASSERT_EQ_U64(source_sites, 29U);
    YEW_ASSERT_EQ_U64(compiled_sites, 62U);
    YEW_ASSERT_EQ_U64(checked_sites, compiled_sites);
    YEW_ASSERT(!mutate || mutated);
}

void test_syn_embed_runtime_merged_mask_mutation_fails(void)
{
    pid_t pid = fork();
    int status;

    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        const char *program = yew_test_program_path();

        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0)
            _exit(126);
        (void)close(null_fd);
        if (setenv("YEW_SYN_TEST_NARROW_EMBED_MASK", "1", 1) != 0)
            _exit(126);
        execl(program, program, "--filter",
              "syn_embed_runtime_merged_first_mask_covers_all_bytes",
              (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            YEW_ASSERT(false);
    }
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 1);
}

void test_syn_embed_runtime_coverage_uses_resident_definition_and_site(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-coverage\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"css\",end:\"inline\"}}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    RuntimeFix fix;
    SynBuf syn;
    TextBuf *tb;
    SynSettleReport report;
    SynCoverage host_coverage;
    SynCoverage guest_coverage;
    SynEngine *guest;
    const SynDef *guest_def;
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    u32 css = yew_syn_lang_by_name((const u8 *)"css", 3U);

    runtime_open(&fix, src);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, fix.engine);
    tb = yew_textbuf_from_bytes((const u8 *)"OPENxEND\n", 9U);
    YEW_ASSERT_NOT_NULL(tb);
    yew_syn_attach(&syn, 1U, tb);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(1U), INT64_MAX, &report);
    YEW_ASSERT(yew_syn_embed_pump(&syn, fix.engine,
                                  YEW_SYN_EMBED_LOAD_BUDGET_US));
    guest = yew_syn_engine_for(css);
    guest_def = yew_syn_def_resident(fix.engine, css);
    YEW_ASSERT_NOT_NULL(guest);
    YEW_ASSERT_NOT_NULL(guest_def);
    YEW_ASSERT(yew_syn_coverage_init(&host_coverage, fix.def));
    YEW_ASSERT(yew_syn_coverage_init(&guest_coverage, guest_def));
    yew_syn_engine_set_coverage(fix.engine, &host_coverage);
    yew_syn_engine_set_coverage(guest, &guest_coverage);
    yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"OPENxEND", 8U, &out);
    YEW_ASSERT_EQ_U64(host_coverage.contexts[fix.def->root], 1U);
    YEW_ASSERT_EQ_U64(host_coverage.contexts[1], 1U);
    YEW_ASSERT_EQ_U64(host_coverage.embeds[1], 1U);
    YEW_ASSERT(guest_coverage.contexts[guest_def->root] >= 1U);
    yew_syn_engine_set_coverage(guest, NULL);
    yew_syn_engine_set_coverage(fix.engine, NULL);
    yew_syn_coverage_free(&guest_coverage);
    yew_syn_coverage_free(&host_coverage);
    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    runtime_close(&fix);
}

void test_syn_embed_runtime_inline_root_defers_outer_exit_until_guest_root(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-inline-root\"},contexts:{"
        "main:{rules:["
        "{match:\"OPEN\",push:\"bridge\",embed:{lang:\"@self\",end:\"inline-root\"}},"
        "{match:\"[{]\",push:\"nested\"}]},"
        "nested:{rules:[{match:\"[}]\",pop:1}]},"
        "bridge:{rules:[{match:\"[}]\",pop:1,end:true}]}}}";
    RuntimeFix fix;
    SynState trace[9];

    runtime_open(&fix, src);
    YEW_ASSERT_EQ_U64(fix.def->ctxs[2].embed.end,
                      SYN_EMBED_END_INLINE_ROOT);
    YEW_ASSERT(yew_syn_stack_trace(fix.engine, YEW_SYN_STATE_ROOT,
                                   (const u8 *)"OPEN{x}}", 8U, trace,
                                   YEW_ARRAY_LEN(trace)));
    YEW_ASSERT_EQ_U64(trace[7].depth, 3U);
    YEW_ASSERT_EQ_U64(trace[7].ndef, 2U);
    YEW_ASSERT_EQ_U64(trace[8].depth, 1U);
    YEW_ASSERT_EQ_U64(trace[8].ndef, 1U);
    runtime_close(&fix);
}

void test_syn_embed_runtime_line_continuation_keeps_unprefixed_guest(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"runtime-line-cont\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"@self\",end:\"line-continuation\"}}]},"
        "bridge:{at_eol:\"pop\",rules:[]}}}";
    RuntimeFix fix;
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *state;

    runtime_open(&fix, src);
    YEW_ASSERT_EQ_U64(fix.def->ctxs[1].embed.end,
                      SYN_EMBED_END_LINE_CONTINUATION);
    yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"OPENecho \\", 10U, &out);
    state = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                              out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 3U);
    YEW_ASSERT_EQ_U64(state->ndef, 2U);
    yew_syn_line(fix.engine, out.exit_state,
                 (const u8 *)"unprefixed \\", 12U, &out);
    state = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                              out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 3U);
    YEW_ASSERT_EQ_U64(state->ndef, 2U);
    yew_syn_line(fix.engine, out.exit_state, (const u8 *)"done", 4U, &out);
    state = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                              out.exit_state);
    YEW_ASSERT_NOT_NULL(state);
    YEW_ASSERT_EQ_U64(state->depth, 1U);
    YEW_ASSERT_EQ_U64(state->ndef, 1U);
    {
        static const u8 even[] = {
            'O', 'P', 'E', 'N', 'e', 'c', 'h', 'o', ' ', '\\', '\\'
        };

        yew_syn_line(fix.engine, YEW_SYN_STATE_ROOT, even,
                     YEW_ARRAY_LEN(even), &out);
        state = yew_syn_state_get(yew_syn_engine_states(fix.engine),
                                  out.exit_state);
        YEW_ASSERT_NOT_NULL(state);
        YEW_ASSERT_EQ_U64(state->depth, 1U);
        YEW_ASSERT_EQ_U64(state->ndef, 1U);
    }
    runtime_close(&fix);
}
