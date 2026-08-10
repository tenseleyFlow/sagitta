#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syn/defs.h"

#include "syn_toy.h"

static void quiet_diag(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static bool bit_has(const u8 bits[32], u8 byte)
{
    return (bits[byte >> 3U] & (u8)(1U << (byte & 7U))) != 0U;
}

static char *first_read_ini(size_t *len)
{
    FILE *file = fopen("runtime/syntax/ini.fl", "rb");
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

static SynDef *first_compile(Arena *arena, DiagCtx *dc, const char *src,
                             size_t len)
{
    u32 nerr;
    u32 nwarn;
    u32 file_id = fl_diag_add_file(dc, "first.fl", src, len);
    SynDef *def = yew_syn_def_compile(arena, dc, (const u8 *)src, len,
                                      file_id, &nerr, &nwarn);

    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(nerr, 0U);
    YEW_ASSERT_EQ_U64(nwarn, 0U);
    return def;
}

void test_syn_firstbyte_ini_includes_every_observed_leading_byte(void)
{
    static const u8 tails[][12] = {
        "", "a", "0", " ", "true", "=value", "]", "\"text\""
    };
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    char *src;
    size_t len;
    u32 rule;

    src = first_read_ini(&len);
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, quiet_diag, NULL);
    def = first_compile(&arena, &dc, src, len);
    for (rule = 0U; rule < def->nrules; rule++) {
        u32 byte;

        for (byte = 0U; byte < 256U; byte++) {
            bool observed = false;
            u32 tail;

            if (def->rules[rule].re == NULL)
                continue;
            for (tail = 0U; tail < YEW_ARRAY_LEN(tails); tail++) {
                u8 sample[16];
                size_t tail_len = strlen((const char *)tails[tail]);
                YewReInput in;
                YewReMatch match;

                sample[0] = (u8)byte;
                (void)memcpy(sample + 1U, tails[tail], tail_len);
                in = yew_re_input_bytes(sample, 1U + tail_len);
                if (yew_re_match_at(def->rules[rule].re, &in, BYTEOFF(0U),
                                    &match))
                    observed = true;
            }
            YEW_ASSERT(!observed || bit_has(def->rules[rule].first,
                                            (u8)byte));
        }
    }
    YEW_ASSERT(yew_syn_def_firstbyte_check(def, NULL, NULL));
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
    free(src);
}

void test_syn_firstbyte_toy_definition_passes_the_shared_selfcheck(void)
{
    SynToy toy;
    u32 bad_rule = UINT32_MAX;
    u8 bad_byte = UINT8_MAX;

    syn_toy_init(&toy);
    YEW_ASSERT(yew_syn_def_firstbyte_check(&toy.def, &bad_rule, &bad_byte));
    YEW_ASSERT_EQ_U64(bad_rule, UINT32_MAX);
    YEW_ASSERT_EQ_U64(bad_byte, UINT8_MAX);
    syn_toy_free(&toy);
}

void test_syn_firstbyte_literal_class_utf8_and_icase_sets_are_safe(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"firsts\" }, contexts: {\n"
        "  main: { rules: [\n"
        "    { match: \"x\", icase: true },\n"
        "    { match: \"[0-2]\" },\n"
        "    { match: \"é\" },\n"
        "    { match: \"^z\" }\n"
        "  ] }\n"
        "} }";
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    u32 byte;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, quiet_diag, NULL);
    def = first_compile(&arena, &dc, src, strlen(src));
    YEW_ASSERT_EQ_U64(def->nrules, 4U);
    YEW_ASSERT(bit_has(def->rules[0].first, (u8)'x'));
    YEW_ASSERT(bit_has(def->rules[0].first, (u8)'X'));
    YEW_ASSERT(bit_has(def->rules[1].first, (u8)'0'));
    YEW_ASSERT(bit_has(def->rules[1].first, (u8)'1'));
    YEW_ASSERT(bit_has(def->rules[1].first, (u8)'2'));
    YEW_ASSERT(bit_has(def->rules[2].first, UINT8_C(0xc3)));
    for (byte = 0U; byte < 256U; byte++)
        YEW_ASSERT(bit_has(def->rules[3].first, (u8)byte));
    YEW_ASSERT(yew_syn_def_firstbyte_check(def, NULL, NULL));
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
}

void test_syn_firstbyte_deliberate_narrowing_is_detected(void)
{
    static const char src[] =
        "{ syntax: 1, language: { name: \"narrow\" }, contexts: {\n"
        "  main: { rules: [{ match: \"x\" }] }\n"
        "} }";
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    u32 bad_rule = UINT32_MAX;
    u8 bad_byte = 0U;

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, quiet_diag, NULL);
    def = first_compile(&arena, &dc, src, strlen(src));
    YEW_ASSERT(bit_has(def->rules[0].first, (u8)'x'));
    def->rules[0].first[(u8)'x' >> 3U] &=
        (u8)~(1U << ((u8)'x' & 7U));
    YEW_ASSERT(!yew_syn_def_firstbyte_check(def, &bad_rule, &bad_byte));
    YEW_ASSERT_EQ_U64(bad_rule, 0U);
    YEW_ASSERT_EQ_U64(bad_byte, (u8)'x');
    yew_syn_def_dispose(def);
    arena_free_all(&arena);
}
