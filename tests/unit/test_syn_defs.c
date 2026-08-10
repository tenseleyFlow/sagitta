#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syn/defs.h"

typedef struct DefDiag {
    FlDiagLevel level[32];
    FlSpan sp[32];
    char msg[32][256];
    u32 n;
} DefDiag;

typedef struct DefFix {
    Arena arena;
    DiagCtx dc;
    DefDiag diag;
} DefFix;

static void def_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                     const char *msg, const char *rendered)
{
    DefDiag *d = ctx;

    (void)rendered;
    if (d->n < YEW_ARRAY_LEN(d->msg)) {
        d->level[d->n] = level;
        d->sp[d->n] = sp;
        (void)snprintf(d->msg[d->n], sizeof(d->msg[d->n]), "%s", msg);
    }
    d->n++;
}

static void def_fix_open(DefFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, def_sink, &f->diag);
}

static void def_fix_close(DefFix *f, SynDef *def)
{
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&f->arena);
}

static SynDef *def_compile(DefFix *f, const char *src, u32 *nerr,
                           u32 *nwarn)
{
    u32 file_id = fl_diag_add_file(&f->dc, "test.fl", src, strlen(src));

    return yew_syn_def_compile(&f->arena, &f->dc, (const u8 *)src,
                               strlen(src), file_id, nerr, nwarn);
}

static char *read_whole(const char *path, size_t *len)
{
    FILE *file = fopen(path, "rb");
    char *src;
    long end;

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_END), 0);
    end = ftell(file);
    YEW_ASSERT(end >= 0L);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    src = malloc((size_t)end + 1U);
    YEW_ASSERT_NOT_NULL(src);
    YEW_ASSERT_EQ_U64(fread(src, 1U, (size_t)end, file), (size_t)end);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    src[end] = '\0';
    *len = (size_t)end;
    return src;
}

static bool diag_has(const DefFix *f, FlDiagLevel level, const char *msg)
{
    u32 i;

    for (i = 0U; i < f->diag.n && i < YEW_ARRAY_LEN(f->diag.msg); i++) {
        if (f->diag.level[i] == level && strcmp(f->diag.msg[i], msg) == 0)
            return true;
    }
    return false;
}

void test_syn_defs_ini_compiles_to_expected_context_table(void)
{
    static const char *const names[] = {"main", "value", "strings", "dq", "sq"};
    static const u32 first[] = {0U, 5U, 11U, 13U, 15U};
    static const u32 count[] = {5U, 6U, 2U, 2U, 1U};
    static const u8 dflt[] = {YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT,
                              YEW_ATTR_STRING, YEW_ATTR_STRING};
    static const u8 eol[] = {SYN_OP_STAY, SYN_OP_POP, SYN_OP_STAY,
                             SYN_OP_POP, SYN_OP_POP};
    static const u8 flags[] = {0U, YEW_SYN_CTX_UNIT_SPAN, 0U,
                               YEW_SYN_CTX_UNIT_ATOM, YEW_SYN_CTX_UNIT_ATOM};
    DefFix f;
    SynDef *def;
    char *src;
    size_t len;
    u32 nerr;
    u32 nwarn;
    u32 i;

    src = read_whole("runtime/syntax/ini.fl", &len);
    def_fix_open(&f);
    (void)fl_diag_add_file(&f.dc, "runtime/syntax/ini.fl", src, len);
    def = yew_syn_def_compile(&f.arena, &f.dc, (const u8 *)src, len, 0U,
                              &nerr, &nwarn);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 0U);
    YEW_ASSERT_EQ_U64(nwarn, 0U);
    YEW_ASSERT_EQ_U64(f.diag.n, 0U);
    YEW_ASSERT_EQ_STR(def->name, "ini");
    YEW_ASSERT_EQ_U64(def->root, 0U);
    YEW_ASSERT_EQ_U64(def->nctxs, 5U);
    YEW_ASSERT_EQ_U64(def->nrules, 16U);
    for (i = 0U; i < def->nctxs; i++) {
        YEW_ASSERT_EQ_STR(yew_syn_ctx_name(def, (u16)i), names[i]);
        YEW_ASSERT_EQ_U64(def->ctxs[i].first_rule, first[i]);
        YEW_ASSERT_EQ_U64(def->ctxs[i].nrules, count[i]);
        YEW_ASSERT_EQ_U64(def->ctxs[i].dflt_attr, dflt[i]);
        YEW_ASSERT_EQ_U64(def->ctxs[i].at_eol, eol[i]);
        YEW_ASSERT_EQ_U64(def->ctxs[i].flags, flags[i]);
    }
    YEW_ASSERT_EQ_U64(def->ctxs[1].eol_nop, 1U);
    YEW_ASSERT_EQ_U64(def->ctxs[3].eol_nop, 1U);
    YEW_ASSERT_EQ_U64(def->ctxs[4].eol_nop, 1U);
    YEW_ASSERT_NULL(yew_syn_ctx_name(def, 5U));
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(def, 5U), "\"");
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(def, 6U), "'");
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(def, 7U), "[;#].*$");
    YEW_ASSERT_EQ_U64(def->rules[2].consume, 1U);
    YEW_ASSERT_EQ_U64(def->rules[2].caps[1], YEW_ATTR_VARIABLE_MEMBER);
    YEW_ASSERT_EQ_U64(def->rules[3].op, SYN_OP_PUSH);
    YEW_ASSERT_EQ_U64(def->rules[3].target, 1U);
    YEW_ASSERT_EQ_U64(def->rules[5].op, SYN_OP_PUSH);
    YEW_ASSERT_EQ_U64(def->rules[5].target, 3U);
    YEW_ASSERT_EQ_U64(def->rules[6].target, 4U);
    YEW_ASSERT_EQ_U64(def->rules[8].attr, YEW_ATTR_BOOLEAN);
    YEW_ASSERT_EQ_U64(def->rules[9].attr, YEW_ATTR_NUMBER);
    YEW_ASSERT_EQ_U64(def->rules[10].attr, YEW_ATTR_VARIABLE);
    YEW_ASSERT_EQ_U64(def->rules[13].attr, YEW_ATTR_STRING_ESCAPE);
    YEW_ASSERT_EQ_U64(def->rules[14].op, SYN_OP_POP);
    YEW_ASSERT_EQ_U64(def->rules[14].nop, 1U);
    YEW_ASSERT_NULL(yew_syn_rule_pattern(def, def->nrules));
    def_fix_close(&f, def);
    free(src);
}

void test_syn_defs_alias_include_and_rule_features_pack(void)
{
    static const char src[] =
        "{ syntax: 1, attrs: { kw: \"keyword.control\" },\n"
        "  language: { name: \"features\", extensions: [\"feat\"],\n"
        "    filenames: [\"Featurefile\", \"*.feature\"],\n"
        "    shebangs: [\"feature\"], first_line: \"^FEATURE\", priority: 7,\n"
        "    comment: { line: \"//\", block: [\"/*\", \"*/\"] } },\n"
        "  root: \"main\", contexts: {\n"
        "    main: { include: \"common\", rules: [\n"
        "      { match: \"(B)(C)\", captures: { 1: \"number\", 2: \"string\" },\n"
        "        consume: 1, push: [\"left\", \"right\"], set_aux: 2, strip: true, value: true },\n"
        "      { match: \"D\", set: \"left\", value: false },\n"
        "      { aux: \"line_eq\", pop: true },\n"
        "      { match: \"I\", push: \"indent\" } ] },\n"
        "    common: { icase: true, rules: [{ match: \"A\", attr: \"kw\" }] },\n"
        "    left: { at_eol: \"set:main\", rules: [\n"
        "      { aux: \"literal\", aux_pre: \"r\", aux_post: \"#\", pop: 1 } ] },\n"
        "    right: { at_eol: \"pop:2\", rules: [\n"
        "      { aux: \"fence_close\", pop: 1 } ] },\n"
        "    indent: { rules: [ { aux: \"indent_lt\", pop: 1 } ] },\n"
        "  } }";
    DefFix f;
    SynDef *def;
    const SynLangDesc *lang;
    u32 nerr;
    u32 nwarn;
    u32 lang_id;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 0U);
    YEW_ASSERT_EQ_U64(nwarn, 0U);
    YEW_ASSERT_EQ_U64(def->nctxs, 5U);
    YEW_ASSERT_EQ_U64(def->nrules, 9U);
    YEW_ASSERT_EQ_STR(yew_syn_ctx_name(def, 0U), "main");
    YEW_ASSERT_EQ_STR(yew_syn_ctx_name(def, 1U), "common");
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(def, 0U), "A");
    YEW_ASSERT_EQ_U64(def->rules[0].attr, YEW_ATTR_KEYWORD_CONTROL);
    YEW_ASSERT_EQ_STR(yew_syn_rule_pattern(def, 1U), "(B)(C)");
    YEW_ASSERT_EQ_U64(def->rules[1].caps[1], YEW_ATTR_NUMBER);
    YEW_ASSERT_EQ_U64(def->rules[1].caps[2], YEW_ATTR_STRING);
    YEW_ASSERT_EQ_U64(def->rules[1].consume, 1U);
    YEW_ASSERT_EQ_U64(def->rules[1].op, SYN_OP_PUSH);
    YEW_ASSERT_EQ_U64(def->rules[1].npush, 2U);
    YEW_ASSERT_EQ_U64(def->rules[1].push[0], 2U);
    YEW_ASSERT_EQ_U64(def->rules[1].push[1], 3U);
    YEW_ASSERT(def->rules[1].flags & YEW_SYN_RULE_SET_AUX);
    YEW_ASSERT(def->rules[1].flags & YEW_SYN_RULE_STRIP);
    YEW_ASSERT(def->rules[1].flags & YEW_SYN_RULE_SET_VALUE);
    YEW_ASSERT_EQ_U64(def->rules[1].aux_group, 2U);
    YEW_ASSERT_EQ_U64(def->rules[2].op, SYN_OP_SET);
    YEW_ASSERT_EQ_U64(def->rules[2].target, 2U);
    YEW_ASSERT(def->rules[2].flags & YEW_SYN_RULE_CLR_VALUE);
    YEW_ASSERT_EQ_U64(def->rules[3].aux_match, SYN_AUXM_LINE_EQ);
    YEW_ASSERT_EQ_U64(def->rules[3].op, SYN_OP_POP);
    YEW_ASSERT_EQ_U64(def->rules[4].op, SYN_OP_PUSH);
    YEW_ASSERT_EQ_U64(def->rules[4].target, 4U);
    YEW_ASSERT_EQ_U64(def->rules[5].attr, YEW_ATTR_KEYWORD_CONTROL);
    YEW_ASSERT_EQ_U64(def->rules[6].aux_match, SYN_AUXM_LITERAL);
    YEW_ASSERT(def->rules[6].aux_pre != 0U);
    YEW_ASSERT(def->rules[6].aux_post != 0U);
    YEW_ASSERT_EQ_U64(def->rules[7].aux_match, SYN_AUXM_FENCE_CLOSE);
    YEW_ASSERT_EQ_U64(def->rules[8].aux_match, SYN_AUXM_INDENT_LT);
    YEW_ASSERT(def->rules[8].flags & YEW_SYN_RULE_ZERO_POP);
    YEW_ASSERT_EQ_U64(def->rules[8].op, SYN_OP_POP);
    YEW_ASSERT_EQ_U64(def->ctxs[2].at_eol, SYN_OP_SET);
    YEW_ASSERT_EQ_U64(def->ctxs[2].eol_target, 0U);
    YEW_ASSERT_EQ_U64(def->ctxs[3].at_eol, SYN_OP_POP);
    YEW_ASSERT_EQ_U64(def->ctxs[3].eol_nop, 2U);
    lang_id = yew_syn_lang_for("x.feat", NULL, 0U);
    YEW_ASSERT(lang_id != YEW_LANG_NONE);
    lang = yew_syn_lang_desc(lang_id);
    YEW_ASSERT_NOT_NULL(lang);
    YEW_ASSERT_EQ_STR(lang->name, "features");
    YEW_ASSERT_EQ_U64(lang->priority, 7U);
    YEW_ASSERT_EQ_STR(lang->comment.line, "//");
    YEW_ASSERT_EQ_STR(lang->comment.block_open, "/*");
    YEW_ASSERT_EQ_STR(lang->comment.block_close, "*/");
    def_fix_close(&f, def);
}

static void expect_one_error(const char *src, const char *message,
                             u32 line, u32 col)
{
    DefFix f;
    SynDef *def;
    u32 nerr;
    u32 nwarn;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 1U);
    YEW_ASSERT_EQ_U64(nwarn, 0U);
    YEW_ASSERT_EQ_U64(f.diag.n, 1U);
    YEW_ASSERT_EQ_U64(f.diag.level[0], FL_DIAG_ERROR);
    YEW_ASSERT_EQ_STR(f.diag.msg[0], message);
    YEW_ASSERT_EQ_U64(f.diag.sp[0].file_id, 0U);
    YEW_ASSERT_EQ_U64(f.diag.sp[0].line, line);
    YEW_ASSERT_EQ_U64(f.diag.sp[0].col, col);
    YEW_ASSERT(f.diag.sp[0].len > 0U);
    def_fix_close(&f, def);
}

void test_syn_defs_unknown_schema_diagnostic_is_exact_and_spanned(void)
{
    static const char src[] =
        "{\n"
        "    syntax: 2,\n"
        "    language: { name: \"diag\" },\n"
        "    contexts: { main: { rules: [] } },\n"
        "}\n";

    expect_one_error(src,
                     "unknown schema version 2 (this build understands 1)",
                     2U, 13U);
}

void test_syn_defs_unknown_rule_key_suggests_match(void)
{
    static const char src[] =
        "{\n"
        "  syntax: 1,\n"
        "  language: { name: \"diag\" },\n"
        "  contexts: {\n"
        "    main: {\n"
        "      rules: [{ matchh: \"x\" }],\n"
        "    },\n"
        "  },\n"
        "}\n";

    expect_one_error(src, "unknown key 'matchh' (did you mean 'match'?)",
                     6U, 17U);
}

void test_syn_defs_unknown_attr_suggests_string(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\", attr: \"strng\" }] },\n"
        "} }";

    expect_one_error(src, "unknown attr 'strng' (did you mean 'string'?)",
                     2U, 39U);
}

void test_syn_defs_unknown_push_context_is_rejected(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\", push: \"strings2\" }] },\n"
        "} }";

    expect_one_error(src, "no context named 'strings2'", 2U, 39U);
}

void test_syn_defs_two_state_operations_are_rejected(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\", push: \"child\", pop: 1 }] },\n"
        "  child: { rules: [{ match: \"x\", pop: 1 }] },\n"
        "} }";

    expect_one_error(src,
                     "rule has both 'push' and 'pop'; a rule performs exactly one state op",
                     2U, 19U);
}

void test_syn_defs_consume_requires_an_existing_capture(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"(a)(b)\", consume: 3 }] },\n"
        "} }";

    expect_one_error(src, "consume: 3 but the pattern has 2 capture groups",
                     2U, 47U);
}

void test_syn_defs_capture_index_is_bounded_to_seven(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"(a)\", captures: { 9: \"string\" } }] },\n"
        "} }";

    expect_one_error(src, "capture group 9 is out of range (0-7)",
                     2U, 47U);
}

void test_syn_defs_invalid_pattern_reports_pattern_offset(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"[abc\" }] },\n"
        "} }";
    DefFix f;
    SynDef *def;
    u32 nerr;
    u32 nwarn;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 1U);
    YEW_ASSERT_EQ_U64(nwarn, 0U);
    YEW_ASSERT_EQ_U64(f.diag.n, 1U);
    YEW_ASSERT_EQ_U64(f.diag.sp[0].file_id, 0U);
    YEW_ASSERT_EQ_U64(f.diag.sp[0].line, 2U);
    YEW_ASSERT_EQ_U64(f.diag.sp[0].col, 28U);
    YEW_ASSERT(strstr(f.diag.msg[0], "invalid pattern at offset") != NULL);
    YEW_ASSERT(strstr(f.diag.msg[0], "unterminated") != NULL);
    def_fix_close(&f, def);
}

void test_syn_defs_pop_count_above_four_is_rejected(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\", pop: 5 }] },\n"
        "} }";

    expect_one_error(src, "pop count 5 is out of range (1-4)", 2U, 38U);
}

void test_syn_defs_push_list_above_four_is_rejected(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\", push: [\"a\",\"b\",\"c\",\"d\",\"e\"] }] },\n"
        "  a:{rules:[{match:\"x\",pop:1}]}, b:{rules:[{match:\"x\",pop:1}]},\n"
        "  c:{rules:[{match:\"x\",pop:1}]}, d:{rules:[{match:\"x\",pop:1}]},\n"
        "  e:{rules:[{match:\"x\",pop:1}]},\n"
        "} }";
    DefFix f;
    SynDef *def;
    u32 nerr;
    u32 nwarn;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NULL(def);
    YEW_ASSERT(nerr >= 1U);
    YEW_ASSERT(diag_has(&f, FL_DIAG_ERROR,
                        "push list has 5 entries (limit 1-4)"));
    def_fix_close(&f, def);
}

void test_syn_defs_include_cycle_is_rejected(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { include: \"escapes\", rules: [] },\n"
        "  escapes: { include: \"main\", rules: [] },\n"
        "} }";
    DefFix f;
    SynDef *def;
    u32 nerr;
    u32 nwarn;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NULL(def);
    YEW_ASSERT(nerr >= 1U);
    YEW_ASSERT(f.diag.n >= 1U);
    YEW_ASSERT(strstr(f.diag.msg[0], "include cycle reaches") != NULL);
    YEW_ASSERT(f.diag.sp[0].line >= 2U);
    YEW_ASSERT(f.diag.sp[0].col > 0U);
    def_fix_close(&f, def);
}

void test_syn_defs_reachable_context_must_have_a_reducing_edge(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\", push: \"stuck\" }] },\n"
        "  stuck: { rules: [{ match: \"y\" }] },\n"
        "} }";

    expect_one_error(src, "context 'stuck' can never be popped", 3U, 10U);
}

void test_syn_defs_indent_lt_must_be_first_and_pop(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"diag\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\" }, { aux: \"indent_lt\", pop: 1 }] },\n"
        "} }";
    DefFix f;
    SynDef *def;
    u32 nerr;
    u32 nwarn;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 1U);
    YEW_ASSERT(diag_has(&f, FL_DIAG_ERROR,
                        "indent_lt must be the first rule in its context"));
    def_fix_close(&f, def);
}

void test_syn_defs_static_push_depth_reserves_four_runtime_frames(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"deep\" }, contexts: {\n"
        "main:{rules:[{match:\"a\",push:\"c1\"}]},\n"
        "c1:{rules:[{match:\"a\",push:\"c2\"},{match:\"z\",pop:1}]},\n"
        "c2:{rules:[{match:\"a\",push:\"c3\"},{match:\"z\",pop:1}]},\n"
        "c3:{rules:[{match:\"a\",push:\"c4\"},{match:\"z\",pop:1}]},\n"
        "c4:{rules:[{match:\"a\",push:\"c5\"},{match:\"z\",pop:1}]},\n"
        "c5:{rules:[{match:\"a\",push:\"c6\"},{match:\"z\",pop:1}]},\n"
        "c6:{rules:[{match:\"a\",push:\"c7\"},{match:\"z\",pop:1}]},\n"
        "c7:{rules:[{match:\"a\",push:\"c8\"},{match:\"z\",pop:1}]},\n"
        "c8:{rules:[{match:\"a\",push:\"c9\"},{match:\"z\",pop:1}]},\n"
        "c9:{rules:[{match:\"a\",push:\"c10\"},{match:\"z\",pop:1}]},\n"
        "c10:{rules:[{match:\"a\",push:\"c11\"},{match:\"z\",pop:1}]},\n"
        "c11:{rules:[{match:\"a\",push:\"c12\"},{match:\"z\",pop:1}]},\n"
        "c12:{rules:[{match:\"z\",pop:1}]}\n"
        "} }";

    expect_one_error(
        src,
        "context nesting can reach depth 13; the cap is 16 with 4 levels reserved for runtime recursion (see YEW_SYN_DEPTH_MAX)",
        2U, 6U);
}

void test_syn_defs_validation_warnings_are_nonfatal_and_exact(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"warnings\" }, contexts: {\n"
        "  main: { default: \"error\", rules: [\n"
        "    { match: \"x\" }, { match: \"x\" },\n"
        "    { match: \"y\", push: \"user\" }] },\n"
        "  legacy: { rules: [] },\n"
        "  shared: { default: \"text\", rules: [{ match: \"z\" }] },\n"
        "  user: { include: \"shared\", at_eol: \"pop\", rules: [] },\n"
        "} }";
    DefFix f;
    SynDef *def;
    u32 nerr;
    u32 nwarn;

    def_fix_open(&f);
    def = def_compile(&f, src, &nerr, &nwarn);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 0U);
    YEW_ASSERT_EQ_U64(nwarn, 4U);
    YEW_ASSERT(diag_has(&f, FL_DIAG_WARNING,
                        "default: 'error' paints every unmatched byte red; did you mean 'text'?"));
    YEW_ASSERT(diag_has(&f, FL_DIAG_WARNING,
                        "rule 2 is unreachable: rule 1 has the same pattern"));
    YEW_ASSERT(diag_has(&f, FL_DIAG_WARNING,
                        "context 'legacy' is unreachable"));
    YEW_ASSERT(diag_has(&f, FL_DIAG_WARNING,
                        "only 'rules' is used at an include site"));
    def_fix_close(&f, def);
}
