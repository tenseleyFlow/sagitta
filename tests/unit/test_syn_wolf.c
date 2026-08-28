#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "syn/attr.h"
#include "syn/defs.h"
#include "syn/engine.h"

/*
 * Canonical bytes from wolf-lang ab23a23f2b190d6f9b45271149b5ac7032b8133e,
 * re-verified byte-identical at f9ee9aaa6712eead72ede2e933d1bf57b45cf1bc.
 * SHA-256: ca543483de3e53fa455e51c24dcfb59fca5141435a59f680d2710ba051326359
 */
static const char wolf_keywords[] =
    "as\nasm\nassume\nborrow\nbreak\ncomptime\nconst\ncontinue\ncopy\n"
    "defer\ndistinct\ndyn\nelse\nenum\nerrdefer\nexport\nextern\nfalse\n"
    "fn\nfor\nfreeze\nhandle\nif\nimpl\nimport\nin\nlet\nloop\nmatch\n"
    "move\nmut\nproc\npub\nregion\nreturn\nscope\nselect\nshared\nspawn\n"
    "struct\ntake\ntrait\ntrue\ntype\nunsafe\nuse\nvar\nweak\nwhen\nwhile\n";

static u8 wolf_attr_at(const SynLineOut *out, u32 off)
{
    u32 i;

    for (i = 0U; i < out->n; i++) {
        u32 hi = out->spans[i].start + out->spans[i].len;

        if (off >= out->spans[i].start && off < hi)
            return out->spans[i].attr;
    }
    return UINT8_MAX;
}

static u8 wolf_attr_of(SynEngine *engine, const char *line,
                       const char *needle, u32 within)
{
    SynSpan spans[256];
    SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    const char *found = strstr(line, needle);

    YEW_ASSERT_NOT_NULL(found);
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, (const u8 *)line,
                 (u32)strlen(line), &out);
    YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
    return wolf_attr_at(&out, (u32)(found - line) + within);
}

static u32 wolf_line(SynEngine *engine, u32 entry, const char *line,
                     SynLineOut *out)
{
    yew_syn_line(engine, entry, (const u8 *)line, (u32)strlen(line), out);
    YEW_ASSERT_EQ_U64(out->stop, YEW_SYN_STOP_OK);
    return out->exit_state;
}

static SynEngine *wolf_engine(void)
{
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named("wolf"));
    SynEngine *engine;

    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    return engine;
}

void test_syn_wolf_keyword_contract_is_exact_and_closed(void)
{
    char actual[sizeof(wolf_keywords)];
    static const char *near_misses[] = {
        "c", "rc", "pool", "from", "timeout", "noalias", "pkg",
        "self", "out", "inout", "lateout", "reg"
    };
    SynEngine *engine = wolf_engine();
    FILE *fp = fopen("tests/syn/wolf/lexical-contract.txt", "rb");
    size_t n;
    size_t i;
    size_t lines = 0U;

    YEW_ASSERT_NOT_NULL(fp);
    n = fread(actual, 1U, sizeof(actual), fp);
    YEW_ASSERT_EQ_I64(ferror(fp), 0);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_U64(n, sizeof(wolf_keywords) - 1U);
    YEW_ASSERT_EQ_MEM(actual, wolf_keywords, sizeof(wolf_keywords) - 1U);
    for (i = 0U; i < sizeof(wolf_keywords) - 1U; i++)
        lines += wolf_keywords[i] == '\n';
    YEW_ASSERT_EQ_U64(lines, 50U);

    {
        const char *word = wolf_keywords;

        while (*word != '\0') {
            const char *end = strchr(word, '\n');
            char token[16];
            size_t len;
            u8 attr;

            YEW_ASSERT_NOT_NULL(end);
            len = (size_t)(end - word);
            YEW_ASSERT(len < sizeof(token));
            memcpy(token, word, len);
            token[len] = '\0';
            attr = wolf_attr_of(engine, token, token, 0U);
            YEW_ASSERT(attr == YEW_ATTR_BOOLEAN ||
                       attr == YEW_ATTR_KEYWORD_CONTROL ||
                       attr == YEW_ATTR_KEYWORD_OP ||
                       attr == YEW_ATTR_KEYWORD_STORAGE);
            word = end + 1;
        }
    }
    for (i = 0U; i < YEW_ARRAY_LEN(near_misses); i++)
        YEW_ASSERT_EQ_U64(wolf_attr_of(engine, near_misses[i],
                                      near_misses[i], 0U),
                          YEW_ATTR_VARIABLE);
    yew_syn_engine_free(engine);
}

void test_syn_wolf_strings_formats_and_raw_fences_balance(void)
{
    const char *ordinary = "fn\"ordinary {value:>{width}}\"";
    const char *general = "path\"raw {value}\"";
    const char *nested = "\"{map[{key: 1}].value:0>8} {{literal}}\"";
    SynEngine *engine = wolf_engine();
    unsigned hashes;

    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, ordinary, "fn", 0U),
                      YEW_ATTR_KEYWORD_STORAGE);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, ordinary, "{value", 0U),
                      YEW_ATTR_STRING_INTERP);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, ordinary, ":>", 0U),
                      YEW_ATTR_STRING_SPECIAL);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, ordinary, "{width", 0U),
                      YEW_ATTR_STRING_INTERP);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, general, "path", 0U),
                      YEW_ATTR_TYPE);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, general, "{value", 0U),
                      YEW_ATTR_STRING_SPECIAL);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, nested, ": 1", 0U),
                      YEW_ATTR_OPERATOR);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, nested, ":0>", 0U),
                      YEW_ATTR_STRING_SPECIAL);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, nested, "{{", 0U),
                      YEW_ATTR_STRING_ESCAPE);

    for (hashes = 0U; hashes <= 16U; hashes++) {
        char line[80];
        SynSpan spans[64];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        size_t off = 0U;
        unsigned i;

        line[off++] = 'r';
        for (i = 0U; i < hashes; i++)
            line[off++] = '#';
        line[off++] = '"';
        memcpy(line + off, "raw {body}", 10U);
        off += 10U;
        line[off++] = '"';
        for (i = 0U; i < hashes; i++)
        line[off++] = '#';
        line[off] = '\0';
        wolf_line(engine, YEW_SYN_STATE_ROOT, line, &out);
        {
            const SynState *exit = yew_syn_state_get(
                yew_syn_engine_states(engine), out.exit_state);
            const SynState *root = yew_syn_state_get(
                yew_syn_engine_states(engine), YEW_SYN_STATE_ROOT);

            YEW_ASSERT_NOT_NULL(exit);
            YEW_ASSERT_NOT_NULL(root);
            YEW_ASSERT_EQ_U64(exit->depth, 1U);
            YEW_ASSERT_EQ_U64(exit->lost, 0U);
            YEW_ASSERT_EQ_U64(exit->f[0].ctx, root->f[0].ctx);
            YEW_ASSERT_EQ_U64(exit->f[0].def, root->f[0].def);
        }
        YEW_ASSERT_EQ_U64(wolf_attr_at(&out, 2U + hashes),
                          YEW_ATTR_STRING_SPECIAL);
    }
    yew_syn_engine_free(engine);
}

void test_syn_wolf_numbers_comments_and_declarations_are_pinned(void)
{
    const char *numbers = "1. 1..10 1.e5 1.0 9e-3 0xff 0o7 0b1 1_000";
    const char *decl = "#[test] fn run() { let value = true }";
    SynEngine *engine = wolf_engine();

    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "1.", 0U),
                      YEW_ATTR_NUMBER);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "1.", 1U),
                      YEW_ATTR_PUNCT_DELIM);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "1..10", 1U),
                      YEW_ATTR_OPERATOR);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "1.e5", 1U),
                      YEW_ATTR_METHOD);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "1.0", 0U),
                      YEW_ATTR_NUMBER);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "9e-3", 0U),
                      YEW_ATTR_NUMBER);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, numbers, "0xff", 0U),
                      YEW_ATTR_NUMBER);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "/// doc", "///", 0U),
                      YEW_ATTR_COMMENT_DOC);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "//! doc", "//!", 0U),
                      YEW_ATTR_COMMENT_DOC);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "//// ordinary", "////", 0U),
                      YEW_ATTR_COMMENT);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, decl, "#[", 0U),
                      YEW_ATTR_ATTRIBUTE);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, decl, "run", 0U),
                      YEW_ATTR_FUNCTION);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, decl, "value", 0U),
                      YEW_ATTR_VARIABLE);
    yew_syn_engine_free(engine);
}

void test_syn_wolf_refresh_pins_shebang_and_builtin_types(void)
{
    static const char *builtins[] = {
        "bool", "byte", "f32", "f64", "i8", "i16", "i32", "i64", "int",
        "str", "u8", "u16", "u32", "u64", "uint", "wrapping"
    };
    /* Load the REPO definition by path: the registry def can resolve to
     * an installed runtime, and this test pins the checked-in file. */
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    SynEngine *engine;
    size_t i;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    def = yew_syn_def_load(&arena, &dc, "runtime/syntax/wolf.fl");
    YEW_ASSERT_NOT_NULL(def);
    engine = yew_syn_engine_new(def);
    YEW_ASSERT_NOT_NULL(engine);

    /* [gram.lex.shebang]: trivia on the FIRST line of the file only. */
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "#!/usr/bin/env wolf", "#!", 0U),
                      YEW_ATTR_COMMENT);
    {
        SynSpan spans[64];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        u32 state;

        state = wolf_line(engine, YEW_SYN_STATE_ROOT,
                          "let a = 1", &out);
        out.n = 0U;
        (void)wolf_line(engine, state, "#! not trivia here", &out);
        YEW_ASSERT(wolf_attr_at(&out, 0U) != YEW_ATTR_COMMENT);
    }

    /* The closed builtin-type set renders as type.builtin... */
    for (i = 0U; i < YEW_ARRAY_LEN(builtins); i++)
        YEW_ASSERT_EQ_U64(wolf_attr_of(engine, builtins[i],
                                      builtins[i], 0U),
                          YEW_ATTR_TYPE_BUILTIN);
    /* ...whole words only, and prefixed strings keep their reading. */
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "f640", "f640", 0U),
                      YEW_ATTR_VARIABLE);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "str\"raw\"", "str", 0U),
                      YEW_ATTR_TYPE);

    /* Recent std surface needs no special rows: generic call/method. */
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "os_random(32)", "os_random", 0U),
                      YEW_ATTR_FUNCTION);
    YEW_ASSERT_EQ_U64(wolf_attr_of(engine, "s.chars()", ".chars", 0U),
                      YEW_ATTR_METHOD);
    yew_syn_engine_free(engine);
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
}

void test_syn_wolf_contextual_words_are_position_bounded(void)
{
    static const struct {
        const char *line;
        const char *word;
        u8 attr;
    } cases[] = {
        {"import c \"x.h\"", "c", YEW_ATTR_KEYWORD},
        {"region rc Arena", "rc", YEW_ATTR_TYPE},
        {"region pool Arena", "pool", YEW_ATTR_TYPE},
        {"assume noalias x", "noalias", YEW_ATTR_KEYWORD},
        {"pub(pkg) fn f() {}", "pkg", YEW_ATTR_KEYWORD},
        {"fn f(self: T) {}", "self", YEW_ATTR_VARIABLE_PARAM},
        {"select { value from q }", "from", YEW_ATTR_KEYWORD},
        {"select { timeout after }", "timeout", YEW_ATTR_KEYWORD},
        {"asm { out(reg) x }", "out", YEW_ATTR_KEYWORD_OP},
        {"asm { inout(reg) x }", "inout", YEW_ATTR_KEYWORD_OP},
        {"asm { lateout(reg) x }", "lateout", YEW_ATTR_KEYWORD_OP},
        {"asm { out(reg) x }", "reg", YEW_ATTR_TYPE},
        {"in", "in", YEW_ATTR_KEYWORD_OP},
    };
    SynEngine *engine = wolf_engine();
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        YEW_ASSERT_EQ_U64(wolf_attr_of(engine, cases[i].line,
                                      cases[i].word, 0U),
                          cases[i].attr);
    yew_syn_engine_free(engine);
}

void test_syn_wolf_unsafe_c_ignores_lexical_braces_for_depth(void)
{
    static const char *lines[] = {
        "unsafe c {",
        "if (ready) {",
        "call(\"} in string\"); /* { and } in comment */",
        "char brace = '}'; // } in line comment",
        "}",
        "}",
    };
    SynEngine *engine = wolf_engine();
    u32 state = YEW_SYN_STATE_ROOT;
    u32 outer;
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(lines); i++) {
        SynSpan spans[128];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

        state = wolf_line(engine, state, lines[i], &out);
        if (i == 0U)
            outer = state;
        if (i == 2U || i == 3U)
            YEW_ASSERT(state != outer);
        if (i == 4U)
            YEW_ASSERT_EQ_U64(state, outer);
    }
    YEW_ASSERT_EQ_U64(state, YEW_SYN_STATE_ROOT);
    yew_syn_engine_free(engine);
}
