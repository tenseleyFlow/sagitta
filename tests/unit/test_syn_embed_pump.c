#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "syn/defs.h"
#include "syn/engine.h"
#include "text/piece.h"

void test_syn_embed_pump_is_idle_only_and_loads_one(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"pump-host\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"javascript\",end:\"inline\",fallback:\"code\"}}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    SynEngine *engine;
    SynBuf syn;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *pending;
    const SynState *resident;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 file;
    u32 js = yew_syn_lang_by_name((const u8 *)"javascript", 10U);
    char status[256];

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    file = fl_diag_add_file(&dc, "pump.fl", src, strlen(src));
    def = yew_syn_def_compile(&arena, &dc, (const u8 *)src, strlen(src),
                              file, &errors, &warnings);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(errors, 0U);
    YEW_ASSERT_EQ_U64(warnings, 0U);
    YEW_ASSERT(js != YEW_LANG_NONE);
    engine = yew_syn_engine_new(def);
    YEW_ASSERT_NULL(yew_syn_def_resident(engine, js));

    yew_syn_compile_count_reset();
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"OPENbody", 8U,
                 &out);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    pending = yew_syn_state_get(yew_syn_engine_states(engine),
                                out.exit_state);
    YEW_ASSERT_NOT_NULL(pending);
    YEW_ASSERT_EQ_U64(pending->depth, 2U);
    YEW_ASSERT_EQ_U64(pending->ndef, 1U);
    YEW_ASSERT((pending->flags & YEW_SYN_F_EMBED_PEND) != 0U);
    YEW_ASSERT_EQ_U64(pending->aux[pending->ndef], js);

    yew_syn_buf_init(&syn);
    syn.engine = engine;
    syn.lang = 1U;
    syn.entry.data = malloc(2U * sizeof(*syn.entry.data));
    YEW_ASSERT_NOT_NULL(syn.entry.data);
    syn.entry.len = 2U;
    syn.entry.cap = 2U;
    syn.entry.data[0] = YEW_SYN_STATE_ROOT;
    syn.entry.data[1] = out.exit_state;
    syn.wave = LINENO(1U);
    syn.settled_to = LINENO(2U);

    YEW_ASSERT(!yew_syn_embed_pump(&syn, engine,
                                   YEW_SYN_FRAME_BUDGET_US));
    YEW_ASSERT_NULL(yew_syn_def_resident(engine, js));
    YEW_ASSERT(yew_syn_embed_pump(&syn, engine,
                                  YEW_SYN_EMBED_LOAD_BUDGET_US));
    YEW_ASSERT_NOT_NULL(yew_syn_def_resident(engine, js));
    YEW_ASSERT_EQ_U64(syn.wave.v, 0U);
    YEW_ASSERT_EQ_U64(syn.entry.data[0], YEW_SYN_STATE_ROOT);
    YEW_ASSERT_EQ_U64(syn.entry.data[1], YEW_SYN_STATE_UNKNOWN);
    YEW_ASSERT(!yew_syn_embed_pump(&syn, engine,
                                   YEW_SYN_EMBED_LOAD_BUDGET_US));

    out.n = 0U;
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)"OPENbody", 8U,
                 &out);
    resident = yew_syn_state_get(yew_syn_engine_states(engine),
                                 out.exit_state);
    YEW_ASSERT_NOT_NULL(resident);
    YEW_ASSERT_EQ_U64(resident->ndef, 2U);
    YEW_ASSERT_EQ_U64(resident->depth, 3U);
    YEW_ASSERT_EQ_U64(resident->f[2].def, 1U);
    YEW_ASSERT_NOT_NULL(yew_syn_engine_def_at(engine, 1U));
    YEW_ASSERT_NULL(yew_syn_engine_def_at(engine, YEW_SYN_DEF_MAX));

    syn.entry.data[1] = out.exit_state;
    syn.wave = LINENO(1U);
    yew_syn_status(&syn, 2U, status, sizeof(status));
    YEW_ASSERT(strstr(status, "root=pump-host active=javascript") != NULL);
    YEW_ASSERT(strstr(status, "defs=2/4 depth=3/16") != NULL);
    YEW_ASSERT(strstr(status, "embed_pending=0") != NULL);

    free(syn.entry.data);
    syn.entry.data = NULL;
    syn.entry.len = 0U;
    syn.entry.cap = 0U;
    yew_syn_engine_free(engine);
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
}

void test_syn_lang_by_name_is_length_checked_and_io_free(void)
{
    u32 js = yew_syn_lang_by_name((const u8 *)"javascript", 10U);
    u32 i;

    YEW_ASSERT(js != YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_by_name((const u8 *)"javascript", 9U),
                      YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_by_name((const u8 *)"javascriptx", 11U),
                      YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_by_name(NULL, 0U), YEW_LANG_NONE);
    yew_syn_compile_count_reset();
    for (i = 0U; i < 64U; i++)
        YEW_ASSERT_EQ_U64(
            yew_syn_lang_by_name((const u8 *)"javascript", 10U), js);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
}

void test_syn_embed_pump_keeps_one_line_request_after_balanced_exit(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"pump-inline\"},contexts:{"
        "main:{rules:[{match:\"OPEN\",push:\"bridge\",embed:{lang:\"css\",end:\"inline\",fallback:\"code\"}}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    SynEngine *engine;
    SynBuf syn;
    SynSpan spans[8];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    SynLineOut repeated = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const SynState *exit;
    TextBuf *tb;
    SynSettleReport report;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 css = yew_syn_lang_by_name((const u8 *)"css", 3U);
    u32 file;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    file = fl_diag_add_file(&dc, "pump-inline.fl", src, strlen(src));
    def = yew_syn_def_compile(&arena, &dc, (const u8 *)src, strlen(src),
                              file, &errors, &warnings);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(errors, 0U);
    YEW_ASSERT_EQ_U64(warnings, 0U);
    engine = yew_syn_engine_new(def);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"OPENbodyEND", 11U, &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    YEW_ASSERT_NOT_NULL(exit);
    YEW_ASSERT_EQ_U64(exit->depth, 1U);
    YEW_ASSERT_EQ_U64(exit->ndef, 1U);
    YEW_ASSERT((exit->flags & YEW_SYN_F_EMBED_PEND) == 0U);
    YEW_ASSERT_NULL(yew_syn_def_resident(engine, css));
    yew_syn_line(engine, YEW_SYN_STATE_ROOT,
                 (const u8 *)"OPENbodyEND", 11U, &repeated);
    YEW_ASSERT_EQ_U64(repeated.exit_state, out.exit_state);
    YEW_ASSERT_EQ_U64(repeated.n, out.n);
    YEW_ASSERT_NULL(yew_syn_def_resident(engine, css));

    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, engine);
    tb = yew_textbuf_from_bytes(
        (const u8 *)"OPENbodyEND\nOPENbodyEND\n", 24U);
    YEW_ASSERT_NOT_NULL(tb);
    yew_syn_attach(&syn, 1U, tb);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(2U), INT64_MAX,
                   &report);
    YEW_ASSERT(report.fixpoint);
    YEW_ASSERT_EQ_U64(syn.embed_pending, css);
    YEW_ASSERT_EQ_U64(syn.embed_pending_line.v, 0U);
    YEW_ASSERT_EQ_U64(syn.embed_pending_count, 1U);
    yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
    YEW_ASSERT_EQ_U64(syn.embed_pending, css);
    YEW_ASSERT_EQ_U64(syn.embed_pending_line.v, 0U);
    YEW_ASSERT_EQ_U64(syn.embed_pending_count, 1U);
    YEW_ASSERT(yew_syn_embed_pump(&syn, engine,
                                  YEW_SYN_EMBED_LOAD_BUDGET_US));
    YEW_ASSERT_NOT_NULL(yew_syn_def_resident(engine, css));
    YEW_ASSERT_EQ_U64(syn.wave.v, 0U);
    YEW_ASSERT_EQ_U64(syn.embed_pending, YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(syn.embed_pending_count, 0U);

    yew_syn_detach(&syn);
    yew_textbuf_free(tb);
    yew_syn_engine_free(engine);
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
}

void test_syn_embed_pump_keeps_more_than_three_guests_resident(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"pump-many\"},contexts:{"
        "main:{rules:["
        "{match:\"J\",push:\"bridge\",embed:{lang:\"javascript\",end:\"inline\"}},"
        "{match:\"C\",push:\"bridge\",embed:{lang:\"css\",end:\"inline\"}},"
        "{match:\"S\",push:\"bridge\",embed:{lang:\"sh\",end:\"inline\"}},"
        "{match:\"P\",push:\"bridge\",embed:{lang:\"python\",end:\"inline\"}}]},"
        "bridge:{rules:[{match:\"END\",pop:1,end:true}]}}}";
    static const char *const lines[] = {
        "JxEND\n", "CxEND\n", "SxEND\n", "PxEND\n"
    };
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    SynEngine *engine;
    SynBuf syn;
    u32 errors = 0U;
    u32 warnings = 0U;
    u32 file;
    u32 i;
    char status[256];

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    file = fl_diag_add_file(&dc, "pump-many.fl", src, strlen(src));
    def = yew_syn_def_compile(&arena, &dc, (const u8 *)src, strlen(src),
                              file, &errors, &warnings);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(errors, 0U);
    YEW_ASSERT_EQ_U64(warnings, 0U);
    engine = yew_syn_engine_new(def);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, engine);
    for (i = 0U; i < YEW_ARRAY_LEN(lines); i++) {
        TextBuf *tb = yew_textbuf_from_bytes((const u8 *)lines[i],
                                             strlen(lines[i]));
        SynSettleReport report;

        YEW_ASSERT_NOT_NULL(tb);
        yew_syn_attach(&syn, 1U, tb);
        yew_syn_settle(&syn, tb, LINENO(0U), LINENO(1U), INT64_MAX,
                       &report);
        YEW_ASSERT(report.fixpoint);
        YEW_ASSERT(yew_syn_embed_pump(&syn, engine,
                                      YEW_SYN_EMBED_LOAD_BUDGET_US));
        YEW_ASSERT_NOT_NULL(yew_syn_engine_def_at(engine, (u8)(i + 1U)));
        yew_textbuf_free(tb);
    }
    syn.entry.data[0] = YEW_SYN_STATE_ROOT;
    syn.wave = LINENO(0U);
    yew_syn_status(&syn, 1U, status, sizeof(status));
    YEW_ASSERT(strstr(status, "defs=1/4 depth=1/16") != NULL);
    yew_syn_detach(&syn);
    yew_syn_engine_free(engine);
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
}
