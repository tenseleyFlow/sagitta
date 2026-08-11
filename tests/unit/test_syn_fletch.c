#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl/lex.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "util/arena.h"
#include "util/intern.h"

static char *fletch_example_read(size_t *len)
{
    static const char marker[] =
        "\n# ---------------------------------------------------------------------";
    FILE *file = fopen("tests/fletch/14-example.fl", "rb");
    char *source;
    char *end;
    long size;

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_END), 0);
    size = ftell(file);
    YEW_ASSERT(size > 0L);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    source = malloc((size_t)size + 1U);
    YEW_ASSERT_NOT_NULL(source);
    YEW_ASSERT_EQ_U64(fread(source, 1U, (size_t)size, file), (size_t)size);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    source[size] = '\0';
    end = strstr(source, marker);
    YEW_ASSERT_NOT_NULL(end);
    *len = (size_t)(end - source) + 1U;
    source[*len] = '\0';
    return source;
}

static u64 attr_bit(u8 attr)
{
    YEW_ASSERT(attr < 64U);
    return UINT64_C(1) << attr;
}

static u64 fletch_token_attrs(FlTokKind kind)
{
    switch (kind) {
    case FL_T_INT:
    case FL_T_FLOAT:
        return attr_bit(YEW_ATTR_NUMBER);
    case FL_T_STRING:
        return attr_bit(YEW_ATTR_STRING) |
               attr_bit(YEW_ATTR_STRING_ESCAPE) |
               attr_bit(YEW_ATTR_ERROR);
    case FL_T_IDENT:
        return attr_bit(YEW_ATTR_TEXT) |
               attr_bit(YEW_ATTR_FUNCTION) |
               attr_bit(YEW_ATTR_VARIABLE_MEMBER) |
               attr_bit(YEW_ATTR_METHOD);
    case FL_T_LPAREN:
    case FL_T_RPAREN:
    case FL_T_LBRACKET:
    case FL_T_RBRACKET:
    case FL_T_LBRACE:
    case FL_T_RBRACE:
        return attr_bit(YEW_ATTR_PUNCT_BRACKET);
    case FL_T_COMMA:
    case FL_T_COLON:
    case FL_T_SEMI:
        return attr_bit(YEW_ATTR_PUNCT_DELIM);
    case FL_T_DOT:
        return attr_bit(YEW_ATTR_PUNCT_DELIM) | attr_bit(YEW_ATTR_METHOD);
    case FL_T_ATBRACKET:
        return attr_bit(YEW_ATTR_KEYWORD) |
               attr_bit(YEW_ATTR_PUNCT_BRACKET);
    case FL_T_PLUS:
    case FL_T_MINUS:
    case FL_T_STAR:
    case FL_T_SLASH:
    case FL_T_PERCENT:
    case FL_T_EQ:
    case FL_T_EQEQ:
    case FL_T_BANGEQ:
    case FL_T_LT:
    case FL_T_LE:
    case FL_T_GT:
    case FL_T_GE:
        return attr_bit(YEW_ATTR_OPERATOR);
    case FL_T_AND:
    case FL_T_AS:
    case FL_T_IN:
    case FL_T_NOT:
    case FL_T_OR:
        return attr_bit(YEW_ATTR_KEYWORD_OP);
    case FL_T_BREAK:
    case FL_T_CATCH:
    case FL_T_CONTINUE:
    case FL_T_ELSE:
    case FL_T_FOR:
    case FL_T_IF:
    case FL_T_RETURN:
    case FL_T_TRY:
    case FL_T_WHILE:
        return attr_bit(YEW_ATTR_KEYWORD_CONTROL);
    case FL_T_EDIT:
    case FL_T_FN:
    case FL_T_IMPORT:
    case FL_T_LET:
    case FL_T_MACRO:
        return attr_bit(YEW_ATTR_KEYWORD_STORAGE);
    case FL_T_FALSE:
    case FL_T_TRUE:
        return attr_bit(YEW_ATTR_BOOLEAN);
    case FL_T_NIL:
        return attr_bit(YEW_ATTR_CONSTANT_BUILTIN);
    case FL_M_COUNT:
        return attr_bit(YEW_ATTR_MOTION_COUNT);
    case FL_M_UNIT:
        return attr_bit(YEW_ATTR_MOTION_UNIT);
    case FL_M_ARROW:
        return attr_bit(YEW_ATTR_MOTION_ARROW);
    case FL_M_H:
        return attr_bit(YEW_ATTR_PUNCT_BRACKET);
    case FL_M_LPAREN:
    case FL_M_RPAREN:
    case FL_M_END:
        return attr_bit(YEW_ATTR_PUNCT_BRACKET);
    case FL_M_INSERT:
        return attr_bit(YEW_ATTR_KEYWORD) |
               attr_bit(YEW_ATTR_STRING) |
               attr_bit(YEW_ATTR_STRING_ESCAPE) |
               attr_bit(YEW_ATTR_ERROR);
    case FL_M_DEL:
    case FL_M_ESC:
        return attr_bit(YEW_ATTR_KEYWORD_CONTROL);
    case FL_M_WORD:
        return attr_bit(YEW_ATTR_MOTION_CMD);
    case FL_T_COMMENT:
        return attr_bit(YEW_ATTR_COMMENT);
    case FL_T_NEWLINE:
    case FL_T_EOF:
    case FL_T_ERROR:
    case FL_T_KIND__N:
        return 0U;
    }
    return 0U;
}

void test_syn_fletch_spec14_tokens_match_runtime_definition(void)
{
    size_t len;
    char *source = fletch_example_read(&len);
    u8 *cover = calloc(len == 0U ? 1U : len, sizeof(*cover));
    u8 *attrs = calloc(len == 0U ? 1U : len, sizeof(*attrs));
    size_t *line_start = calloc(len + 2U, sizeof(*line_start));
    bool seen[YEW_ATTR__COUNT] = {false};
    u32 lang = yew_syn_lang_named("fletch");
    const SynDef *def = yew_syn_def_for(lang);
    SynEngine *engine = yew_syn_engine_for(lang);
    SynStateTab *states;
    u32 state = YEW_SYN_STATE_ROOT;
    size_t nlines = 1U;
    size_t base = 0U;
    size_t i;
    Arena arena;
    Interner interner;
    DiagCtx dc;
    FlLexer lexer;
    u32 tokens = 0U;

    YEW_ASSERT_NOT_NULL(cover);
    YEW_ASSERT_NOT_NULL(attrs);
    YEW_ASSERT_NOT_NULL(line_start);
    YEW_ASSERT(lang != YEW_LANG_NONE);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_NOT_NULL(engine);
    YEW_ASSERT_EQ_STR(def->name, "fletch");

    for (i = 0U; i < len; i++) {
        if (source[i] == '\n')
            line_start[nlines++] = i + 1U;
    }

    while (base < len) {
        size_t end = base;
        SynSpan spans[256];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U,
                          YEW_SYN_STOP_OK};
        u32 si;

        while (end < len && source[end] != '\n')
            end++;
        yew_syn_line(engine, state, (const u8 *)source + base,
                     (u32)(end - base), &out);
        YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
        for (si = 0U; si < out.n; si++) {
            u32 j;

            YEW_ASSERT(out.spans[si].start + out.spans[si].len <= end - base);
            YEW_ASSERT(out.spans[si].attr < YEW_ATTR__COUNT);
            seen[out.spans[si].attr] = true;
            for (j = 0U; j < out.spans[si].len; j++) {
                size_t at = base + out.spans[si].start + j;
                cover[at]++;
                attrs[at] = out.spans[si].attr;
            }
        }
        state = out.exit_state;
        {
            const SynState *produced =
                yew_syn_state_get(yew_syn_engine_states(engine), state);
            YEW_ASSERT_NOT_NULL(produced);
            YEW_ASSERT_EQ_U64(produced->ndef, 1U);
            for (u8 depth = 0U; depth < produced->depth; depth++)
                YEW_ASSERT_EQ_U64(produced->f[depth].def, 0U);
        }
        base = end < len ? end + 1U : end;
    }

    arena_init(&arena);
    interner_init(&interner, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_diag_add_file(&dc, "fletch-spec-14.fl", source, len);
    fl_lex_init(&lexer, &arena, &dc, &interner, source, len, 0U);
    lexer.keep_comments = true;
    for (;;) {
        FlTok token = fl_lex_next(&lexer);
        u64 allowed;
        size_t start;
        size_t j;

        YEW_ASSERT(token.kind != FL_T_ERROR);
        if (token.kind == FL_T_EOF)
            break;
        YEW_ASSERT(token.sp.line > 0U);
        YEW_ASSERT((size_t)token.sp.line <= nlines);
        start = line_start[token.sp.line - 1U] + token.sp.col - 1U;
        YEW_ASSERT(start + token.sp.len <= len);
        if (token.kind == FL_T_NEWLINE) {
            YEW_ASSERT_EQ_U64(token.sp.len, 1U);
            YEW_ASSERT_EQ_U64((u8)source[start], (u8)'\n');
            continue;
        }
        allowed = fletch_token_attrs(token.kind);
        YEW_ASSERT(allowed != 0U);
        YEW_ASSERT(token.sp.len > 0U);
        for (j = 0U; j < token.sp.len; j++) {
            YEW_ASSERT_EQ_U64(cover[start + j], 1U);
            YEW_ASSERT((allowed & attr_bit(attrs[start + j])) != 0U);
        }
        tokens++;
    }
    YEW_ASSERT(tokens > 100U);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&dc), 0U);

    YEW_ASSERT(seen[YEW_ATTR_MOTION_UNIT]);
    YEW_ASSERT(seen[YEW_ATTR_MOTION_ARROW]);
    YEW_ASSERT(seen[YEW_ATTR_MOTION_COUNT]);
    YEW_ASSERT(seen[YEW_ATTR_MOTION_CMD]);

    states = yew_syn_engine_states(engine);
    for (i = YEW_SYN_STATE_ROOT; i < yew_syn_state_count(states); i++) {
        const SynState *produced = yew_syn_state_get(states, (u32)i);

        YEW_ASSERT_NOT_NULL(produced);
        YEW_ASSERT_EQ_U64(produced->ndef, 1U);
        for (u8 depth = 0U; depth < produced->depth; depth++)
            YEW_ASSERT_EQ_U64(produced->f[depth].def, 0U);
    }

    interner_free(&interner);
    arena_free_all(&arena);
    free(line_start);
    free(attrs);
    free(cover);
    free(source);
}
